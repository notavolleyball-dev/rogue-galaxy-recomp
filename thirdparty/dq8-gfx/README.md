# Hardware GS backend from DQ8-Recompiled

The SDL3 GPU implementation of PS2Recomp's GS raster backend
(`GSRasterBackend`), taken from
[DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) by Sinan
Karakaya, GPL-3.0 (see `LICENSE`), at commit
`3e74da68d7633db9996457dca4a9388ece0847c0` (2026-10-11). DQ8-Recompiled
builds on the same PS2Recomp fork this project pins.

| Path | Origin |
|---|---|
| `src/gfx/gs/`, `src/gfx/backends/sdlgpu/` | `src/gfx/gs/`, `src/gfx/backends/sdlgpu/`, unchanged |
| `cmake/Dq8Shaders.cmake`, `Dq8EmbedSpirv.cmake`, `Dq8EmbedText.cmake`, `Dq8HostSimd.cmake` | `cmake/`, unchanged |
| `CMakeLists.txt` | written for this project (DQ8's builds the same targets inside its own tree) |
| `window_icon.cpp` | replaces `sdlgpu_window_icon.cpp`, which embeds DQ8's icon; sets none |

Not taken: DQ8's in-game menu (`src/ui`), GS dump tools (`src/gfx/gsdump`)
and anything game-specific.
