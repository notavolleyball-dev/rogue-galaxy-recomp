# Renderer, presenter and menu from DQ8-Recompiled

Taken from [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled)
by Sinan Karakaya, GPL-3.0 (see `LICENSE`), at commit
`3e74da68d7633db9996457dca4a9388ece0847c0` (2026-10-11), where it was the SDL3
GPU implementation of PS2Recomp's GS raster backend (`GSRasterBackend`).
DQ8-Recompiled builds on the same PS2Recomp fork this project pins.

Rogue Galaxy no longer emulates the GS (M1 of the game's
`docs/NATIVE-ONLY.md`, 2026-11-11): the GS backend, its render-target and
texture caches, GS local memory (`src/gfx/gs`) and the GS shaders are
deleted. What is left:

| Path | What |
|---|---|
| `src/gfx/native/` | the native renderer (Rogue Galaxy: added; the game's `docs/NATIVE-PIPELINE.md`): it takes the game's records and draws them. It links SDL3 alone, not the runtime, so no GS header compiles there |
| `src/gfx/backends/sdlgpu/sdlgpu_presenter.*` | the presenter (Rogue Galaxy: written from the backend's window and present code): the window, the device, the renderer's frames fitted into the window, the menu over them, captures and the stats line |
| `src/gfx/backends/sdlgpu/sdlgpu_device.*` | the SDL GPU device and the presentation passes (the window's sides, FSR 1), SPIR-V only |
| `src/gfx/backends/sdlgpu/sdlgpu_display.cpp`, `sdlgpu_window.h` | how the picture is fitted into the window; display options and the overlay hook the menu draws through |
| `src/gfx/backends/sdlgpu/sdlgpu_input.*` | keyboard and gamepad input, with remappable bindings and controller selection |
| `src/gfx/backends/sdlgpu/shaders/` | `present_triangle.vert` (DQ8's `gs_reinterpret.vert`), `present_sides.frag`, `present_fsr.frag` |
| `src/ui/` | DQ8's in-game menu (Dear ImGui), with the changes below |
| `cmake/Dq8Shaders.cmake`, `Dq8EmbedSpirv.cmake`, `Dq8EmbedText.cmake`, `Dq8EmbedBinary.cmake` | DQ8's `cmake/`, unchanged |
| `CMakeLists.txt` | written for this project |
| `window_icon.cpp` | replaces `sdlgpu_window_icon.cpp`, which embeds DQ8's icon; sets none |

Dear ImGui is the submodule at `../imgui`, the commit DQ8 pins. The notes on
the GS backend's changes for Rogue Galaxy are in the history, at the tag
`gs-path-final`.

## Changes to the menu

The menu is DQ8's with its name taken off: the project name, About text,
links and screenshot prefix are this project's, the crown icon is a disc,
`RG_SETTINGS_FILE` and `RG_UI_SCRIPT` replace `DQ8_SETTINGS_FILE` and
`DQ8_UI_SCRIPT`, and the settings file is `settings.ini` under SDL's
preferences folder for `RogueGalaxy/Recompiled`. Its defaults differ where
this game does: internal resolution 2x and mailbox presentation. The
"Sharper picture" switch (the GS display's line blend) went with the GS
backend. The host side (pause, speed, volume, the frame
count the counter uses) is in `src/graphics.cpp`. The Game page lists the
game's own options (`HostServices::gameOptions`, kept under `[game]`, in
the sections `HostServices::gameSections` names):
switches, and numbers shown as sliders (a `GameOption` whose maximum is
above its minimum, `HostServices::setGameNumber`) between - and + buttons
that move them by the option's step, with a Default button. A switch that
is not `remembered` stays out of the file and shows what
`HostServices::gameOptionState` says; `Overlay::setGameOptionValue`,
`setGameNumberValue` and `setSides` take a change made outside the menu
(the game's own Settings screen) and keep it as the menu's own. A `restart` switch changed
since the run started says in red that it waits for the next start. F6,
the game's free camera, cannot be bound to a pad input, as the menu's own
keys cannot, and `Overlay::capturingBinding` tells the host when the menu
is waiting for a key to bind. The window opens with its last size, place
and maximized state (`[window]` `x`, `y`, `maximized`;
`SdlGpuWindowPlacement`). Display > Borders fills the window beside a
picture that leaves it uncovered (`SdlGpuSides`, `sides =` under
`[display]`): black, the picture blurred and at three quarters of its
brightness (halved by bilinear blits to 256 texels wide, then nine taps in
`present_sides.frag`), or an
image the host loads (`SdlGpuPresenter::setSideImageLoader`), each scaled to
cover the window and drawn before the picture, which then loads instead
of clearing; the script's `sides black|blurred|image` sets it.

## Since the GS backend

GPU time by label (Rogue Galaxy: added). `DQ8_GFX_GPU_TIMES=1` labels the
GPU work (the native pipeline's frame, the present, everything else as
"other") with `SDL_InsertGPUDebugLabel`, and SDL, patched
(`cmake/patches/sdl3-gpu-timestamps.patch`, Vulkan only), writes a GPU
timestamp at each label. When a command buffer is done the time from each
label to the next is added to that label's total; every two seconds
`[gputime]` lines give each label's GPU milliseconds and count per second,
heaviest first. Off, nothing changes.

`DQ8_GFX_HIDDEN_WINDOW=1` creates the window hidden and never raises it,
for scripted runs that should leave the desktop alone; the drawing is the
same and `DQ8_GFX_SCREENSHOT_DIR` still writes frames.
