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

### `BloomExtractBlurMaybe_179360` (1976 bytes; M3)

- **Arguments:** `a0` = an object: `+0`, `+4` the two buffers' descriptors
  (a, b); `+0x10`, `+0x14`, `+0x18` the color it is added at (floats, times
  64), `+0x1C` its alpha (times 128), `+0x20` the blur's offset (times 16).
  Caller: `TaskDrawTBox_3387a0+0x124`.
- **Writes:** besides the packet stack, `ctx->f270->f80.hi = 0` (a word of
  the draw context's frame record): the call keeps running for it.
- **What it draws** (sixteenths of a pixel; 0x80 = 1):
  1. a cleared to 0; the frame onto a at `-8`, nearest (`GsPktSetTexFilter
     0`), color `(0, 0, 0, 0x80)`, alpha test LESS than 0x80; then an
     untextured sprite over a, `Cs Ad` (A=Cs B=0 C=Ad D=0), color 0x80: a's
     color becomes its alpha (the frame's alpha where it was under 0x80,
     else 0), its alpha 0x80;
  2. b cleared, four sprites of a onto b at `-12 ± v` along both diagonals,
     color 0x40, preset 9 (`Cs + Cd`); then a cleared and the same from b;
     `v = (s32)(16 x20)`;
  3. a onto the frame, preset 9, frame mask 1 (FBMSK `0xFF000000`: alpha
     kept), color `((s32)(64 x10), (s32)(64 x14), (s32)(64 x18),
     (s32)(128 x1C))`.
- **The hook emits:** a `Pass` (`PassKind::Bloom`), as the glare's.
- **Open:** the filter of the blur's sprites (not set in their blocks; taken
  as bilinear).

### `DofFilterDrawMaybe_17d150` (1532 bytes; M3, not ported yet)

- **Arguments:** `a0` = n, the number of levels; `a1` = an array of n
  distances (floats); `a2` = the work buffer's descriptor (copied into a
  local node); `f12` = a strength (`scale`). Caller:
  `GameSceneDofDraw_1cefb0+0x1d0`.
- **What it draws:** for each level i: `GfxBlitTexRect_141000` copies the
  previous level (first the frame) into the next rectangle of the work
  buffer, each 2/3 the size of the one before (rectangles side by side,
  half a pixel up and left), filter bilinear (`GsTex1SetFilter 1`); then a
  triangle strip over the whole frame, textured from that rectangle (UVs a
  texel in from its edges), Z test on with ZTST 2 (GEQUAL), Z write off,
  frame mask 1 (alpha kept), blend on (no ALPHA set in the block), a raw
  `TEXA = 0x8000000080` (TA0 = TA1 = 0x80), color `(0x80, 0x80, 0x80,
  (s32)(128 scale))`, its vertices' Z alternating between
  `ProjectDepth(d_i)` and `ProjectDepth(d_i + d_i / 10)` column after
  column (16 columns across).
- **`ProjectDepth_15d0a0(d)`:** the point `(x, y, -d, w)` (x, y, w from the
  constant at `0x47DF20`) through the matrix at `*(0x4810A4) + 0xF20 + 0x200`
  (`MtxMulPoint_13c890`), z / w truncated: the GS Z of a point at distance d
  in front of the camera.
- **Blocking (to read next):** the ALPHA register a fresh primitive block
  carries (`DrawListBeginPrim_141b00` / `GsPktBegin_13f7e0`'s template),
  which decides how `128 scale` blends; `MtxMulPoint_13c890`'s convention
  (row or column vector), to compute the Z in the hook. With those, the
  pass is: the levels as blurred copies, each laid over the frame where
  the scene's depth is behind the level's distance (the renderer's depth
  from the producer's `standardDepth` of the two Z values), at the
  strength's alpha.
