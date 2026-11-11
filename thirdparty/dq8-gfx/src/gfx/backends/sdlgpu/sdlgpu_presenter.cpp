#include "gfx/backends/sdlgpu/sdlgpu_presenter.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/native/native_pipeline.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

// Counters of the runtime's movie decoder and EE scheduler, for the stats
// line. Declared here rather than in a header so measuring costs no rebuild
// of the recompiled corpus, which includes those headers.
extern "C++" {
extern std::atomic<uint64_t> g_mpegGetPictureNanos;
extern std::atomic<uint64_t> g_mpegGetPictureCount;
extern std::atomic<uint64_t> g_mpegWriteFrameNanos;
extern std::atomic<uint64_t> g_mpegWriteFrameCount;
extern std::atomic<uint64_t> g_mpegDemuxNanos;
extern std::atomic<uint64_t> g_mpegDemuxCount;
extern std::atomic<uint64_t> g_mpegDemuxRefusedCount;
extern std::atomic<uint64_t> g_mpegPendingEsPeakBytes;
extern std::atomic<uint64_t> g_eeGuestDispatchCount;
extern std::atomic<uint64_t> g_eeTransferThrowCount;
extern std::atomic<uint64_t> g_eeTransferSuspendCount;
extern std::atomic<uint64_t> g_eeRunLoopIterations;
extern std::atomic<uint64_t> g_eeRunLoopResumeCount;
extern std::atomic<uint64_t> g_eeProcessPendingEventsCount;
extern std::atomic<uint64_t> g_eeEnterGuestCount;
extern std::atomic<uint32_t> g_mpegDemuxThreadId;
extern std::atomic<uint32_t> g_mpegGetPictureThreadId;
}

namespace dq8::gfx {

namespace {

std::function<std::string()> &statsExtraLine() {
    static std::function<std::string()> provider;
    return provider;
}

std::function<void(double)> &presentObserver() {
    static std::function<void(double)> observer;
    return observer;
}

// DQ8_GFX_STATS_EVERY=N prints the stats every N frames shown; 0 when unset.
uint32_t statsInterval() {
    static const uint32_t interval = [] {
        const char *value = std::getenv("DQ8_GFX_STATS_EVERY");
        return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return interval;
}

} // namespace

void setStatsExtraLine(std::function<std::string()> provider) {
    statsExtraLine() = std::move(provider);
}

void setPresentObserver(std::function<void(double)> observer) {
    presentObserver() = std::move(observer);
}

struct SdlGpuPresenter::Impl {
    // The device's pending command buffer and the renderer: the presenting
    // thread's, and the menu's (stats) and a capture's from the same thread.
    mutable std::mutex mutex;
    SdlGpuDevice device;
    native::Renderer renderer;

    SDL_Window *window = nullptr;
    // What openWindow was given; the FPS readout keeps it in front.
    std::string windowTitle;
    bool windowCloseRequested = false;
    SdlPadInput pad;

    // Window-thread state: the overlay, how the picture is fitted, and the
    // textures the fitting and captures need.
    SdlGpuOverlay *overlay = nullptr;
    bool overlayCaptured = false;
    SdlGpuDisplayOptions displayOptions{};
    SDL_GPUPresentMode appliedPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    SdlGpuPresenter::GameWidescreenQuery widescreenQuery = nullptr;
    // Anything shown, and the last frame the game itself presented.
    std::chrono::steady_clock::time_point lastShown{};
    std::chrono::steady_clock::time_point lastGameFrame{};
    SdlGpuRect lastRect{};
    struct ScratchTexture {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0u, height = 0u;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
    };
    // The last frame the renderer drew, kept to show again (the menu over a
    // paused game) and to read back.
    ScratchTexture frame;
    bool haveFrame = false;
    ScratchTexture sharpTexture, compositeTexture, captureTexture, fsrTexture;
    // FSR 1: whether the last picture was enlarged on both sides, and
    // whether its passes failed, for the menu to say when it cannot act.
    bool fsrEnlarged = false;
    bool fsrUnavailable = false;
    // What fills the window beside the picture: the picture halved down to
    // a small copy for the blurred sides, and the player's image, loaded
    // each time that choice is made.
    std::array<ScratchTexture, 8> blurLevels;
    SdlGpuSideImageLoader sideImageLoader;
    SDL_GPUTexture *sideImage = nullptr;
    uint32_t sideImageWidth = 0u, sideImageHeight = 0u;
    SdlGpuSides lastSides = SdlGpuSides::Black;
    bool sidesFailed = false;
    struct CaptureRequest {
        bool withOverlay = false;
        SdlGpuCaptureCallback done;
    };
    std::vector<CaptureRequest> captureRequests;
    std::string lastWindowError;

    std::atomic<uint32_t> requestedScale{0u};
    std::atomic<uint32_t> activeScale{1u};
    uint32_t scale = 1u;

    SdlGpuPresenterStats counts{};
    // DQ8_GFX_SHOW_FPS's title.
    uint64_t titlePresents = 0u;
    std::chrono::steady_clock::time_point titleSampleStart{};
    // DQ8_GFX_NATIVE_CAPTURE.
    bool nativeCaptureAttempted = false;
    uint32_t nativeCaptureNumber = 0u;
    // Wall time spent presenting, for the stats line.
    uint64_t presentNanos = 0u;

