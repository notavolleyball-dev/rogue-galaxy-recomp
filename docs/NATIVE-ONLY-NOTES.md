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
  restore the frame's. The hook skips the call (M3 step 2): the pass
  stands at a marker in the list.
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
- **Writes:** besides the packet stack, `ctx->f270->f80.hi = 0`: the word
  at `*(ctx+0x270)+0x84` cleared (0x1793e4-0x179420). The hook makes that
  write itself and skips the call (M3 step 2).
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

### `DofFilterDrawMaybe_17d150` (1532 bytes; M3)

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
- **Settled since:** `MtxMulPoint_13c890` (VU0 macro: `ACC = row0 x`,
  `+= row1 y`, `+= row2 z`, `out = ACC + row3 w`) is a row vector times the
  matrix; the constant at `0x47DF20` is `(0, 0, 0, 1)`; so
  `ProjectDepth(d) = (s32)((M32 - d M22) / (M33 - d M23))` with M at
  `*(*(0x4810A4) + 0xF20) + 0x200`. The hook can compute it; the producer
  turns it into the renderer's depth (`standardDepth`). The Z tests:
  `GsPktEnableZTest(0)` is ALWAYS, `(1)` GEQUAL; `GsPktSetZTest(0, 1)` ALWAYS,
  `(2)` GEQUAL, `(3)` GREATER (`GsTestSetZTest_156f40`); the last call wins.
- **The default ALPHA:** a fresh primitive block's template
  (`GsPktInit_13f7f0` -> `GsEnvInit_1573b0` -> `GsEnvSetDefaults_1572d0`)
  takes preset 1 of the table at `0x449EF0`: `(Cs - Cd) As + Cd`, ordinary
  alpha blending; the strip blends at alpha `128 scale`.
- **The hook emits:** a `Pass` (`PassKind::DepthOfField`): the levels'
  count, the alpha, and per level the two GS Z (`ProjectDepth` computed in
  the hook), which the producer turns into the renderer's depth; the depth
  target is the one the scene's 3D draws last used.
