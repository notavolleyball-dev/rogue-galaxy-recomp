#pragma once

#include <memory>

class GSRasterBackend;
class PS2Runtime;
namespace dq8::gfx
{
class SdlGpuBackend;
}
namespace dq8::ui
{
class Overlay;
}

// GS rendering. RG_GS picks the raster backend:
//   sdlgpu  (default when built with it) the SDL3 GPU backend from
//           DQ8-Recompiled, in its own window, with DQ8's in-game menu (F1)
//           over it; RG_GS_SCALE=N sets the internal resolution for the run
//           (the menu's saved setting otherwise, default 2),
//           RG_GS_NO_WINDOW=1 presents through the runtime's window instead
//   sw      the framework's software rasterizer
// Either runs on its own thread behind an ordered queue unless RG_GS_WORKER=0.
namespace rg::graphics
{

class Setup
{
public:
    // Before PS2Runtime::initialize(): a backend that owns the window has to
    // be the presenter by then, or the runtime opens its own.
    explicit Setup(PS2Runtime &runtime);
    ~Setup();

    // After initialize(), which creates GS local memory.
    void install(PS2Runtime &runtime);

private:
    std::unique_ptr<GSRasterBackend> m_backend;
    // The window's owner, which the runtime keeps after install(); the menu
    // draws over it and comes down before it does.
    dq8::gfx::SdlGpuBackend *m_window = nullptr;
    std::unique_ptr<dq8::ui::Overlay> m_menu;
    const char *m_name = "sw";
};

} // namespace rg::graphics
