# SDL GPU presenter

The window the game is shown in, and what goes with it. The game is drawn by
the native renderer (`../../native`); nothing here knows the GS.

| File | Responsibility |
| --- | --- |
| `sdlgpu_presenter` | The window, the renderer's frames fitted into it under the menu, captures, the stats line |
| `sdlgpu_device` | The SDL GPU device, its pending command buffer, and the presentation passes (sides, FSR 1) |
| `sdlgpu_display` | How the picture is fitted into the window |
| `sdlgpu_window` | Display options and the overlay hook the in-game menu (`src/ui`) draws through |
| `sdlgpu_input` | Keyboard and gamepad input, with remappable bindings and controller selection |
| `shaders` | The presentation passes' shaders |
