// The SDL3 GPU device the presenter and the native renderer share, and the
// two presentation passes drawn on it: the window's sides and AMD FSR 1.
//
// Rogue Galaxy: reduced to this in M1 of the game's docs/NATIVE-ONLY.md. It
// was the GS backend's device, with the GS draw, reinterpret, index-8,
// local-copy and display pipelines; the GS is no longer emulated.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace dq8::gfx {

class SdlGpuDevice {
public:
    SdlGpuDevice() = default;
    ~SdlGpuDevice();

    SdlGpuDevice(const SdlGpuDevice &) = delete;
    SdlGpuDevice &operator=(const SdlGpuDevice &) = delete;

    // SPIR-V only: the native renderer's shaders are.
    bool create(std::string &error);
    void destroy();

    SDL_GPUDevice *handle() const { return m_device; }
    bool valid() const { return m_device != nullptr; }
    std::string driverName() const;

    // Reads a whole RGBA8 texture back, waiting for the GPU.
    bool downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                         std::vector<uint8_t> &pixels, std::string &error) const;

    // The window beside the picture (SdlGpuSides): `source` stretched over
    // the whole `width` by `height` target, blurred and darkened by
    // `brightness` when `blur`.
    bool drawSides(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                   uint32_t width, uint32_t height, SDL_GPUTexture *source, uint32_t sourceWidth,
                   uint32_t sourceHeight, bool blur, float brightness, std::string &error);
    // AMD FSR 1: EASU from `region` of `source` (x, y, width, height) into
    // `to` of `target`, or with `rcas` RCAS at `sharpness` stops.
    bool drawFsr(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                 const uint32_t to[4], SDL_GPULoadOp loadOp, SDL_GPUTexture *source, const uint32_t region[4],
                 bool rcas, float sharpness, std::string &error);

    // One command buffer that collects work until submitted: the native
    // renderer's frame records into it, the presentation submits it.
    SDL_GPUCommandBuffer *pendingCommands(std::string &error, const char *what = nullptr);
    bool hasPending() const { return m_pending != nullptr; }
    bool submitPending(std::string &error);
    bool submitPendingAndWait(std::string &error);
    void cancelPending();
    uint64_t submissions() const { return m_submissions; }

    // DQ8_GFX_GPU_TIMES=1: a GPU timestamp at each debug label (the SDL
    // patch), the GPU's time logged by label.
    bool gpuTimes() const { return m_gpuTimes; }

private:
    bool ensureLinearSampler(std::string &error);
    SDL_GPUGraphicsPipeline *fullscreenPipeline(const void *fragment, size_t fragmentSize,
                                                SDL_GPUTextureFormat format, const char *what, std::string &error);
    void retire();

    SDL_GPUDevice *m_device = nullptr;
    const bool m_gpuTimes = [] {
        const char *value = std::getenv("DQ8_GFX_GPU_TIMES");
        return value && *value && *value != '0';
    }();
    SDL_GPUSampler *m_linearSampler = nullptr;
    SDL_GPUGraphicsPipeline *m_sidesPipeline = nullptr;
    SDL_GPUTextureFormat m_sidesFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline *m_fsrPipelines[2] = {}; // by the format below: RGBA8, the window's
    SDL_GPUTextureFormat m_fsrFormats[2] = {SDL_GPU_TEXTUREFORMAT_INVALID, SDL_GPU_TEXTUREFORMAT_INVALID};

    SDL_GPUCommandBuffer *m_pending = nullptr;
    uint64_t m_submissions = 0u;
    std::deque<SDL_GPUFence *> m_inFlight;
};

} // namespace dq8::gfx