    // ---------------------------------------------------------------
    // Textures
    // ---------------------------------------------------------------

    bool ensureScratch(ScratchTexture &scratch, uint32_t width, uint32_t height, SDL_GPUTextureFormat format) {
        if (scratch.texture && scratch.width == width && scratch.height == height && scratch.format == format)
            return true;
        releaseScratch(scratch);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        scratch.texture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!scratch.texture)
            return false;
        scratch.width = width;
        scratch.height = height;
        scratch.format = format;
        return true;
    }

    void releaseScratch(ScratchTexture &scratch) {
        if (scratch.texture)
            SDL_ReleaseGPUTexture(device.handle(), scratch.texture);
        scratch = {};
    }

    static void blitRegion(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source, SdlGpuRect from,
                           SDL_GPUTexture *destination, SdlGpuRect to, SDL_GPUFilter filter,
                           SDL_GPULoadOp loadOp) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.x = from.x;
        blit.source.y = from.y;
        blit.source.w = from.width;
        blit.source.h = from.height;
        blit.destination.texture = destination;
        blit.destination.x = to.x;
        blit.destination.y = to.y;
        blit.destination.w = to.width;
        blit.destination.h = to.height;
        blit.load_op = loadOp;
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = filter;
        SDL_BlitGPUTexture(commands, &blit);
    }

    // ---------------------------------------------------------------
    // Fitting the picture into the window
    // ---------------------------------------------------------------

    // The part of the frame that is shown: the crop at the frame's scale, at
    // most an eighth of it. Half a game pixel more at a raised scale, where
    // the game's half-pixel offsets move its border by a texel or so.
    SdlGpuRect pictureRegion() const {
        const uint32_t factor = std::max(scale, 1u);
        const uint32_t crop = displayOptions.crop * factor + (displayOptions.crop != 0u ? factor / 2u : 0u);
        const uint32_t trim = std::min(crop, std::min(frame.width, frame.height) / 8u);
        return {trim, trim, frame.width - 2u * trim, frame.height - 2u * trim};
    }

