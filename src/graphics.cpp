#include "graphics.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "debug/gs_census.h"
#include "debug/gs_timing.h"
#include "depth_reads.h"
#include "flip_pacing.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"

#if defined(RG_HAS_SDLGPU)
#include <SDL3/SDL.h>

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "runtime/ps2_pad_host.h"
#endif

namespace rg::graphics
{
namespace
{

std::string env(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return (value && *value) ? value : fallback;
}

#if defined(RG_HAS_SDLGPU)
// Called once per frame by the runtime when the backend owns the window:
// services SDL events, toggles fullscreen on F11 and hands the host pad to
// the runtime.
bool pump(void *userData)
{
    auto *backend = static_cast<dq8::gfx::SdlGpuBackend *>(userData);
    debug::notePresenterLoop();
    if (!backend->pumpEvents())
    {
        return false;
    }
    static bool f11WasDown = false;
    const bool f11Down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F11];
    if (f11Down && !f11WasDown)
    {
        SDL_Window *window = backend->window();
        SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
    }
    f11WasDown = f11Down;
    uint32_t held = 0u, pressed = 0u, sticks = 0u;
    backend->hostPadState(held, pressed, sticks);
    ps2PadPublishHostState(held, pressed, sticks);
    return true;
}
#endif

} // namespace

Setup::Setup(PS2Runtime &runtime)
{
#if defined(RG_HAS_SDLGPU)
    if (env("RG_GS", "sdlgpu") == "sdlgpu")
    {
        // RG_SHOW_FPS=1: the backend's frame rate readout in the window
        // title, which it reads under its DQ8-Recompiled name.
        if (env("RG_SHOW_FPS", "0") != "0")
        {
            _putenv("DQ8_GFX_SHOW_FPS=1");
        }
        std::string error;
        std::unique_ptr<dq8::gfx::SdlGpuBackend> sdl = dq8::gfx::createSdlGpuBackend(error);
        if (!sdl)
        {
            std::fprintf(stderr, "[rg] SDL GPU backend unavailable (%s); using the software rasterizer\n",
                         error.c_str());
            return;
        }
        const uint32_t scale = static_cast<uint32_t>(std::strtoul(env("RG_GS_SCALE", "2").c_str(), nullptr, 10));
        sdl->setResolutionScale(scale ? scale : 1u);
        if (env("RG_GS_NO_WINDOW", "0") != "0")
        {
            std::printf("[rg] GS: sdlgpu, presenting through the runtime window\n");
        }
        // 4:3, the shape the game is shown at, so the picture fills it.
        else if (std::string windowError; sdl->openWindow("Rogue Galaxy Recompiled", 1280, 960, windowError))
        {
            runtime.setExternalPresenter(&pump, sdl.get());
            // VSync makes each present wait for the monitor's refresh while
            // holding SDL's submission lock, which the GS worker then waits
            // on. Mailbox shows the newest frame at the next refresh without
            // tearing and without the wait; RG_PRESENT_MODE=vsync restores it.
            const std::string mode = env("RG_PRESENT_MODE", "mailbox");
            dq8::gfx::SdlGpuDisplayOptions options = sdl->displayOptions();
            options.presentMode = mode == "vsync"       ? SDL_GPU_PRESENTMODE_VSYNC
                                  : mode == "immediate" ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                        : SDL_GPU_PRESENTMODE_MAILBOX;
            if (sdl->supportsPresentMode(options.presentMode))
            {
                sdl->setDisplayOptions(options);
            }
        }
        else
        {
            std::fprintf(stderr, "[rg] could not open the SDL window (%s); presenting through the runtime\n",
                         windowError.c_str());
        }
        std::printf("[rg] GS: sdlgpu (%s), internal scale %ux\n", sdl->driverName().c_str(),
                    sdl->resolutionScale());
        m_backend = std::move(sdl);
        m_name = "sdlgpu";
    }
#else
    (void)runtime;
#endif
}

Setup::~Setup() = default;

void Setup::install(PS2Runtime &runtime)
{
    std::unique_ptr<GSRasterBackend> backend = std::move(m_backend);
    if (!backend)
    {
        backend = std::make_unique<GSCpuBackend>();
    }
    // In 3D scenes the software rasterizer was more than half of the game
    // thread's time; behind the queue it runs beside the game instead.
    if (env("RG_GS_WORKER", "1") != "0")
    {
        backend = std::make_unique<GSThreadedBackend>(debug::timeGsBackend(std::move(backend), "worker"));
    }
    backend = paceOnFlips(serveDepthReadsLocally(
        debug::censusGsBackend(debug::timeGsBackend(std::move(backend), "frontend"))));
    runtime.gs().setRasterBackend(std::move(backend));
    std::printf("[rg] GS backend: %s%s\n", m_name, env("RG_GS_WORKER", "1") != "0" ? " on its own thread" : "");
}

} // namespace rg::graphics
