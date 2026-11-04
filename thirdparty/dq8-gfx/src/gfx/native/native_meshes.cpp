// The native pipeline's mesh draws (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md step 4b): the frame's MeshDraw records into the
// scene target. Depth is a float target of its own, reversed (cleared to 0,
// nearer is greater, as the game compares). Nothing of the GS here.

#include "gfx/native/native_pipeline.h"

#include "native_mesh.frag.spv.h"
#include "native_mesh.vert.spv.h"

#include <algorithm>
#include <cstring>

namespace dq8::gfx::native
{
namespace
{
// A vertex as the mesh shader takes it.
struct GpuVertex
{
    float position[3];
    float uv[2];
    float color[4];
};
static_assert(sizeof(GpuVertex) == 36u, "tightly packed");

struct DrawUniforms
{
    float clip[16];
    float viewport[16];
    float target[4];
    float fog[4];
};

struct MaterialUniforms
{
    uint32_t mode[4];
    uint32_t test[4];
    float size[4];
    float blend[4];
    float fogColor[4];
};

void multiply(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}

// The pipeline's state as one key: blend (3 bits), depth compare (3), depth
// write (1), depth used at all (1), color write mask (4).
uint32_t pipelineKey(const Material &m)
{
    const bool depth = m.depthTest != Compare::Always || m.depthWrite;
    return static_cast<uint32_t>(m.blend) | (static_cast<uint32_t>(m.depthTest) << 3) |
           (m.depthWrite ? 1u << 6 : 0u) | (depth ? 1u << 7 : 0u) | (static_cast<uint32_t>(m.colorWrite & 0xFu) << 8);
}

SDL_GPUCompareOp compareOp(Compare c)
{
    switch (c)
    {
    case Compare::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    case Compare::Never: return SDL_GPU_COMPAREOP_NEVER;
    case Compare::Less: return SDL_GPU_COMPAREOP_LESS;
    case Compare::LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case Compare::Equal: return SDL_GPU_COMPAREOP_EQUAL;
    case Compare::GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case Compare::Greater: return SDL_GPU_COMPAREOP_GREATER;
    case Compare::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

bool growBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *&buffer, uint32_t &capacity, uint32_t needed,
                SDL_GPUBufferUsageFlags usage, std::string &error)
{
    if (buffer && capacity >= needed)
        return true;
    if (buffer)
        SDL_ReleaseGPUBuffer(device, buffer);
    uint32_t size = std::max<uint32_t>(capacity, 1u << 20);
    while (size < needed)
        size *= 2u;
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = size;
    buffer = SDL_CreateGPUBuffer(device, &info);
    capacity = buffer ? size : 0u;
    if (!buffer)
        error = std::string("SDL_CreateGPUBuffer(native meshes): ") + SDL_GetError();
    return buffer != nullptr;
}
} // namespace

bool Renderer::ensureMeshes(SDL_GPUDevice *device, std::string &error)
{
    if (m_meshVertex && m_meshFragment)
        return true;
    if (m_meshFailed)
        return false;
    m_meshFailed = true; // until made
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV))
    {
        error = "no SPIR-V: the native mesh shaders have no other build yet";
        return false;
    }
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kNativeMeshVertSpirv);
    vertexInfo.code_size = sizeof(kNativeMeshVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kNativeMeshFragSpirv);
    fragmentInfo.code_size = sizeof(kNativeMeshFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 2u;
    fragmentInfo.num_uniform_buffers = 1u;
    m_meshVertex = SDL_CreateGPUShader(device, &vertexInfo);
    m_meshFragment = m_meshVertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
    if (!m_meshVertex || !m_meshFragment)
    {
        error = std::string("SDL_CreateGPUShader(native mesh): ") + SDL_GetError();
        return false;
    }
    m_depthFormat = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                        ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
                        : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    // A white texel for untextured draws: the shader ignores it, but a
    // sampler binding must be made.
    m_white = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1u, 1u, error);
    if (!m_white)
        return false;
    static const Bytes white = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 255, 255, 255});
    m_uploads.push_back({m_white, 1u, 1u, white});
    m_meshFailed = false;
    return true;
}

