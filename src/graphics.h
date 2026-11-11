#pragma once

#include <memory>

class PS2Runtime;
namespace dq8::gfx
{
class SdlGpuPresenter;
}
namespace dq8::ui
{
class Overlay;
}

// The window, the renderer and the menu: the native renderer draws the
// game's records (docs/NATIVE-ONLY.md), the presenter shows its frames in
// its own window with DQ8's in-game menu (F1) over them. RG_SCALE=N sets the
// internal resolution for the run (the menu's saved setting otherwise).
namespace rg::graphics
{

class Setup
{
public:
    // Before PS2Runtime::initialize(): the window's owner has to be the
    // runtime's presenter by then, or the runtime opens its own. Throws when
    // there is no GPU device or window: nothing could be shown.
    explicit Setup(PS2Runtime &runtime);
    ~Setup();

    // After initialize(), which creates GS local memory.
    void install(PS2Runtime &runtime);

private:
    // The window, the device and the native renderer. Held here: the
    // runtime's GS has no backend (gs_null.h).
    std::unique_ptr<dq8::gfx::SdlGpuPresenter> m_presenter;
    std::unique_ptr<dq8::ui::Overlay> m_menu;
};

} // namespace rg::graphics