    // CLEAR paints the bars black; LOAD keeps the sides drawn under it.
    void drawPicture(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SdlGpuRect rect, SDL_GPULoadOp loadOp) {
        SDL_GPUTexture *source = frame.texture;
        SdlGpuRect from = pictureRegion();
        uint32_t width = from.width;
        uint32_t height = from.height;
        SDL_GPUFilter filter = SDL_GPU_FILTER_LINEAR;
        // FSR 1: EASU up to the window's size into a scratch, then RCAS from
        // it into the window (at 0% sharpness EASU draws into the window and
        // RCAS is skipped). Only when both sides are enlarged: EASU has no
        // prefilter for a side it would shrink. Smooth otherwise, or when the
        // passes cannot be made.
        fsrEnlarged = rect.width > width && rect.height > height;
        if (displayOptions.filter == SdlGpuFilter::Fsr && fsrEnlarged && !fsrUnavailable) {
            const int percent = std::clamp(displayOptions.fsrSharpness, 0, 100);
            const uint32_t region[4] = {from.x, from.y, from.width, from.height};
            const uint32_t to[4] = {rect.x, rect.y, rect.width, rect.height};
            const SDL_GPUTextureFormat windowFormat = SDL_GetGPUSwapchainTextureFormat(device.handle(), window);
            std::string error;
            bool drawn = false;
            if (percent == 0) {
                drawn = device.drawFsr(commands, target, windowFormat, to, loadOp, source, region, false, 0.0f, error);
            } else if (ensureScratch(fsrTexture, rect.width, rect.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                // 1% is RCAS at 2 stops, 100% at 0 (the sharpest).
                const float stops = float(100 - percent) / 49.5f;
                const uint32_t whole[4] = {0u, 0u, rect.width, rect.height};
                drawn = device.drawFsr(commands, fsrTexture.texture, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, whole,
                                       SDL_GPU_LOADOP_DONT_CARE, source, region, false, 0.0f, error) &&
                        device.drawFsr(commands, target, windowFormat, to, loadOp, fsrTexture.texture, whole, true,
                                       stops, error);
            } else {
                error = "no scratch texture for EASU";
            }
            if (drawn)
                return;
            fsrUnavailable = true;
            std::fprintf(stderr, "[present] FSR 1 unavailable, smooth instead: %s\n", error.c_str());
        }
        if (displayOptions.filter == SdlGpuFilter::Nearest) {
            filter = SDL_GPU_FILTER_NEAREST;
        } else if (displayOptions.filter == SdlGpuFilter::Sharp) {
            if (rect.width % width == 0u && rect.height % height == 0u) {
                filter = SDL_GPU_FILTER_NEAREST;
            } else if (rect.width > width || rect.height > height) {
                // A fractional nearest scale duplicates some columns and not
                // others, which shreds one-pixel font stems. Nearest to the
                // whole multiple above the output, then bilinear down to it.
                // Per axis: with one factor, a picture stretched more across
                // than down has its bilinear pass shrink rows below half.
                constexpr uint32_t kLimit = 8192u;
                const auto factorFor = [](uint32_t output, uint32_t size) {
                    return std::max(1u, static_cast<uint32_t>(std::ceil(double(output) / size)));
                };
                const uint32_t factorX = factorFor(rect.width, width);
                const uint32_t factorY = factorFor(rect.height, height);
                if (width * factorX <= kLimit && height * factorY <= kLimit &&
                    ensureScratch(sharpTexture, width * factorX, height * factorY,
                                  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                    // The crop goes first: the bilinear pass then reads no
                    // texel beyond the picture.
                    blitRegion(commands, source, from, sharpTexture.texture,
                               {0u, 0u, width * factorX, height * factorY}, SDL_GPU_FILTER_NEAREST,
                               SDL_GPU_LOADOP_DONT_CARE);
                    source = sharpTexture.texture;
                    width *= factorX;
                    height *= factorY;
                    from = {0u, 0u, width, height};
                }
            }
        }
        blitRegion(commands, source, from, target, rect, filter, loadOp);
    }

    // The picture blurred for SdlGpuSides::Blurred: halved by bilinear
    // blits, each averaging four texels, until it is at most 256 wide.
    SDL_GPUTexture *blurredPicture(SDL_GPUCommandBuffer *commands, uint32_t &width, uint32_t &height) {
        SDL_GPUTexture *source = frame.texture;
        SdlGpuRect from = pictureRegion();
        for (ScratchTexture &level : blurLevels) {
            if (from.width <= 256u || from.height < 2u)
                break;
            const SdlGpuRect to{0u, 0u, from.width / 2u, from.height / 2u};
            if (!ensureScratch(level, to.width, to.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM))
                return nullptr;
            blitRegion(commands, source, from, level.texture, to, SDL_GPU_FILTER_LINEAR, SDL_GPU_LOADOP_DONT_CARE);
            source = level.texture;
            from = to;
        }
        if (source == frame.texture)
            return nullptr;
        width = from.width;
        height = from.height;
        return source;
    }

    void releaseSideImage() {
        if (sideImage)
            SDL_ReleaseGPUTexture(device.handle(), sideImage);
        sideImage = nullptr;
        sideImageWidth = sideImageHeight = 0u;
    }

    // The player's image, through the loader, uploaded in a copy pass ahead
    // of the frame's drawing.
    void loadSideImage(SDL_GPUCommandBuffer *commands) {
        releaseSideImage();
        uint32_t width = 0u, height = 0u;
        std::vector<uint8_t> rgba;
        if (!sideImageLoader || !sideImageLoader(width, height, rgba) || width == 0u || height == 0u ||
            width > 16384u || height > 16384u || rgba.size() < size_t(width) * height * 4u)
            return;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device.handle(), &info);
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = width * height * 4u;
        SDL_GPUTransferBuffer *transfer =
            texture ? SDL_CreateGPUTransferBuffer(device.handle(), &transferInfo) : nullptr;
        void *mapped = transfer ? SDL_MapGPUTransferBuffer(device.handle(), transfer, false) : nullptr;
        SDL_GPUCopyPass *copy = nullptr;
        if (mapped) {
            std::memcpy(mapped, rgba.data(), transferInfo.size);
            SDL_UnmapGPUTransferBuffer(device.handle(), transfer);
            copy = SDL_BeginGPUCopyPass(commands);
        }
        if (copy) {
            SDL_GPUTextureTransferInfo from{};
            from.transfer_buffer = transfer;
            SDL_GPUTextureRegion to{};
            to.texture = texture;
            to.w = width;
            to.h = height;
            to.d = 1u;
            SDL_UploadToGPUTexture(copy, &from, &to, false);
            SDL_EndGPUCopyPass(copy);
            sideImage = texture;
            sideImageWidth = width;
            sideImageHeight = height;
        } else {
            std::fprintf(stderr, "[present] the side image could not be uploaded: %s\n", SDL_GetError());
            if (texture)
                SDL_ReleaseGPUTexture(device.handle(), texture);
        }
        // Released once the copy has run.
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device.handle(), transfer);
    }

    // The window beside a picture that leaves it uncovered, before the
    // picture is drawn over it; false leaves the bars to drawPicture's black.
    bool drawSides(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width, uint32_t height,
                   SdlGpuRect rect) {
        const SdlGpuSides sides = displayOptions.sides;
        if (sides == SdlGpuSides::Image && lastSides != SdlGpuSides::Image)
            loadSideImage(commands);
        lastSides = sides;
        if (sides == SdlGpuSides::Black || (rect.width >= width && rect.height >= height))
            return false;
        SDL_GPUTexture *source = sideImage;
        uint32_t sourceWidth = sideImageWidth, sourceHeight = sideImageHeight;
        bool blur = false;
        float brightness = 1.0f;
        if (sides == SdlGpuSides::Blurred || !source) {
            source = blurredPicture(commands, sourceWidth, sourceHeight);
            blur = true;
            // Half was too dark to make out anything.
            brightness = 0.75f;
        }
        std::string error;
        if (!source || !device.drawSides(commands, target, SDL_GetGPUSwapchainTextureFormat(device.handle(), window),
                                         width, height, source, sourceWidth, sourceHeight, blur, brightness,
                                         error)) {
            if (!error.empty() && !sidesFailed) {
                std::fprintf(stderr, "[present] the sides stay black: %s\n", error.c_str());
                sidesFailed = true;
            }
            return false;
        }
        return true;
    }

