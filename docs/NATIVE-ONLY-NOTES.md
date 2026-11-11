# Native-only rendering: function notes

One section per game function that [NATIVE-ONLY.md](NATIVE-ONLY.md) hooks,
written before its hook (rule 3). Sources:
- `disc/SCUS_974.90` through `tools/mipsdis.py`;
- RGDecomp's C at the commit pinned in `config/SCUS_974.90/funcmap.toml`;
- `tools/rgcpu`'s dataflow analysis, for arguments.

Mark what was confirmed in a running game, and when. Everything else is read
from the code.

## Template

### `Name_address` (size bytes; milestone)

- **Arguments:** registers, their types and meanings.
- **Reads:** structures and offsets.
- **Writes:** structures and offsets, excluding the frame's packet stack.
- **Callees:** the ones that matter.
- **Side effects to reproduce:** every write above that something else reads,
  and who reads it.
- **The hook emits:** records, and when.
- **Open:** what only a running game can settle.

## Notes

### `GlareFilterDrawMaybe_17a0d0` (3348 bytes; M3)

- **Arguments:** `a0` = an argument block: `+0` and `+4` the two work
  buffers' descriptors (a: the bright parts, b: the streaks; TEX0 at
  `+0x38`, height at `+4`), `+8` an s32 alpha reference, `+0xC` the streaks'
  strength, `+0x10` their spread, `+0x14` the strength laid over the frame
  (floats). Caller: `TaskDrawGlare_2f6b20+0x9c` only.
- **Reads:** the draw context (`GetDrawCtx`), the current frame's descriptor
  and rectangle (`DrawEnvGetFrameTexDesc`, `DrawEnvGetFrameRect`).
- **Writes:** the frame's packet stack only (seven primitive blocks); the
  draw environment is pushed, its frame set to a then b, and popped
  (`DrawEnvPush`, `DrawEnvSetFrameFromDesc`, `DrawEnvPop`), with an
  environment record added after each change (`DrawListAddEnv`).
- **Callees:** the `GsPkt*` setters; `GsPktSpriteSplitU` (sprites cut into
  strips of 32 texels of U, for the GS's texture cache: one rectangle each
  here); `GsEnvSetBlendPreset` (the table at `0x449EF0`: preset 8 is
  `A=Cs B=0 C=As D=0`, so `Cs As`).
- **What it draws** (positions in sixteenths of a pixel; colors 0x80 = 1):
  1. a cleared to `(0, 0, 0, 0x80)`;
  2. the frame onto a at `-8`, over a's whole rectangle, its UVs the
     frame's whole; alpha test GREATER than `+8`, no blend;
  3. b: a at `-4`, color `0x40`, preset 8 (`Cs As`); then a again at `-4`,
     color `0x40`, `(0 - Cs) FIX + Cd` with FIX 0x80 (`Cd - Cs`); then b's
     one-pixel border, black and clear (four sprites);
  4. five rounds `i`: `o = (s32)((i + 1) * 24 * x10)`, `hi = o - 8`,
     `lo = -8 - o`, `c = (s32)(128 xC (5 - i) / 5)`: four sprites of b onto
     b, `Cs + Cd`, at `(x0 + hi/2, y0 + hi/2, x1 + hi/2, y1 + hi/4)`,
     `(… lo/2, lo/2, lo/2, lo/4)`, `(x0 + lo, y0 + hi, x1 + lo, y1 + hi)`,
     `(x0 + hi, y0 + lo, x1 + hi, y1 + lo)` (the `/4` on y1 is the game's),
     UVs `(16, 16)` to `((w - 1) 16, (h - 1) 16)`;
  5. back on the frame: b over the whole frame, `Cs + Cd`, color
     `clamp((s32)(128 x14), 0, 255)`, UVs as above.
- **Side effects to reproduce:** none outside the packet stack: the
  environment is pushed and popped, and the list's environment records
  restore the frame's.
- **The hook emits:** a `Pass` (`PassKind::Glare`), its parameters read at
  the call; the call still runs and its packet range is replaced by the pass
  where the VIF1 thread reaches it (until M5 lets the hook skip the call:
  then every record is pushed in the game's order).
- **Differences:** the GS clamps every blend to `[0, 0xFF]`; the renderer's
  float targets do not, so b is clamped through a scratch target after the
  subtract and before each round and the composite. Each round reads b as
  it was before the round (the GS read it as its own sprites changed it).
  The sprites are filtered (the work buffers' descriptors are read
  bilinear; unconfirmed).
- **Open:** whether the frame is always the scene when the glare runs.
