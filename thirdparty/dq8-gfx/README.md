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
  against GSMem). The writers use the layout in closed form (a texel's
  piece, byte and nibble inside its 256-byte block are a fixed shuffle of
  x with a per-row flip; a `static_assert` checks the formulas against the
  column tables) and write whole 64-byte columns at a time with SSE where a
  run covers them: four rows of a PSMT4 texture at once, no read-modify-
  write. Whole-texture uploads went from about 1.8 to 0.05 ns a texel for
  PSMT4 and from 0.53 to 0.05 for PSMT8; CT32 and Z32 2.5x. Only rows
  narrower than one group fall back to texel-by-texel writes.
  `-DDQ8_GS_VRAM_SCALAR` builds the portable kernels, same bytes.
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
- `sdlgpu_backend.cpp`, `sdlgpu_targets`: a sprite that replaces every
  pixel of its colour surface (a clear) takes the surface over
  (`GsTargetCache::claimWhole`) instead of importing the pages aliased views
  own and the pending content of local memory, which it would overwrite
  anyway. Rogue Galaxy clears its per-object offscreen buffer before each
  object, and those buffers alias each other and the pages its textures go
  to. `DQ8_GFX_NO_CLAIM=1` imports as before.
- `sdlgpu_backend.cpp`: a textured primitive counts only the texture pages
  it can sample (its UV rectangle, when the coordinates are plain) against
  the queued draws, not the texture's whole 2^TW x 2^TH footprint, which is
  512 lines over the game's 448-line buffers and made every strip of its
  frame copies flush the other targets' queued draws. And a draw that
  ignores depth keeps the depth attachment of the draws around it when
  their depth surface already exists at the size it needs (tested ALWAYS,
  not written), so the clear of an object's offscreen buffer and the object
  share one render pass (`DQ8_GFX_NO_PASSIVE_DEPTH=1` for the old
  behaviour).
- Render-target readbacks (a resolve: the target downloaded into local
  memory, with a fence wait that drains the GPU) are counted by reason in
  the `DQ8_GFX_STATS_EVERY` report and named in `DQ8_GFX_TRACE_TARGETS`,
  which `gsSetTargetTrace` can now switch on for a window of frames (the
  game's `RG_GS_TRACE_BEFORE`). Rogue Galaxy provoked about eight a frame
  in the field, a quarter of the worker's time; these take them away:
  - `gs/gs_vram.cpp`: `gsMarkPages` is exact for a block base inside a
    page, through the format's block table, instead of marking the next
    page for every cell. The game writes its palettes at block offsets
    0x1c..0x1f, and the page after each held a render target.
  - `sdlgpu_targets`, `sdlgpu_backend.cpp`: host writes in CT16, T8 and T4
    patch a live native CT32 target word by word, as CT32 ones did
    (`patchHostWrite` with the format's block layout; a word is counted
    when every one of its bytes, or nibbles, has arrived, across payloads;
    whole blocks of a page go out as runs of 8x8 cells along each cell
    row, since the formats' block order scatters them; the owners of each
    page are found once per write, not once per cell), instead of reading
    back the pages they cover in part. Local copies into such a target
    patch it too, and read back only the source pages whose words local
    memory does not hold. `DQ8_GFX_PATCH_CT32_ONLY=1` keeps the patching to
    CT32. With ownership kept, a target's queued patches grow (every
    texture uploaded into its pages), so what needs only part of it
    current brings up only that part (`refreshRegion`): the GPU expansion
    of an 8-bit texture from a target, and the snapshot padding below.
  - `sdlgpu_textures.cpp`, `sdlgpu_targets`: a T8, T4 or CT16 texture read
    asks for the words it occupies (`markReadPages`), so a texture uploaded
    into pages a target still owns needs no readback; and the pages of a
    target that a draw reaches no texel of (the UV rectangle the backend
    already bounds) are left to it, their texels marked unresolved until a
    draw reaches them. The game declares a 512x512 texture over the 512x416
    it uploads, and the lines below lie in a live target.
  - `sdlgpu_backend.cpp`: a feedback snapshot's padding row (the texel row
    under the surface a bilinear tap at its edge reaches) comes from the
    texture of the target owning that row: copied for a CT32 surface, and
    for a CT16 view made by the reinterpret shader
    (`SdlGpuDevice::recordReinterpret`) from the owner's page row copied
    into a scratch. `DQ8_GFX_NO_GPU_PADDING=1` reads it from local memory
    as before.
- `sdlgpu_device`, and every place that acquired a command buffer: a
  texture upload, a target's patches, a page import, an 8-bit expansion
  from a target, a reinterpretation, a local copy and a clear were each a
  command buffer submission of their own, and each flush of the queued
  draws another. Rogue Galaxy made about 150 submissions a frame in the
  field (it flushes about 70 times a frame: feedback snapshots, transfers
  into pages the queued draws touch, aliased views), and the driver and
  kernel were near half of the GS worker. They are recorded into one
  pending command buffer instead (`SdlGpuDevice::pendingCommands`), in
  the order they happen, which is the order the GPU runs them, as with
  immediate submissions. A flush records the queued draws after them and
  submits once `DQ8_GFX_SUBMIT_DRAWS` draws (96) are in it; a readback, a
  present or the budget for resources SDL cycles per use (feedback
  snapshots and local copies, 96 MiB) sends it sooner. The vertices of
  one submission's flushes append to one buffer instead of cycling it
  every flush, and the upload transfer buffers (textures, patches,
  vertices) come from pools guarded by submission serials
  (`SdlGpuUploadPool`), not per-submission fences or SDL cycling, which
  keeps a copy of a buffer's whole capacity per upload in flight. The
  field's frames go out in 4 to 5 submissions; the stats line shows
  `flushes=` and `submits=`. `DQ8_GFX_SUBMIT_DRAWS=0` submits at every
  flush, as before.

One change fixes a result rather than speed: in `sdlgpu_backend.cpp`, a
scissor ending past the buffer (0 to 512 with FBW 8, in Rogue Galaxy's
menus) no longer grows the colour and depth surfaces past FBW pages unless
the geometry reaches there (a clear never does), and a draw's host scissor
and written region are clipped to its surface. Grown surfaces took in the page after the
buffer; texture uploads to it then discarded the depth buffer mid-frame.
