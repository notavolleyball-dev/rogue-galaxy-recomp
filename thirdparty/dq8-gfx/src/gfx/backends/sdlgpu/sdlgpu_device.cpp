#include "gfx/backends/sdlgpu/sdlgpu_device.h"

#include "present_fsr.frag.spv.h"
#include "present_sides.frag.spv.h"
#include "present_triangle.vert.spv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

SdlGpuDevice::~SdlGpuDevice() {
    destroy();
}

bool SdlGpuDevice::create(std::string &error) {
    m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!m_device) {
        error = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
        return false;
    }
    return true;
}

void SdlGpuDevice::destroy() {
    if (!m_device)
        return;
    cancelPending();
    SDL_WaitForGPUIdle(m_device);
    for (SDL_GPUFence *fence : m_inFlight)
        SDL_ReleaseGPUFence(m_device, fence);
    m_inFlight.clear();
    if (m_sidesPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_sidesPipeline);
    m_sidesPipeline = nullptr;
    m_sidesFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    for (SDL_GPUGraphicsPipeline *&pipeline : m_fsrPipelines) {
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
        pipeline = nullptr;
    }
    if (m_linearSampler)
        SDL_ReleaseGPUSampler(m_device, m_linearSampler);
    m_linearSampler = nullptr;
    SDL_DestroyGPUDevice(m_device);
    m_device = nullptr;
}

std::string SdlGpuDevice::driverName() const {
    if (!m_device)
        return {};
    const char *name = SDL_GetGPUDeviceDriver(m_device);
    return name ? name : std::string{};
}

bool SdlGpuDevice::downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                                   std::vector<uint8_t> &out, std::string &error) const {
    SDL_GPUDevice *device = m_device;
    if (!device || !texture) {
        error = "no texture to download";
        return false;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(bytes);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!download) {
        error = std::string("SDL_CreateGPUTransferBuffer(download): ") + SDL_GetError();
        return false;
    }
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(download): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUFence *fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1u);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        error = std::string("SDL_WaitForGPUFences(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    std::memcpy(out.data(), mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    return true;
}

bool SdlGpuDevice::ensureLinearSampler(std::string &error) {
    if (m_linearSampler)
        return true;
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_linearSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (!m_linearSampler) {
        error = std::string("SDL_CreateGPUSampler(present): ") + SDL_GetError();
        return false;
    }
    return true;
}

// A pass over the whole target from one triangle (present_triangle.vert),
// its fragment shader taking one sampler and one uniform buffer.
SDL_GPUGraphicsPipeline *SdlGpuDevice::fullscreenPipeline(const void *fragmentCode, size_t fragmentSize,
                                                          SDL_GPUTextureFormat format, const char *what,
                                                          std::string &error) {
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kPresentTriangleVertSpirv);
    vertexInfo.code_size = sizeof(kPresentTriangleVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(fragmentCode);
    fragmentInfo.code_size = fragmentSize;
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 1u;
    fragmentInfo.num_uniform_buffers = 1u;
    SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!vertex) {
        error = std::string("SDL_CreateGPUShader(") + what + " vertex): " + SDL_GetError();
        return nullptr;
    }
    SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
    if (!fragment) {
        error = std::string("SDL_CreateGPUShader(") + what + " fragment): " + SDL_GetError();
        SDL_ReleaseGPUShader(m_device, vertex);
        return nullptr;
    }
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    SDL_ReleaseGPUShader(m_device, fragment);
    SDL_ReleaseGPUShader(m_device, vertex);
    if (!pipeline)
        error = std::string("SDL_CreateGPUGraphicsPipeline(") + what + "): " + SDL_GetError();
    return pipeline;
}

