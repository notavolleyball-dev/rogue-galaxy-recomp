// GSNativeEffect's sprites on the GPU (gs_effect.frag; Rogue Galaxy: added).

#include "gfx/backends/sdlgpu/sdlgpu_device.h"

#include <algorithm>
#include <string>

#include "gs_effect.frag.spv.h"
#include "gs_reinterpret.vert.spv.h"

namespace dq8::gfx {

bool SdlGpuDevice::supportsEffects() const {
    return m_device && (SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
}

SDL_GPUTexture *SdlGpuDevice::effectTexture(uint32_t slot, uint32_t width, uint32_t height, std::string &error) {
    if (slot >= m_effectTextures.size()) {
        error = "effectTexture: no such slot";
        return nullptr;
    }
    EffectTexture &entry = m_effectTextures[slot];
    if (entry.texture && (entry.width != width || entry.height != height)) {
        SDL_ReleaseGPUTexture(m_device, entry.texture);
        entry.texture = nullptr;
    }
    if (!entry.texture) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        entry.texture = SDL_CreateGPUTexture(m_device, &info);
        if (!entry.texture) {
            error = std::string("SDL_CreateGPUTexture(effect buffer): ") + SDL_GetError();
            return nullptr;
        }
        entry.width = width;
        entry.height = height;
    }
    return entry.texture;
}

void SdlGpuDevice::releaseEffects() {
    for (SDL_GPUGraphicsPipeline *&pipeline : m_effectPipelines) {
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
        pipeline = nullptr;
    }
    for (EffectTexture &entry : m_effectTextures) {
        if (entry.texture)
            SDL_ReleaseGPUTexture(m_device, entry.texture);
        entry = {};
    }
    if (m_effectLinearSampler)
        SDL_ReleaseGPUSampler(m_device, m_effectLinearSampler);
    m_effectLinearSampler = nullptr;
}

bool SdlGpuDevice::effectPipeline(uint32_t index, std::string &error) {
    SDL_GPUGraphicsPipeline *&pipeline = m_effectPipelines[index];
    if (pipeline)
        return true;
    if (!m_effectLinearSampler) {
        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_effectLinearSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
        if (!m_effectLinearSampler) {
            error = std::string("SDL_CreateGPUSampler(effect): ") + SDL_GetError();
            return false;
        }
    }
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
    vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsEffectFragSpirv);
    fragmentInfo.code_size = sizeof(kGsEffectFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 2u;
    fragmentInfo.num_uniform_buffers = 1u;
    SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!vertex) {
        error = std::string("SDL_CreateGPUShader(effect vertex): ") + SDL_GetError();
        return false;
    }
    SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
    if (!fragment) {
        error = std::string("SDL_CreateGPUShader(effect fragment): ") + SDL_GetError();
        SDL_ReleaseGPUShader(m_device, vertex);
        return false;
    }
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const EffectBlend blend = EffectBlend(index / 2u);
    if (blend != EffectBlend::Write) {
        // The GS's blends with D = Cd, clamped as the target clamps; the
        // alpha written is As.
        SDL_GPUColorTargetBlendState &state = colorTarget.blend_state;
        state.enable_blend = true;
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_color_blendfactor =
            blend == EffectBlend::Over ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
        state.color_blend_op = blend == EffectBlend::Subtract ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    if (index & 1u) {
        colorTarget.blend_state.enable_color_write_mask = true;
        colorTarget.blend_state.color_write_mask =
            SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    }
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    SDL_ReleaseGPUShader(m_device, fragment);
    SDL_ReleaseGPUShader(m_device, vertex);
    if (!pipeline) {
        error = std::string("SDL_CreateGPUGraphicsPipeline(effect): ") + SDL_GetError();
        return false;
    }
    return true;
}

bool SdlGpuDevice::effectOp(SDL_GPUTexture *target, uint32_t width, uint32_t height, SDL_GPUTexture *source,
                            bool linear, SDL_GPUTexture *previous, EffectBlend blend, bool writeAlpha,
                            const SDL_Rect *scissor, const GsEffectUniforms &uniforms, std::string &error) {
    const uint32_t index = uint32_t(blend) * 2u + (writeAlpha ? 0u : 1u);
    if (!effectPipeline(index, error))
        return false;
    SDL_GPUCommandBuffer *commands = pendingCommands(error, "native effect");
    if (!commands)
        return false;
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    // An effect's own buffer is drawn whole from `previous`.
    info.load_op = scissor ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr);
    if (!pass) {
        cancelPending();
        error = std::string("SDL_BeginGPURenderPass(effect): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_effectPipelines[index]);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect whole{0, 0, int(width), int(height)};
    SDL_SetGPUScissor(pass, scissor ? scissor : &whole);
    // Something is bound to each slot the shader declares; it reads only
    // what the op uses.
    SDL_GPUTexture *sourceTexture = source ? source : previous;
    SDL_GPUTexture *previousTexture = previous ? previous : source;
    if (!sourceTexture) {
        sourceTexture = previousTexture = effectTexture(uint32_t(m_effectTextures.size() - 1u), 1u, 1u, error);
        if (!sourceTexture) {
            SDL_EndGPURenderPass(pass);
            return false;
        }
    }
    const SDL_GPUTextureSamplerBinding bindings[2] = {
        {sourceTexture, linear ? m_effectLinearSampler : m_sampler},
        {previousTexture, m_sampler},
    };
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, 2u);
    SDL_PushGPUFragmentUniformData(commands, 0u, &uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

} // namespace dq8::gfx
