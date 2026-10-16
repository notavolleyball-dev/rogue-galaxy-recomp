// Replaces DQ8-Recompiled's sdlgpu_window_icon.cpp, which embeds that
// project's icon. No icon is set; on Windows DQ8's version did not set one
// either.

#include "gfx/backends/sdlgpu/sdlgpu_window.h"

namespace dq8::gfx
{

void setWindowIcon(SDL_Window *window)
{
    (void)window;
}

} // namespace dq8::gfx