    void clearTarget(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target) {
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr))
            SDL_EndGPURenderPass(pass);
    }

    int gameWidescreen() const { return widescreenQuery ? widescreenQuery() : -1; }

    void applyPresentMode() {
        if (displayOptions.presentMode == appliedPresentMode)
            return;
        SDL_GPUPresentMode mode = displayOptions.presentMode;
        if (!SDL_WindowSupportsGPUPresentMode(device.handle(), window, mode))
            mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (SDL_SetGPUSwapchainParameters(device.handle(), window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            appliedPresentMode = displayOptions.presentMode;
    }

    // SDL moves resources to defragment memory at a submission that
    // presents: it copies each one within that submission and points its
    // handle at the copy. Commands recorded and not yet submitted would
    // still write the old one. So a submission that presents goes after the
    // pending commands, under the lock.
    template <typename Submission>
    auto submitAfterPending(Submission submission) {
        std::lock_guard lock(mutex);
        std::string error;
        if (!device.submitPending(error))
            std::fprintf(stderr, "[present] pending commands: %s\n", error.c_str());
        return submission();
    }

    // Fits the last frame into the window, draws the overlay over it, and
    // serves any pending capture; with no frame, the overlay over black.
    bool showInWindow(std::string &error) {
        if (!window)
            return true;
        const auto fail = [&](const char *context) {
            error = std::string(context) + SDL_GetError();
            return false;
        };
        applyPresentMode();
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return fail("SDL_AcquireGPUCommandBuffer(present): ");
        if (device.gpuTimes())
            SDL_InsertGPUDebugLabel(commands, "present");

        SDL_GPUTexture *swapchain = nullptr;
        uint32_t swapchainWidth = 0u;
        uint32_t swapchainHeight = 0u;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &swapchainWidth, &swapchainHeight)) {
            SDL_CancelGPUCommandBuffer(commands);
            return fail("SDL_WaitAndAcquireGPUSwapchainTexture: ");
        }
        // Also when minimised, so an idle overlay does not retry every pump.
        lastShown = std::chrono::steady_clock::now();
        if (!swapchain || swapchainWidth == 0u || swapchainHeight == 0u) {
            // Minimised or otherwise unavailable this frame; nothing to show.
            if (!submitAfterPending([&] { return SDL_SubmitGPUCommandBuffer(commands); }))
                return fail("SDL_SubmitGPUCommandBuffer(minimized): ");
            return true;
        }
        const SdlGpuRect whole{0u, 0u, swapchainWidth, swapchainHeight};

        // A capture composes off screen first, since a swapchain image cannot
        // be read back.
        SDL_GPUTexture *target = swapchain;
        std::optional<CaptureRequest> capture;
        if (!captureRequests.empty() &&
            ensureScratch(compositeTexture, swapchainWidth, swapchainHeight,
                          SDL_GetGPUSwapchainTextureFormat(device.handle(), window))) {
            capture = std::move(captureRequests.front());
            captureRequests.erase(captureRequests.begin());
            target = compositeTexture.texture;
        }

        SdlGpuRect rect{};
        if (haveFrame) {
            const SdlGpuRect picture = pictureRegion();
            rect = sdlGpuDisplayRect(displayOptions, picture.width, picture.height, swapchainWidth, swapchainHeight,
                                     gameWidescreen());
            const bool sides = drawSides(commands, target, swapchainWidth, swapchainHeight, rect);
            drawPicture(commands, target, rect, sides ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR);
        } else {
            clearTarget(commands, target);
        }
        lastRect = rect;

        SdlGpuRect captured{};
        const auto copyOut = [&](SdlGpuRect from) {
            if (from.width != 0u &&
                ensureScratch(captureTexture, from.width, from.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                captured = {0u, 0u, from.width, from.height};
                blitRegion(commands, target, from, captureTexture.texture, captured, SDL_GPU_FILTER_NEAREST,
                           SDL_GPU_LOADOP_DONT_CARE);
            }
        };
        if (capture && !capture->withOverlay)
            copyOut(rect);
        if (overlay)
            overlay->render(commands, target, swapchainWidth, swapchainHeight);
        if (capture && capture->withOverlay)
            copyOut(whole);
        if (target != swapchain)
            blitRegion(commands, target, whole, swapchain, whole, SDL_GPU_FILTER_NEAREST, SDL_GPU_LOADOP_DONT_CARE);

        if (!submitAfterPending([&] { return SDL_SubmitGPUCommandBuffer(commands); }))
            return fail("SDL_SubmitGPUCommandBuffer(present): ");

        if (capture) {
            SdlGpuCapture result;
            std::string captureError;
            if (captured.width != 0u && device.downloadTexture(captureTexture.texture, captured.width,
                                                               captured.height, result.rgba, captureError)) {
                result.width = captured.width;
                result.height = captured.height;
            } else {
                std::fprintf(stderr, "[present] capture failed: %s\n",
                             captureError.empty() ? "nothing on screen" : captureError.c_str());
                result.rgba.clear();
            }
            if (capture->done)
                capture->done(std::move(result));
        }
        return true;
    }

    // Shows the last frame again under a fresh overlay, while the game shows
    // none: paused, loading, or not started yet.
    void refreshWindow() {
        std::string error;
        const bool shown = showInWindow(error);
        ++counts.refreshes;
        if (!shown && error != lastWindowError)
            std::fprintf(stderr, "[present] window refresh: %s\n", error.c_str());
        lastWindowError = shown ? std::string() : error;
    }

    // A change of the internal resolution, at the start of a frame: the
    // renderer's targets are made at the new scale from then on.
    void applyRequestedScale() {
        const uint32_t requested = requestedScale.exchange(0u, std::memory_order_acq_rel);
        if (requested == 0u || requested == scale)
            return;
        scale = requested;
        activeScale.store(scale, std::memory_order_release);
        std::fprintf(stderr, "[present] internal resolution %ux\n", scale);
    }

    // ---------------------------------------------------------------
    // Reports
    // ---------------------------------------------------------------

    void updatePerformanceTitle() {
        static const bool enabled = [] {
            const char *value = std::getenv("DQ8_GFX_SHOW_FPS");
            return value && std::strcmp(value, "0") != 0;
        }();
        if (!enabled || !window)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (titleSampleStart == std::chrono::steady_clock::time_point{}) {
            titleSampleStart = now;
            return;
        }
        ++titlePresents;
        const double seconds = std::chrono::duration<double>(now - titleSampleStart).count();
        if (seconds < 2.0)
            return;
        char title[192];
        std::snprintf(title, sizeof(title), "%s | %.1f fps", windowTitle.c_str(), titlePresents / seconds);
        SDL_SetWindowTitle(window, title);
        std::fprintf(stderr, "[present] %.2f fps over %.3f s\n", titlePresents / seconds, seconds);
        titleSampleStart = now;
        titlePresents = 0u;
    }

    // DQ8_GFX_NATIVE_CAPTURE=FILE: when FILE.trigger exists, the next frame
    // shown is written to FILE as a BMP (and with
    // DQ8_GFX_NATIVE_CAPTURE_SERIES each trigger to FILE.N.bmp).
    void captureShownFrame() {
        static const char *path = std::getenv("DQ8_GFX_NATIVE_CAPTURE");
        static const bool series = std::getenv("DQ8_GFX_NATIVE_CAPTURE_SERIES") != nullptr;
        if (!path || !*path || (nativeCaptureAttempted && !series) || !haveFrame)
            return;
        const std::string trigger = std::string(path) + ".trigger";
        FILE *request = std::fopen(trigger.c_str(), "rb");
        if (!request)
            return;
        std::fclose(request);
        nativeCaptureAttempted = true;
        std::string output = path;
        if (series) {
            // Consume each request before readback so it cannot capture every frame.
            if (std::remove(trigger.c_str()) != 0)
                return;
            output += "." + std::to_string(++nativeCaptureNumber) + ".bmp";
        }
        std::vector<uint8_t> pixels;
        std::string error;
        if (device.downloadTexture(frame.texture, frame.width, frame.height, pixels, error)) {
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                SDL_CreateSurfaceFrom(frame.width, frame.height, SDL_PIXELFORMAT_RGBA32, pixels.data(),
                                      frame.width * 4u),
                SDL_DestroySurface);
            if (surface && SDL_SaveBMP(surface.get(), output.c_str())) {
                std::fprintf(stderr, "[native-capture] %s %ux%u frame=%llu\n", output.c_str(), frame.width,
                             frame.height, static_cast<unsigned long long>(counts.presents));
                return;
            }
            error = SDL_GetError();
        }
        std::fprintf(stderr, "[native-capture] failed: %s\n", error.c_str());
    }

    // At every frame shown: the present observer, and every
    // DQ8_GFX_STATS_EVERY frames the stats lines.
    void reportStats() {
        using Clock = std::chrono::steady_clock;
        const uint32_t interval = statsInterval();
        // The longest gap between two frames in the interval: a hitch of a
        // frame or two disappears into the mean fps.
        static Clock::time_point previousPresent = Clock::now();
        static double longestGap = 0.0;
        if (interval != 0u || presentObserver()) {
            const Clock::time_point presented = Clock::now();
            const double gap = std::chrono::duration<double>(presented - previousPresent).count();
            previousPresent = presented;
            longestGap = std::max(longestGap, gap);
            if (const auto &observe = presentObserver())
                observe(gap);
        }
        if (interval == 0u || (counts.presents % interval) != 0u)
            return;
        static Clock::time_point last = Clock::now();
        static bool primed = false;
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        // The first call sets the baseline.
        struct Previous {
            uint64_t presentNanos, submissions, refreshes, gpuCalls, meshes, passes, copies;
            uint64_t getPictureNanos, getPictureCount, writeFrameNanos, writeFrameCount, demuxNanos, demuxCount;
            uint64_t refused, dispatches, throws, suspends, loops, resumes, pumps, enters;
        };
        static Previous was{};
        Previous is{};
        is.presentNanos = presentNanos;
        is.submissions = device.submissions();
        is.refreshes = counts.refreshes;
        const native::Renderer::Totals totals = renderer.totals();
        is.gpuCalls = totals.gpuCalls;
        is.meshes = totals.meshesDrawn;
        is.passes = totals.passes;
        is.copies = totals.copies;
        is.getPictureNanos = g_mpegGetPictureNanos.load(std::memory_order_relaxed);
        is.getPictureCount = g_mpegGetPictureCount.load(std::memory_order_relaxed);
        is.writeFrameNanos = g_mpegWriteFrameNanos.load(std::memory_order_relaxed);
        is.writeFrameCount = g_mpegWriteFrameCount.load(std::memory_order_relaxed);
        is.demuxNanos = g_mpegDemuxNanos.load(std::memory_order_relaxed);
        is.demuxCount = g_mpegDemuxCount.load(std::memory_order_relaxed);
        is.refused = g_mpegDemuxRefusedCount.load(std::memory_order_relaxed);
        is.dispatches = g_eeGuestDispatchCount.load(std::memory_order_relaxed);
        is.throws = g_eeTransferThrowCount.load(std::memory_order_relaxed);
        is.suspends = g_eeTransferSuspendCount.load(std::memory_order_relaxed);
        is.loops = g_eeRunLoopIterations.load(std::memory_order_relaxed);
        is.resumes = g_eeRunLoopResumeCount.load(std::memory_order_relaxed);
        is.pumps = g_eeProcessPendingEventsCount.load(std::memory_order_relaxed);
        is.enters = g_eeEnterGuestCount.load(std::memory_order_relaxed);
        if (!primed) {
            primed = true;
            was = is;
            return;
        }
        const auto d = [](uint64_t a, uint64_t b) { return static_cast<unsigned long long>(a - b); };
        const auto share = [&](uint64_t nanosNow, uint64_t nanosWas) {
            return seconds > 0.0 ? static_cast<double>(nanosNow - nanosWas) / 1e9 / seconds * 100.0 : 0.0;
        };
        std::fprintf(stderr,
                     "[gfx] %.1f fps (backend %.0f%% of wall) over %u frames | draws=%llu meshes=%llu passes=%llu"
                     " copies=%llu submits=%llu refreshes=%llu | scale %ux | longest frame %.1f ms\n"
                     "  movie: get-picture=%llu (%.0f%%) write-frame=%llu (%.0f%%) demux=%llu (%.0f%%) refused=%llu"
                     " es-buffer-peak=%lluKiB | threads: demux=%u get-picture=%u | ee-dispatches=%llu (%llu/frame)"
                     " throws=%llu\n"
                     "  sched: loop=%llu (%.0f/s) resumes=%llu pumps=%llu enter-guest=%llu suspends=%llu\n",
                     seconds > 0.0 ? static_cast<double>(interval) / seconds : 0.0,
                     share(is.presentNanos, was.presentNanos), interval, d(is.gpuCalls, was.gpuCalls),
                     d(is.meshes, was.meshes), d(is.passes, was.passes), d(is.copies, was.copies),
                     d(is.submissions, was.submissions),
                     d(is.refreshes, was.refreshes), scale, longestGap * 1e3, d(is.getPictureCount, was.getPictureCount),
                     share(is.getPictureNanos, was.getPictureNanos), d(is.writeFrameCount, was.writeFrameCount),
                     share(is.writeFrameNanos, was.writeFrameNanos), d(is.demuxCount, was.demuxCount),
                     share(is.demuxNanos, was.demuxNanos), d(is.refused, was.refused),
                     static_cast<unsigned long long>(g_mpegPendingEsPeakBytes.load(std::memory_order_relaxed) / 1024u),
                     g_mpegDemuxThreadId.load(std::memory_order_relaxed),
                     g_mpegGetPictureThreadId.load(std::memory_order_relaxed), d(is.dispatches, was.dispatches),
                     static_cast<unsigned long long>((is.dispatches - was.dispatches) / std::max<uint32_t>(1u, interval)),
                     d(is.throws, was.throws), d(is.loops, was.loops),
                     seconds > 0.0 ? static_cast<double>(is.loops - was.loops) / seconds : 0.0,
                     d(is.resumes, was.resumes), d(is.pumps, was.pumps), d(is.enters, was.enters),
                     d(is.suspends, was.suspends));
        longestGap = 0.0;
        was = is;
        if (const auto &extra = statsExtraLine()) {
            const std::string line = extra();
            if (!line.empty())
                std::fprintf(stderr, "  %s\n", line.c_str());
        }
        std::fputs(renderer.statsLine().c_str(), stderr);
    }

    // ---------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------

    bool pumpEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                windowCloseRequested = true;
            if (overlay && overlay->handleEvent(event))
                continue;
            // Hotplug always reaches the pad; input only while the game has it.
            const bool hotplug = event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED;
            if (hotplug || !overlay || !overlay->capturesInput())
                pad.handleEvent(event);
        }
        const bool captured = overlay && overlay->capturesInput();
        if (overlayCaptured && !captured)
            pad.suppressHeld();
        overlayCaptured = captured;
        pad.poll(SDL_GetKeyboardState(nullptr), SDL_GetKeyboardFocus() == window && !captured);
        // Only once the game has stopped presenting: between its own frames a
        // refresh would double the presents and could hold the next one up.
        const auto now = std::chrono::steady_clock::now();
        if (overlay && overlay->wantsFrames() && now - lastGameFrame >= std::chrono::milliseconds(50) &&
            now - lastShown >= std::chrono::milliseconds(15))
            refreshWindow();
        return !windowCloseRequested;
    }
};

