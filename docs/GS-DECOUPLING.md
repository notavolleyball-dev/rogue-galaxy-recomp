# Decoupling the native renderer from the GS

Written 2026-11-01, after a week of work whose stated goal was dropping the
GS and whose results kept the renderer shaped like it. This records why, and
the plan that ends it. The phase record is [NATIVE-RENDERER.md](NATIVE-RENDERER.md).

## Where we are still coupled

1. **The input is GS packets.** The game builds GIF packets (about 4,800
   `GsPkt*` call sites for 2D, plus the VU1 programs' packets), the frontend
   decodes GS registers, the sdlgpu backend draws from that state. The
   phase 4 text called a thin translator "enough for phase 5" and it was
   kept as the end state instead of a stepping stone.
2. **Every change is gated on byte-identical output with the GS path.**
   `cmpvram.py` against `vg1`/`vb1` is a good safety net and a bad design
   force: it is why the glare's twenty-pass zoom streak, the sixteen-strip
   resample, the frame read as 8-bit indices and the per-object offscreen
   buffers are reproduced pass for pass.
3. **The backend models GS memory.** Page ownership, imports, patches,
   resolves, the host-writes memory and block tracking still exist. The game
   no longer allocates `gs_vram` (phase 4 step 4); the model stayed.
4. **Performance work followed the GS's shape.** Fusing or regrouping the
   GS's passes saves 7-12% each (measured); the effects as a native
   renderer would write them are fewer, simpler passes.

## What was measured (2026-11-01)

- Deck mode, 4x, 60 fps field: 29.1 fps (`f53f358`) to 56.0 fps; battle
  56.0 fps, GPU 86-99% busy. The test is capped at 60, so the gap is larger.
- Skipping every effect op that writes the frame: 41.2 to 36.4 W (12%).
- Object layers at 2x instead of 4x (`DQ8_GFX_LAYER_SCALE=2`): 54.4 to 58.1
  fps (7%).
- Under `DQ8_GFX_GPU_TIMES=1` the effects were about 44% of labelled GPU
  time under deck mode: glare 141 ms/s, composite layers 76, CBlur 66,
  plane shadow 60, strips 48. The tiny passes cost per-pass overhead, not
  fill.
- **Not measured:** deck mode at 1x and 2x, where a handheld would run. Do
  this before choosing what to optimise.

## The plan

1. **Stop requiring byte equality with the GS path** for effects. Compare
   by eye and with pixel statistics and tolerances; keep the GS path behind
   the flag as the fallback, not as the oracle. Keep byte gates only where an
   output is a data transfer (readbacks, uploads).
2. **Replace effects by what they are for**, not how the game drew them:
   the glare (a bloom: downsample, blur, add), the plane shadow, CBlur, the
   depth of field and the frame resample, as fixed native passes at chosen
   resolutions, merged into one post pass reading the frame once. New
   effects land there. Unfinished and unproven: grouping the glare's taps
   (`DQ8_GFX_FX_TAPS`, `gs_effect_taps.frag`), whose screenshots were not
   confirmed equal to the ungrouped run.
3. **A native draw list for 2D.** Sprites with a texture id, a rectangle, a
   colour and a blend, UI at output resolution; the `GsPkt*` functions feed
   it directly, family by family, starting with the setup and sprite
   functions the HUD, menus and fades use most. The packet translator stays
   for whatever has not moved.
4. **Textures by upload id**, not by address: an upload names a texture; a
   draw that reads memory the game drew (the frame as palette indices, the
   palettes at `03f80`) is replaced by an explicit native pass.
5. **Delete the memory model** (page ownership, imports, patches, resolves,
   the host-writes memory) once nothing reads it; this is phase 5.

## Order

Measure deck mode at 1x and 2x first; take step 1; then step 3 on the HUD
and menus (the largest independent piece), then step 2 as the profile ranks
it. No full playthrough gate: the feature flag keeps the GS path.