- **Open:** the work buffer's format (the strip reads it with `TEXA` 0x80:
  taken as read without its alpha, 1). With those, the
  pass is: the levels as blurred copies, each laid over the frame where
  the scene's depth is behind the level's distance (the renderer's depth
  from the producer's `standardDepth` of the two Z values), at the
  strength's alpha.

### `ScreenColorFilterMaybe_179c60` (M3)

- **Arguments:** `a0`, `a1`, `a2` = r, g, b (bytes, 0x80 = 1). Callers:
  `GameSceneTintApply_1ce8f0` (the scene's tint).
- **What it draws:** the frame viewed as CT16 (a node over the frame's TBP,
  twice its height, `0x3A` written into its TEXA-like field) drawn onto the
  same view 8 pixels over with `TEXA = 0x80 << 32`, which copies each
  pixel's RG half onto its BA half: the frame's alpha gets green's bits
  2-7, keeping its own bits 0-1 (the renderer's `ChannelShuffle::
  GreenToAlpha`); then an untextured sprite over the frame, `Cs Ad` (A=Cs
  B=0 C=Ad D=0), color `(r, g, b, 0x80)`.
- **The hook emits:** a `Pass` (`PassKind::ColorFilter`); the call runs and
  its packets are replaced (as the glare's).

### `RefractionCaptureFrame_13ffd0` (1108 bytes; M3; from the disassembly, no RGDecomp source)

- **Arguments:** `a0` = the draw context, `a1` = a shift. Caller:
  `CharaDrawTranslucentPass_1f8250+0x43c`.
- **Writes outside the packet stack:** the node at `ctx+0x3BD8` (reset,
  then an image of `1 << a1` square, CT32, its TBP at `ctx+0x3C10` set to
  a word of `*(ctx+0x3BA0) + 4` less the image's size in blocks), a byte at
  `ctx+0x3C14`, and `ctx+0x270` (moved back by 0x190 when past
  `ctx+0x280`). The refraction materials sample that node: the call keeps
  running.
- **What it draws:** the frame (its own descriptor) into the node, one
  sprite, UVs the frame's whole, positions the node's whole, color 0x80,
  blend off (`GsPktSetAlphaBlend(4)` writes its low bit), no alpha test.
- **The hook emits:** after the call, a `Pass` (`PassKind::Capture`) into
  the node's buffer (declared at its size if the game never declared it).
- **Open:** the filter (the frame descriptor's TEX1; taken as bilinear).

### `ShadowMapBlur_145b60` (M3)

- **Arguments:** `a0` = the draw context: the shadow map's node at
  `+0x3A70` (w `+2`, h `+4`, TEX0 `+0x38`), its partner's pointer at
  `+0x3AE8` (none: nothing drawn).
- **What it draws** (sixteenths of a pixel; 0x80 = 1): the map into its
  partner at color 0x2E, no blend, then back at 0x80; eight rounds `i`, into
  the partner when `i` is even, else the map, from the other read as CT24
  (`psm = 1`, TEXA TA0 0x80: alpha 1), bilinear, preset 9 (`Cs + Cd`),
  color 0x26, shifted by the tables at `0x449D10` / `0x449D30` (x: 20, -20,
  0, 0; y: 0, 0, 20, -20; by `i % 4`) less 8; then the map viewed as CT16
  (twice its height) copied onto itself 8 pixels over with no frame mask:
  each pixel's RG half onto its BA half, whole (blue = red, alpha = green,
  byte for byte); then, after the environment's pop, a line strip around
  `(0, 0)`-`(w - 1, h - 1)` in black, alpha 0.
- **Writes outside the packet stack:** the view stack's pop
  (`ViewStackPop_1443e0`), the light state's packet into the list
  (`LightStateWritePacket_1484a0`): the call keeps running.
- **The hook emits:** a `Pass` (`PassKind::ShadowMap`).
- **Open:** the frame the border's line strip goes to is the one the
  environment pops back to; taken as the map (the blur follows its render).

### `FocusFilterDrawMaybe_17c000` (M3)

- **Arguments:** `a0` = an argument block: names of four buffers (A, A',
  C, C', found in the draw context's name table by name; each pair the same
  size), a texture to load at `+0`, distances at `+0x84` (f) and `+0x88`.
- **What it draws first:** into the frame, alpha only (frame mask 2), an
  untextured sprite of alpha 0x80; then four sprites with the Z test GEQUAL,
  no Z write, alpha `128 col[i]` (col = 0.75, 0.35, 0.25, 0), at the GS Z
  `ProjectDepth(sc[i])` (sc = 0.5 f, 0.667 f, f + 0.75 (f - f88), f): the
  frame's alpha becomes a mask by depth band. Then the frame into A and into
  C (preset 0: Cs), C's color set to `0x3C Ad` (A=Cs B=0 C=Ad D=0), both
  copied into their partners (`GfxBlitTexRectPixelsAt_141810`), then blur
  rounds along `(0, -1), (-1, 0), (0, 1), (1, 0)` (the rest of the function,
  past this note, still to read) and the composite back.
- **The rest:** four rounds between A and A' (each the other shifted by
  `(0, -1), (-1, 0), (0, 1), (1, 0)` times 18 sixteenths, less 8), blended
  `(Cs - Cd) FIX + Cd` with FIX 0x50, alpha kept (frame mask 1); four
  rounds between C and C' (24 sixteenths), preset 9 (`Cs + Cd`), alpha
  kept; C viewed as CT16 copied onto itself 8 pixels over (frame mask 3):
  its red and green onto its blue and alpha; C into A, alpha only (frame
  mask 2): A's alpha the blurred mask; back on the frame, A over it, preset
  1 (`(Cs - Cd) As + Cd`), color alpha 0x40, the frame's alpha kept.
- **The hook emits:** a `Pass` (`PassKind::Focus`). The four buffers are
  found by name in the call, so the hook takes them from the frames the call
  sets (`DrawEnvSetFrameFromDesc`, recorded by `native_capture.cpp`'s hook
  while the call runs): A, C, then A' first among the rounds, C' first
  among the second rounds.
- **Open:** the CT16 copy under frame mask 3 is taken as the shadow map's
  (blue = red, alpha = green).

### `DrawListEndPlaneShadowPass_146cc0` (M3)

- **Arguments:** `a0` = the draw context. Nothing is drawn unless
  `+0x3890` (set by the begin) is nonzero. The mask's node is at `+0x38D0`
  and its blurred copy's at `+0x39B0` (TEX0 at `+0x38`, width `+2`, height
  `+4`). Flags: `+0x39A0` (blur), `+0x39A4` (red and green onto blue and
  alpha), `+0x39A8` (colored). Strength at `+0x3A18` (f), color at
  `+0x3A20..+0x3A28` (f). The blur's offsets come from the six-entry s32
  tables at `0x449CD0` (x) and `0x449CF0` (y), in sixteenths.
- **What it draws:**
  - On the mask, still its environment, an untextured whole sprite, preset
    3 (`Cd - Cs As`, Cs and As 1.0): the mask less one step, clamped at 0
    by the GS.
  - Without the blur: on the frame, the mask read as CT24 with TEXA
    `TA0 = 128 strength`, AEM 1 (black gives alpha 0), alpha test GEQUAL 1,
    preset 7 (`Cd (1 - As)`), the whole frame from the whole mask.
  - With the blur:
    - Into the copy, the mask as CT24 with `TA0 = 0x27`, AEM, HIGHLIGHT2,
      preset 8 (`Cs As`), offset by -12 sixteenths.
    - Six rounds of the copy added onto itself, shifted by the table's
      offsets less 8 sixteenths, color `(0x24, 0x24, 0x24, 0x10)`.
    - With `+0x39A4`, the copy's red and green onto its blue and alpha (the
      CT16 self-copy).
    - On the frame, the copy over it, color only (alpha kept):
      - colored: color `128 (r, g, b)`, alpha `128 (1 - strength)`, preset 1;
      - `+0x39A4` without color: alpha `128 strength`, preset 7;
      - otherwise: color `128 strength`, preset 10 (`Cd - Cs`).
- **Side effects:** `ViewReapplyCameraRec`, `LightStateWritePacket` (VU1
  data, not GS packets), so the call keeps running and only its GS packets
  are dropped.
- **The hook emits:** a `Pass` (`PassKind::PlaneShadow`); the renderer
  clamps the mask and the copy through scratches (float targets don't clamp
  the way the GS's COLCLAMP did).

### `CBlurTexBlurMaybe_1789e0` (M3)

- **Arguments:** `a0` = an object. Nothing is drawn unless `+0` is
  nonzero. The trail P is named at `+8` and its history Q at `+0x28`; both
  are found in the draw context's name table (`ctx+0x3BA0`,
  `NameTableFindByName_159fb0`: the name plus the table's suffix at
  `+0x1E4`, hashed into 101 buckets at `+0x30`, each entry's name at `+8`).
  `+0x6C` picks the feedback blur, `+0x70` (f) is the history's strength.
  The taps are the s32 tables at `0x47DFD0` (x) and `0x47DFE0` (y), four
  each.
- **What it draws:**
  - Without feedback, P into Q first (`GfxBlitTexRectPixelsAt_141810` is
    (ctx, source, rect, destination, ...): Q keeps P as it was).
  - On P, alpha kept (frame mask 1): an untextured `0xFF` sprite, then the
    same sprite again as `Cs Ad` (A=Cs B=0 C=Ad D=0): P's color is its alpha
    (the mask `CBlurDrawMaybe` wrote), doubled.
  - Feedback: P onto itself, preset 9 (`Cs + Cd`), color `0x20`, eight
    sprites at UV `16 + 8 tap` sixteenths (the four taps, twice).
  - Otherwise: Q onto P, preset 9, color `128 strength`, UV one pixel over.
  - P viewed as CT16, twice as tall, copied onto itself in eight-pixel
    columns under frame mask 3 with TEXA `TA1 = 0x80`: its red and green
    onto its blue and alpha. P's alpha is the trail.
- **The hook emits:** a `Pass` (`PassKind::CBlur`). The renderer reads each
  feedback tap from a copy of P as the tap before left it.

### `CBlurDrawMaybe_1784e0` (M3)

- **Arguments:** `a0` the object above (nothing unless `+0`, nor when
  `+0x68`); `a1` a color (r, g, b, a floats); `a2` a blend preset; `a3`
  small; `t0`, `t1` the shift (dx, dy). The sprite S is named at `+0x28`
  when small, else at `+0x48`. When small, the shift is scaled from the
  environment's size (`*(ctx+0x270)` `+4`, `+8`) to S's.
- **What it draws:**
  - Not small: the frame (`DrawEnvGetDispFbTexDesc_144a00`: the
    environment's FRAME at `+0x54`, its size at `+0x14`, `+0x18`) copied
    into S, W by H (`GfxBlitTexRectPixels_141710`).
  - Small: S filled white, alpha 0x80.
  - On S, alpha only (frame mask 2): 0 when shifted, then P's alpha at the
    shift.
  - On the frame, Z test ALWAYS without Z write, alpha kept (frame mask 1),
    preset `a2`: S over the frame, color `128 (r, g, b)`, alpha `64 a`,
    from `(-1 - dx, -2 - dy)` to `(W - dx - 1, H - dy - 2)`.
- **The hook emits:** a `Pass` (`PassKind::CBlurDraw`).

### `ModelDrawOffscreenCompositeMaybe_16aac0` (M3; its tail)

- **Arguments:** `a0` the settings (`+0x20` the offscreen buffer's node,
  `+0x24` (f) the glow's size, `+0x28` a point to project, `+0x40..+0x48`
  the glow's color, `+0x50`, `+0x5C` on, `+0x60` the lay-over's kind,
  `+0x64` the glow's mode); `a1` the object; `f12` a scale; `f13` the
  alpha. `ModelDrawOffscreenCompositeAtMaybe_16b8a0` copies a quad into
  `a0+0x30` and calls it.
- **The head:** the parts' bounds (`ModelPrepareDrawMaybe`, the box at
  each part's `+0x20`) united and clipped to the centered viewport; on the
  buffer (origin mode 2: centered), a clear of that box widened by 8 pixels
  and a pixel more, then the parts' models.
- **The tail:** back on the frame, TEX0's PSM set to CT24 in the frame
  descriptor's copy (`+0x3A` bits 4-9 are PSM, not a level of detail) with
  TEXA `TA0 = 0x80`, AEM:
  - When glowing, `ScreenTexOverlayDrawMultiMaybe_16a510(pkt, rect, tex,
    color, mode, ofs)`: preset 1, alpha test GREATER 0, linear; mode 1 one
    copy, mode 2 two at `+-3/4 ofs`, mode 3 four at `(+-ofs.x, 0)` and
    `(0, +-ofs.y)`. The color is the settings' mixed toward the table at
    `0x47DF60` by the float at `0x47DF54`.
  - `ScreenTexOverlayDrawSingleMaybe_16a780(pkt, rect, tex, rgba)` (it
    reads no more arguments; the composite's fifth is unused), color the
    s32 table at `0x47DF80`, alpha test GEQUAL 1: with `+0x60` 0, preset 1
    at alpha `128 alpha`; with `+0x60` and a glow, CT32 and `(Cs - Cd) FIX
    + Cd` with FIX `128 alpha`; otherwise both, the second CT32 with alpha
    test GEQUAL 0x81.
  - The rect is (ly, lx, hy, hx) in centered sixteenths. The overlay calls
    write no GS packets: each is a VIF packet that runs VU1's overlay
    program (`MSCAL 0x12C`, docs/VU1-NATIVE.md's `:960` rows), whose
    sprites cover the vertex bounds within the rect, at `rect / 16 +
    center` (the environment's `+0x0C`, `+0x10`) from texels `rect / 16 +
    size / 2`.
  - When the object's character blur is on (`a1+0x454` its trail, `a1+0x458`
    (f) the strength, above the float at `0x47D24C`): on the trail, alpha
    only, the buffer's texels `lx / 16 + w / 2 + 1 .. hx / 16 + w / 2 - 1`
    scaled to the trail, alpha `128 strength`, GEQUAL 1.
- **The hooks emit:** an `ObjectLayer` pass for each overlay call
  (recorded while a composite runs) and one for the blur mask, all under
  the composite's unit; the VIF1 thread sends them at the tail, reading
  the object's layer when its head was routed there. The whole rect is
  drawn: outside the object the layer is its clear, alpha 0, which the
  alpha test drops.
- **Open:** a projected point (`+0x28`) turns on Z writes for the
  lay-over; the Z the overlay program writes is not reproduced.

### `ScreenFadeBlockCaptureFrame_177ed0` and `ScreenFadeBlockDraw_177540` (M3)

- **Capture:** with `a0+0x2C` set and its `+0x50` (an EE buffer) set, the
  frame's descriptor (`DrawEnvGetFrameTexDesc`) downloaded into that buffer
  (`GsDownloadImage_140f20`: `sceGsSetDefStoreImage`, `sceGsExecStoreImage`).
- **Draw:** with `+0x24` and a texture node at `+0x2C` (whose image is the
  captured buffer), the node loaded (`DrawListAddTexLoad`) and drawn with
  TEXA `0x8080 / 0x80`, preset 1, either as wipe strips (`+0x20 == 1`,
  `ScreenFadeBlockDrawWipeStrips_177a70`) or tiled over the screen at
  `(r, g, b, a * +0x28)`; else an untextured tint. `+0x30` lays the display
  buffer over itself, zoomed by `+0x3C`.
- **Native:** every colour read (`native_capture.cpp`) is also kept on the
  GPU at the renderer's resolution (`ColorRead::keep`, a target per EE
  buffer). An upload from that buffer while it still holds the bytes the
  read wrote (their hash) becomes a texture copied from the kept target
  (`Texture::fromTarget`), addressed at the game's size: the faded frame
  is as sharp as it was drawn. The read still writes the EE buffer, for
  any reader on the CPU.

### Draw tasks and layers (`TaskDraw*`, 62; M4)

- **What:** the task manager's draw callbacks, each drawing one part of
  the frame into the draw list being built. The packets a task writes (from
  the list's memory-stack top at its entry to the top at its return) are
  its, and their 2D gets the task's layer (`Draw2D::layer`), the VU1
  programs' kicks by the list that holds their MSCAL.
- **Layers, by the task's name (to confirm in play):**
  - Menu: every `TaskDrawMenu*`, `TransPorter`, `DiscChange`, `Factory`,
    `InsectCage`, `Monography`;
  - Text: `Message`, `Caption`, `TBox`;
  - Fade: `Fade`;
  - Overlay: `PauseFilter`, `GrayScaleFilter`, `DebugAction`;
  - Hud: `IconStealth`, `Status`, `StatusEffect`, `ObjectCursor`,
    `TargetMng1/2`, `ThinkCircle`, `AttackChain2`, `JudgeObject`;
  - Scene: the rest (the characters, the sky, the effects, the events,
    the NPCs, the insects in the field).
- **Open:** whether the `Event1-4` tasks draw text or letterboxes (Scene
  for now); whether `TBox` is the treasure box's message (Text) or a 3D box.

## M5: models, from the code (2026-11-11)

Read from the decompiled C and the disassembly (and the VU1 pieces'
microcode); nothing here is confirmed in a running game yet. Guesses are
marked as such.


### Model resources and meshes (M5)

Sources: RGDecomp C under `build/rgdecomp/src/main/`, checked against the
disassembly (`tools/mipsdis.py`) where it matters. Tags used below:
**[dis]** confirmed in the disassembly, **[C]** the decompiled C only (it
looks right, but nobody has checked it instruction by instruction),
**[guess]** a guess.

##### 0. Summary

* **Most meshes' vertex data is a prebuilt VIF packet that is built once, at load.**
  `ModelMeshCreate_14e4d0` creates a `ModelMeshLinks` (vptr `0x470010`)
  for every ordinary mesh. Its `BuildFromRes` (`0x15af00`) calls
  `ModelMeshBuildUnpackPktMaybe_15b2a0` once. That function writes, into the model's
  persistent MemStack, one VIF packet per chunk (an UNPACK of the header,
  positions, normals, UVs, colors, skin, then MSCNT). For each material it
  also writes a list of DMA **REF** tags to those chunks, ending in RET.
  Each frame the game only emits a **CALL** to that REF list
  (`ModelEmitDrawPacketsMaybe_14ec20`), so the chunk bytes are never rewritten. **[dis]**
* **This holds for skinned meshes too.** Bone slot indices and weights are
  baked into the chunk packets. Only the bone palette (a per-frame RET
  packet UNPACKed to quads `900-4*nb .. 899`) changes. So the skinned
  half that Issue 2 measured as "never repeated" has **static input**.
  Only the VU1 output changes. **[dis]/[C]**
* The exception is meshes marked `MODEL_RM` in a map config
  (`MapModelCfgModelRm_195cd0` sets class bit 1). Those become a plain
  `ModelMesh` (vptr `0x470040`): the arrays are copied to the heap and the
  same `BuildUnpackPkt` runs **every frame** into the frame's packet buffer
  (`ModelMeshGetGroupPktsMaybe_15b150`). Cloth uses this:
  `ClothBuildFromMeshMaybe_185500` needs the mesh's `+0xF0` group list,
  which Links clears, and `ClothWriteMeshVerts_1850f0` →
  `VertsPackQuantized_164bb0` writes positions back. **[C]** That cloth is the only
  user is a **[guess]**.
* The CPU does no per-vertex work per frame for static meshes. At load it
  de-indexes, splits into chunks, and converts the skin data. Dequantizing,
  skinning, lighting, UV effects, transform, cull and fog all run on the VU1.

##### 1. Resource layout (".MDS", relocated in place by `ModelResRelocate_14bfd0`)

Offsets are from the start of the file. All pointers are offsets in the file and are
made absolute by `ModelResRelocate_14bfd0`. The relocation base differs per
structure, as noted below.

###### Header (`Hdr`)
| off | type | meaning | source |
|---|---|---|---|
| 0x00 | char[4] | magic `" MDS"` (`D_00458B00`); after relocation byte 0 is set to `'@'` → `"@MDS"` (`D_00458AF8`), which marks it as already relocated | Relocate **[C]**, strings dumped from the ELF |
| 0x0C | s32 | frame count | `ModelResGetFrameCount_14c850` |
| 0x14 | s32 | frame record stride | `ModelResFindFrameByName_14c660` |
| 0x18 | ptr (file-relative) | frame array | " |
| 0x20 | s32 | mesh count | `ModelResGetMeshCount_14c5c0` |
| 0x24 | ptr (file-relative) | first mesh node (a linked list) | `ModelResGetMeshInfo_14c440` |
| 0x2C | s32 | material count | `ModelResGetMaterialCount_14c640` |
| 0x30 | s32 | material record stride | GetMeshInfo / Relocate |
| 0x34 | ptr (file-relative) | material array | " |
| 0x38 | ptr (file-relative) | string table; name fields below are relative to it | Relocate |
| 0x40 | off (string-relative) | ? (relocated against the string table when it is not 0) | Relocate |

###### Frame record (stride at hdr+0x14)
`+0x00` name (string-relative), `+0x04` second string (string-relative),
`+0x0C` s32 mesh index (−1 means no mesh; the Relocate count `self->cnt` is the
number with ≥0). `MapModelCfgModelRm_195cd0` uses `+0x0C` to get from a frame
name to its mesh.

###### Mesh node (list from hdr+0x24, linked by `+0x80`)
`ModelResGetMeshInfo_14c440(out, res, i)` walks to node *i* and returns an
8-word `Info`: `{node, matCount(hdr+0x2C), matStride(+0x30), matArray(+0x34),
primGroupCount(node+0x70), totalIndices, listTris, stripTris}`.
Every `ModelResGet*` getter takes this `Info` (`*Info` is the node).

| node off | type | meaning | getter |
|---|---|---|---|
| 0x10 | u8 | position format: 8 / 16 / 32 (bits per component; 32 = float) | `GetPosFormat_14cd70` (default 0x20) |
| 0x11 | u8 | normal format | `GetNormalFormat_14cd50` |
| 0x12 | u8 | UV format | `GetUvFormat_14cd30` |
| 0x13 | u8 | must be 0x10 for skinning to count (meaning unknown) | `GetSkinInfluenceCount_14cdb0` |
| 0x14 | u8 | weight format: 8 (percent) or 16 (1/32768) | `GetWeightFormat_14cd10` |
| 0x20 | f32x4 | position scale (×256 by the getter when the format is 8) | `GetPosScale_14ccb0` |
| 0x30 | f32x4 | "position offset". Copied to mesh+0x80, but nothing that draws reads it (not in the VU header, not added by `ModelReadVertices_14c9f0`). **[guess]** bounds / unused | `GetPosOffsetMaybe_14cc90` |
| 0x40 | f32x4 | normal scale (×256 when the format is 8) | `GetNormalScale_14cc30` |
| 0x50 | f32x4 | UV scale (×256 when the format is 8) | `GetUvScale_14cbd0` |
| 0x60 | ptr (node-relative) | vertex buffer block `VB` | `GetVertexBufMaybe_14cd90/_14ce60` |
| 0x68 | s32 | mesh class; bit 1 = MODEL_RM, rebuilt per frame | `Get/SetMeshClass_14cea0/_14ce80` |
| 0x6C | u16 | mesh mode: 0 = indexed (the normal case); 1 = "Mode1" (below) | `GetMeshMode_14cec0` |
| 0x70 | s32 | prim group count | `GetPrimGroupCount_14ce40` |
| 0x74 | ptr (node-relative) | prim group list `PG` | `GetPrimGroupList_14ce20` |
| 0x80 | ptr (node-relative) | next node | Relocate |

###### Vertex buffer block `VB` (node+0x60; pointers relative to VB)
| off | meaning |
|---|---|
| 0x00 / 0x04 | position count / array: 3 comps × fmt bits (s8/s16/f32) |
| 0x08 / 0x0C | normal count / array: 3 comps × fmt |
| 0x10 / 0x14 | UV count / array: 2 comps × fmt (u8/u16/f32) |
| 0x18 / 0x1C | color count / array: RGBA u8 (128 = 1.0) |
| 0x20 | skin influences per vertex (n) |
| 0x24 | weights: n per position, u8 percent or u16 (fmt node+0x14) |
| 0x28 | bone indices: n × s16 per position (−1 = none); a frame/bone index |
| 0x30, 0x40 | bounds quads; mesh+0x30 / +0x20 (w forced to 1). These are the `min`/`max` that `ModelPrepareDrawMaybe_14f760` culls with. Which one is min is not settled |
| 0x50 | quad → mesh+0x10 (**[guess]** a bounding sphere) |
| 0x60 | f32 → mesh+0x120 |

Each of the four streams (position, normal, UV, color) is a separate array
with its own count. Triangles index into them with **separate index lists per
stream**, like OBJ.

Decode rules. CPU (`VertexDecodeS16_14b410`, `S8_14b3a0`, `F32_14b4a0`,
`UvDecodeU16_14b300`, `U8_14b260`, `F32_14b480`):
`pos = s16/32768 * scale`, `pos = s8/128 * scale`, `uv = u16/32768 * scale`,
`uv = u8/128 * scale`. The VU1 does the same thing. The VIF sign-extends
(positions, normals) or zero-extends (UVs, colors) to int32. B then does
`/32768 * quad77/78/79`, and quad 77-79 holds the scale ×256 for the 8-bit
formats, so the results are identical.

###### Prim group `PG` (node+0x74; linked; pointers relative to PG)
| off | meaning |
|---|---|
| 0x00 | next (PG-relative) |
| 0x04 | u16 prim id. It goes **verbatim into the chunk header TOP+0.x**: 3 = triangle list, 4 = strip, 0x11 / 0x13 = the outline edge streams (2 or 4 points per edge; `ModelResCalcPrimCountMaybe_15c780` handles `(id & 0xF0) == 0x10`) |
| 0x06 | u16 index count `n` (the same for every stream) |
| 0x08 | u8 item count (one per material that draws this group) |
| 0x0C | ptr item array, 16 bytes each: `{s32 material index, ptr UV index list, ptr normal index list, ptr color index list}` (all u16 × n; PG-relative; NULL means no stream) |
| 0x10 | ptr position index list (u16 × n) |

A PG with several items draws the same triangles once per material. An item
whose material type is 2, 3, 9 or 11, when it is not the first item, gets
sort priority `+10000` and **reuses the first item's chunk packets**
(`it->f14 = arr`; `BuildUnpackPkt` copies `xC/x10` from it). **[dis]**
So two materials' REF lists point at the same chunk bytes (multi-pass).

###### Material record (stride hdr+0x30, array hdr+0x34). `MeshMaterialBuildPkt_14ac90`
| off | meaning |
|---|---|
| 0x00 | quad → VU1 quad **82** (material color; .w is the alpha, scaled by `+0x53/128` when flags&0x80) |
| 0x20 | quad → VU1 quad **84** (effect parameters: wave 84.x, swell 84.xy …) |
| 0x30 | name (string-relative), strdup'd into mat+0x10 |
| 0x34 | texture name (string-relative) → `NameTableFindByName_159fb0(drawctx+0x3BA0, name)` → texture node |
| 0x38 | string-relative ? |
| 0x40 | s32 material type. Picks the GIF tag: 1 = RGBAQ + 8×XYZF2 (outlines), 4 = XYZF2 only (shadow volume, forces ABE), 5 = PRIM + 4×(ST,RGBAQ,XYZF2) (particles), others = ST,RGBAQ,XYZF2. 6/8 use the refraction texture node; 6 also sets FRAME mask; 11 drops TME and puts 800 in 84.y if ≤2. It also indexes the VU1 program table `D_00491280` (the program pieces) |
| 0x44 | s32 flags → VU1 87.y (2 = TME, 8 = FGE, 0x20 = ABE, 0x80 = FBA + alpha scale, 0x200 clears 4, 0x400 = FRAME mask, 0x2000 = alpha fail; 1 / 4 / 0x40 pick program variants via mat+0x18 / +0x1A) |
| 0x48 | blend preset (−1 none) → GsEnv ALPHA |
| 0x4C | u8: IIP bit in PRMODE |
| 0x4D / 0x4E | alpha test func / ref (0xFF none) |
| 0x4F | dest alpha mode (DATE) |
| 0x50 | z test (0xFF none) |
| 0x51 | z mask |
| 0x52 | s8 → 87.z = 1 + 0.01 × v (depth scale) |
| 0x53 | u8 alpha (with flags 0x80) |

Runtime `MeshMaterial` (0x50 bytes; one per material per model; built at
load by `ModelBuildMeshes_152380`): `+0x00` packet EE address (& 0x0FFFFFFF),
`+0x04` s16 its qwc, `+0x06` u8 per-frame color re-upload flag, `+0x07` the
desc's +0x52, `+0x08` PRMODE bits, `+0x0A` type, `+0x0C` texture node,
`+0x10` name, `+0x14` GsEnv (TEST / ALPHA / ZBUF / FBA / FRAME), `+0x18/+0x1A`
program variant indices, `+0x1C` flags, `+0x20` color, `+0x30` param.

The material packet is built at load and only REF'd later:
* `STCYCL 1,1; UNPACK V4-32 ×3 → quad 82` : {color, 0 (quad 83), param (quad 84)}
* `STCYCL 1,1; UNPACK V4-32 ×3 → quad 86` : {vertex GIF tag (NLOOP filled by the VU from TOP.z), PRMODE/flags/depth scale (quad 87), 0 (quad 88)}
* if there is a texture: `DIRECT 3` + GIF A+D: **TEX1_1 = texnode+0x40, CLAMP_1 = texnode+0x48**.

**TEX0 is not in the material packet.** The first time a material is used in
a frame, `DrawListWriteTexEnvPacket_148370` → `TexEnvBuildDmaChainMaybe_14abb0`
builds a frame-local packet: CNT(6) + DIRECT GsEnv (FRAME, ZBUF, TEST,
ALPHA …), **REF to the material packet**, optionally (`mat+0x06`) an UNPACK
of mat+0x20 to quad 82, then DIRECT A+D **TEX0_1 = texnode+0x38**, then RET.
The cache `tbl->addrs[j]` is cleared each frame by `ModelPrepareDrawMaybe_14f760`.

##### 2. Mesh objects, the prebuilt packet, and what a chunk carries

###### Construction (load)
`ModelBuildFrames_1506c0` / `ModelBuildFromRes_150f60` → `ModelBuildMeshes_152380`
(per model: builds the materials, then for each mesh
`ModelMeshCreate_14e4d0(Info, heap, drawctx+0xD0 temp heap)`):

| mode / class | class (vptr at +0x50) | Build (vtable +0x24) | GetGroupPkts (+0x18) |
|---|---|---|---|
| mode 0, class&1 == 0 | **ModelMeshLinks** 0x140 B, vptr 0x470010 | `15af00`: `BuildFromRes(copy=0)` into the temp heap, then **once** vtable +0x28 = `BuildUnpackPkt` into the persistent heap top, `MemStackAllocQw`; copies the group list to **+0x130**; frees the temp heap; zeroes +0xB0..+0xF0 | `15aeb0`: returns the stored lists, builds nothing (returns 0 quads) |
| mode 0, class&1 (MODEL_RM) | **ModelMesh** 0x130 B, vptr 0x470040 | `15c670` → `BuildFromRes(copy=1)` (arrays and index lists memcpy'd into the heap) | `15b150`: **calls `BuildUnpackPkt(dst = frame buffer cur)` every call**, i.e. every frame |
| mode 1 | **ModelMeshMode1** 0x120 B, vptr 0x4701c0 | `164770`: float positions (32-bit), VB = `{a, b(material?), c count, p0 pos 6 floats/vertex?, p1, p2, p3, q20, q30}` | `1645c0`: emits a stub CNT/RET with no geometry; one material. Not investigated: **[guess]** the EE uploads float vertices elsewhere (the "raw float" B pieces) |

ModelMesh / Links object fields (from `BuildFromResMaybe_15b7c0` **[dis]**,
`ModelMeshInitMaybe_15c8c0`, `WriteVuHeader_15b1d0`):
`+0x00` VIF BASE (0x8C), `+0x04` VIF OFFSET (0x17C, or 0x17C − 2·nb when skinned),
`+0x08/+0x0C` per-buffer input size (0xBE, or OFFSET/2) → quad 76.xy,
`+0x10` VB+0x50, `+0x20/+0x30` bounds, `+0x40..+0x4C` stats (prim groups,
indices, list tris, strip tris), `+0x60..` build data (`+0x64/68/6C` pos/nrm/uv
fmt, `+0x70` pos scale → q77, `+0x80` pos offset, `+0x90` nrm scale → q78,
`+0xA0` uv scale → q79, `+0xB0..+0xCC` count/ptr ×4 streams, `+0xD0` skin
table, `+0xD4..+0xE0` index-list groups), `+0xF0` group list (plain ModelMesh),
`+0x124` bone palette base (900 − 4·nb; 0 when not skinned) → **q76.z**,
`+0x128` nb (bones used, ≤ 36), `+0x12C` bone list (resource bone index per
palette slot), `+0x130` group list (Links).

Group node (0x14 B; `ModelMeshGroupFindOrAddMaybe_15c680`, sorted by priority):
`{next, items, priority (= material index + 10, +10000 for multi-pass), material index, REF-list address}`.

###### Skin conversion at load (`BuildFromRes`, **[dis]**)
Per position vertex i, for k < min(n, 4): skip index < 0 or weight 0;
`w = (fmt==8 ? (u8 << 15)/100 : u16) / 128`, capped at 255. Retail quirk:
`i == 75 && k == 0` gets `w++` (it really is in the disassembly at `0x15c4a0`).
The bone gets a palette slot x (the first 36 distinct bones; the 37th and
later are dropped). Stored as `s8 idx = −2x−2`, `u8 w`. Then `w[0] += 255 − Σw`.
Table entry is 12 B `{s8 idx[4], u8 w[4], s32 count}` at mesh+0xD0.

###### Chunk packet (`ModelMeshBuildUnpackPktMaybe_15b2a0`, **[dis]**)
For each group, for each item (unless it shares the first item's packets):
max vertices per chunk `r = ModelResCalcPrimCountMaybe_15c780(mesh, primId, &overlap)`:
`v = (fC − 4) / (nb > 0 ? 6 : 4)` (quads per vertex: 4 streams, plus 2 skin quads);
list: `3·(v/3) − 3`; strip: `2·(v/2) − 4`, overlap 2 (each later chunk
repeats the previous 2 indices). Outline ids 0x11/0x13 use `/6` and round to 2
or 4. For unskinned meshes (fC = 190) that is **42 vertices**. The chunk count is `ceil(n / r)`.

Each chunk, written at `p` (all addresses TOP-relative, FLG bit 0x8000; n = len, m = n+1):
```
STCYCL 1,1
UNPACK V4-32 ×2 → TOP+0
  TOP+0 = { primId, n, n | 0x8000 (NLOOP|EOP for the vertex tag), m }      // m = normals' offset from TOP+2
  TOP+1 = { 2m (UVs), 3m (colors), 4m (skin), (bones−1)·2 }                // .w written only when skinned (0/2/6); otherwise whatever the buffer held
UNPACK V3-{8,16,32} signed   n → TOP+2          positions, de-indexed through the position index list
UNPACK V3-{8,16,32} signed   n → TOP+2+m        normals   (only if the item has a normal index list and the mesh has normals)
UNPACK V2-{8,16,32} unsigned n → TOP+2+2m       UVs       (only if UV index list)
UNPACK V4-8        unsigned  n → TOP+2+3m       colors RGBA u8 (only if color index list; 8-bit only)
[skinned: MeshPktUnpackBoneRefsMaybe_164ec0, de-indexed through the POSITION index list]
  STCYCL CL=2,WL=1
  UNPACK S-8 / V2-8 / V4-8 signed   n → TOP+2+4m      (every other quad) palette indices (−2x−2), 1 / 2 / 4 bones (the chunk's max)
  UNPACK same, unsigned             n → TOP+2+4m+1    weights 0..255
MSCNT                                                       0x17000000
pad to a quad with NOPs
```
`item.xC = chunk start & 0x0FFFFFFF`, `item.x10 = qwc` of all of that item's chunks
together. After a group's items: one **REF** quad per item
`{0x30000000 | qwc, addr, 0, 0}`, then `{0x60000000 (RET), 0, 0x11000000 (FLUSH), 0}`.
`group.x10` = the address of this REF list, which is the CALL target.

Note: one REF covers **all** of an item's chunks (they are contiguous). The VIF
sees one REF per (group, item) and inside it n_chunks × (UNPACKs + MSCNT).

###### Per-frame chain for one model draw (`ModelPrepareDrawMaybe_14f760` + `ModelEmitDrawPacketsMaybe_14ec20`)
For each frame item that uses a mesh (frustum-culled on the CPU with the mesh's bounds box):
1. `CALL p14`: `VifPktUploadMatrix_148440` RET packet `UNPACK V4-32 ×4 → quads 60-63` = the frame's world matrix (obj+0xA0), then **MSCAL 0x000** (the base program).
2. `CALL p18`: `ModelMesh(Mode1)WriteVuHeader` RET packet (9 quads): VIF `BASE = +0x00`, `OFFSET = +0x04`; `UNPACK → 76..79` = `{+0x08, +0x0C, +0x124, 0}`, `{pos scale, w = posfmt != 32}`, normal scale, UV scale; `UNPACK → 81` = `{mode (bit0 → flags 8, 1/3 clip modes, |0x40 point-lit), 0, 0, alpha scale = ctx scale × obj+0x2E/16384}`; `UNPACK → 42` = color (the object's, or the environment's).
3. `CALL p1C`: `ModelBuildSkinMtxPacketMaybe_150390` RET packet: FLUSH, `UNPACK V4-32 ×4nb → 900−4nb` (absolute address), the bone matrices in **mesh object space** `inv(meshFrameWorld) · boneWorld · inv(boneBind) · meshFrameBind`, slot nb−1 first, so slot x lands at 896 − 4x. An empty RET when not skinned.
4. `cur += mesh->GetGroupPkts(cur, matIdx[], pkt[], n)`. Links writes nothing. A MODEL_RM mesh rebuilds all its chunks into `cur` here.
5. For each group i: `DrawCtxAddChangedCallTags` (VU1 program pieces from `D_00491280[ctx+0x3C40]` by material type, LOD = flags&3, variants; only emitted when they change); `CALL texenv(material)`; `CNT{MSCAL 0x320}`; `CALL pkt[i]` (the group's REF list → static chunk packets).

The same `GetGroupPkts` / `MSCAL` / `CALL` pattern is used by
`ModelQueueSortedPackets_14f0d0`, `ModelEmitShadowCasterPacketsMaybe_14e780`
and `EffModelDrawNodes_1a7b80`. The last one also writes **quad 88** (UV
scroll) with `MeshPktWriteOffsetMaybe_14ab30` and quad 82 with
`MeshPktWriteColourMaybe_14ab70`, per draw.

###### Where the VU1 quads come from (per chunk)
| quads | source | how often it changes |
|---|---|---|
| TOP+0 .. TOP+2+4m+2n | chunk packet | static (Links); per frame for MODEL_RM |
| 60-63 | p14 (world matrix) | per draw |
| 76-79 | p18 (from the mesh object) | constant per mesh |
| 81, 42 | p18 (from the draw `info`) | per draw |
| 82-84, 86-87 | material packet (82 may be overridden by texenv or EffModel) | constant per material (82 sometimes per draw) |
| 88 | EffModel only | per draw |
| 900−4nb..899 | p1C | per draw |
| 4-11, 24-27, lights … | camera / scene packets, base program | per frame |

Correction to `docs/VU1-NATIVE.md`: quad 76.z is the bone palette base
(mesh+0x124 = 900 − 4·nb), not "another branch of A (never set in the field)".
It is nonzero exactly for skinned meshes. That is what `bonesOf()` in
`src/vu1_native.cpp` already tests.

##### 3. CPU vs VU1 work

* **CPU, at load:** relocation; de-indexing (each stream through its own u16
  index list) into per-chunk UNPACK streams; chunk splitting with strip
  overlap; skin conversion (weights → u8 summing to 255, ≤4 influences, a
  palette of ≤36 bones, index → −2x−2); material packets; group REF lists.
* **CPU, per frame per draw:** world matrix; CPU culling with the bounds box
  (`BoxTransformExtentsVu0Maybe`, the clip modes in `info.mode`); point-light
  overlap; the VU header; bone palette matrices; texenv (TEX0, GS env); the
  call chain. **No per-vertex work** except for MODEL_RM meshes, where
  `BuildUnpackPkt` runs again (de-index and copy) and cloth simulates and
  quantizes positions (`VertsPackQuantized_164bb0`: `v · 32768/scale`, VU0 ftoi0).
* **VU1 (the native pieces):** dequantize (`/32768 · q77-79`), skinning
  (Σ w/255 · M[900 + 2·idx]), F (UV scroll / sphere / wave / reflection …),
  C (colors and lights), D/E (transform, guard band / backface, fog, ST·Q), GIF output.

##### 4. Proposed identity keys

**A. Chunk level (no game hook needed; the VIF1 thread can see it):**
`key = (REF tag addr & 0x0FFFFFFF, REF tag qwc)` taken from the REF quad inside the
CALLed group list. That memory is in the model's MemStack and is not
rewritten while the model is loaded, so the write watch can vouch for
`[addr, addr + 16·qwc)`. Add the watch's stamp (or a load generation) to
catch reuse after the area's heap is reset. The decoded geometry also
depends on quads 77-79 (constant per mesh, from p18). Hash them into the key,
or key at the mesh level. Multi-pass materials REF the same chunk bytes from
two groups: share the vertex buffer and key the *draw* as (chunk key, material).
**[guess]** That sharing may explain the "4% repeated inside the frame" in Issue 2.

**B. Mesh level (for drawing from game-level hooks):**
* Load hook: the return of `ModelMeshLinksBuildFromResMaybe_15af00` (`a0` = the mesh object,
  `a1` = `Info`, so `Info.f0` = the resource node). At that point the
  resource arrays and index lists are readable (before the de-indexing, if the
  hook goes in at its entry), and the group list at +0x130 gives
  `{material index, REF list}`. Key = **ModelMesh object address + load
  generation**. For reuse across area reloads, key by a hash of the resource
  node's streams and index lists. **[guess]** That the resource stays resident after
  the build is unverified: Links keeps no pointer into it, so do not rely on it after the hook.
* Draw hook: `ModelEmitDrawPacketsMaybe_14ec20` (or right after `ModelPrepareDrawMaybe_14f760`).
  For each item: mesh = `set->models[item.mdl]`, world matrix = p14+0x10..0x4F
  (obj+0xA0), info (mode / alpha / color) = p18 quads 81 and 42, bones = p1C. For each
  group: material = `tbl->meshes[matIdx]` (MeshMaterial: packet, texture
  node → TEX0 +0x38 / TEX1 +0x40 / CLAMP +0x48, GsEnv +0x14). A draw is then
  `{mesh id, group (material index), world matrix, bone palette, info, material state}`.
* MODEL_RM (class bit 1, ModelMesh vptr 0x470040): not static. Treat as
  dynamic, keyed by the mesh object, with content re-sent per frame. A dirty
  signal could come from `ClothWriteMeshVerts_1850f0` / `VertsPackQuantized_164bb0`.
* Skinned meshes: the same static keys. The per-draw data adds the bone palette,
  so vertex-shader skinning needs no per-frame vertex upload.

##### Open / not checked
* What node+0x13 means, and VB+0x50 / +0x60.
* Which of VB+0x30 and VB+0x40 is min and which is max.
* The Mode1 meshes' real data path.
* The `D_00491280` program table layout (which pieces each material type and variant selects).
* Whether `ModelResGetPosOffset` is ever non-zero.
* The decompiled C of `MeshMaterialBuildPkt`, `ModelPrepareDraw` and `ModelEmitDrawPackets` was not checked against the disassembly line by line. The chunk packet (`15b2a0`) and the skin loop (`15b7c0` tail) were.


### Model draws (M5)

Read from RGDecomp C, then checked against the disassembly where it mattered
(the skin palette, the item layout, the VU1 address scan, the Mode1 header,
and the group-packets dispatch). "(guess)" marks anything not read directly
from code. Addresses are SCUS_974.90.

Terms used below:
- **inst**: the model instance (the first argument of every function here). In
  RGDecomp it is called `Ctx`, `O150390` or `T001502b0`.
- **dctx**: the draw context, `GetDrawCtx_15de80()`. In RGDecomp it is called
  `Scene`, `Mgr` or `Ctx`.
- **req**: the draw request for one model part. In RGDecomp it is called
  `Req` or `Ent`.
- **frame**: one node of the instance's hierarchy. It is also a bone.
- **mesh**: a `ModelMesh*` object. Emit calls it `Model`; Prepare calls it
  `Mdl`.
- **material**: a `MeshMaterial`. Emit calls it `Mesh`.

Matrix convention, from `MtxMul_13d240` and `MtxMulChain4_13cff0` (VU0 asm):
- A matrix is stored as 4 row quads, and points are row vectors:
  `v' = v.x*r0 + v.y*r1 + v.z*r2 + v.w*r3`. The translation is in r3.
- `MtxMul(out, A, B)` = B*A (row-vector), so B is applied first.
- `MtxMulChain4(out, A, B, C, D)` = D*C*B*A, so D is applied first.
- `MtxMulAffine_13d1d0(out, A, B)` = B*A, using only xyz of rows 0-2 of B.
- The producer's `whole = model*view*(4-7)` (src/native_producer.cpp) is
  the same convention.

##### 1. Call sequence for one model draw

Entry points. All are (inst a0, dctx a1, req a2), and all run
`ModelUpdateFrameWorlds_1502b0` → `ModelMeshUpdateBoundsMaybe_150170` →
`ModelPrepareDrawMaybe_14f760` → an emitter:

| Entry | Emitter |
|---|---|
| `ModelDraw_14f6d0` (Character2Draw* etc.) | `ModelEmitDrawPacketsMaybe_14ec20` |
| `ModelDrawSorted_14f650` | `ModelQueueSortedPackets_14f0d0` (same records, put in the sort list via `DrawListAddNode_140a10`; also calls `DrawListEnsureViewPacket_148240` and `LightStateGetPacket_1428f0` and stores those addresses in each node) |
| `ModelDrawShadowCaster_14f5d0` | `ModelEmitShadowCasterPacketsMaybe_14e780` (same loop, one fixed material and texenv for every group) |
| `ModelObjExDraw_1ad7a0`, `ModelObjDrawShadowModel_1ad690`, `ModelDrawOffscreenCompositeMaybe_16aac0` | Call Prepare and Emit directly |

Other emitters exist outside this path: the effect models
(`EffModelDrawNodes_1a7b80`, `EffQuadDrawNodes_1a7890`) build their own
packets with `VifPktUploadMatrix`, `VuProgSelResolve` and
`DmaPktAddMscalCall`. They do not use Prepare or Emit.

###### Per-frame setup (once per view, not per draw)

- **`DrawListAddViewPacket_1482e0`** → `CameraWriteViewPacket_148740`. It
  takes `cam = *(dctx+0xF20)` (written `s` below) and makes one DMA `ret`
  packet of 21 qw: UNPACK V4-32 of 20 quads to VU1 quads 0-19. It stores the
  packet's address in `dctx+0x36B0`; `DrawListEnsureViewPacket` reuses it.

  | VU1 quad | From |
  |---|---|
  | 0 | GIF tag {1, 0x20000000, 0xE0, 0} |
  | 1 | Depth: y = s+0x18; z, w from s+0x18..0x24 |
  | 2 | {0, 0, s+0x20, 1/(s+0x24)} |
  | 3 | {4095, 4095, s+0x24, 1/(s+0x20)} |
  | 4-7 | `MtxMul(s+0x130, s+0xF0)` = (s+0xF0)*(s+0x130): view → GS window |
  | 8-11 | s+0xF0: projection, view → clip |
  | 12-15 | s+0x130: clip → window |
  | 16 | s+0x240 (viewport and guard-band scale) |
  | 17, 18 | s+0x4D0, s+0x4E0 (guard band) |
  | 19 | FOGCOL GIF tag {0x8000, 0x10000000, 0xE, 0} |

- **`DrawListAddCameraMatrixPacket_1481b0`** → `DrawPktBuildCameraMatrix_1486c0`.
  A `ret` packet of 6 qw, UNPACK of 5 quads to quad 24 (VIF `0x6C050018`):
  - quads 24-27 = `cam+0xB0` (world → view, 4 rows);
  - quad 28 = `cam+0x170` (the eye position).
- **`DrawListAddLightPacket_148120`** → `LightStateWritePacket_1484a0`.
  `env = *(dctx+0x36B4)`; UNPACK to quad 30 (`0x6C..001E`):

  | VU1 quad | From |
  |---|---|
  | 30-32 | Transpose of env+0x20 (directional lights) |
  | 33-36 | env+0x60 (light colors) |
  | 37 | env+0x10 (ambient) |
  | 38 | env+0xA0 |
  | 39 | env+0xB0 (color clamp) |
  | 40 | Fog coefficients |
  | 41 | FOGCOL |
  | 42 | env+0xC0 (default object color) |
  | 43 | env+0xD0 |
  | 44-51 | 4 point lights at env+0xE0 + i*0x30, one {position, color} pair each; w = 1/radius, zeroed when radius ≤ 0 |

###### Prepare (`ModelPrepareDrawMaybe_14f760`): per item, into this frame's buffer

Prepare walks `req->list` (+0x44), skipping items whose key does not match
`req->key` (+0x40). For each remaining item:

1. Get the frame: `frame = inst+0x104 → p[item.frameIdx]`. If `frame+0x25 == 0`
   (hidden), set p14 = p18 = 0 and skip it.
2. Get the mesh: `mesh = req+0xC → p[item.meshIdx]`.
3. Set the variant flags, as in `ModelMeshGetVuVariantFlagsMaybe_14e430`:
   - bit 1: skinned (`mesh+0x128 > 0 && mesh+0x12C`);
   - bit 2: position format `mesh+0x64 != 0x20`.
4. Compute `m = frame+0xA0` (the frame's world matrix) and
   `wm = m * cam+0xB0` (model → view).
5. Cull and pick the clip mode, testing the mesh box by center and extents
   (`BoxToCenterExtentsVu0_14ff40`, then `BoxTransformExtentsVu0_14fed0`) on
   the camera's plane sets:
   - `cam+0x3D0` and `cam+0x410` (frustum and depth) decide whether the item
     is visible. Not visible: p14 = p18 = 0, skip. Visible: `item.depth = b.x`
     from the 0x410 test.
   - `info.mode`: start at 3 if `cam+0x10` or `frame+0x26 & 1`, else 1. If
     the box is wholly inside the guard band (the `cam+0x490` test passes,
     then all of `cam+0x450`), mode = 0.
6. Point lights: if `cam+0xC` (lights on), each point light whose radius is
   above 0 is tested. Its position goes through `inverse(m)` and is tested
   against the mesh box grown by 1.25 × radius. A hit sets flag 4 and
   `mode |= 0x40`.
7. Set the remaining flags: 8 if `mode & 1`; 0x10 if `frame+0x26 & 8` (sort
   by depth).
8. Write three packets, one after another, and keep their addresses in the
   item:
   - **p14 = `VifPktUploadMatrix_148440`** (6 qw). A `ret` tag carrying
     STCYCL and UNPACK V4-32, 4 quads to **quads 60-63** (`0x6C04003C`) = `m`
     (the frame's world matrix, row-vector). Then one qw holding
     **MSCAL 0x000**, which runs the base piece (`runBase`). Base writes:
     - quads 64-66 (the model rows, normalized);
     - quads 68-71 = model*view*(4-7);
     - quads 72-75 = model*view*(8-11);
     - quads 100-102 (lights in object space);
     - quad 111.z (the sign of the determinant).

     It also kicks the FOGCOL packet at quad 19.
   - **p18 = `mesh->vtbl[+0x14]` (WriteVuHeader)**, 9 qw.
     `ModelMeshWriteVuHeaderMaybe_15b1d0`; Mode1 is `_1646a0`. The VIF codes
     and VU1 quads it writes:

     | Writes | From |
     |---|---|
     | VIF BASE | mesh+0x00 |
     | VIF OFFSET | mesh+0x04 |
     | quad 76 | {mesh+0x08, mesh+0x0C, mesh+0x124 (the palette's VU address, 0 when not skinned), 0} |
     | quad 77 | mesh+0x70 (position scale), w = (mesh+0x64 != 0x20) |
     | quad 78 | mesh+0x90 (normal scale; Mode1 uses +0x80) |
     | quad 79 | mesh+0xA0 (UV scale; Mode1 uses +0x90) |
     | quad 81 | {info.mode, 0, 0, info.scale} |
     | quad 42 | info.col |

     The values behind quads 81 and 42:
     - `info.scale = inst+0x170 * frame+0x2E / 16384`;
     - `info.col` = the quad at `*(frame+0x20) + 0x10` when that pointer is
       set and the word it points to is non-zero, else `env+0xC0`.
   - **p1C = `ModelBuildSkinMtxPacketMaybe_150390`**: see section 2.
9. If `req+0x10` (bounds wanted), set `req+0x20`/`req+0x30` from
   `AabbProjectScreenBounds_13c4f0(cam+0x2D0, m, box)` over every item. These
   are the EE screen bounds the composite uses.
10. After the loop, clear the material texenv cache (`req+0x8 → +8[0..cnt]`).

The item node, at `node+0x10`:

| Offset | Field |
|---|---|
| +0 | key |
| +4 | frameIdx |
| +8 | meshIdx |
| +0xC | flags |
| +0x10 | depth (f32) |
| +0x14 | p14 |
| +0x18 | p18 |
| +0x1C | p1C |

`node+0` is the next pointer. Verified in the disassembly at 0x14edbc.

###### Emit (`ModelEmitDrawPacketsMaybe_14ec20`): per item, then per group

For each item with a matching key and p14 and p18 not zero:

1. Add `dctx+0x3D10..0x3D20` stats (`mesh+0x40..0x4C`).
2. If flag 0x10: `DrawListDepthToSortKey(item.depth)`.
3. Add call tags to **p14, p18, p1C**, in that order.
4. Call `n = mesh->vtbl[+0x18](cur, B[], A[], tbl->cnt)` (GetGroupPkts).
   B[i] is the material index (ending at -1) and A[i] the group's DMA chain.
5. For each group i with `A[i] != 0`:
   - `t = req+0x8 → materials[B[i]]` (a MeshMaterial).
   - Texenv: get it, or build it once per frame with
     `DrawListWriteTexEnvPacket_148370(dctx, cur, t)`:
     - a GS env packet (FRAME and ZBUF from `t+0x14`);
     - `TexEnvBuildDmaChainMaybe_14abb0`: a `ref` to the prebuilt material
       packet at `t+0`/`t+4`
       (`MeshMaterialBuildPkt_14ac90`: quads 82-84, 86-88, plus GS texture
       registers). Optionally it also writes quad 82 from `t+0x20` (if
       `t+6`) and a TEX0 refresh from `t+0xC`.
   - The microcode, by `DrawCtxAddChangedCallTags_13fd70`, which caches the
     last call per slot at `dctx+0x3C48..0x3C5C`.
     `row = 0x491280 + dctx+0x3C40 * 0xC0 + t+0xA * 16`.
     The slots are A, F, E (`row+0`, `+4`, `+8`) and:
     - B = `(row+0x300)[flags & 3]`;
     - C = `(row+0x304)[t+0x18 + (flags & 4 ? 2 : 0)]`;
     - D = `(row+0x308)[(t+0x1A + (flags & 8 ? 2 : 0)) | (dctx+0x3CCC ? 4 : 0)]`.
   - A call tag to the texenv chain.
   - `DmaPktAddMscalCall_145b20(A[i])`: the qw at 0x47DE60 is a `cnt` tag of
     qwc 0 carrying VIF **MSCAL 0x064** (slot A, 0x320), followed by a call
     tag to `A[i]`.

**The geometry reference (A[i])** depends on the mesh class. The vtable
pointer is at `mesh+0x50`.

| Vtable | Class | A[i] (groups at) | Per draw |
|---|---|---|---|
| 0x470010 | `ModelMeshLinks` | +0x130 list: {next, , , +0xC material, +0x10 chain} | Nothing built. The chain was built once at load (`ModelMeshLinksBuildFromResMaybe_15af00` calls BuildUnpackPkt into the model heap). **Stable memory, so the right identity.** |
| 0x470040 | `ModelMesh` | +0xF0 groups: {next, ents, , +0xC material, +0x10 chain} | **Rebuilds every chunk into this frame's buffer** by `ModelMeshBuildUnpackPktMaybe_15b2a0`. It copies vertices through index lists (`MeshPktUnpackV2/V3/V4Indexed`, bone refs). Each chunk: STCYCL; UNPACK of a 2-qw header to TOP (0x8000); the sections; MSCNT (0x17000000). Each group's chain is a list of `ref` tags (0x30000000) to its chunks, ending `ret` + FLUSH. This is where the per-frame vertex copy comes from. |
| 0x4701c0 | `ModelMeshMode1` | One group: material at +0xA8; chain at +0xC0 = the 2-qw header just built (`cnt` qwc 0 with STCYCL, then `ret` with FLUSH) | Nothing to draw. (guess: its geometry comes from elsewhere, or it is a placeholder.) |

Vtable slots (vptr at +0x50):

| Slot | Method |
|---|---|
| +0x8 | type id |
| +0xC | IsKindOf (`v0C(2)` = "has skin and format data") |
| +0x10 | GetPkt |
| +0x14 | WriteVuHeader |
| +0x18 | GetGroupPkts |
| +0x1C | Init |
| +0x20 | GetPkt or BuildDmaHeader (Mode1) |
| +0x24 | BuildDefault |
| +0x28 | BuildUnpackPkt |

Which class the field uses most is not counted (guess: Links for characters
and props). Count it by vptr in a hook before deciding the mesh id.

Order of VU1 state for one item:
1. Camera, view and lights (earlier in the list).
2. p14: model matrix, then MSCAL 0 (base).
3. p18: header.
4. p1C: palette.
5. Per group: microcode, then texenv and material (82-88), then MSCAL A, then
   the chunks (MSCNT each).

##### 2. Where the inputs live in EE memory at the call

Reading at Emit entry (a0 inst, a1 dctx, a2 req):

- **Model matrix**: `frame = *(*(inst+0x104)+4)[item.frameIdx]`; the matrix
  is at `frame+0xA0`, 64 bytes, row-vector. It is the same as `p14+0x10` (the
  uploaded copy). It may hold scale or a mirror; base normalizes the rows and
  keeps the determinant sign in quad 111.z for the winding. The frame worlds
  are fresh at this point (`ModelUpdateFrameWorlds_1502b0` ran first; billboard
  frames are faced to the eye there).
- **Camera**: `cam = *(dctx+0xF20)`.

  | Offset | Holds |
  |---|---|
  | +0xC | lights on |
  | +0x10 | force clipping |
  | +0x18..0x24 | depth range |
  | +0xB0 | view (64 bytes) |
  | +0xF0 | projection to clip |
  | +0x130 | clip → window |
  | +0x170 | eye |
  | +0x240, +0x4D0, +0x4E0 | quads 16-18 |
  | +0x2D0 | world → screen, for the EE box |
  | +0x3D0, +0x410, +0x450, +0x490 | plane sets for culling |

  Caveat (guess): VU1 holds the camera from the last
  `DrawListAddCameraMatrixPacket` / `ViewPacket`. These are called at view
  pushes and pops (`ViewStackPop_1443e0`, `ViewReapplyCameraRec_142e90`,
  menus, `CompassHudRender`, `DrawModelWithCamera_28bf80`). If a caller changes
  `*(dctx+0xF20)` without re-adding the packets, Emit-time `cam` differs from
  VU1's. The safest source is the packet itself: the view packet at
  `*(dctx+0x36B0)` (+0x10 = quad 0), and the camera-matrix packet (hook
  `DrawPktBuildCameraMatrix_1486c0` and keep its output).
- **Skin palette**: `p1C` (`ModelBuildSkinMtxPacketMaybe_150390`, checked in
  the disassembly):
  - qw0 is a `ret` tag with qwc n-1. If n == 1, the item is not skinned.
  - qw1 is VIF FLUSH and `UNPACK V4-32 addr = mesh+0x124, num = 4 *
    mesh+0x128`.
  - Then `nb = mesh+0x128` matrices, 4 qw each, **in reverse order**: the
    first is bone nb-1. Bone x lands at VU1 quad `900 - 4 - 4x`, since
    `mesh+0x124 = 900 - 4*nb`. A vertex's bone ref is stored as `-2x-2` and
    VU reads `900 + 2*ref` (`ModelMeshBuildFromResMaybe_15b7c0`).
  - At most 36 bones a mesh; up to 4 weights a vertex, /255.
  - Bone matrix for list entry e (`frame index = mesh+0x12C[e]`):
    `inv(frame.world) <- boneFrame.world <- inv(bind[boneIdx]) <- bind[frameIdx]`.
    In row-vector order:
    `v * bind[frameIdx] * inv(bind[boneIdx]) * world(boneFrame) * inv(world(frame))`.
    - `bind = *(inst+0x108)`, 64 bytes per frame index.
    - A missing bone frame gives identity.

  So the palette is in the mesh frame's space, and VU then applies quads
  60-63 (that frame's world). To build it without the packet, read
  `inst+0x104`, `inst+0x108`, `mesh+0x128/0x12C` and the frame's +0xA0.
- **Mesh box**: `mesh+0x20` and `mesh+0x30` (vec4 each). The **decomp's names
  are swapped**: +0x20 is the max corner and +0x30 the min. Two places show it:
  - The point-light test reads `max[i] - r <= p <= min[i] + r`.
  - `ModelMeshUpdateBoundsMaybe` adds the pad `mesh+0x120` to +0x20 and
    subtracts it from +0x30.

  For skinned meshes the box is recomputed each frame from the bone frames
  (`ModelCalcNodesBoundsMaybe_1531c0`).
- **Material**: `req+0x8` = {cnt, MeshMaterial*[] (+4), texenv cache[] (+8)}.

  | MeshMaterial offset | Field |
  |---|---|
  | +0 / +4 | Prebuilt VU and GS packet: address / qwc |
  | +6 | Color-override flag |
  | +0xA | VU program row |
  | +0xC | Texture node |
  | +0x14 | GS env object |
  | +0x18 / +0x1A | C / D group index |
  | +0x20 | Override color |
- **Per-object constants** for the record: `info.mode`, `info.scale` and
  `info.col` (above). It is simplest to read them back from `p18`: quad 81 at
  `p18+0x6C..0x7B` and quad 42 at `p18+0x80`.

##### 3. VU1 memory results the game reads back

**No EE code reads VU1 data memory.** I scanned the whole text:
- every `lui 0x1100`/`0x1101`: 5 hits, all the VIF FLUSH constant
  0x11000000 stored into packets;
- the data for the words 0x1100C000 and 0x1100C620: no hits.

**The reader of quads 98/99 is a VU1 program**: the overlay B
`6e42dac6`, run by MSCAL 0x12C (0x960). The chain:

1. `ModelDrawOffscreenCompositeMaybe_16aac0` runs `ModelPrepareDraw` with
   `req+0x10 = 1`, so the EE box lands in `req+0x20/0x30`. The box is unioned
   over the parts, clamped to the viewport, and padded by 8 px (0x80
   sixteenths). The composite then clears that rectangle offscreen.
2. `DrawListResetClipRangeMaybe_148040` uploads quad 98 = (+2^24 ×4) and
   quad 99 = (−2^24 ×4) (VIF `0x6C020062`) and sets `dctx+0x3CCC = 1`.
3. The composite calls Emit for each part. With `dctx+0x3CCC` set, the D index
   gets `| 4`, which selects the depth-and-bounds D pieces (`b8ea8ed5`,
   `634107b0`, `93cfd2f5` in src/vu1_native.cpp). Each widens 98/99 by every
   vertex's (x, y, z, fog) after the divide (`widenBounds`).
4. `DrawCtxClearClipRangeSetMaybe_148030` writes `dctx+0x3CCC = 0`.
5. `ScreenTexOverlayDrawMulti_16a510` and `ScreenTexOverlayDrawSingle_16a780`
   → `ScreenTexOverlayWriteSpriteMaybe_16a890` upload quads 140-144 (the EE
   rectangle at 140/141, offsets 142/143, GIF tag 144) and MSCAL 0x12C. The
   overlay draws 32-px sprite columns over
   `max(140, 98 − 1) .. min(141, 99 + 1)`.
   - The z and w lanes of 140-143 are never written by the EE; they hold
     whatever was in the packet buffer. The sprite z is
     `max(140.z, 98.z)`, so it is unreliable anyway.

The composite is the only caller of the reset and the overlay writers, so
98/99 matter only inside a composite tail.

**Today's native path already ignores them.** `native_effects.cpp`
`overlayCall` builds the ObjectLayer passes from the EE rectangle (a1),
`vif1_thread.cpp` `finishTail` withholds the tail's kicks, and
`applyMemoryEffects` keeps 98/99 right only for the packet path. So:
- if the overlay stays a record (M5 step 6), nothing needs 98/99;
- if the overlay VU1 program still runs, reproduce them from the box:

  1. Start from ±2^24 at the reset.
  2. For each mesh drawn while `dctx+0x3CCC` is set (the D index has bit 2),
     take the 8 corners of `mesh+0x30` (min) .. `mesh+0x20` (max).
  3. Transform each corner by quads 68-71, which are
     `frame.world * cam+0xB0 * (cam+0xF0 * cam+0x130)`, row-vector.
  4. q = 1/w; take x*q, y*q, `min(z*q * 87.z, 1.y)` and
     `clamp(40.x + 40.w*w, ≤ 40.y, ≥ 40.z)`.
  5. Take the min and max lane by lane into 98/99.

  The result contains the per-vertex box only when every corner has w > 0
  (guess: true for a visible composite character). Otherwise clamp to 140/141.
  An oversized box should not show: the area beyond the vertices was cleared
  to alpha 0, and the overlay alpha-tests (GREATER 0, or GEQUAL 1/0x81)
  (guess, from the composite's GS state; check by play).

Other VU1 memory effects (base: 64-75, 100-102, 111; outline: quad 1023) are
read only by later VU1 pieces of the same draw. Nothing on the EE reads them.

##### 4. What a hook needs to build the record

**Hook point**: `ModelEmitDrawPacketsMaybe_14ec20`. Hook
`ModelQueueSortedPackets_14f0d0` and `ModelEmitShadowCasterPacketsMaybe_14e780`
too, or these draws are missed. Prepare has run at all three, so everything
below is final.

src/native_effects.cpp already wraps Emit for composites (`modelEmitDrawPackets`,
`afterEmit`). The new hook has to share that wrapper.

For each item node in `req+0x44` (stop on `next == 0`):
- skip if `req+0x40 != 0 && node+0x10 != req+0x40`;
- skip if `node+0x24 == 0 || node+0x28 == 0`;
- skip if the mesh pointer is 0.

| Field | Read |
|---|---|
| frame | `*(*(a0+0x104)+4)[node+0x14]` |
| model | `frame+0xA0` (64 bytes), or `node+0x24 → +0x10` |
| previous model | Not in the game. Keep it in the hook keyed by (frame address, mesh address) or (a0, frameIdx, meshIdx); reset when the address is reused (guess) |
| camera | View packet `*(a1+0x36B0)` +0x10 → quads 0-19; camera-matrix packet (hook `DrawPktBuildCameraMatrix_1486c0`) → quads 24-28; or `*(a1+0xF20)` +0xB0/+0xF0/+0x130/+0x170 (see the caveat in section 2) |
| lights | `*(a1+0x36B4)` (env) as `LightStateWritePacket` reads it, or hook `DrawListAddLightPacket_148120` |
| mesh | `*(*(a2+0xC)+4)[node+0x18]`; class by `*(mesh+0x50)` (vtable 0x470010, 0x470040 or 0x4701c0) |
| mesh id | Links: the group chain address (`+0x130` list `+0x10`) and the model heap's write-watch stamp. ModelMesh: its resource arrays (`+0xB4`/`+0xBC`/`+0xC4`/`+0xCC`/`+0xD0` sources, `+0xF0` groups' entries) with a stamp (guess) |
| groups | Links: walk `mesh+0x130` → {material idx +0xC, chain +0x10}. ModelMesh: walk `+0xF0` likewise. Do not call GetGroupPkts yourself: for ModelMesh it writes into the frame buffer |
| material | `*(*(a2+0x8)+4)[idx]` (MeshMaterial); its prebuilt packet `t+0`/`t+4` holds 82-88; program row `t+0xA`, C and D indices `t+0x18`/`t+0x1A` |
| program variant | B index `node+0x1C & 3`; C `+2` if `& 4`; D `+2` if `& 8`, `| 4` if `a1+0x3CCC`; set `a1+0x3C40`; rows at 0x491280 |
| per-object constants | quad 76 at `node+0x28 → +0x20`; 77-79 at +0x30..+0x5F; 81 at +0x6C..+0x7B (mode, scale); 42 at +0x80 (color) |
| skin palette | `node+0x2C`: qwc = `(*p & 0xFFFF)`; if > 0, `nb` = NUM/4 of the UNPACK at +0x1C, matrices from +0x20, in reverse bone order. Bone x is `pal[nb-1-x]` |
| depth sort | `node+0x20` (f32), used when flags & 0x10 |
| bounds needed | `a1+0x3CCC != 0` (inside a composite); see section 3 |

To make the chain carry no geometry:
- Links meshes: drop the call to `A[i]` (or replace the chain with the record),
  but keep p14 and p18. p14 runs base, and base's FOGCOL kick and memory are
  what the remaining VU1 pieces (outlines, shadows) read.
- ModelMesh meshes: BuildUnpackPkt's per-frame copy is the cost. Skipping it
  needs a hook on `ModelMeshBuildUnpackPktMaybe_15b2a0` (or GetGroupPkts
  0x15b150) that returns a stub chain. (guess: the chain only feeds VU1, so
  this is safe, but the frame buffer's `MemStackAllocQw` size comes from
  Emit's `cur` and must stay consistent.)


### Lighting, UV generation and fog (M5)

Sources read: VU1 microcode disassembled from the ELF (`tools/vu1/piece.py` + `vudis.py --all`) for
C 780d503e, 70362be4, 91f542d0, 2460e528, e3bca52a; F d3f6f631, 150031ee, 0594a063, 9df36d6a (head);
D a786efab (fog/colour part); the native ports in `src/vu1_native.cpp` (Lights, PointLights, SphereMap,
Reflection, Wave, Ramp, Beam, Swell, readChunk) and `gsNativeMeshPoint` in
`thirdparty/PS2Recomp/ps2xRuntime/include/runtime/gs/gs_types.h`; RGDecomp C for the EE functions named below;
the piece tables read out of the ELF at 0x449DF0 (C group), 0x449E30 (D group), 0x47DEC0/0x47DED0 (B) and
the F pointers of `VuMicrocodeTableInitMaybe_152830`.
**[GUESS]** marks anything inferred and not read.

Conventions: row vectors (`v' = v * M`, M's rows are quads q..q+3), as the VU code does (`MULAx ACC, row0, v.x` ...).
`n` = the vertex normal *as B leaves it*; `p` = the position as B (and the F that edits positions) leaves it.
"c15(k)" = the int16 vertex colour word / 32768 (ITOF15).

---------------------------------------------------------------------------------------------------------

##### 0. The vertex inputs (slot B, already ported)

- Position: `p = int16/32768 * quad77.xyz`, optionally skinned (bones at quads 900+2*index, weights/255).
- Normal: `n = int16/32768 * quad78.xyz` (normals section = TOP+2 + TOP.w), skinned with rows 0-2 only.
  **Never renormalized** anywhere downstream (not in B, C or F). Its length is whatever 78 makes it
  (**[GUESS]** 78 ≈ (1,1,1) so |n| ≈ 1; 77/78/79 come from the mesh: `ModelMeshWriteVuHeaderMaybe_15b1d0`
  writes 76-79 from mesh +0x08/+0x0C/+0x124, +0x70, +0x90, +0xA0).
- UV: `uv = int16/32768 * quad79.xy` (UV section = TOP+2 + TOP+1.x).
- Vertex colour: 4 int16 words in the colours section (TOP+2 + TOP+1.y). Read only by C.

Order in the strip program A (07151c25): B, **F, then C**, then D (E for clipping). So F's edits of positions
(wave, swell) are what C's point lights see; F reads normals before C, C never changes normals.

##### 1. Slot C: vertex colour / lighting

###### 1.1 How the variant is picked (EE side, read)

There are no runtime branches between lit/unlit inside a piece; the EE loads a different C piece.
`ModelEmitDrawPacketsMaybe_14ec20`: C index `k4 = mesh.f18 + (partFlags & 4 ? 2 : 0)`, with
`mesh.f18 = (matFlags & 1 ? 4 : 0) + (matFlags & 4 ? 1 : 0)` (`MeshMaterialBuildPkt_14ac90`; matFlags = desc+0x44,
which is also what goes to quad 87.y). Strip C group (ELF 0x449DF0):

| idx | bit0 lit (mat 0x4) | bit1 point (part 0x4) | bit2 vtx colour (mat 0x1) | piece | native `Color::` |
|---|---|---|---|---|---|
| 0 | | | | 780d503e | Material |
| 1 | x | | | 91f542d0 | Lit |
| 2 | | x | | 780d503e | Material |
| 3 | x | x | | 127ca0b4 | LitPoint (unverified) |
| 4 | | | x | 70362be4 | PerVertex |
| 5 | x | | x | 2460e528 | LitVertex |
| 6 | | x | x | 70362be4 | PerVertex |
| 7 | x | x | x | e3bca52a | LitPointVertex (unverified) |

- matFlags 0x200 clears 0x4 (`if (flags & 0x200) flags &= ~4`), so a 0x200 material is never lit.
- partFlags 0x4 (point-light variant) is set by `ModelPrepareDrawMaybe_14f760` when the camera's light mode
  (`cam+0x0C`, `CameraSetLightModeMaybe_15ccc0`) is on and any of the 4 point lights with range > 0, moved into
  model space by the inverse model matrix, lies within `1.25 * range` of the model's AABB.
- Flags read at run time inside the pieces (quad 87.y, = matFlags): **0x200** (unlit pieces: 43 instead of 42/83),
  **0x100** (pieces with vertex colours: additive instead of multiplicative). Nothing else in C.

###### 1.2 Unlit, one colour (780d503e) and vertex colours (70362be4) — read from microcode

```
mat   = quad82;  mat.w *= quad81.w
base  = (87.y & 0x200) ? mat * quad43                        // all 4 lanes
                       : mat * quad42 + vec4(quad83.xyz * 128, 0)   // MULA mat*42; MADDi (83.xyz,w=0)*128
780d503e: colour = base for every vertex
70362be4, 87.y & 0x100 == 0:  colour = base * (c15 * 256)        // xyzw; c15*256 = int16/128, so 0x80 = 1.0
70362be4, 87.y & 0x100 != 0:  colour.rgb = base.rgb + c15.rgb*256*128   // = base + the raw int16
                              colour.a   = base.a * (c15.a * 256)
```
Units: quad 82 is a [0,1] material colour (alpha ≤ 1; `MeshMaterialBuildPkt` clamps 82.w to [0,1] and scales
by 0.993 with matFlags 0x80); 42/43 are 0-255 "light" colours with 128 = 1.0 (LightState defaults 128,128,128,128);
so base ≈ 0-255 GS units. **83 is written as 0 by the material packet** (`MeshMaterialBuildPkt`: UNPACK 3 at 82 =
(desc+0x00, 0, desc+0x20)); a scan of the main ELF found no other UNPACK to 83, so the `+83*128` term is 0 in
practice (**[GUESS]** for overlays, not scanned).

###### 1.3 Lit, directional (91f542d0) — read from microcode

Inputs: quads 100-102 (directional lights in **object space**, written by the base program; lane i = light i),
33-36 light colours (xyz), 37 ambient (xyzw), 82 material, 81.w alpha scale.
```
mat.xyz = q82.xyz;  mat.w = q82.w * q81.w
C_i     = q(33+i).xyz * mat.xyz              (i = 0..3)
amb     = q37 * mat                           // xyzw; amb.w is the vertex alpha
d       = max(vec4(dot-lanes), 0)             // d[i] = q100[i]*n.x + q101[i]*n.y + q102[i]*n.z  (ACC order x, y, z)
rgb     = amb.rgb*1 + C_0*d[0] + C_1*d[1] + C_2*d[2] + C_3*d[3]   // ACC in this order
a       = amb.w = q37.w * q82.w * q81.w
```
- No normalization of n, no clamp in C (only `max(d,0)` per light). 42, 43, 83 and the 0x200/0x100 flags are
  **not read** by the lit pieces (the per-object tint in 42 does not reach lit meshes).
- Light direction convention: lit when `n·L > 0`, so L is the unit vector **toward** the light
  (`NodeObjDrawLit_1947f0` stores the normalized light-node direction, w = 0).
- Object-space lights (base program `runBase`, i = 0..2):
  `U_i = q(60+i).xyz / |q(60+i).xyz|` (stored in 64-66, w kept) and
  `q(100+i)[lane] = q30[lane]*U_i.x + q31[lane]*U_i.y + q32[lane]*U_i.z`.
  Equivalently `d[i] = dot(n * U, Lworld_i)`, where `n*U` = the normal through the model's row-normalized
  rotation (U = quads 64-66) and `Lworld_i = (q30[i], q31[i], q32[i])`. For GLSL: transform n by U (3x3),
  dot with world L. (Not renormalized: with non-uniform scale the result differs from a true normal matrix.)

###### 1.4 Lit × vertex colour (2460e528) — read from microcode
`lit` = 1.3's (rgb, a). `cv = c15 * 256` (xyzw).
- 87.y & 0x100 == 0: `out = lit * cv` (all four lanes; a = amb.w * cv.a).
- 87.y & 0x100 != 0: `out.rgb = lit.rgb + cv.rgb * 128` (= + raw int16), `out.a = amb.w * cv.a`.
  (The native port declines 0x100; it is now fully read, and is the same rule as 70362be4's.)

###### 1.5 Lit + 4 point lights (127ca0b4) — native `PointLights`, oracle-checked, no recording
Inputs: 60-63 model matrix, 64-66 U, point light k (k=0..3): position `q(44+2k).xyz`, `q(44+2k).w = 1/range`,
colour `q(45+2k).xyz`.
```
W  = p * M60                      // (p,1) through quads 60-63: world position
N  = n * U                        // quads 64-66
dk = Pk - W                       // unnormalized
s  = max(dot(dk, N), 0)
fk = max(s / |dk| - s * q(44+2k).w, 0)      // = max(0, n·dir) * |N| * (1 - |dk|/range)  (linear falloff to 0 at range)
rgb = lit_dir.rgb + Σk ( q(45+2k).xyz * q82.xyz ) * fk      // the 4 summed first, then added to 1.3's rgb
a   = 1.3's a
```
A light with range ≤ 0 is sent as zero quads (position 0, colour 0, w 0) so contributes 0.
The interpreter's first two vertices of a chunk see the next light's 1/|d| (VU P/Q timing); do the intended math.

###### 1.6 Lit + points × vertex colour (e3bca52a) — read from microcode (and native)
`lit` = 1.5's. `cv = c15*256`. `s = (87.y & 0x100) ? -1 : +1` (vf6 = s, vf7 = -s):
```
out.rgb = max(lit.rgb * cv.rgb * s, (lit.rgb + cv.rgb * 128) * -s)
out.a   = lit.a * cv.a
```
For lit ≥ 0 and cv ≥ 0 this is exactly `lit*cv` (no 0x100) or `lit + raw int16` (0x100), same as 1.4.
Copy the max() form to stay identical for negative inputs.

###### 1.7 Output range and clamp (slot D, read in a786efab; native `gsNativeMeshPoint`)
C writes float RGBA into the output buffer; D does `rgba = FTOI0(min(colour, quad39))` per lane (truncate
toward zero, **no lower clamp**; a negative value becomes a negative int whose low byte the GIF takes —
**[GUESS]** never happens for non-negative inputs). quad 39 = LightState +0xB0, default (255,255,255,255).
GS meaning: RGB 128 = 1.0 under MODULATE (`tex * rgb / 128`), A 128 = 1.0 for blending.
GLSL: `col = trunc(min(c, q39))` then feed the existing modulate/alpha path (0-255 units).

##### 2. Slot F: UV generation (and position edits)

Picked per material **row** = material type (desc+0x40 → mesh.fA; `VuProgSelResolveMaybe`, table at 0x491280).
F pieces read out of the ELF table:

| row (material type) | F | what |
|---|---|---|
| 0 | d3f6f631 | scroll by 88.xy (the default) |
| 2 | 150031ee | sphere map |
| 3 | 0594a063 | ramp (toon) — no recording |
| 6 | b1d29245 | beam (transporter effect) |
| 7 | 9df36d6a | wave (bends positions, UVs unchanged) |
| 8 | 2aa11602 | none (empty) |
| 9 | ad209cd3 | reflection map |
| 10 | 15be21a5 | projected texture (projector program) |
| 11 | a04c11ce | swell (positions) — no recording |

All F's write UVs back into the UV section before D; D then sends `S = u*Q, T = v*Q, Q = 1/w`
(w = the clip w of quads 68-71) except the affine D's (Q = 1).

###### 2.1 Scroll (d3f6f631) — read
`off = q88.xy`. If both lanes are exactly 0 the piece does nothing (FMAND on the Z flags); otherwise
`uv = uv + off` for every vertex. Units = UV after the 79 scale (1.0 = one texture repeat).
quad 88 is 0 from the material packet; only effect models write it (`EffModelDrawNodes_1a7b80` →
`MeshPktWriteOffsetMaybe_14ab30`: 88 = node+0x50 + node+0xA0). (**[GUESS]** the loop is pipelined and expects ≥ 2
vertices; with count 1 it would run past the end.)

###### 2.2 Sphere map (150031ee) — read
```
R_i.xy = q(64+i).x * q24.xy + q(64+i).y * q25.xy + q(64+i).z * q26.xy     (i = 0..2)   // U * Vrot, xy columns
uv     = ((n.x*R_0 + n.y*R_1 + n.z*R_2) + 1) * 0.5
```
i.e. `uv = 0.5 * ((n * U * Vview3x3).xy + 1)` — the view-space normal's x,y; no renormalization, no y flip.
quads 24-27 = the world→view matrix (`DrawPktBuildCameraMatrix_1486c0`: cam+0xB0..0xE0), 28 = cam+0x170 (eye).

###### 2.3 Reflection map (ad209cd3) — native `Reflection`, recorded
```
W   = p * M60                          // world position
V   = (W - q28.xyz) * ERLENG           // normalized eye→vertex (q28 = eye position)
N   = n * U                            // not normalized
r   = V - N * 2*(V·N)
uv  = ((r.x + 1) * 0.5, (r.z + 1) * 0.5)   // world x and z
```

###### 2.4 Ramp (0594a063) — read, no recording
`u = (n.x*q100.x + n.y*q101.x + n.z*q102.x + 1) * 0.5` (ACC order x, y, then z), `v = 0`.
= `(dot(n*U, Lworld_0) + 1)/2` with directional light 0; not clamped (with |n| ≤ 1, in [0,1]). Toon ramp.

###### 2.5 Wave (9df36d6a) — native `Wave`, recorded (edits positions, UVs untouched)
```
a = q84.x;  q = q900.z / (q84.y - q84.x)
d = y - a;  t = q * max(d, 0)
c = 1 - t*t/2;  s = ESIN(t)   (VU polynomial: x*(1 + x²(-0.16666657 + x²(0.0083330255 + x²(-0.00019807414 + x²*2.601887e-6)))))
x' = c*x - s*d;   y' = c*d + s*x + a;   z, normals unchanged
```
**[GUESS]** q900.z (first bone-matrix quad) carries the wave's amount/phase from the skin-matrix packet
(`ModelBuildSkinMtxPacketMaybe_150390`); 84 = desc+0x20 of the material.

###### 2.6 Beam (b1d29245) — native `Beam`, recorded
```
(x,y,w) = (p,1) * columns x, y, w of quads 72-75
cx = q75.x / q75.w;  cy = -q75.y / q75.w
dx = x/w - cx;  dy = -y/w - cy
u = ((dx * q82.z + cx + (q82.x - 1)*0.05) + 1) * 0.5
v = ((dy * q82.z + cy + (q82.y - 1)*0.05) + 1) * 0.5
```
Its D (1bd1b5c0) is affine (Q = 1) and the colour is (128,128,128, vertex alpha) clamped by 39.

###### 2.7 Swell (a04c11ce) — native `Swell`, oracle-checked, no recording
```
wc   = (p,1) · w-column of quads 68-71            // the vertex's clip w
D    = q12.x*q8.x + q13.x*q8.y + q14.x*q8.z + q15.x*q8.w
k    = q84.x / D;  sc = clamp((q84.y - wc) / q84.y, 0.2, 1)
p    = p + n * k * wc * sc
```
Material type 11 forces 84.y = 800 when ≤ 2 (`MeshMaterialBuildPkt`). Also writes colours that the one-colour C
then overwrites.

###### 2.8 Projected texture (15be21a5)
UV = (p * M60 * quads 30-33).xy / w (quads 30-33 here are the projector matrix written by
`ShadowMapWriteTexMtxPktMaybe_147e30`, UNPACK 4 at 30 — it **overwrites the directional light quads 30-32**);
colour (128,128,128, 82.w*81.w*128). Native: `kProjectorF`.

##### 3. EE side: where the light data comes from

###### 3.1 The light-state record (`LightState`, `Rec`)
Current record: `drawCtx (D_004810A4) + 0x36B4`. Fog record: `drawCtx + 0x36B8`. Camera: `drawCtx + 0xF20`
(`cam+0x00` fog enable, `cam+0x0C` point-light mode, `cam+0xB0` view matrix, `cam+0x170` eye).

| Offset | Content | Default (`LightStateInit_156b50`) | VU1 quad |
|---|---|---|---|
| +0x00 | id | | |
| +0x04 | address of the packet already emitted for it (0 = not yet) | 0 | |
| +0x10 | ambient rgb, w = alpha factor | (0,0,0,128) | 37 |
| +0x20..+0x5F | 4 directional lights, row i = direction xyz (unit, toward the light), w = 0 | 0 | 30-32 (transposed: lane i = light i) |
| +0x60..+0x9F | 4 directional colours, row i = colour rgb (w unused) | 0 | 33-36 |
| +0xA0 | (unused by strip lighting) | 0 | 38 |
| +0xB0 | colour clamp | (255,255,255,255) | 39 |
| +0xC0 | default tint ("env color", used when the object has none) | (128,128,128,128) | 42 (then overwritten per draw, see below) |
| +0xD0 | 0x200-material colour | (128,128,128,128) | 43 |
| +0xE0 + 0x30k | point light k (k=0..3): +0x00 position xyz (w gets 1/range at send time), +0x10 colour, +0x20 range | range -1 (off) | 44+2k, 45+2k |

Setters (all act on `*(drawCtx+0x36B4)`):
- `LightStateSetDirLight_15cbb0(i, dir, col)`: row i of +0x20 (w forced 0) and of +0x60.
- `LightStateSetAmbient_15cb30(v)`: +0x10. `LightStateGetAmbient_15cb10`.
- `LightStateSetMatrices_15cc00(dirs4x4, cols4x4)`: +0x20..+0x9F wholesale (characters' own lights:
  `Character2DrawWithOwnLightMaybe_1ae720` passes chara+0x630 and +0x6F0, blended over time by
  `Character2LightBlendStep/SetTarget` — linear blend, **[GUESS]** not renormalized).
- `LightStateSetPointLight_15ca80(k, pos, col, r)` → `LightSlotSet_156af0(rec, k, pos, col, range)` with
  `range = 0.5 * (r * sqrt(max(col.r, col.g, col.b)) + r)`.
- Map/scene lights: `NodeObjDrawLit_1947f0` pushes a copy (`LightStatePush_1429f0`) and fills it from the node's
  light list: type 0 dir (normalized `NodeTransformDir` of the light node's translation, colour * flicker; filled
  from index 3 down), 1 ambient set, 2 point (`NodeTransformPoint` position, colour * flicker, radius it+0x70;
  filled from index `m144-1` down; also turns the camera's point mode on), 3 ambient add, 4 tint.
  `MapAreaModelApplyNearestPlightsMaybe_188590` fills point slots 3,2,1,0 with the nearest map point lights.
  Time of day: `TimeOfDayBlendLightSet_18cb10` / `TimeOfDayLightCopy_18d140` (not read in detail).
- Fog: `FogSet_15cc60(r,g,b, near, far, nearAmt, farAmt)` → fog record +0x00 near, +0x04 far, +0x08..0x0A rgb
  bytes, +0x14 nearAmt, +0x18 farAmt (`MapEnvApplyFog_1e5ba0` from the map env data); `CameraSetFogEnable_15cce0`
  → cam+0x00.

Space **[GUESS]**: directions and point positions are in the space quads 60-63 map into (world), since the VU
uses them with `n*U` and `p*M60`; `NodeTransformDir/Point(self, …)` produce that frame.

###### 3.2 `LightStateWritePacket_1484a0(drawCtx, pkt)` — what lands in VU1 memory
One DMA `ret` tag (QWC = n-1) + `STCYCL 1,1` + `UNPACK V4-32, num = n-1, addr = 30` (absolute), then 22 quads:

| VU1 quad | From |
|---|---|
| 30, 31, 32 | rows 0-2 of transpose(+0x20 matrix): q30 = (L0.x, L1.x, L2.x, L3.x), q31 = y's, q32 = z's |
| 33-36 | +0x60 rows 0-3 (colours) |
| 37 | +0x10 (ambient) |
| 38 | +0xA0 |
| 39 | +0xB0 (colour clamp) |
| 40 | fog coefficients `FogCalcCoefs_156c40(fogRec)` if `cam+0x00 != 0 && fogRec != 0`, else (0, 255, 255, 0) |
| 41 | A+D FOGCOL: (rgb bytes of fogRec+0x08..0x0A, 0x3D) if fogging, else 0 |
| 42 | +0xC0 |
| 43 | +0xD0 |
| 44+2k | point k position xyz, w = 1/range (written back into the record at +0x0C); zero quad if range ≤ 0 |
| 45+2k | point k colour; zero quad if range ≤ 0 |

`DrawListAddLightPacket_146b00` (caller `TaskDrawShadowMap_1f70f0`) and `_148120` (callers: the four
`Character2Draw*` functions, `TaskUpdateDrawInfo_2f6ff0`, `CompassHudRender_370fb0`) both: take the current
draw buffer, call `LightStateWritePacket` at its end, allocate the quads and add a DMA call to it.
`LightStateEmitOnce_142950` (from `NodeObjDrawLit`) and `LightStateResetEmit_142b70` do the same but record the
packet address in rec+0x04 so it is sent once per record. **Every path goes through `LightStateWritePacket_1484a0`**,
so one hook there sees every light set: read `*(ctx+0x36B4)` (+ `ctx+0x36B8`, `ctx+0xF20`) or simply the 22
quads it just wrote at pkt+0x10 (exactly quads 30-51), keyed by the packet address the draw list CALLs.

###### 3.3 Per-draw quads that the lighting/UV math also needs
- 60-63 model matrix (`VifPktUploadMatrix_148440`, followed by MSCAL 0 = the base program, which derives
  64-66, 68-75, 100-102, 111.z from it and the then-current 30-32, 4-11, 24-27).
- 24-28 view matrix + eye (`DrawPktBuildCameraMatrix_1486c0`).
- 76-79 mesh decode header, **81** = (Info.mode, 0, 0, Info.scale) and **42** = Info.col
  (`ModelMeshWriteVuHeaderMaybe_15b1d0` / `…Mode1…_1646a0`, Info from `ModelPrepareDrawMaybe_14f760`):
  Info.mode 0/1/3 (bit 1 = clipping, slot E; bit 0 → part flag 8 = guard-band D), |0x40 when a point light is near;
  Info.scale = ctx+0x170 * obj.h2E/16384 (fade; 81.w multiplies 82.w); Info.col = obj colour (obj+0x20 → +0x10)
  if set, else LightState +0xC0.
- 82-84, 86-88 material (`MeshMaterialBuildPkt_14ac90`): 82 material colour (desc+0x00; w clamped/scaled with
  flag 0x80), 83 = 0, 84 = desc+0x20 (wave/swell parameters), 86 vertex GIF tag, 87 = (PRMODE bits, matFlags,
  1 + 0.01*desc.s8[0x52] (depth scale), 0), 88 = 0. PRMODE bits: matFlags 0x2 → TME, 0x8 → FGE (fog),
  0x20 → ABE, desc+0x4C → IIP (Gouraud). Other matFlags: 0x1 vertex colours, 0x4 lit, 0x10 black FOGCOL, 0x40
  backface cull (D index bit 0), 0x80 FBA + alpha scale, 0x100 additive vertex colour, 0x200 colour from 43 (unlit),
  0x400 frame mask, 0x1000 winding, 0x2000 alpha-fail mode.
  82 is also rewritten by `TexEnvBuildDmaChainMaybe_14abb0` (tex env with +0x06 set) and by effect models
  (`MeshPktWriteColourMaybe_14ab70`).

##### 4. Fog (slot D) — read in a786efab, same in every strip D

```
wc = (p,1) · w-column of quads 68-71                       // the clip w (68-71 = model * view * quads 4-7)
F  = max(min(q40.x + q40.w * wc, q40.y), q40.z)           // MULAx ACC.w = 40.x; MADDw += 40.w*wc; MINIy; MAXz
XYZF2.F = (FTOI4(F) >> 4) & 0xFF = trunc(F)               // 255 = no fog, 0 = all fog colour
```
Constants (`FogCalcCoefs_156c40`, fog record f: near = f[0], far = f[1], nearAmt = f[5], farAmt = f[6]):
```
p1 = 255 * (1 - nearAmt)          → q40.y  (the cap)
p2 = 255 * (1 - farAmt)           → q40.z  (the floor)
q40.w = (p1 - p2) / (near - far)  (slope; also stored in f[4])
q40.x = (far*p1 - near*p2) / (far - near)   (intercept; also f[7])
```
So F goes linearly from p1 at wc = near to p2 at wc = far, clamped between. Fog off (no fog record or
cam+0x00 == 0): q40 = (0, 255, 255, 0) → F = 255. Use `max(min(.,y),z)`, not GLSL `clamp` (differs if p2 > p1).
FOGCOL = quad 41 (A+D, reg 0x3D) = fog rgb bytes; with 87.y & 0x10 its low word is zeroed → black fog
(additive materials). Fog only shows where PRMODE.FGE (matFlags 0x8) is set.
GS blend: `rgb' = (F * rgb + (255 - F) * FOGCOL) >> 8` per pixel, F interpolated across the triangle
(**[GUESS]** linear in screen space, not perspective-corrected). wc's unit: **[GUESS]** view-space depth in world
units (the w lane of the window projection in quads 4-7); near/far are in the same unit.
The current `native_mesh.vert` already does `clamp(fog.x + fog.w*clip.w, fog.z, fog.y)` per vertex from the header.

##### 5. Open points for the GLSL port

- 87.y & 0x100 for 70362be4/2460e528 is now read (1.2, 1.4): additive raw int16 rgb, multiplicative alpha.
- 127ca0b4 / e3bca52a / 0594a063 / a04c11ce were never seen running; their math is from the code + oracle.
- 83 is 0 for every mesh found; keep the term for safety.
- Normals are not renormalized; with non-uniform model scale `n*U` is not a true normal transform — copy it.
- Lights are per LightState record, not per frame: characters push their own (`Character2Draw*`), map nodes push
  copies (`NodeObjDrawLit`); quads 30-32 are also clobbered by the projector matrix. A shader input must follow
  the record active for each draw (the light packet CALLed before it), not a global.

### The GsPkt builder: an exact map

Static reading of `disc/SCUS_974.90` (tools/fninfo.py, tools/mipsdis.py) and
RGDecomp's C where it exists (checked against the disassembly; RGDecomp's
names for fields are not trusted). Nothing here was run. gp = 0x485170.
Addresses in `(…)` are the instructions that show the claim.

Register numbers used below (GS A+D addresses): PRIM 0x00, RGBAQ 0x01,
ST 0x02, UV 0x03, XYZ2 0x05, TEX0_1 0x06, CLAMP_1 0x08, TEX1_1 0x14,
PRMODECONT 0x1A, TEXA 0x3B, TEXFLUSH 0x3F, SCISSOR_1 0x40, ALPHA_1 0x42,
TEST_1 0x47, FBA_1 0x4A, FRAME_1 0x4C, ZBUF_1 0x4E. Only context 1 is ever
used (PRIM.CTXT is never set, see `+0x70`).

---------------------------------------------------------------------------

#### 0. Summary table

"Emits" = what lands in the packet at the call. "State" = builder fields
only, written to the packet later (by the function named in brackets).
`b` = the builder (a0).

| Function | Emits (GS regs / packet words) | State-only? | Notes |
|---|---|---|---|
| `DrawListBeginPrim_141b00` | nothing | yes | `b = ctx+0x130`; GsPktInit + GsPktBegin; returns b |
| `DrawListEndPrim_141a20` | RET qword (via End); draw-env packet (RET 9 + DIRECT 9: FRAME, ZBUF, XYOFFSET, SCISSOR, PRMODECONT, COLCLAMP, DTHE, TEST_1); two DMA CALL tags into the list | no | |
| `GsPktCtorMaybe_13fb80` | — | yes | static init only |
| `GsPktInit_13f7f0` | — | yes | full reset of state from the draw env |
| `GsPktBegin_13f7e0` | — | yes | `+4=1`, `+0xF0=0` |
| `GsPktEnd_13f790` | DMA RET qword | no | returns block address |
| `GsPktOpen_13f4a0` | CNT9/DIRECT9: GIF A+D{TEXFLUSH, PRMODECONT=1}, GIF A+D{ALPHA_1, TEST_1, ZBUF_1, FBA_1, FRAME_1} | no | the env snapshot point |
| `GsPktOpenPrim_13f730` | Open + BeginPrimAD | no | sets PRIM type, Q=1.0 |
| `GsPktBeginPrimAD_13f6b0` | CNT tag (open), GIF A+D tag (open), PRIM | no | PRIM snapshot point |
| `GsPktSetPrimType_13f470` | BeginPrimAD | no | sets type, Q=1.0, `+0x114=0`; does NOT close the previous tag |
| `GsPktOpenGifTag_13f3b0` | CNT tag (open), **PACKED GIF tag, PRE=1, PRIM, NREG/REGS given** | no | **non-A+D** |
| `GsPktFinalizeTags_13f260` | patches the open CNT/DIRECT/GIF NLOOP | no | packed-aware |
| `GsPktClose_13f5b0` | TEXFLUSH, patches tags (A+D NLOOP), commits | no | |
| `GsPktCommit_13f190` | commits (no tag patch, no TEXFLUSH) | no | used after FinalizeTags |
| `GsPktReserveQwords_13f230` | advances write pointer | no | returns old pointer: **raw writes** |
| `GsPktPushQword_13f170` | one raw qword | no | **non-A+D** (packed data) |
| `GsPktAddDmaCallTag_13f1f0` | one DMA CALL tag | no | **non-A+D** |
| `GsPktRawWriteBeginStubMaybe_13f250` / `…EndStubMaybe_13f1e0` | nothing (`jr ra`) | — | markers only |
| `GsPktWriteAD_13ec80` | A+D (reg=a1, data=a2) | no | regs used by callers: 0x40, 0x3B, 0x3F |
| `GsPktSetTexture_13ec00` | A+D TEXFLUSH, TEX1_1, TEX0_1, CLAMP_1 | no | copies the texture node into `+0x78`; filter from `+0xE0` |
| `GsPktSetTexa_13eca0` | A+D TEXA | no | |
| `GsPktSetST_13ed10` / `GsPktSetSTVec_13ecc0` | A+D ST | no | also Q → `+0x110` |
| `GsPktSetUV_13ed60` | A+D UV (texels ×16) | no | |
| `GsPktSetUVFixed_13ed90` | A+D UV (already ×16) | no | |
| `GsPktSetRGBAQ_13ee40` / `GsPktSetRGBAQVec_13edd0` | A+D RGBAQ (Q from `+0x110`) | no | |
| `GsPktSetXYZ_13ef10`, `…VecI_13eeb0`, `…Pixel_13f110`, `…PixelVec_13ef70`, `…f_13f060`, `…fVec_13efe0` | A+D XYZ2 (+ `+0x120/+0x124`) | no | units differ, see §7 |
| `GsPktDrawSprite_13ea30` | **PACKED** tag {RGBAQ, XYZ2, XYZ2}, SPRITE, 3 qwords | no | **non-A+D** |
| `GsPktAddTexSprite_13e520` | A+D PRIM/tex/(RGBAQ) then **PACKED** {UV,XYZ2,UV,XYZ2} strips or {RGBAQ,UV,XYZ2,UV,XYZ2} | no | **non-A+D** |
| `GsPktTileScreenSprites_177cf0` | **PACKED** {UV, XYZ2}, SPRITE, 64×32 tiles over the frame | no | **non-A+D** |
| `GsPktSpriteUVRect_26eb30` | A+D UV, XYZ2, UV, XYZ2 | no | |
| `GsPktSpriteSplitU_17adf0` | A+D (UV, XYZ2, UV, XYZ2) × n strips of 32 texels | no | |
| `GsPktDrawLine3D_1cca80` | A+D RGBAQ, XYZ2, XYZ2 (if both ends project) | no | debug only |
| `GsPktSetScissor_28c2e0` | OpenPrim(SPRITE) + A+D SCISSOR_1 + Close | no | full segment, no vertices |
| `GsPktSetScissorDefault_28c2d0` | as SetScissor, offsets (3,3,1,1) | no | |
| `GsPktSetScissorFull_26fe60` | OpenPrim(SPRITE) + SCISSOR_1 (0..w-1, 0..h-1) + Close | no | |
| `GsPktSetScissorRows_26ff00` | OpenPrim(SPRITE) + SCISSOR_1 (0..w-1, y0..y1) + Close | no | |
| `GsPktSetOriginMode_13e2f0` | — | yes (XYZ offsets now) | `+0x11C`, `+0x120`, `+0x124` |
| `GsPktSetFog_13e370` | — | yes [BeginPrimAD/OpenGifTag] | PRIM.FGE |
| `GsPktSetAntialias_13e390` | — | yes [same] | PRIM.AA1 |
| `GsPktSetGouraud_13e3b0` | — | yes [same] | PRIM.IIP |
| `GsPktSetTextured_13e400` | — | yes [same] | PRIM.TME |
| `GsPktSetUseUV_13e3e0` | — | yes [same] | PRIM.FST |
| `GsPktSetAlphaBlend_13e500` | — | yes [same] | PRIM.ABE |
| `GsPktSetTexFilter_13e3d0` | — | yes [SetTexture] | `+0xE0` |
| `GsPktSetFrameMask_13e420` | — | yes [Open] | FRAME_1.FBMSK, `+0x6C` |
| `GsPktSetZWriteMask_13e430` | — | yes [Open] | ZBUF_1.ZMSK, `+0xE4` |
| `GsPktSetZTest_13e450` | — | yes [Open] | TEST_1.ZTE/ZTST |
| `GsPktEnableZTest_13e460` | — | yes [Open] | TEST_1 ZTST ALWAYS/GEQUAL |
| `GsPktSetDestAlphaTest_13e4a0` | — | yes [Open] | TEST_1.DATE |
| `GsPktSetDestAlphaMode_13e4b0` | — | yes [Open] | TEST_1.DATM (and DATE) |
| `GsPktSetAlphaTestFunc_13e4c0` | — | yes [Open] | TEST_1.ATST/AREF |
| `GsPktEnableAlphaTest_13e4d0` | — | yes [Open] | TEST_1.ATE |
| `GsPktSetAlphaReg_13e4e0` | — | yes [Open] | ALPHA_1 raw |
| `GsPktSetBlendMode_13e4f0` | — | yes [Open] | ALPHA_1 from preset table |
| `GsPktDisableTests_13f9e0` | — | yes | |
| `GsPktSetDefaultState_13fa80` | — | yes | |
| `GsPktSetup2DState_26fdc0` | — | yes | font / message window state |
| `GsPktSetupUiDrawState_2932b0` | — | yes | |
| `GsPktSetup2DState_3892a0` | — | yes | insect-cage UI state |
| `GsEnvInit_1573b0` | — | yes | env GIF tag + reg addresses + defaults |
| `GsEnvSetDefaults_1572d0` | — | yes | |
| `GsEnvSetDefaultsMaybe_1571d0` | — | yes | 3D materials only |
| `GsEnvSetAlpha_157140` | — | yes | |
| `GsEnvSetBlendPreset_157150` | — | yes | table at 0x449EF0 |
| `GsEnvWritePacket_157190` | 6 qwords (env GIF tag + 5 A+D) | no | returns 6 |

---------------------------------------------------------------------------

#### 1. Where the builder lives, and what a block becomes

- The draw context: `ctx = *(u32*)0x4810A4` (`GetDrawCtx_15de80`:
  `lw v0,-0x40cc(gp)`).
  - `ctx+0x00` s32: index of the current buffer.
  - `ctx+0x04` u32: the draw list's write cursor (DMA chain; EndPrim
    appends CALL tags here).
  - `ctx+0x08 + 4*i`: per-buffer header pointer; the **packet memstack**
    is `header + 8` (or 0 when the header is 0) (0x141b0c-0x141b2c).
  - `ctx+0x130 .. +0x267`: **the builder** (one per context; every 2D
    block uses this same object).
  - `ctx+0x270`: pointer to the current draw environment (`DrawEnv`).
- Memstack (`MemStackAllocQw_14d950(stack, n)`): `+0x20` lock (nonzero:
  allocation returns 0), `+0x28` base address, `+0x2C` top (in qwords),
  `+0x34` capacity (qwords; over it the function spins on `+0x54` and
  returns 0). Returns `base + top*16` and advances top by n.
- DrawEnv fields the builder uses: `+0x04` width (px), `+0x08` height,
  `+0x0C` cx, `+0x10` cy (centre in GS primitive pixels, 2048-biased:
  `DrawEnvSetCenter_157560` stores `p+0x800`), `+0x70` its GIF tag,
  `+0x80` FRAME_1, `+0x90` ZBUF_1, `+0xA0` XYOFFSET_1, `+0xB0`
  SCISSOR_1, `+0xC0` PRMODECONT, `+0xD0` COLCLAMP, `+0xE0` DTHE, `+0xF0`
  TEST_1 (each data + address; the order is libgs's `sceGsDrawEnv1`, copied
  from `sceGsDBuff+0x50` by `GfxSetVideoMode_1451f0` at 0x14580c-0x145898,
  and `sceGsSetDefDrawEnv` writes addresses 0x4C,0x4E,0x18,0x40,0x1A,0x46,
  0x45,0x47 in that order).

##### The packet shape of one block (all on the memstack, contiguous)

```
GsPktOpen:         [CNT qwc=9 | VIF NOP | VIF DIRECT 9]
                   [GIF A+D NLOOP=2 EOP]  (0, TEXFLUSH)  (1, PRMODECONT)
                   [GIF A+D NLOOP=5 EOP]  ALPHA_1 TEST_1 ZBUF_1 FBA_1 FRAME_1   (GsEnvWritePacket)
GsPktBeginPrimAD:  [CNT qwc=? | NOP | DIRECT ?]  <- +0xF8/+0xFC/+0x104/+0x108
                   [GIF A+D NLOOP=? EOP]          <- +0x100
                   (PRIM image, PRIM)
                   … A+D setters …
GsPktClose:        (0, TEXFLUSH), then the three ? patched, then committed
   or
GsPktOpenGifTag:   [CNT ? | NOP | DIRECT ?] [GIF PACKED PRE=1 PRIM NREG REGS NLOOP=?]
                   … packed qwords …
GsPktFinalizeTags: the ? patched (NLOOP = qwords / NREG); GsPktCommit commits
…more Open … Close/Commit segments…
GsPktEnd:          [RET qwc=0]
```

- Every sub-packet is a DMA **CNT** tag whose two VIF words are NOP and
  DIRECT n (n = the same qword count): the whole block goes to VIF1 → GIF
  PATH2, except the callouts in §9 (a CALL to a VU microcode upload, and a
  VIF UNPACK + MSCAL).
- `GsPktEnd` returns `+0xF0` = the address of the first Open's CNT tag
  `& 0x0FFFFFFF`, or 0 when no Open happened since Begin.
- `DrawListEndPrim` then (0x141a40-0x141ae4):
  1. if that address is nonzero: writes `{0x50000000 (CALL, qwc 0), addr,
     0, 0}` (the qword at gp-0x7330 = 0x47DE40 = `0x00000000_50000000`, word
     1 replaced) at `ctx->+4`, advances `ctx->+4` by 16;
  2. always (when the memstack exists): `DrawEnvWritePacket_157460(env,
     top)` writes `{RET qwc 9 | NOP | DIRECT 9}` + the env's 9 qwords
     (`env+0x70..+0xFF`: GIF tag, FRAME, ZBUF, XYOFFSET, SCISSOR,
     PRMODECONT, COLCLAMP, DTHE, TEST_1) at the memstack top, allocates
     its 10 qwords, and appends a second CALL to it.
  So the list reads: … CALL block … CALL env-restore …; everything the
  block changed in FRAME/ZBUF/SCISSOR/TEST/etc. is put back after it.
  ALPHA_1, FBA_1, TEX0/TEX1/CLAMP, TEXA, PRMODE, FOGCOL are **not**
  restored by that packet (ALPHA/FBA are set again by the next block's
  Open; TEX*/TEXA are whatever was written last).
- `DrawListBeginPrim(ctx)` (0x141b00): `GsPktInit(ctx+0x130, memstack,
  *(ctx+0x270))`, `GsPktBegin(ctx+0x130)`, returns `ctx+0x130`.
- Six functions inline the same Begin/End pair on `ctx+0x130` without
  calling DrawListBeginPrim/EndPrim (a hook on those two misses them):
  `RefractionCaptureFrame_13ffd0` (0x1401b8/0x1402b8),
  `GfxBlitTexRect_141000` (0x141140/0x141324), `GfxEndFrame_141c20`
  (0x141e84/0x141f7c), `DrawListEmitFrameEnvMaybe_142280`
  (0x1422bc/0x1422f8), `DrawListClearScreen_1423c0` (0x142420/0x1424e8),
  `GfxDrawPerfMeter_1431c0` (0x1432d0/0x143570). They write the same two
  CALL tags themselves (e.g. 0x14132c-0x1413c0).
- **Blocks nest.** `DrawListAddTexLoad_1418c0` → (texture has an
  animation table) `TexAnimeTableApplyMaybe_162f50` (0x1419ec) →
  `DrawListBeginPrim(ctx)` (0x162f88) … `DrawListEndPrim` (0x162fc8), on
  the same builder. `ScreenFadeBlockDraw_177540` calls
  `DrawListAddTexLoad` at 0x177618 inside its own block (begun 0x177588):
  the inner GsPktInit resets every state the outer block had set
  (0x177598-0x1775c8: Z test, alpha test, blend, Z mask) and the inner
  GsPktBegin zeroes `+0xF0`. The inner block is a complete, separate
  block (its CALL precedes the outer one in the list). A hook has to treat
  BeginPrim as "reset the builder", not as "push".
- Things the game allocates on the memstack between BeginPrim and the
  first Open (e.g. DrawListAddTexLoad's upload, 0x14196c) sit before the
  block and are called from the list before the block's CALL. Nothing may
  allocate between a segment's Open and its Close/Commit (the segment is
  written at the memstack top and allocated only at Close/Commit).

---------------------------------------------------------------------------

#### 2. The builder's layout (`b = ctx+0x130`, size 0x138)

| Off | Type | Meaning | Written by | Read by |
|---|---|---|---|---|
| +0x00 | u32 | memstack pointer (0: no output) | Ctor (0), Init (a1) | Open, OpenPrim, Close, Commit, End |
| +0x04 | s32 | "in a block": 1 after Begin, 0 after End | Init (0), Begin, End | no GsPkt function (open: any reader) |
| +0x08..+0x0F | — | unused | — | — |
| +0x10..+0x6F | GsEnv (0x60) | §3 | GsEnv*, GsTest*, GsZbuf*, GsFrame*, GsFba* | Open (copied verbatim) |
| +0x10 | u128 | env GIF tag: A+D, NLOOP 5, EOP, NREG 1 | GsEnvInit | Open |
| +0x20 / +0x28 | u64 / u64 | ALPHA_1 data / 0x42 | | |
| +0x30 / +0x38 | u64 / u64 | TEST_1 data / 0x47 | | |
| +0x40 / +0x48 | u64 / u64 | ZBUF_1 data / 0x4E | Init copies `env+0x90` | |
| +0x50 / +0x58 | u64 / u64 | FBA_1 data / 0x4A | | |
| +0x60 / +0x68 | u64 / u32 | FRAME_1 data / 0x4C | Init copies `env+0x80` (GsFrameSet) | |
| +0x6C | s32 | frame-mask mode (-1 none; 0..4), in FRAME's address qword's high word (GS ignores it) | GsEnvSetDefaults (-1), GsFrameSetMaskMode | GsFrameSet |
| +0x70 | u64 | **PRIM register image**: bits 0-2 type, 3 IIP, 4 TME, 5 FGE, 6 ABE, 7 AA1, 8 FST, 9 CTXT, 10 FIX | Ctor (0x100), Init, setters, OpenPrim/SetPrimType/OpenGifTag/AddTexSprite (type) | BeginPrimAD (whole u64 as PRIM data, 0x13f714), OpenGifTag (`& 0x7FF` into the tag, 0x13f41c) |
| +0x78..+0xDF | tex node (0x68 = 13 u64) | a copy of the texture descriptor last passed to SetTexture; node `+0x38` TEX0 (= b+0xB0), `+0x40` TEX1 (b+0xB8), `+0x48` CLAMP (b+0xC0); the rest unused here | Init (NameTableNodeReset: TEX0=TEX1=0, CLAMP WMS=WMT=1), SetTexture (NameTableCopyNode + GsTex1SetFilter) | SetTexture |
| +0xE0 | s32 | texture filter for the next SetTexture: 0 nearest, 1/2 linear (with mipmaps: 0 → MMIN 2, 1/2 → MMIN 5) | Ctor 1, Init 1, SetDefaultState 1, SetTexFilter | SetTexture → GsTex1SetFilter_15ac10 |
| +0xE4 | s32 | Z-write-mask shadow (1 = ZMSK set) | Ctor 0, Init 1, SetZWriteMask, DisableTests 1, SetDefaultState 1 | SetZWriteMask only (re-reads it, 0x13e438) |
| +0xE8 | s32 | 1 = no open segment / no memstack (Close and Commit do nothing); 0 = segment open | Ctor 1, Init 1, Open/OpenPrim (1, then 0 if `+0` ≠ 0) | Close, Commit. Never set back to 1 by Close/Commit |
| +0xEC | u32 | segment start (memstack top at Open) | Open (0x13f4d8) | Close, Commit (allocation = `(F4-EC)/16`) |
| +0xF0 | u32 | block start `& 0x0FFFFFFF` (first Open after Begin) | Begin (0), Open if 0 | End (return value) |
| +0xF4 | u32 | **write pointer** | Open, BeginPrimAD, OpenGifTag, all emitters, FinalizeTags/Close (roll back) , ReserveQwords | everything that emits |
| +0xF8 | u32 | current CNT tag address (roll-back point) | BeginPrimAD, OpenGifTag | FinalizeTags, Close |
| +0xFC | u32 | same as +0xF8 (base of the DIRECT count; always equal) | same | same |
| +0x100 | u32 | current GIF tag address | Open (its header tag), BeginPrimAD, OpenGifTag | FinalizeTags, Close |
| +0x104 | u32 | CNT tag word 0 address (= +0xF8) | BeginPrimAD, OpenGifTag | FinalizeTags, Close |
| +0x108 | u32 | VIF DIRECT word address (CNT tag + 0xC) | same | same |
| +0x110 | f32 | **Q** for RGBAQ (bits stored raw in RGBAQ[63:32]) | Ctor 1.0, OpenPrim/SetPrimType/OpenGifTag/AddTexSprite 1.0, SetST/SetSTVec (q) | SetRGBAQ, SetRGBAQVec, AddTexSprite (0x13e5b8). **Init does not reset it** |
| +0x114 | s32 | 1 = the open tag is PACKED (from OpenGifTag), 0 = A+D | OpenGifTag 1, SetPrimType 0, AddTexSprite 0 | FinalizeTags only (Close ignores it). OpenPrim/BeginPrimAD do not reset it |
| +0x118 | s32 | NREG of the open PACKED tag | OpenGifTag (t0) | FinalizeTags (divisor, 0x13f390) |
| +0x11C | s32 | origin mode: 1 none, 2 frame centre, other (0 = default) frame top-left | Ctor 0, Init 0, SetOriginMode | SetOriginMode, DrawSprite, AddTexSprite |
| +0x120 | s32 | X offset (1/16 px) added by the A+D XYZ setters | Ctor, Init, SetOriginMode | SetXYZ* |
| +0x124 | s32 | Y offset (1/16 px) | same | same |
| +0x128 | s32 | `env->cx << 4` | Init (0x13f850) | SetOriginMode, DrawSprite, AddTexSprite |
| +0x12C | s32 | `env->cy << 4` | Init | same |
| +0x130 | s32 | `env->w << 4` | Init | same |
| +0x134 | s32 | `env->h << 4` | Init | same |

Origin offsets (`+0x120/+0x124`), from SetOriginMode (0x13e2f0) and the
same inline formula in Init/DrawSprite/AddTexSprite:
- mode 1: `(0, 0)`: coordinates are raw GS primitive coordinates.
- mode 2: `(cx·16, cy·16)`: relative to the frame's centre.
- any other (0, the default): `(cx·16 − trunc(w·16/2), cy·16 − trunc(h·16/2))`
  = the draw env's XYOFFSET (for even w/h): relative to the frame's top
  left. Signed division truncates toward zero (0x13e328-0x13e334).

---------------------------------------------------------------------------

#### 3. GsEnv (embedded at `b+0x10`; also used standalone by 3D materials)

Layout (offsets relative to the GsEnv): `+0x00` GIF tag, then five A+D
qwords: `+0x10` ALPHA_1, `+0x20` TEST_1, `+0x30` ZBUF_1, `+0x40` FBA_1,
`+0x50` FRAME_1; `+0x5C` frame-mask mode. It is emitted only by
`GsEnvWritePacket` (from GsPktOpen, and from DrawListWriteTexEnvPacket on a
material's own GsEnv). **Changes to it after a segment's Open take effect at
the next Open**, never mid-segment.

##### `GsEnvInit_1573b0(env)` (164 bytes)
- `sq zero,0(env)`; tag NLOOP=5 (0x1573e0), EOP=1 (byte 1 bit 7,
  0x157408), NREG=1 (byte 7 high nibble, 0x157420), REGS0=0xE (A+D,
  0x157430), FLG=0 (PACKED). Address words: `+0x18`=0x42, `+0x28`=0x47,
  `+0x38`=0x4E, `+0x48`=0x4A, `+0x58`=0x4C (32-bit store; `+0x5C` is the
  mask mode). Data: `+0x10`=0x44, `+0x20`=0x5000B (both overwritten next).
  ZBUF/FBA/FRAME data low words are **not** initialized here.
- Tail-calls `GsEnvSetDefaults`.
- Callers: GsPktInit, MeshMaterialBuildPkt_14ac90 (3D), CameraRecInit_156940.

##### `GsEnvSetDefaults_1572d0(env)` (224 bytes)
- ALPHA = `*(u64*)0x449EF8` (preset 1, 0x44).
- TEST: ATE=0, ATST=1 (ALWAYS), AREF=0, DATE=0, DATM=0, ZTE=1, ZTST=1
  (ALWAYS). AFAIL untouched (0 from Init's 0x5000B).
- ZBUF.ZMSK (`+0x34` bit 0) = 0; FBA (`+0x40` bit 0) = 0; `+0x5C` = −1.

##### `GsEnvSetDefaultsMaybe_1571d0(env)` (256 bytes; 3D material path only)
- Same ALPHA; TEST ATE=1, ATST=5 (GEQUAL), AREF=1, DATE=DATM=0, ZTE=1,
  ZTST=2 (GEQUAL); FBA=0; `+0x5C`=−1; ZMSK untouched.

##### `GsEnvSetAlpha_157140(env, const u64 *alpha)`
- `env+0x10 = *alpha` (raw ALPHA_1 data).

##### `GsEnvSetBlendPreset_157150(env, s32 i)`
- If `0 <= i < 12`: `env+0x10 = ((u64*)0x449EF0)[i]`; else nothing.
- The table (ALPHA_1 = A[1:0] B[3:2] C[5:4] D[7:6], FIX[39:32]; A/B/D: 0 Cs,
  1 Cd, 2 zero; C: 0 As, 1 Ad, 2 FIX):

| i | value | A B C D, FIX | result |
|---|---|---|---|
| 0 | 0x80_0000002A | 0 0 FIX Cs, 0x80 | Cs (no blend) |
| 1 | 0x44 | Cs Cd As Cd | (Cs−Cd)·As + Cd — default |
| 2 | 0x48 | Cs 0 As Cd | Cs·As + Cd (additive) |
| 3 | 0x42 | 0 Cs As Cd | Cd − Cs·As (subtractive) |
| 4 | 0x80_00000064 | Cs Cd FIX Cd, 0x80 | (Cs−Cd)·1 + Cd = Cs |
| 5 | 0x4A | 0 0 As Cd | Cd (writes nothing visible) |
| 6 | 0x89 | Cd 0 As 0 | Cd·As |
| 7 | 0x46 | 0 Cd As Cd | Cd·(1−As) |
| 8 | 0x88 | Cs 0 As 0 | Cs·As |
| 9 | 0x80_00000068 | Cs 0 FIX Cd, 0x80 | Cs + Cd |
| 10 | 0x80_00000062 | 0 Cs FIX Cd, 0x80 | Cd − Cs |
| 11 | 0 | Cs Cs As Cs | Cs |

##### `GsEnvWritePacket_157190(env, dst)` → 6
- Six `lq/sq`: the tag and the five A+D qwords copied verbatim. Returns 6
  (qwords), which GsPktOpen adds to `+0xF4` (0x13f584).

##### The GsEnv field helpers (not GsPkt*, but they are what the setters call)

| Function | Effect on the GsEnv |
|---|---|
| `GsTestSetAlphaTest_157100(e, en)` | ATE = (en ≠ 0) |
| `GsTestSetAlphaFunc_1570c0(e, atst, aref)` | ATST = atst & 7, AREF = aref & 0xFF |
| `GsTestSetAlphaFail_1570a0(e, f)` | AFAIL = f & 3 (3D only) |
| `GsTestSetDate_156ff0(e, en)` | DATE = en & 1 |
| `GsTestSetDestAlphaMode_157010(e, m)` | m=0: DATE=0, DATM=0; m=1: DATM=0; other: DATM=1 |
| `GsTestSetZTest_156f40(e, m)` | ZTE=1 always; m=0 or 1: ZTST=1 ALWAYS; 2: GEQUAL; 3: GREATER; other: ZTST unchanged |
| `GsZbufSetZMask_156ee0(e, m)` | m=0: ZMSK=0 (Z written); m=1: ZMSK=1; other: unchanged |
| `GsZbufCopyBaseFmt_156ea0(e, zbuf*)` | ZBP and PSM from a ZBUF value, ZMSK kept (materials only) |
| `GsFbaSetEnable_156e80(e, en)` | FBA = en & 1 |
| `GsFrameSet_156e10(e, frame*)` | if `+0x5C == -1`: FRAME = *frame; else FRAME low word from *frame, FBMSK kept |
| `GsFrameSetMaskMode_156cc0(e, m)` | `+0x5C = m`; FBMSK: 0 → 0; 1 → 0xFF000000 (alpha kept); 2 → 0x00FFFFFF (only alpha written); 3 → 0x0000FFFF; 4 → 0xFFFFFFFF (nothing written); other: `+0x5C = -1`, FBMSK unchanged |

(The `mmi2` words in GsFrameSet/GsFrameSetMaskMode are PCPYLD; only mode 4's
one, 0x156de4, changes the low 64 bits: it builds the 0xFFFFFFFF mask.)

---------------------------------------------------------------------------

#### 4. The state a block starts with (`GsPktInit_13f7f0(b, stack, env)`)

In order (0x13f7f0-0x13f9d8):
1. `+0x00 = stack`, `+0x04 = 0`, `+0xE8 = 1`.
2. `NameTableNodeReset(b+0x78)`: TEX0 = TEX1 = 0, CLAMP = WMS 1 / WMT 1
   (CLAMP), other node fields zeroed.
3. `GsEnvInit(b+0x10)` (§3).
4. ZBUF_1 = `env+0x90` (whole 64 bits, `DrawEnvGetZbufReg_157770`).
5. FRAME_1 = `env+0x80` (whole, mask mode is −1).
6. `+0x128..+0x134` = cx, cy, w, h ×16; origin mode 0 (top left) with its
   offsets (the inline copy of SetOriginMode(0)).
7. PRIM: ABE=0 (0x13f90c); later TME=0, FST=1, IIP=0, AA1=0, FGE=0. **Type
   bits 0-2, CTXT and FIX are not touched** (type stays from the last use;
   CTXT/FIX are 0 since the ctor's 0x100 and nothing sets them).
8. ATE=1 (with ATST still ALWAYS); DATE=0; ZTE=1, ZTST=GEQUAL; `+0xE4=1`,
   ZMSK=1; FBA=0; `+0xE0=1` (bilinear).

So a fresh block's env registers are:
- ALPHA_1 = 0x44; TEST_1 = 0x50003 (ATE 1, ATST ALWAYS, ZTE 1, ZTST GEQUAL);
- ZBUF_1 = the env's with ZMSK=1; FBA_1 = 0; FRAME_1 = the env's (with its
  FBMSK);
- PRIM = type | FST (0x100 | type).

Not reset by Init: `+0x110` (Q), `+0x114`, `+0x118`, `+0xEC..+0x108`,
`+0xF0` (Begin zeroes it).

`GsPktCtorMaybe_13fb80(b)` (StaticInit only): NameTableNodeCtor(b+0x78),
`+0=0`, `+0xE8=1`, `+0x110=1.0f`, `+0x70=0x100`, `+0xE0=1`, `+0xE4=0`,
`+0x11C=+0x120=+0x124=0`; returns b.

`GsPktBegin_13f7e0(b)`: `+0x04 = 1`, `+0xF0 = 0`.

`GsPktEnd_13f790(b)` → `+0xF0`: `+0x04 = 0`; `q = MemStackAllocQw(+0, 1)`;
`q = {0x60000000, 0, 0, 0}` (RET); returns `+0xF0`. The RET is written even
when nothing was opened (then nothing calls it).

---------------------------------------------------------------------------

#### 5. Framing functions

##### `GsPktOpen_13f4a0(b)` (260 bytes)
- `+0xE8 = 1`; if `+0x00 == 0` return. `+0xE8 = 0`.
- `+0xEC = stack->base + stack->top*16`; if `+0xF0 == 0`: `+0xF0 = +0xEC &
  0x0FFFFFFF`. `+0xF4 = +0xEC`.
- Writes (0x13f51c-0x13f578):
  - `{0x10000009, 0, 0, 0x50000009}`: CNT qwc 9; VIF NOP; VIF DIRECT 9.
  - GIF tag `{0x00008002, 0x10000000, 0x0000000E, 0}`: NLOOP 2, EOP,
    PACKED, NREG 1, A+D. `+0x100` = its address.
  - A+D `(0, 0x3F)` TEXFLUSH; A+D `(1, 0x1A)` PRMODECONT.AC = 1.
  - `GsEnvWritePacket(b+0x10, …)`: env tag + ALPHA_1, TEST_1, ZBUF_1,
    FBA_1, FRAME_1 (values as they are in the builder now).
- `+0xF4` += 6·16. Does not touch `+0xF8/+0xFC/+0x104/+0x108/+0x114`.
- Callers (17): GsPktOpenPrim, shadow passes, ScreenTexOverlayDraw*,
  ModelDrawOffscreenComposite, ScreenFadeBlockDraw.

##### `GsPktOpenPrim_13f730(b, s32 type)` (96 bytes; 223 callers)
- `+0xE8 = 1`; if `+0x00 == 0` return (type not stored). `+0xE8 = 0`;
  PRIM type = `type & 7`; `+0x110 = 1.0f`; `GsPktOpen(b)`;
  `GsPktBeginPrimAD(b)`.
- Types passed (a1 constant at the call, scanned over all 223 sites): 6
  SPRITE at 102 sites; 4 TRISTRIP 29 (menus, loading screen, minimap,
  trails, DofFilter, sky band); 3 TRIANGLE 12 (HP gauges, minimap,
  effect sprites, sun light); 5 TRIFAN 5 (sun flare/glare/light, clipped
  Gouraud quad, rotated quad); 1 LINE 8 (debug wire shapes, cloth debug, guard effect);
  2 LINESTRIP 7 (door cursor, save-menu frames, GfxEndFrame, ShadowMapBlur);
  0 POINT 4 (`daddu a1,zero,zero`: AttackChainDraw ×2, TaskDrawMonography,
  TaskDrawStatus — whether those really draw points is open); 4 sites pass
  a register (DebugMenuWidgetDrawWindow, DrawListEndPlaneShadowPass ×2,
  HpGaugeDrawBar).

##### `GsPktBeginPrimAD_13f6b0(b)` (124 bytes)
- `+0xF8 = +0xFC = +0xF4`; a zeroed qword (the CNT tag to patch);
  `+0x104` = it, `+0x108` = it + 0xC.
- `+0x100` = next qword: GIF `{0x8000, 0x10000000, 0xE, 0}` (NLOOP 0 to
  patch, EOP, PACKED, NREG 1, A+D).
- A+D `(b+0x70 as u64, 0x00)`: **PRIM**. `+0xF4` advanced 3 qwords.

##### `GsPktSetPrimType_13f470(b, s32 type)` (40 bytes)
- `+0x114 = 0`; type bits = `type & 7`; `+0x110 = 1.0f`; tail-calls
  BeginPrimAD. **Does not finalize the tag already open**: callers call
  GsPktFinalizeTags first, or call it right after GsPktOpen (whose header
  needs no patch).
- Callers: ScreenTexOverlayDrawMulti/Single, ScreenFadeBlockDraw.

##### `GsPktOpenGifTag_13f3b0(b, s32 type, u32 regsLo, u32 regsHi, s32 nreg)` (180 bytes) — PACKED
- `+0x114 = 1`; type bits = `type & 7`; `+0x110 = 1.0f`; `+0xF8 = +0xFC =
  +0xF4`; zeroed CNT qword (`+0x104`, `+0x108` as above); `+0x118 = nreg`;
  `+0x100` = the GIF tag: `w0 = 0x8000` (NLOOP 0 to patch, EOP), `w1 =
  nreg<<28 | (PRIM & 0x7FF)<<15 | 0x4000` (FLG 0 PACKED, PRE 1, PRIM = the
  whole image incl. type), `w2 = regsLo`, `w3 = regsHi`. (0x13f414-0x13f450)
- Callers: AddTexSprite (regs 0x4343 n4; 0x43431 n5), DrawSprite (0x441
  n3), TileScreenSprites (0x43 n2), ScreenFadeBlockDrawWipeStrips_177a70
  (TRISTRIP, 0x43 n2).

##### `GsPktFinalizeTags_13f260(b)` (336 bytes)
- `qwc = (F4−F8)/16 − 1`, `vif = (F4−FC)/16 − 1`, `gif = (F4−[+0x100])/16 −
  1` (signed /16 truncating).
- If `qwc <= 0 || vif <= 0`: `F4 = F8` (drops the empty tag) and return.
- `*[+0x104] = qwc | 0x10000000`; `*[+0x108] = vif | 0x50000000`;
  `*[+0x100] (w0) = (+0x114 ? gif / +0x118 : gif) | 0x8000`.
- No TEXFLUSH, no allocation, `+0x114` left as is.

##### `GsPktClose_13f5b0(b)` (252 bytes; 221 callers)
- If `+0xE8 != 0` return.
- A+D `(0, 0x3F)` TEXFLUSH; then the same patch as FinalizeTags but always
  with the A+D NLOOP (`gif | 0x8000`; ignores `+0x114`); roll back to F8
  when empty; then `MemStackAllocQw(+0, (F4−EC)/16)`.
- Leaves `+0xE8 = 0` and `+0xEC` unchanged (a second Close would append
  and allocate again).
- Convention in the game: OpenPrim … Close (A+D path); Open → SetPrimType
  … FinalizeTags [→ OpenGifTag … FinalizeTags] → Commit (packed path).
  Close after an OpenGifTag would corrupt the packed tag (it is never done).

##### `GsPktCommit_13f190(b)` (68 bytes)
- If `+0xE8 == 0`: `MemStackAllocQw(+0, (F4−EC)/16)`. Nothing else.

##### `GsPktReserveQwords_13f230(b, n)` → old `+0xF4` (20 bytes)
- `v0 = +0xF4`; `+0xF4 += n*16`. Used as `p = Reserve(0)`, write raw,
  `Reserve(7)` (ScreenTexOverlayWriteSpriteMaybe, §9).

##### `GsPktPushQword_13f170(b, const u128 *q)`
- `*(u128*)F4 = *q; F4 += 16` (raw; the data qwords of a PACKED tag).

##### `GsPktAddDmaCallTag_13f1f0(b, u32 addr)`
- `*(u128*)F4 = {0x50000000, addr & 0x0FFFFFFF, 0, 0}` (DMA CALL, qwc 0);
  F4 += 16. Only caller: ModelDrawOffscreenCompositeMaybe (0x16b180) with
  0x445C60 (§9).

##### `GsPktRawWriteBeginStubMaybe_13f250` / `GsPktRawWriteEndStubMaybe_13f1e0`
- `jr ra; nop`. They bracket raw writes (ScreenTexOverlayWriteSprite,
  ModelDrawOffscreenComposite); good hook points to know a raw stretch
  starts and ends.

---------------------------------------------------------------------------

#### 6. State-only setters (no packet bytes)

PRIM image bits (`b+0x70`, taken at the next BeginPrimAD / OpenGifTag /
AddTexSprite; the PRIM already written in the open tag is not changed):

| Function | Args | Effect |
|---|---|---|
| `GsPktSetGouraud_13e3b0` | (b, en) | IIP = en & 1 |
| `GsPktSetTextured_13e400` | (b, en) | TME = en & 1 |
| `GsPktSetFog_13e370` | (b, en) | FGE = en & 1 (one caller: SunFlareDrawQuadsMaybe 0x17b59c) |
| `GsPktSetAlphaBlend_13e500` | (b, en) | ABE = en & 1 |
| `GsPktSetAntialias_13e390` | (b, en) | AA1 = en & 1 |
| `GsPktSetUseUV_13e3e0` | (b, en) | FST = en & 1 (1: UV, 0: STQ) |

GsEnv (taken at the next GsPktOpen); each tail-calls the helper of §3 on
`b+0x10`:

| Function | Args | Effect |
|---|---|---|
| `GsPktSetBlendMode_13e4f0` | (b, preset) | ALPHA_1 = table[preset] if 0..11 |
| `GsPktSetAlphaReg_13e4e0` | (b, const u64 *alpha) | ALPHA_1 = *alpha |
| `GsPktEnableAlphaTest_13e4d0` | (b, en) | ATE = en ≠ 0 |
| `GsPktSetAlphaTestFunc_13e4c0` | (b, atst, aref) | ATST, AREF |
| `GsPktSetDestAlphaTest_13e4a0` | (b, en) | DATE |
| `GsPktSetDestAlphaMode_13e4b0` | (b, m) | DATM (m=0 also clears DATE) |
| `GsPktSetZTest_13e450` | (b, m) | ZTE=1; 0/1 ALWAYS, 2 GEQUAL, 3 GREATER |
| `GsPktEnableZTest_13e460` | (b, en) | en=0: ZTest(0) ALWAYS; else ZTest(2) GEQUAL |
| `GsPktSetZWriteMask_13e430` | (b, m) | `+0xE4 = m`; ZMSK per GsZbufSetZMask (0 writes Z, 1 not) |
| `GsPktSetFrameMask_13e420` | (b, mode) | FBMSK per GsFrameSetMaskMode; `+0x6C = mode` |

Others:

| Function | Args | Effect |
|---|---|---|
| `GsPktSetTexFilter_13e3d0` | (b, f) | `+0xE0 = f` (used by the next SetTexture only) |
| `GsPktSetOriginMode_13e2f0` | (b, m) | `+0x11C = m`; `+0x120/+0x124` per §2 |
| `GsPktDisableTests_13f9e0` | (b) | ZTE=1 ZTST ALWAYS; `+0xE4=1`, ZMSK=1; ABE=0; ATE=0; DATE=0; TME=0 |
| `GsPktSetDefaultState_13fa80` | (b) | ABE=0; ATE=1; DATE=0; ZTST GEQUAL; `+0xE4=1` ZMSK=1; FBA=0; TME=0; FST=1; `+0xE0=1`; IIP=0; AA1=0; FGE=0 (Init's tail; ALPHA/FRAME/origin untouched) |
| `GsPktSetup2DState_26fdc0` | (b; returns if b == 0) | ABE=1; preset 1; ATE=1; ATST=6 GREATER, AREF=1; ZTST ALWAYS; ZWriteMask(0) → ZMSK=0, `+0xE4=0`; IIP=0; TME=1; `+0xE0=0` nearest; AA1=1. Callers: FontDrawText*, MesWinDraw* |
| `GsPktSetupUiDrawState_2932b0` | (b; no null check) | preset 1; ABE=1; AA1=0; ATE=1 (ATST as it was); ZTST ALWAYS; TME=1; FST=1; origin 0; ZMSK=1; IIP=0. Callers: UiDrawWidgets, SaveMenu* |
| `GsPktSetup2DState_3892a0` | (b; no null check) | ABE=1; AA1=0; ATE=1; ZTST ALWAYS; TME=1; FST=1; origin 1 (raw GS coords); `+0xE0=1`; ZMSK=1; IIP=0 (ALPHA not set). Callers: InsectCage* |

Note `GsPktSetup2DState_26fdc0` leaves Z writes **on** (ZMSK=0) with Z test
ALWAYS (0x26fe18: `SetZWriteMask(b, 0)`).

---------------------------------------------------------------------------

#### 7. A+D emitters

Each writes one qword `{u64 data, u64 addr}` at `+0xF4` and adds 0x10,
unless said otherwise. **No value is masked**: fields are sign-extended to
64 bits and ORed (e.g. a negative X sets all bits above it, so Y and Z of
that XYZ2 become 0xFFFF / 0xFFFFFFFF). A hook that reproduces the GS view
must apply the same OR, or at least treat negative/oversized inputs as the
GS would see them.

| Function | Args (registers) | Units | Qword(s) |
|---|---|---|---|
| `GsPktWriteAD_13ec80` | a1 = reg (u64), a2 = data (u64) | raw | `(a2, a1)` |
| `GsPktSetTexa_13eca0` | a1 = u64 | raw TEXA | `(a1, 0x3B)` |
| `GsPktSetTexture_13ec00` | a1 = texture node* (may be 0) | — | if a1: NameTableCopyNode(b+0x78, a1) (13 u64); GsTex1SetFilter(b+0x78, +0xE0); then `(0, 0x3F)`, `(b+0xB8, 0x14)` TEX1_1, `(b+0xB0, 0x06)` TEX0_1, `(b+0xC0, 0x08)` CLAMP_1; F4 += 0x40. a1 = 0: nothing at all |
| `GsPktSetST_13ed10` | f12 = s, f13 = t, f14 = q | floats | `(bits(s) \| bits(t)<<32, 0x02)`; `+0x110 = q` |
| `GsPktSetSTVec_13ecc0` | a1 = float[3] {s, t, q} | floats | same |
| `GsPktSetUV_13ed60` | a1 = u, a2 = v (s32) | texels | `(u<<4 \| (v<<4)<<16, 0x03)` |
| `GsPktSetUVFixed_13ed90` | a1 = u, a2 = v (s32) | 1/16 texel | `(u \| v<<16, 0x03)` |
| `GsPktSetRGBAQ_13ee40` | a1 r, a2 g, a3 b, t0 a (s32) | 0..255, 0x80 = 1 | `(r \| g<<8 \| b<<16 \| a<<24 \| bits(+0x110)<<32, 0x01)` |
| `GsPktSetRGBAQVec_13edd0` | a1 = float[4] {r,g,b,a} | floats, VFTOI0 (truncate) | as SetRGBAQ |
| `GsPktSetXYZ_13ef10` | a1 x, a2 y, a3 z (s32) | 1/16 px | `((x+ox) \| (y+oy)<<16 \| z<<32, 0x05)` |
| `GsPktSetXYZVecI_13eeb0` | a1 = s32[3] {x,y,z} | 1/16 px | same |
| `GsPktSetXYZPixel_13f110` | a1 x, a2 y, a3 z (s32) | pixels | `(((x<<4)+ox) \| ((y<<4)+oy)<<16 \| z<<32, 0x05)` |
| `GsPktSetXYZPixelVec_13ef70` | a1 = s32[3] | pixels | same |
| `GsPktSetXYZf_13f060` | f12 x, f13 y, f14 z | float px | x, y by VFTOI4 (×16, truncate), z by VFTOI0; then as SetXYZ |
| `GsPktSetXYZfVec_13efe0` | a1 = float[4] {x,y,z,_} | float px | same (cop2 0x4b8a517d = VFTOI4.xy, 0x4a4a517c = VFTOI0.z) |

`ox, oy` = `+0x120, +0x124`. All XYZ go to XYZ2 (vertex kick); none of the
builder's functions writes XYZ3, XYZF2/3, FOG, FOGCOL, PRMODE, PABE,
COLCLAMP, DTHE, XYOFFSET.

RGBAQ's Q is `+0x110` at the time of the call: 1.0 after the last
OpenPrim/SetPrimType/OpenGifTag, or the q of the last SetST/SetSTVec.

Registers that callers write through `GsPktWriteAD` (all 28 sites, scanned
for the a1 constant): SCISSOR_1 0x40 (scissor helpers, HpGauge*, Mes*,
Menu*/TextClip scissor callbacks, FontSetScissor), TEXA 0x3B (DofFilter,
ParticleSwirl, ScreenFadeBlockDraw ×3, SunFlareDrawQuads, TexAnime),
TEXFLUSH 0x3F (TexAnimeTableApply).

---------------------------------------------------------------------------

#### 8. Composite helpers

##### `GsPktSpriteUVRect_26eb30(b, s32 rect[4], s32 uv[4], s32 z)` (240 bytes)
- rect = {x, y, w, h} in pixels, uv = {u, v, w, h} in texels (loaded with
  `lwc1` but used as ints).
- Emits: `SetUV(u, v)`, `SetXYZPixel(x, y, z)`, `SetUV(u+w, v+h)`,
  `SetXYZPixel(x+w−1, y+h−1, z)`: four A+D qwords (UV, XYZ2, UV, XYZ2).
  The XY end is inclusive (−1), the UV end is not.
- The caller has opened a SPRITE. Callers: FontDrawGlyph_26de80,
  FontDrawGlyphOutline_26e450.

##### `GsPktSpriteSplitU_17adf0(b, x0 a1, y0 a2, x1 a3, y1 t0, u0 t1, v0 t2, u1 t3, v1 [sp+0])` (356 bytes)
- All in 1/16 units (pixels and texels).
- `du = u1−u0`, `n = du/512 (trunc) + (du % 512 != 0)` (512 = 32 texels).
- For i in 0..n−1: `SetUVFixed(u0 + i·du/n, v0)`, `SetXYZ(x0 + i·dx/n, y0,
  0)`, `SetUVFixed(u0 + (i+1)·du/n, v1)`, `SetXYZ(x0 + (i+1)·dx/n, y1, 0)`
  (integer divisions, `dx = x1−x0`). Z is 0.
- du ≤ 0: n = 0 (nothing), except −511 ≤ du < 0 gives n = 1 (one strip).
- Callers: GlareFilterDrawMaybe only (8 sites).

##### `GsPktDrawSprite_13ea30(b, const s32 rec[12])` (464 bytes) — PACKED
- `OpenGifTag(b, 6 SPRITE, 0x441, 0, 3)`: REGS RGBAQ, XYZ2, XYZ2.
- Writes `rec[0..3]` (RGBA, one s32 each; Q from the GS's current Q) as is;
  then xyz0 = `rec[4..7]`, xyz1 = `rec[8..11]` (x, y 1/16 px; z; the 4th word
  carries ADC in bit 15), x/y adjusted by the origin (mode 1 raw copy;
  mode 2 `+cx16/+cy16`; else `+cx16 − w16/2`, `+cy16 − h16/2`).
- `FinalizeTags` (NLOOP 1). Caller commits.
- Callers: ShadowMapRender_146680, DrawListEndPlaneShadowPass_146cc0,
  DrawListBeginPlaneShadowPass_147b80.

##### `GsPktAddTexSprite_13e520(b, const rec*)` (1284 bytes) — A+D then PACKED
Record: `+0x00` RGBA s32[4]; `+0x10` xyz0 s32[4]; `+0x20` xyz1 s32[4];
`+0x30` split flag; `+0x40` texture node*; `+0x50` uv0 s32[4] (u, v in 1/16
texel); `+0x60` uv1 s32[4].

Path A, when `rec+0x30 != 0` and `x0−x1 == u0−u1` (1:1 texels):
1. `+0x114=0`, type 6, Q=1.0, BeginPrimAD (PRIM), SetTexture(rec+0x40)
   (TEXFLUSH, TEX1, TEX0, CLAMP), A+D RGBAQ from rec[0..3] with Q 1.0
   (0x13e594-0x13e5d8), FinalizeTags.
2. `OpenGifTag(b, 6, 0x4343, 0, 4)`: REGS UV, XYZ2, UV, XYZ2.
3. Origin-adjusted xyz0/xyz1 (as DrawSprite). For `x = x0, u = u0` while
   `x < x1`, step 0x200 (32 px / 32 texels): qwords `uv0' = {u, uv0.v,
   uv0.w2, uv0.w3}`, `xyz0' = {x, xyz0.y, …}`, `uv1' = {min(u+0x200, u1),
   uv1.v, …}`, `xyz1' = {min(x+0x200, x1), xyz1.y, …}` (0x13e790-0x13e7fc).
4. FinalizeTags (NLOOP = strips).

Path B otherwise:
1. Same as A.1 but **without** the RGBAQ A+D (0x13e814-0x13e84c).
2. `OpenGifTag(b, 6, 0x43431, 0, 5)`: REGS RGBAQ, UV, XYZ2, UV, XYZ2.
3. Qwords: `rec+0x00`, `rec+0x50`, adjusted xyz0, `rec+0x60`, adjusted
   xyz1. FinalizeTags (NLOOP 1).

Callers: ShadowMapBlur_145b60, DrawListEndPlaneShadowPass_146cc0 (each
followed by GsPktCommit).

##### `GsPktTileScreenSprites_177cf0(b)` (480 bytes) — PACKED
- `OpenGifTag(b, 6, 0x43, 0, 2)`: REGS UV, XYZ2.
- `L = DrawEnvGetLeft(ctx)·16` (`cx − trunc(w/2)`), `T =
  DrawEnvGetTop(ctx)·16`, from `GetDrawCtx()`'s env, **not** from the
  builder's origin (differs from origin mode 0 by 8 for an odd w or h).
- For x in 0, 64, … < w; y in 0, 32, … < h (pixels): PushQword UV `{x·16,
  y·16, 0, 0}`, XYZ2 `{x·16+L, y·16+T, 0, 0}`, UV `{(x+64)·16, (y+32)·16,
  0, 0}`, XYZ2 `{(x+64)·16+L, (y+32)·16+T, 0, 0}`. Tiles past the edge are
  not clipped.
- FinalizeTags. Callers: ScreenFadeBlockDraw (0x1777dc, 0x177850), after
  its own Open/SetPrimType/SetTexture/RGBAQ/FinalizeTags; then Commit.

##### `GsPktDrawLine3D_1cca80(b, float p0[3], float p1[3], float rgba[4])`
- `ProjectToScreen_15d340` on both (matrix `*(0x4810A4)+0xF20 → +0x1C0`;
  out = s32 {x·16, y·16, z, 0}, nonzero when inside 0..0xFFF and the depth
  range). If both visible: SetRGBAQVec(rgba), SetXYZVecI(p0'),
  SetXYZVecI(p1') (the builder offset is added on top). Returns the last
  callee's v0 (meaningless). Callers: EvMapObjDebugDraw,
  EvMapObjNodeDrawAxes (debug).

##### `GsPktSetScissor_28c2e0(b, s32 rect[4], s32 enable, s32 ofs[4] or 0)` (332 bytes)
- b == 0: nothing.
- enable == 0: `OpenPrim(b, 6)`; `WriteAD(0x40, (w−1)<<16 | (h−1)<<48)` (w, h
  of `GetDrawCtx()`'s env: 0..w−1, 0..h−1); `Close(b)`.
- enable ≠ 0: `r = rect + ofs` (component-wise, ofs may be 0);
  `OpenPrim(b, 6)`; `WriteAD(0x40, r.x | (r.x+r.w)<<16 | r.y<<32 |
  (r.y+r.h)<<48)` (inclusive end, no −1; unmasked); `Close(b)`.
- Side effects: a full segment (header with the env registers, PRIM =
  SPRITE with the current bits, SCISSOR, TEXFLUSH); PRIM type becomes 6
  and Q 1.0.

##### `GsPktSetScissorDefault_28c2d0(b, rect, enable)`
- Tail-calls SetScissor with `ofs = 0x47EB50` = {3, 3, 1, 1}.

##### `GsPktSetScissorFull_26fe60(b)`
- b == 0: nothing. `OpenPrim(b, 6)`, `WriteAD(0x40, (w−1)<<16 | (h−1)<<48)`,
  `Close(b)`.

##### `GsPktSetScissorRows_26ff00(b, s32 y0, s32 y1)`
- b == 0: nothing. `OpenPrim(b, 6)`, `WriteAD(0x40, (w−1)<<16 | y0<<32 |
  y1<<48)`, `Close(b)`. Caller: MesWinDraw_270cb0.

The scissor lasts until the block's end: DrawListEndPrim's env packet
writes SCISSOR_1 back.

---------------------------------------------------------------------------

#### 9. Writes that are not A+D qwords (the ones that matter most)

1. **PACKED GIF tags** (`GsPktOpenGifTag`, PRE=1 with the PRIM image):
   - `GsPktDrawSprite`: {RGBAQ, XYZ2, XYZ2} × 1, SPRITE.
   - `GsPktAddTexSprite`: {UV, XYZ2, UV, XYZ2} × strips, or
     {RGBAQ, UV, XYZ2, UV, XYZ2} × 1, SPRITE (after an A+D tag with PRIM,
     the texture registers and maybe RGBAQ).
   - `GsPktTileScreenSprites`: {UV, XYZ2} pairs, SPRITE.
   - `ScreenFadeBlockDrawWipeStrips_177a70` (not GsPkt*, uses OpenGifTag +
     PushQword): {UV, XYZ2} pairs, TRISTRIP (type 4), 17 iterations of 2
     vertices (0x177cac: `slti v0,s0,0x11`).
   In PACKED mode RGBAQ takes R/G/B/A from bits 0/32/64/96 of the qword
   (Q from the GS's internal Q, not `+0x110`); UV from bits 0 and 32;
   XYZ2 X bits 0-15, Y 32-47, Z 64-95, ADC bit 111.
2. **Raw qwords via `GsPktPushQword`** (the packed data above).
3. **A VU1 program call via `GsPktReserveQwords`**:
   `ScreenTexOverlayWriteSpriteMaybe_16a890` writes 7 qwords after a
   `ReserveQwords(0)` (0x16a914) and `ReserveQwords(7)` (0x16aa84):
   `{CNT 6 | STCYCL 1,1 (0x01000101) | UNPACK V4-32 num 5 addr 0x8C
   (0x6C05008C)}`, 5 data qwords (rect /16 as floats, centre offsets,
   texel offsets, the sprite GIF tag `{0x8001, 0x40000000, 0x4343, 0}` =
   NLOOP 1, EOP, NREG 4, UV XYZ2 UV XYZ2), then `{MSCAL 0x12C
   (0x1400012C), 0, 0, FLUSH (0x11000000)}`. Quads 140/141 z, w lanes are
   left as whatever was in memory. This is the VU1 overlay program already
   described in docs/NATIVE-ONLY-NOTES.md (`ModelDrawOffscreenComposite`
   section and "the reader of quads 98/99"). Callers:
   ScreenTexOverlayDrawMulti/Single, inside segments built as Open →
   SetPrimType(6) → SetTexture → SetRGBAQ → FinalizeTags → WriteSprite →
   Commit.
4. **A DMA CALL via `GsPktAddDmaCallTag`**:
   `ModelDrawOffscreenCompositeMaybe` 0x16b168-0x16b190: Open (header),
   CALL 0x445C60, Commit. 0x445C60 is `{RET qwc 0x1E | NOP | MPG num 0x3B
   addr 0x12C (0x4A3B012C)}`: the overlay microprogram's upload.
5. `GsPktWriteAD` with an arbitrary register (only 0x40/0x3B/0x3F are used).

Everything else in the builder is A+D (PACKED tags with REGS = 0xE).

---------------------------------------------------------------------------

#### 10. What callers read back (what a hook that skips originals must keep)

Return values used:
- `DrawListBeginPrim` → the builder (`ctx+0x130`): every caller keeps it.
- `GsPktEnd` → block address (`+0xF0`, 0 when no Open): DrawListEndPrim and
  the six inline copies turn it into a CALL tag (no CALL when 0).
- `GsPktReserveQwords` → the write pointer: ScreenTexOverlayWriteSprite
  writes 7 qwords there, then advances with `Reserve(7)`; GsPktCommit then
  allocates `(F4−EC)/16`. So `+0xF4` and `+0xEC` must be coherent for
  these raw writers (or they must be kept on the original path).
- `GsEnvWritePacket` → 6 (GsPktOpen advances by it; DrawListWriteTexEnvPacket
  sizes its packet with it).
- `GsPktCtorMaybe` → b (static init).
- All other GsPkt* return nothing useful.

Builder fields read by callers directly: none found. A scan of every
function that calls a GsPkt* function, for loads/stores at the builder's
field offsets through the registers they pass as a0 (and a broader scan
for offsets 0xE0-0x134 on any base), found only unrelated structures and
stack arrays. Callers read the frame's size/centre through the DrawEnv
getters (`DrawEnvGetWidth/Height/CenterX/CenterY/Left/Top`), not from
`+0x128..+0x134`. (Heuristic; a pointer stored and reloaded later would be
missed: open in principle.)

Builder state that later GsPkt calls depend on (keep coherent if any
original still runs after a skipped one):
- `+0x70` PRIM image (BeginPrimAD, OpenGifTag), the GsEnv (Open), `+0xE0`
  (SetTexture), `+0x110` Q (RGBAQ), `+0x11C..+0x134` (XYZ setters,
  DrawSprite, AddTexSprite), `+0xE8/+0xEC/+0xF0/+0xF4..+0x118` (framing),
  `+0x00` (whether anything is written at all).
- State persists across blocks only where Init does not reset it: PRIM type
  bits, Q (`+0x110`), `+0x114/+0x118`.
- The memstack top: the game's own allocations (DrawListEndPrim's env
  packet, texture loads) assume Close/Commit allocated exactly what the
  segments wrote.

When the snapshot happens (for producing records):
- Env registers (ALPHA_1, TEST_1, ZBUF_1, FBA_1, FRAME_1): at GsPktOpen
  (OpenPrim, SetScissor*). A setter called after Open applies only from
  the next Open.
- PRIM: at BeginPrimAD (OpenPrim, SetPrimType, AddTexSprite) and
  OpenGifTag.
- TEX0/TEX1/CLAMP: at SetTexture (filter from `+0xE0` at that moment).
  A block that never calls SetTexture uses whatever TEX0 the GS had (not
  restored by the env packet).
- TEXA: whenever SetTexa / WriteAD(0x3B) is called; never restored.
- SCISSOR_1: SetScissor*/WriteAD(0x40); restored at the block's end.
- Q: at each SetRGBAQ(Vec).
- XYZ offsets: at each XYZ setter (`+0x120/+0x124` at that moment).

---------------------------------------------------------------------------

#### 11. Hazards for a hook

- If `+0x00` is 0 (no memstack), Open/OpenPrim write nothing and leave
  `+0xF4` stale, but every A+D setter still writes through `+0xF4` (into
  an old packet). Close/Commit do nothing. End would call
  `MemStackAllocQw(0, 1)`.
- `MemStackAllocQw` returns 0 when the stack is locked (`+0x20`) or full;
  GsPktEnd then writes its RET to address 0.
- SetPrimType does not close the previous tag; OpenPrim does not reset
  `+0x114`; Close ignores `+0x114` and always appends TEXFLUSH.
- A segment's bytes are written at the memstack top before they are
  allocated; any allocation between Open and Close/Commit would be
  overwritten.
- BeginPrim nests and resets the shared builder (§1).
- A+D values are not masked (§7); PACKED XYZ2 is (16-bit fields).
- Origin mode 0 vs DrawEnvGetLeft/Top differ by half a pixel for odd sizes
  (TileScreenSprites vs everything else).

---------------------------------------------------------------------------

#### 12. Open

- Whether anything reads builder `+0x04` (the "in a block" flag): no GsPkt
  function does.
- DrawEnv `+0xC0..+0xF0` naming rests on the libgs order (`sceGsDrawEnv1`:
  FRAME, ZBUF, XYOFFSET, SCISSOR, PRMODECONT, COLCLAMP, DTHE, TEST_1) and on
  the copy in GfxSetVideoMode. If it holds, `DrawEnvSetFrame_1575e0` clears
  bit 0 of `+0xC0` (0x157708-0x15775c), i.e. PRMODECONT.AC = 0 in every env
  restore packet after it (RGDecomp calls the field DTHE); GsPktOpen writes
  PRMODECONT = 1 at the head of every segment, which would be why. Worth
  one runtime look at the env packet's `+0xC8` address word.
- The four `GsPktOpenPrim` sites whose type is a register, and the four
  that pass 0 (POINT), see §5.
- What VU1 program 0x12C draws exactly is in docs/NATIVE-ONLY-NOTES.md; not
  re-derived here.

---------------------------------------------------------------------------

(The side note this map ended with, that `native_blocks` read past
`GsPktEnd`'s RET and so read no block, was right; fixed on
`native-only/m4b-2d`.)
