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