SdlGpuPresenter::SdlGpuPresenter() : m_impl(std::make_unique<Impl>()) {}

SdlGpuPresenter::~SdlGpuPresenter() {
    Impl &impl = *m_impl;
    if (impl.device.valid()) {
        std::string ignored;
        impl.device.submitPendingAndWait(ignored);
        SDL_WaitForGPUIdle(impl.device.handle());
        for (auto *scratch : {&impl.frame, &impl.sharpTexture, &impl.compositeTexture, &impl.captureTexture,
                              &impl.fsrTexture})
            impl.releaseScratch(*scratch);
        for (auto &level : impl.blurLevels)
            impl.releaseScratch(level);
        impl.releaseSideImage();
        impl.renderer.release(impl.device.handle());
        if (impl.window) {
            SDL_ReleaseWindowFromGPUDevice(impl.device.handle(), impl.window);
            SDL_DestroyWindow(impl.window);
            impl.window = nullptr;
        }
    }
    impl.device.destroy();
}

std::unique_ptr<SdlGpuPresenter> SdlGpuPresenter::create(std::string &error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        error = std::string("SDL_InitSubSystem(video): ") + SDL_GetError();
        return nullptr;
    }
    std::unique_ptr<SdlGpuPresenter> presenter(new SdlGpuPresenter());
    if (!presenter->m_impl->device.create(error))
        return nullptr;
    return presenter;
}

