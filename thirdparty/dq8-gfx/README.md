# Hardware GS backend from DQ8-Recompiled

The SDL3 GPU implementation of PS2Recomp's GS raster backend
(`GSRasterBackend`), taken from
[DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) by Sinan
Karakaya, GPL-3.0 (see `LICENSE`), at commit
`3e74da68d7633db9996457dca4a9388ece0847c0` (2026-10-11). DQ8-Recompiled
builds on the same PS2Recomp fork this project pins.

| Path | Origin |
|---|---|
| `src/gfx/gs/`, `src/gfx/backends/sdlgpu/` | `src/gfx/gs/`, `src/gfx/backends/sdlgpu/`, with the changes below |
| `src/ui/` | `src/ui/`, the in-game menu (Dear ImGui), with the changes below |
| `cmake/Dq8Shaders.cmake`, `Dq8EmbedSpirv.cmake`, `Dq8EmbedText.cmake`, `Dq8EmbedBinary.cmake`, `Dq8HostSimd.cmake` | `cmake/`, unchanged |
| `CMakeLists.txt` | written for this project (DQ8's builds the same targets inside its own tree) |
| `window_icon.cpp` | replaces `sdlgpu_window_icon.cpp`, which embeds DQ8's icon; sets none |

Not taken: DQ8's GS dump tools (`src/gfx/gsdump`) and anything
game-specific. Dear ImGui is the submodule at `../imgui`, the commit DQ8 pins.

## Changes to the menu

The menu is DQ8's with its name taken off: the project name, About text,
links and screenshot prefix are this project's, the crown icon is a disc,
`RG_SETTINGS_FILE` and `RG_UI_SCRIPT` replace `DQ8_SETTINGS_FILE` and
`DQ8_UI_SCRIPT`, and the settings file is `settings.ini` under SDL's
preferences folder for `RogueGalaxy/Recompiled`. Its defaults differ where
this game does: internal resolution 2x, mailbox presentation (vsync holds
SDL's submission lock while the GS worker waits on it) and the line blend
left as the game draws it. The host side (pause, speed, volume, the frame
count the counter uses) is in `src/graphics.cpp`.

## Changes for Rogue Galaxy

Rogue Galaxy uploads mostly 4- and 8-bit textures every frame and resolves
render targets about ten times a frame, both far more than DQ8; these
changes speed those paths up without changing their results. The first
commit of this folder is upstream as is, so `git log -p` on it shows them.

- `gs/gs_vram`: `writeRunT8`, `writeRunT4`, `writeRunCT32`, `writeRunZ32`
  and `readRowT4`, built on the same `PixelStorageTraits` and layout tables
  as GSMem, so addresses match (the game's `RG_SELFTEST=gs` checks them
  against GSMem). The writers go span by span: along a row only x changes
  until the row or the page ends, so the page arithmetic and the lookup in
  the megabyte-sized page table happen once per span, not per texel.
- `gs/gs_transfer.cpp`: host-to-local PSMT8, PSMT4, PSMCT32 and PSMZ32
  transfers write runs instead of one dispatched `write()` per pixel.
- `sdlgpu_textures.cpp`: PSMT4 textures expand through the palette
  `buildPalette` already resolved; the general path re-read every texel's
  CLUT entry from local memory.
- `sdlgpu_targets`: resolves reuse one download transfer buffer and one
  downscale staging texture instead of creating and releasing them each
  time (they wait for their fence, so both are idle again by the next).
- `sdlgpu_backend.cpp`: the feedback snapshot's tail upload reuses one
  transfer buffer mapped with cycling; the FPS readout keeps the window's
  own title in front instead of "DQ8Recomp".
- `sdlgpu_backend.cpp`, `sdlgpu_textures`: a host-to-local transfer no
  longer flushes the queued draws unless they touch its pages (draw into
  them or into a surface they belong to, or sample from them). Rogue Galaxy
  uploads about 350 textures a frame between its draws, and each flush is a
  command buffer submission. The textures queued draws hold were expanded
  when they were queued, and the texture cache now tells the backend before
  it rewrites or releases a texture, so a draw still holding it is flushed
  first. `DQ8_GFX_FLUSH_ON_TRANSFER=1` restores the flush at every transfer.

One change fixes a result rather than speed: in `sdlgpu_backend.cpp`, a
scissor ending past the buffer (0 to 512 with FBW 8, in Rogue Galaxy's
menus) no longer grows the colour and depth surfaces past FBW pages unless
the geometry reaches there (a clear never does), and a draw's host scissor
and written region are clipped to its surface. Grown surfaces took in the page after the
buffer; texture uploads to it then discarded the depth buffer mid-frame.
