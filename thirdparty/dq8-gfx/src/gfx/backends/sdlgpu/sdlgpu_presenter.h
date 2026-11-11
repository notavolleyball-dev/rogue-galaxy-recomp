// The window and what is shown in it: the SDL3 GPU device, the native
// renderer's frames fitted into the window, the menu over them, and the
// host's input (Rogue Galaxy: added, M1 of the game's docs/NATIVE-ONLY.md).
//
// This was the GS backend (a GSRasterBackend) when the GS was emulated; the
// game is now drawn by the native renderer (gfx/native) from records, and
// nothing of the GS is here.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dq8::gfx {

class SdlPadInput;

// A line the embedder appends to every DQ8_GFX_STATS_EVERY report (without
// the trailing newline), e.g. how busy its own threads were over the
// interval; empty for none.
void setStatsExtraLine(std::function<std::string()> provider);

// Called at every present with the seconds since the previous one, on the
// presenting thread, e.g. to notice a hitch as it happens.
void setPresentObserver(std::function<void(double)> observer);

// Where openWindow puts the window: its top left on the desktop when
// placed, centred otherwise, and maximized.
struct SdlGpuWindowPlacement {
    bool placed = false;
    int x = 0;
    int y = 0;
    bool maximized = false;
};

// Totals since the start, for the menu's statistics page (rates are its
// own differences).
struct SdlGpuPresenterStats {
    uint64_t presents = 0u;       // frames the game finished, shown
    uint64_t refreshes = 0u;      // the last frame shown again under the menu
    uint64_t meshesDrawn = 0u;    // the native renderer's draws
    uint64_t gpuCalls = 0u;       // the GPU draw calls they took
    uint64_t passes = 0u;         // render passes
    uint64_t copies = 0u;         // target copies for draws reading their own target
    uint64_t texturesHeld = 0u;   // textures and palettes on the GPU now
    uint64_t textureBytes = 0u;
    uint64_t retainedMeshes = 0u; // meshes kept in GPU buffers now
    uint64_t submissions = 0u;    // command buffers submitted
};

class SdlGpuPresenter {
public:
    // Null, with `error`, when SDL has no GPU device for SPIR-V.
    static std::unique_ptr<SdlGpuPresenter> create(std::string &error);
    ~SdlGpuPresenter();

    SdlGpuPresenter(const SdlGpuPresenter &) = delete;
    SdlGpuPresenter &operator=(const SdlGpuPresenter &) = delete;

    // The internal resolution, as a multiple of the game's own (1 to 8):
    // the native renderer's targets are made at it. A change while the game
    // runs (the menu's) takes effect at the next frame.
    void setResolutionScale(uint32_t scale);
    uint32_t resolutionScale() const;
    void requestResolutionScale(uint32_t scale);
    // The scale in use; any thread.
    uint32_t activeResolutionScale() const;

    // Opens the window the frames are shown in, claimed for the device.
    bool openWindow(const char *title, uint32_t width, uint32_t height, std::string &error,
                    const SdlGpuWindowPlacement &placement = {});
    bool hasWindow() const;

    // The game's Screen Size option for SdlGpuAspect::Auto, read on the
    // presenting thread: 1 for a wide screen (the window's shape), 0 for
    // 4:3, -1 unknown.
    using GameWidescreenQuery = int (*)();
    void setGameWidescreenQuery(GameWidescreenQuery query);
    int gameWidescreen() const;
    // The image SdlGpuSides::Image shows; before the window presents.
    void setSideImageLoader(SdlGpuSideImageLoader loader);

    // The frame the native renderer drew since the last call, shown in the
    // window under the menu. With `pixels`, the frame is also read back
    // into it (RGBA8, `width` by `height`), for screenshots. Presenting
    // thread. False when nothing was drawn (no window, an error, logged).
    bool presentFrame(std::vector<uint8_t> *pixels, uint32_t &width, uint32_t &height);
    // Answers the native renderer's waiting colour reads from what was drawn
    // last; a no-op when none wait. Presenting thread.
    void serveNativeReads();

    // Pumps SDL events into the menu and the pad, and shows the last frame
    // again under the menu while the game shows none. False when the
    // window wants to close.
    bool pumpEvents();
    // The host's keyboard and gamepad in the runtime's pad encoding, as
    // pumpEvents sampled them.
    void hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const;

    // The rest belongs to the thread that opened the window. The overlay must
    // outlive its installation; install null before destroying it.
    void setOverlay(SdlGpuOverlay *overlay);
    void setDisplayOptions(const SdlGpuDisplayOptions &options);
    const SdlGpuDisplayOptions &displayOptions() const;
    // False when the window cannot present in that mode; VSync is used instead.
    bool supportsPresentMode(SDL_GPUPresentMode mode) const;
    SDL_Window *window() const;
    SDL_GPUDevice *gpuDevice() const;
    SDL_GPUTextureFormat windowTextureFormat() const;
    SdlPadInput &padInput();
    // Where the game picture was drawn at the last frame, in window pixels.
    SdlGpuRect displayRect() const;
    // Whether the FSR 1 filter would act on the picture as it is now shown.
    SdlGpuFsrState fsrState() const;
    // Delivers the next frame shown: the game picture as displayed, or the
    // whole window with the overlay.
    void requestCapture(bool withOverlay, SdlGpuCaptureCallback done);

    std::string driverName() const;
    // SDL's video backend -- "wayland", "x11", "offscreen".
    std::string videoDriverName() const;
    SdlGpuPresenterStats stats() const;

private:
    SdlGpuPresenter();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dq8::gfx