bool Renderer::ensureDepth(SDL_GPUDevice *device, std::string &error)
{
    if (m_depth && m_depthWidth == m_sceneWidth && m_depthHeight == m_sceneHeight)
        return true;
    if (m_depth)
        SDL_ReleaseGPUTexture(device, m_depth);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = m_depthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = m_sceneWidth;
    info.height = m_sceneHeight;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    m_depth = SDL_CreateGPUTexture(device, &info);
    if (!m_depth)
    {
        error = std::string("SDL_CreateGPUTexture(native depth): ") + SDL_GetError();
        m_depthWidth = m_depthHeight = 0;
        return false;
    }
    m_depthWidth = m_sceneWidth;
    m_depthHeight = m_sceneHeight;
    return true;
}

SDL_GPUSampler *Renderer::meshSampler(SDL_GPUDevice *device, uint32_t key)
{
    SDL_GPUSampler *&sampler = m_meshSamplers[key & 7u];
    if (sampler)
        return sampler;
    SDL_GPUSamplerCreateInfo info{};
    const bool linear = key & 1u;
    info.min_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mag_filter = info.min_filter;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = (key & 2u) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    info.address_mode_v = (key & 4u) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler = SDL_CreateGPUSampler(device, &info);
    return sampler;
}

SDL_GPUGraphicsPipeline *Renderer::meshPipeline(SDL_GPUDevice *device, uint32_t key)
{
    if (const auto it = m_meshPipelines.find(key); it != m_meshPipelines.end())
        return it->second;
    const Blend blend = static_cast<Blend>(key & 7u);
    const Compare depthTest = static_cast<Compare>((key >> 3) & 7u);
    const bool depthWrite = (key >> 6) & 1u;
    const bool depth = (key >> 7) & 1u;

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUColorTargetBlendState &b = color.blend_state;
    b.enable_color_write_mask = true;
    b.color_write_mask = static_cast<SDL_GPUColorComponentFlags>((key >> 8) & 0xFu);
    // The stored alpha is the source's, as the game's blend leaves it.
    b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    b.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    switch (blend)
    {
    case Blend::Opaque:
        break;
    case Blend::Alpha:
    case Blend::FixedAlpha:
        b.enable_blend = true;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case Blend::Add:
        b.enable_blend = true;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case Blend::Subtract:
        b.enable_blend = true;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        break;
    case Blend::Multiply:
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        break;
    }

    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0u;
    buffer.pitch = sizeof(GpuVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[3]{};
    attributes[0] = {0u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, position)};
    attributes[1] = {1u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv)};
    attributes[2] = {2u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, color)};

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = m_meshVertex;
    info.fragment_shader = m_meshFragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 3u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; // the producer's indices are already culled
    // Clamped, not clipped, at the far plane: the game's sky sits past it.
    info.rasterizer_state.enable_depth_clip = false;
    info.depth_stencil_state.enable_depth_test = depth;
    info.depth_stencil_state.enable_depth_write = depth && depthWrite;
    info.depth_stencil_state.compare_op = compareOp(depthTest);
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = m_depthFormat;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline)
        std::fprintf(stderr, "[native-pipeline] mesh pipeline %x refused: %s\n", key, SDL_GetError());
    m_meshPipelines.emplace(key, pipeline); // a refusal is kept too: not retried every draw
    return pipeline;
}