void SdlGpuPresenter::setResolutionScale(uint32_t scale) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->scale = std::clamp<uint32_t>(scale, 1u, 8u);
    m_impl->activeScale.store(m_impl->scale, std::memory_order_release);
}

uint32_t SdlGpuPresenter::resolutionScale() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->scale;
}

void SdlGpuPresenter::requestResolutionScale(uint32_t scale) {
    m_impl->requestedScale.store(std::clamp<uint32_t>(scale, 1u, 8u), std::memory_order_release);
}

uint32_t SdlGpuPresenter::activeResolutionScale() const {
    return m_impl->activeScale.load(std::memory_order_acquire);
}

bool SdlGpuPresenter::presentFrame(std::vector<uint8_t> *pixels, uint32_t &width, uint32_t &height) {
    Impl &impl = *m_impl;
    if (!impl.window)
        return false;
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    bool drawn = false;
    {
        std::lock_guard lock(impl.mutex);
        impl.applyRequestedScale();
        SDL_GPUCommandBuffer *commands = impl.device.pendingCommands(error, "native pipeline");
        native::Renderer::Frame out;
        if (commands &&
            impl.renderer.render(impl.device.handle(), commands, std::max<uint32_t>(impl.scale, 1u), out, error) &&
            out.texture) {
            // Kept to show again and to read back: the renderer draws its
            // next frame into the same target.
            if (impl.ensureScratch(impl.frame, out.width, out.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                Impl::blitRegion(commands, out.texture, {0u, 0u, out.width, out.height}, impl.frame.texture,
                                 {0u, 0u, out.width, out.height}, SDL_GPU_FILTER_NEAREST, SDL_GPU_LOADOP_DONT_CARE);
                impl.haveFrame = true;
                drawn = true;
            } else {
                error = std::string("SDL_CreateGPUTexture(frame): ") + SDL_GetError();
            }
        }
    }
    if (!error.empty()) {
        static std::string lastError;
        if (error != lastError)
            std::fprintf(stderr, "[present] %s\n", error.c_str());
        lastError = error;
    }
    if (drawn) {
        ++impl.counts.presents;
        impl.reportStats();
    }
    std::string windowError;
    if (!impl.showInWindow(windowError) && windowError != impl.lastWindowError)
        std::fprintf(stderr, "[present] %s\n", windowError.c_str());
    impl.lastWindowError = windowError;
    impl.lastGameFrame = impl.lastShown;
    if (drawn) {
        impl.updatePerformanceTitle();
        impl.captureShownFrame();
        if (pixels) {
            width = impl.frame.width;
            height = impl.frame.height;
            if (!impl.device.downloadTexture(impl.frame.texture, width, height, *pixels, error)) {
                std::fprintf(stderr, "[present] screenshot: %s\n", error.c_str());
                pixels->clear();
            }
        }
    }
    impl.presentNanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    return drawn;
}

void SdlGpuPresenter::serveNativeReads() {
    Impl &impl = *m_impl;
    if (!native::colorReadsWaiting() && !impl.renderer.dumpPending())
        return;
    std::lock_guard lock(impl.mutex);
    if (!impl.device.valid())
        return;
    // What is pending goes to the GPU first.
    std::string error;
    if (!impl.device.submitPending(error))
        std::fprintf(stderr, "[native-pipeline] before colour reads: %s\n", error.c_str());
    impl.renderer.serveColorReads(impl.device.handle());
}

void SdlGpuPresenter::setSideImageLoader(SdlGpuSideImageLoader loader) {
    m_impl->sideImageLoader = std::move(loader);
}

void SdlGpuPresenter::setGameWidescreenQuery(GameWidescreenQuery query) {
    m_impl->widescreenQuery = query;
}

int SdlGpuPresenter::gameWidescreen() const {
    return m_impl->gameWidescreen();
}

void SdlGpuPresenter::setOverlay(SdlGpuOverlay *overlay) {
    m_impl->overlay = overlay;
    m_impl->overlayCaptured = false;
}

void SdlGpuPresenter::setDisplayOptions(const SdlGpuDisplayOptions &options) {
    m_impl->displayOptions = options;
}

const SdlGpuDisplayOptions &SdlGpuPresenter::displayOptions() const {
    return m_impl->displayOptions;
}

bool SdlGpuPresenter::supportsPresentMode(SDL_GPUPresentMode mode) const {
    return m_impl->window && SDL_WindowSupportsGPUPresentMode(m_impl->device.handle(), m_impl->window, mode);
}

SDL_Window *SdlGpuPresenter::window() const {
    return m_impl->window;
}

SDL_GPUDevice *SdlGpuPresenter::gpuDevice() const {
    return m_impl->device.handle();
}

SDL_GPUTextureFormat SdlGpuPresenter::windowTextureFormat() const {
    return m_impl->window ? SDL_GetGPUSwapchainTextureFormat(m_impl->device.handle(), m_impl->window)
                          : SDL_GPU_TEXTUREFORMAT_INVALID;
}

SdlPadInput &SdlGpuPresenter::padInput() {
    return m_impl->pad;
}

SdlGpuRect SdlGpuPresenter::displayRect() const {
    return m_impl->lastRect;
}

SdlGpuFsrState SdlGpuPresenter::fsrState() const {
    if (m_impl->fsrUnavailable)
        return SdlGpuFsrState::Unsupported;
    return m_impl->fsrEnlarged ? SdlGpuFsrState::Acts : SdlGpuFsrState::NotEnlarged;
}

void SdlGpuPresenter::requestCapture(bool withOverlay, SdlGpuCaptureCallback done) {
    m_impl->captureRequests.push_back({withOverlay, std::move(done)});
}

bool SdlGpuPresenter::openWindow(const char *title, uint32_t width, uint32_t height, std::string &error,
                                 const SdlGpuWindowPlacement &placement) {
    Impl &impl = *m_impl;
    std::lock_guard lock(impl.mutex);
    if (!impl.device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }
    if (impl.window != nullptr)
        return true;
    if (width == 0u || height == 0u) {
        // 4:3, at the largest whole multiple of the game's height that
        // leaves room on the desktop.
        SDL_Rect usable{0, 0, 1280, 960};
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        const uint32_t multiple = std::max<uint32_t>(1u, static_cast<uint32_t>(usable.h * 0.85 / 448.0));
        height = 448u * multiple;
        width = height * 4u / 3u;
    }
    // High pixel density: on a scaled desktop the swapchain is then as many
    // pixels as the window covers, rather than its logical size stretched by
    // the compositor. DQ8_GFX_HIDDEN_WINDOW=1: the window is never shown or
    // raised, for scripted runs that should not take the desktop; frames
    // still reach DQ8_GFX_SCREENSHOT_DIR.
    const char *hiddenWindow = std::getenv("DQ8_GFX_HIDDEN_WINDOW");
    const bool hidden = hiddenWindow && *hiddenWindow && *hiddenWindow != '0';
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          placement.placed ? placement.x : SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          placement.placed ? placement.y : SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (hidden ? SDL_WINDOW_HIDDEN : 0u) |
                              (placement.maximized && !hidden ? SDL_WINDOW_MAXIMIZED : 0u));
    impl.window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!impl.window) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    impl.windowTitle = title ? title : "";
    setWindowIcon(impl.window);
    if (!SDL_ClaimWindowForGPUDevice(impl.device.handle(), impl.window)) {
        error = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
        SDL_DestroyWindow(impl.window);
        impl.window = nullptr;
        return false;
    }
    if (!hidden) {
        SDL_ShowWindow(impl.window);
        SDL_RaiseWindow(impl.window);
    }
    std::string inputError;
    if (!impl.pad.initialize(inputError))
        std::fprintf(stderr, "[pad] %s; keyboard remains available\n", inputError.c_str());
    return true;
}

bool SdlGpuPresenter::hasWindow() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->window != nullptr;
}

// Window thread only, like everything the window and pad touch: the overlay
// calls back into the presenter from here.
bool SdlGpuPresenter::pumpEvents() {
    if (!m_impl->window)
        return true;
    return m_impl->pumpEvents();
}

void SdlGpuPresenter::hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const {
    m_impl->pad.read(held, pressed, sticks);
}

std::string SdlGpuPresenter::driverName() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device.driverName();
}

std::string SdlGpuPresenter::videoDriverName() const {
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? driver : "none";
}

SdlGpuPresenterStats SdlGpuPresenter::stats() const {
    std::lock_guard lock(m_impl->mutex);
    SdlGpuPresenterStats result = m_impl->counts;
    const native::Renderer::Totals totals = m_impl->renderer.totals();
    result.meshesDrawn = totals.meshesDrawn;
    result.gpuCalls = totals.gpuCalls;
    result.passes = totals.passes;
    result.copies = totals.copies;
    result.texturesHeld = totals.texturesHeld;
    result.textureBytes = totals.textureBytes;
    result.retainedMeshes = totals.retainedMeshes;
    result.submissions = m_impl->device.submissions();
    return result;
}

} // namespace dq8::gfx