bool SdlGpuDevice::drawSides(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                             uint32_t width, uint32_t height, SDL_GPUTexture *source, uint32_t sourceWidth,
                             uint32_t sourceHeight, bool blur, float brightness, std::string &error) {
    if (!ensureLinearSampler(error))
        return false;
    if (m_sidesPipeline && m_sidesFormat != format) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_sidesPipeline);
        m_sidesPipeline = nullptr;
    }
    if (!m_sidesPipeline) {
        m_sidesPipeline = fullscreenPipeline(kPresentSidesFragSpirv, sizeof(kPresentSidesFragSpirv), format,
                                             "sides", error);
        if (!m_sidesPipeline)
            return false;
        m_sidesFormat = format;
    }
    SDL_GPUColorTargetInfo targetInfo{};
    targetInfo.texture = target;
    targetInfo.load_op = SDL_GPU_LOADOP_DONT_CARE;
    targetInfo.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &targetInfo, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(sides): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_sidesPipeline);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_GPUTextureSamplerBinding binding{source, m_linearSampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    const float uniforms[8] = {float(width), float(height), brightness, blur ? 1.0f : 0.0f,
                               float(sourceWidth), float(sourceHeight), 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(commands, 0u, uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

bool SdlGpuDevice::drawFsr(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                           const uint32_t to[4], SDL_GPULoadOp loadOp, SDL_GPUTexture *source,
                           const uint32_t region[4], bool rcas, float sharpness, std::string &error) {
    if (!ensureLinearSampler(error))
        return false;
    // A pipeline per target format: the scratch's RGBA8 and the window's.
    size_t slot = 0u;
    while (slot < 2u && m_fsrPipelines[slot] && m_fsrFormats[slot] != format)
        ++slot;
    if (slot == 2u) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_fsrPipelines[1]);
        m_fsrPipelines[1] = nullptr;
        slot = 1u;
    }
    if (!m_fsrPipelines[slot]) {
        m_fsrPipelines[slot] = fullscreenPipeline(kPresentFsrFragSpirv, sizeof(kPresentFsrFragSpirv), format,
                                                  "fsr", error);
        if (!m_fsrPipelines[slot])
            return false;
        m_fsrFormats[slot] = format;
    }
    SDL_GPUColorTargetInfo targetInfo{};
    targetInfo.texture = target;
    targetInfo.load_op = loadOp;
    targetInfo.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &targetInfo, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(fsr): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_fsrPipelines[slot]);
    const SDL_GPUViewport viewport{float(to[0]), float(to[1]), float(to[2]), float(to[3]), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{int(to[0]), int(to[1]), int(to[2]), int(to[3])};
    SDL_SetGPUScissor(pass, &scissor);
    const SDL_GPUTextureSamplerBinding binding{source, m_linearSampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    const float uniforms[12] = {float(to[0]),     float(to[1]),     float(to[2]),     float(to[3]),
                                float(region[0]), float(region[1]), float(region[2]), float(region[3]),
                                rcas ? 1.0f : 0.0f, sharpness, 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(commands, 0u, uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

SDL_GPUCommandBuffer *SdlGpuDevice::pendingCommands(std::string &error, const char *what) {
    if (!m_pending) {
        m_pending = SDL_AcquireGPUCommandBuffer(m_device);
        if (!m_pending)
            error = std::string("SDL_AcquireGPUCommandBuffer(pending): ") + SDL_GetError();
        else if (m_gpuTimes)
            SDL_InsertGPUDebugLabel(m_pending, "other");
    }
    if (m_pending && m_gpuTimes && what)
        SDL_InsertGPUDebugLabel(m_pending, what);
    return m_pending;
}

void SdlGpuDevice::retire() {
    while (!m_inFlight.empty() && SDL_QueryGPUFence(m_device, m_inFlight.front())) {
        SDL_ReleaseGPUFence(m_device, m_inFlight.front());
        m_inFlight.pop_front();
    }
}

bool SdlGpuDevice::submitPending(std::string &error) {
    if (!m_pending)
        return true;
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(m_pending);
    m_pending = nullptr;
    ++m_submissions;
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(pending): ") + SDL_GetError();
        return false;
    }
    m_inFlight.push_back(fence);
    retire();
    return true;
}

bool SdlGpuDevice::submitPendingAndWait(std::string &error) {
    if (!m_pending)
        return true;
    if (!submitPending(error))
        return false;
    if (m_inFlight.empty())
        return true;
    SDL_GPUFence *fences[] = {m_inFlight.back()};
    const bool waited = SDL_WaitForGPUFences(m_device, true, fences, 1u);
    if (!waited)
        error = std::string("SDL_WaitForGPUFences(pending): ") + SDL_GetError();
    SDL_ReleaseGPUFence(m_device, m_inFlight.back());
    m_inFlight.pop_back();
    return waited;
}

void SdlGpuDevice::cancelPending() {
    if (!m_pending)
        return;
    SDL_CancelGPUCommandBuffer(m_pending);
    m_pending = nullptr;
}

} // namespace dq8::gfx