bool Renderer::uploadMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error)
{
    m_firstIndex.clear();
    m_vertexOffset.clear();
    uint64_t vertices = 0u, indices = 0u;
    for (const MeshDraw &d : m_frameMeshes)
    {
        m_firstIndex.push_back(static_cast<uint32_t>(indices));
        m_vertexOffset.push_back(static_cast<int32_t>(vertices));
        vertices += d.vertices.size();
        indices += d.indices.size();
    }
    if (indices == 0u)
        return true;
    const uint64_t vertexBytes = vertices * sizeof(GpuVertex), indexBytes = indices * sizeof(uint32_t);
    if (vertexBytes + indexBytes > (1ull << 31))
    {
        error = "native meshes: a frame over 2 GiB of vertices";
        return false;
    }
    if (!growBuffer(device, m_vertexBuffer, m_vertexCapacity, static_cast<uint32_t>(vertexBytes),
                    SDL_GPU_BUFFERUSAGE_VERTEX, error) ||
        !growBuffer(device, m_indexBuffer, m_indexCapacity, static_cast<uint32_t>(indexBytes),
                    SDL_GPU_BUFFERUSAGE_INDEX, error))
        return false;
    const uint32_t total = static_cast<uint32_t>(vertexBytes + indexBytes);
    if (!m_meshTransfer || m_transferCapacity < total)
    {
        if (m_meshTransfer)
            SDL_ReleaseGPUTransferBuffer(device, m_meshTransfer);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = std::max(total, m_transferCapacity * 2u);
        m_meshTransfer = SDL_CreateGPUTransferBuffer(device, &info);
        m_transferCapacity = m_meshTransfer ? info.size : 0u;
        if (!m_meshTransfer)
        {
            error = std::string("SDL_CreateGPUTransferBuffer(native meshes): ") + SDL_GetError();
            return false;
        }
    }
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, m_meshTransfer, true));
    if (!mapped)
    {
        error = std::string("SDL_MapGPUTransferBuffer(native meshes): ") + SDL_GetError();
        return false;
    }
    auto *v = reinterpret_cast<GpuVertex *>(mapped);
    auto *ix = reinterpret_cast<uint32_t *>(mapped + vertexBytes);
    for (const MeshDraw &d : m_frameMeshes)
    {
        for (const Vertex &in : d.vertices)
        {
            std::memcpy(v->position, in.position, sizeof(v->position));
            std::memcpy(v->uv, in.uv, sizeof(v->uv));
            std::memcpy(v->color, in.color, sizeof(v->color));
            ++v;
        }
        std::memcpy(ix, d.indices.data(), d.indices.size() * sizeof(uint32_t));
        ix += d.indices.size();
    }
    SDL_UnmapGPUTransferBuffer(device, m_meshTransfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source{m_meshTransfer, 0u};
    SDL_GPUBufferRegion destination{m_vertexBuffer, 0u, static_cast<uint32_t>(vertexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    source.offset = static_cast<uint32_t>(vertexBytes);
    destination = {m_indexBuffer, 0u, static_cast<uint32_t>(indexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

void Renderer::drawMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass,
                          uint32_t scale)
{
    if (!m_vertexBuffer || !m_indexBuffer)
        return;
    const SDL_GPUBufferBinding vertexBinding{m_vertexBuffer, 0u};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{m_indexBuffer, 0u};
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const float targetW = float(m_sceneWidth / std::max(scale, 1u));
    const float targetH = float(m_sceneHeight / std::max(scale, 1u));
    SDL_GPUGraphicsPipeline *bound = nullptr;
    for (size_t n = 0; n < m_frameMeshes.size(); ++n)
    {
        const MeshDraw &d = m_frameMeshes[n];
        if (d.indices.empty())
            continue;
        const auto camera = m_cameras.find(d.camera);
        if (d.camera == 0u || camera == m_cameras.end() || !d.hasModel)
        {
            ++m_meshCounts.noCamera;
            continue;
        }
        const Material &m = d.material;
        MaterialUniforms mu{};
        SDL_GPUTexture *image = m_white, *palette = m_white;
        float ownW = 1.0f, ownH = 1.0f;
        if (m.texture != 0u)
        {
            const auto it = m_images.find(m.texture);
            if (it == m_images.end())
            {
                ++m_meshCounts.noTexture;
                continue;
            }
            const Image &img = it->second;
            image = palette = img.texture;
            ownW = float(img.width);
            ownH = float(img.height);
            mu.mode[0] = 1u;
            if (img.format == PixelFormat::Index8)
            {
                const auto p = m_palettes.find(m.palette ? m.palette : img.palette);
                if (p == m_palettes.end())
                {
                    ++m_meshCounts.noTexture;
                    continue;
                }
                palette = p->second.texture;
                mu.mode[0] = 2u;
                mu.mode[3] = p->second.width;
            }
            mu.mode[1] = static_cast<uint32_t>(m.function);
            mu.mode[2] = m.textureAlpha ? 1u : 0u;
        }
        SDL_GPUGraphicsPipeline *pipeline = meshPipeline(device, pipelineKey(m));
        if (!pipeline)
        {
            ++m_meshCounts.noPipeline;
            continue;
        }
        if (pipeline != bound)
        {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
        }
        mu.test[0] = static_cast<uint32_t>(m.alphaTest);
        mu.test[1] = m.alphaRef;
        mu.test[2] = m.alphaFail == AlphaFail::Nothing ? 1u : 0u;
        mu.test[3] = m.fog ? 1u : 0u;
        mu.size[0] = m.textureWidth ? float(m.textureWidth) : ownW;
        mu.size[1] = m.textureHeight ? float(m.textureHeight) : ownH;
        mu.size[2] = ownW;
        mu.size[3] = ownH;
        mu.blend[0] = float(m.blendFactor) / 128.0f;
        mu.blend[1] = m.blend == Blend::FixedAlpha ? 1.0f : 0.0f;
        mu.blend[2] = m.clampU ? 1.0f : 0.0f;
        mu.blend[3] = m.clampV ? 1.0f : 0.0f;
        for (int i = 0; i < 3; ++i)
            mu.fogColor[i] = float(d.fogColor[i]) / 255.0f;

        DrawUniforms du{};
        float modelView[16];
        multiply(d.model, camera->second.view, modelView);
        multiply(modelView, camera->second.projection, du.clip);
        std::memcpy(du.viewport, camera->second.viewport, sizeof(du.viewport));
        du.target[0] = targetW;
        du.target[1] = targetH;
        std::memcpy(du.fog, d.fog, sizeof(du.fog));

        SDL_PushGPUVertexUniformData(commands, 0, &du, sizeof(du));
        SDL_PushGPUFragmentUniformData(commands, 0, &mu, sizeof(mu));
        const uint32_t samplerKey = (m.linear ? 1u : 0u) | (m.clampU ? 2u : 0u) | (m.clampV ? 4u : 0u);
        SDL_GPUSampler *sampler = meshSampler(device, samplerKey);
        const SDL_GPUTextureSamplerBinding bindings[2] = {{image, sampler}, {palette, sampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);

        const int32_t sx0 = std::clamp(d.scissor.x0, 0, int32_t(targetW));
        const int32_t sy0 = std::clamp(d.scissor.y0, 0, int32_t(targetH));
        const int32_t sx1 = std::clamp(d.scissor.x1, sx0, int32_t(targetW));
        const int32_t sy1 = std::clamp(d.scissor.y1, sy0, int32_t(targetH));
        const int32_t s = int32_t(std::max(scale, 1u));
        const SDL_Rect scissor{sx0 * s, sy0 * s, (sx1 - sx0) * s, (sy1 - sy0) * s};
        SDL_SetGPUScissor(pass, &scissor);
        SDL_DrawGPUIndexedPrimitives(pass, static_cast<uint32_t>(d.indices.size()), 1, m_firstIndex[n],
                                     m_vertexOffset[n], 0);
        ++m_meshCounts.drawn;
    }
}

void Renderer::releaseMeshes(SDL_GPUDevice *device)
{
    for (auto &[key, pipeline] : m_meshPipelines)
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    m_meshPipelines.clear();
    for (SDL_GPUSampler *&sampler : m_meshSamplers)
    {
        if (sampler)
            SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
    }
    if (m_meshVertex)
        SDL_ReleaseGPUShader(device, m_meshVertex);
    if (m_meshFragment)
        SDL_ReleaseGPUShader(device, m_meshFragment);
    if (m_depth)
        SDL_ReleaseGPUTexture(device, m_depth);
    if (m_white)
        SDL_ReleaseGPUTexture(device, m_white);
    if (m_vertexBuffer)
        SDL_ReleaseGPUBuffer(device, m_vertexBuffer);
    if (m_indexBuffer)
        SDL_ReleaseGPUBuffer(device, m_indexBuffer);
    if (m_meshTransfer)
        SDL_ReleaseGPUTransferBuffer(device, m_meshTransfer);
    m_meshVertex = m_meshFragment = nullptr;
    m_depth = m_white = nullptr;
    m_vertexBuffer = m_indexBuffer = nullptr;
    m_meshTransfer = nullptr;
    m_vertexCapacity = m_indexCapacity = m_transferCapacity = 0u;
    m_depthWidth = m_depthHeight = 0u;
    m_meshFailed = false;
}
} // namespace dq8::gfx::native
