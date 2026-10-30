# A native renderer in place of the GS

A plan for retiring the GS emulation (local memory, page ownership, feedback
snapshots, readbacks, the register-level state machine) and drawing the game
with a renderer that knows what it is drawing: models with their matrices,
named effect buffers, and a 2D layer. Proposed 2026-10-28. Since then
phase 1 is done for every VU1 program the recordings hold
([VU1-NATIVE.md](VU1-NATIVE.md), 2026-10-29), phase 0's census of the
effects is in "Phase 0" below, and phase 3 has its first three effects: the
glare, the plane shadow and the per-object composites (2026-10-30; see
"Phase 3"). The rest is a plan.

## Why

The GS path works and is fast on a desktop card, but what it can do is capped
by what reaches it: screen-space vertices with fixed-point Z, textures known
only by a VRAM address, and effects drawn as hundreds of sprites that sample
the buffer they write. From [GS-PASS-GRAPH.md](GS-PASS-GRAPH.md) and
[GS-PERFORMANCE.md](GS-PERFORMANCE.md) (Salgin's first scene, 4x), and again
after the native VU1 programs and the GS audit ([GS-AUDIT.md](GS-AUDIT.md);
the walk's last 25 s, standing in the street, 60 fps mode, `RG_VU1_NATIVE=gpu`,
RX 6700 XT):

| Figure | 2026-10-28, 4x | 2026-10-29, 3x | 2026-10-29, 1x |
|---|---|---|---|
| Render passes a frame | 191, of which 2 are the 3D scene | 199 | 187 |
| Snapshot copies a frame (a copy of a target before a feedback draw) | 98 | 121 | 108 |
| Draws a frame | 805, 219 of them no VU1 program's | 644 | 630 |
| Texture transfers a frame | | 352 | 352 |
| GS worker thread | 66% of a core | 46% of the wall | 43% of the wall |
| VIF1 thread | 50-90% busy | 19% (no VU1 program runs) | 19% |
| GPU | 42-50 W of draw work | 66% busy at 1.2 GHz, 32 W | 54% busy at 500 MHz, 15 W |

The per-frame counts barely move between 1x and 3x: what the frame costs at a
low scale is the number of passes, copies and draws, not their pixels (GS-AUDIT's
"Bandwidth" section). That is the work a native renderer removes.

The goal in the ledger is an iGPU at 1080p, the Steam Deck ([FSR.md](FSR.md)) and Android handhelds. The features
people expect from a modern port all need data the GS never sees:

| Feature | What it needs | Blocked today by |
|---|---|---|
| Temporal AA / upscaling | jitter, per-object motion vectors, stable depth | vertices arrive projected; objects have no identity across frames |
| Resolution-independent post (bloom, DoF at their own size) | effects as passes with parameters | effects are sprite chains through aliased VRAM |
| UI at output resolution, 4:3 UI in widescreen | to know a draw is UI | UI and scene are the same GS sprites ("menus and text are stretched", CHANGELOG) |
| MSAA, anisotropic filtering, mip control | native targets and samplers | targets alias texture pages; CT32 viewed as T8 |
| Higher-resolution shadows, PCF | the shadow pass as a pass | the shadow map is a 64x64 / 128x128 GS buffer |
| Texture replacement packs | a stable texture identity | identity is a VRAM address (`gs_replacement_hash` is in flight) |
| Lower power on tile GPUs | few passes | 98 snapshot passes; about 470 MB of tile traffic from the blur alone (arithmetic) |

## What "dropping the GS" means here

- **Goes:** GS local memory as the source of truth (`gs_vram`, page ownership,
  imports, patches, round trips), feedback snapshots and destination copies,
  readbacks into EE memory, CLUT expansion from VRAM, the per-primitive
  register state machine for 3D and post.
- **Stays:** the recompiled game. The EE still builds its DMA chains; we hook
  functions and intercept at program starts, we don't rewrite game logic.
  SDL3 GPU stays the API (Windows, Linux, Android from one backend).
- **Shrinks:** the 2D path. About 4,800 `GsPkt*` call sites draw menus, the
  HUD and fades with GS register writes. They keep arriving as GS packets, but
  a thin translator turns them into a sprite batcher whose textures and
  targets are native objects, with no local-memory model behind them.

## Sources and trust

- **The oracle is this project's GS path.** It renders correctly, and its
  frame tools (`RG_GS_FRAMELOG`, `RG_GS_CAPTURE`, `RG_SELFTEST=gs`,
  byte-identical frames at 1x) are how every native piece is checked.
- **RGDecomp gives names and addresses only.** `functions_*.csv` derive from
  it, and every function this plan hooks is in them. Its descriptions of what
  code does (VU1 row meanings, effect parameters, the native port's design) are
  hypotheses to check here. Its own renderer is not correct yet.
- **Verified in this repo:** VU1 data memory holds projection at quads 8-11,
  view 24-27, model 60-63 and their product 72-75 at every draw start
  captured (`RG_VU1_CAPTURE`, `bc49172`). The microcode manifest
  (`build/vu1-manifest`) shows program starts at pairs 0x64, 0x12C, 0x21C,
  0x320, 0x4C4 and 0x73A. That is a six-slot layout whose pieces the game
  swaps per draw. Across 41 microcode images there are 53 distinct
  (entry, size) signatures, so about ten families of code (`tools/vu1/analyze.py`).
  A draw is matched to itself in the next frame through the model matrix's EE
  source address and the mesh shape (`capture_report.py`: 0.0% uncertain).

## Architecture

Three entry points feed one renderer.

```
EE (recompiled game)
 ├─ model draws ──► DMA chain ──► VIF1 thread ──► at MSCAL 0x64, ported program?
 │                                                 ├─ yes: NativeMesh (VU1 memory → vertex shader)
 │                                                 └─ no:  compiled VU1 → GIF → GS translator
 ├─ effect functions (hooked) ──► NativePass command, ordered by a MARK in the chain
 └─ GsPkt 2D draws ──► GIF ──► sprite batcher (native textures, named targets)
                                           │
                     frame graph: shadow → scene (+velocity) → TAA → post → UI → present
```

### Ordering

Everything must land in the order the game built it. Model draws already do:
they are intercepted on the VIF1 thread at the chain position of their
`MSCAL`. A hooked effect function doesn't call the original. It records its
command in a side table and appends a VIF `MARK` with the command's index to
the chain, so the VIF1 thread submits it at that position. (Phase 0 confirms
the game itself sends no `MARK`.)

### Models: intercept at the program start

The VIF1 thread already knows, at each `MSCAL 0x64`, which pieces are resident
(the program tags in `vif1_thread.cpp`) and what VU1 data memory holds:
matrices, lights and the unpacked vertex chunk. A ported composition becomes
a `NativeMesh` draw:

- **Inputs:** the vertex chunk as raw quadwords (a storage buffer), the
  matrices and light slots as constants, and the composition id. The vertex
  shader decodes the chunk the way the piece in each slot does. That means one
  shader function per piece and one pipeline per family, with the per-draw
  piece choices as specialization constants.
- **GS state:** texture, blend and test come from the context registers the
  chain's `DIRECT` packets set, as the GS path decodes them today, plus the
  GIF tag template the program would kick (read from fixed VU1 quads; to be
  mapped per family).
- **Outputs, two depths:** while GS-path draws share the target (phases 1-3),
  the shader writes the same integer Z the VU1 program computes, so mixed
  draws depth-test against each other. Once every depth consumer is native
  (phase 4), it switches to reversed float depth. Motion vectors come from the
  previous frame's matrices for the same object.

The alternative altitude is hooking `ModelPrepareDrawMaybe` or
`ModelEmitDrawPacketsMaybe` and reading model resources directly, which
would let static meshes live on the GPU. It is the better end state for CPU
cost, but it needs the model formats understood first. The program start
needs nothing but the VU1 memory layout, which we can capture and verify.
Phase 6 revisits it.

**Two steps per family, so features don't wait for CPU savings:**

1. *Replace the output:* the compiled program still runs, keeping every VU1
   side effect the next program reads (data memory, VI registers for an
   `MSCNT` continuation, the quads the base program stores). Its `XGKICK`
   output is discarded and the native draw goes in its place. This unlocks
   every GPU feature and costs no extra CPU.
2. *Skip the run:* a debug mode runs the program after the native draw and
   diffs the state the next start reads. A family whose state is reproduced
   exactly (or not read) stops running the program. This is where the VIF1
   thread's 35-56% goes away.

### Effects: hook the function, not the sprites

Each effect is an EE function with parameters. The native pass takes those
parameters and the scene's targets by name, never VRAM addresses. The GS stage
letters are from [GS-PASS-GRAPH.md](GS-PASS-GRAPH.md); which function makes
which is from phase 0's census below (field and battle, 2026-10-29), in the
order to port them:

| Function | Stage, per frame | Native form |
|---|---|---|
| `GlareFilterDrawMaybe_17a0d0` | A: 159 sprites, 120 of them feedback adds in the 192x192 `02c40` | downsample + separable blur at the buffer's own size. Parity form done: the game's sprites drawn by the renderer (`RG_NATIVE_GLARE=1`, off by default) |
| `ModelDrawOffscreenCompositeMaybe_16aac0`, with the VU1's depth-and-bounds strips and overlay sprites | E: about 20 objects, each cleared, drawn into `02a00` and blended back | done: object layers, a slot per object and the tails in one pass (`RG_NATIVE_COMPOSITE`, on by default) |
| `DrawListBeginPlaneShadowPass_147b80`, `DrawListEndPlaneShadowPass_146cc0`, with the VU1's shadow volumes | F: volumes into `02a00`, 48 feedback sprites of blur in `03800` | stencil volumes, a blur at the buffer's own size. Parity form done as the glare's (`RG_NATIVE_SHADOW=1`, off by default) |
| `TexAnimeTableApplyMaybe_162f50` | 174 sprites into texture pages the scene samples | native render-to-texture into named textures |
| `DofFilterDrawMaybe_17d150` | D: the frame to `02a00` and back (16 sprites in the field) | one or two passes reading native depth |
| `CBlurDrawMaybe_1784e0`, `CBlurTexBlurMaybe_1789e0`; `GfxBlitTexRectPixels_141710` (the display buffer into `02a40`) | small blur in `03f80`, full-frame blends | copy plus a sampled pass |
| `CharaDrawTranslucentPass_1f8250`, `RefractionCaptureFrame_13ffd0` | the frame copied to `03b80` | the scene target sampled directly |
| `BloomExtractBlurMaybe_179360`, `FocusFilterDrawMaybe_17c000` | C, D: the battle now and then; not seen | as the glare |
| `ShadowMapRender_146680`, `ShadowMapBlur_145b60` | not seen in the field or the battle | a shadow pass at a chosen size; blur or PCF |
| `LensFlareDraw_2ed830`, `DepthProbeGetSlot_13fc80` (8x8 Z reads in `GfxEndFrame_141c20`) | not seen | native depth sampled in a small compute pass, read a frame late |
| `ScreenColorFilterMaybe_179c60`, `ScreenFade*`, `ScreenFadeBlockCaptureFrame_177ed0` | fades (the battle's start) | full-screen tint or fade; capture kept on the GPU |
| `GfxFlipBuffersMaybe_143650`, and the game thread's 32-strip transfer at the frame's end | | a present from the scene target, no copy |

Each pass ships twice: a **parity** version that matches the GS path at 1x,
which is the default, and later an **enhanced** version (HDR intermediate,
resolution-scaled kernels) behind a menu option.

### Textures without local memory

TEX0 names a texture by base address, so something must still map addresses to
textures. It is an address table, not memory: per page, the latest upload
(decoded straight into a native texture keyed by a content hash, the scheme
`gs_replacement_hash` already uses) or the named target that last drew
there, with the view it was drawn as. CLUT loads resolve the same way. The
reinterpretations the game uses today (CT32 read as T8 for channel shuffles,
CT16 views of 0x3800) belong to effects. Once those effects are native, the
census should show none left; any that remain get an explicit case.

### 2D and UI

GS packets that no hooked function or VU1 program made go to a sprite
batcher. It reuses the GS path's color semantics (0x80 as 1.0, the integer
blend). It tags draws by the task that issued them (HUD, menu, fade) through
the same thread-local source tag the profile cycle uses for VU1 programs.
Tagged UI draws at output resolution, anchored to a 4:3 safe area in
widescreen.

## Phases

Each phase ends with a measurement on the reference scenes and leaves the game
fully playable. `RG_RENDER=gs` keeps the whole GS path selectable until phase
5. `RG_NATIVE=models,post,ui,...` turns the native categories on one at a
time, and a debug key flips them live on the same frame. (Planned; none of
the three exists yet. Today each piece has its own switch: `RG_VU1_NATIVE`,
`RG_NATIVE_GLARE`, `RG_NATIVE_SHADOW`, `RG_NATIVE_COMPOSITE`; see
[DEVELOPMENT.md](DEVELOPMENT.md).)

**Reference scenes:** Salgin's first scene, the town square (`TOWN=1
walk.sh`), a forced battle (battle bench), the boss gate, an event with an
item message and a battle dissolve (screen captures), the item menu, a movie.

### Phase 0: attribution and census (instrumentation only)

- Tag every draw with its origin: the VU1 composition (all six resident slot
  hashes, not just the entry), or the EE function on whose behalf a `DIRECT`
  packet was built. That means hooks around the effect functions above and the
  task boundaries, setting the source tag. Extend the `[frame]` dump and the
  profile cycle to group by it.
- Log the slot compositions a play session uses, with their draw and vertex
  counts. Map the ledger's heavy tags (`#6bcb`, `#e35a`, `#0970`) to
  compositions.
- Census, per frame: texture sources (upload, target, reinterpreted target,
  Z buffer as texture), readbacks by reason, CLUT sources, any `MARK` the game
  sends.
- Open questions to settle here: what stage E's per-object offscreen pairs
  are for (answered: a soft edge, see "Stage E, why offscreen"); whether a depth-probe read through `depth_reads.cpp` sees the depth
  the GPU drew (the GS backend never reads depth back, so local memory may hold
  a stale Z); what the screen overlay sprite (`ScreenTexOverlayWriteSpriteMaybe_16a890`,
  an `MSCAL` into slot B) relies on in VU1 memory.

**Done when** every draw and texture read in the reference scenes is
attributed. The output is a table in this document of families, functions and
cases, which becomes the work list for phases 1-4.

#### The effects census (2026-10-29)

`RG_DRAW_ORIGINS=FILE` (`src/debug/draw_origins.h`) names the guest function
behind every GS packet: it hooks the draw-list builders (a
`DrawListBeginPrim` to its `DrawListEndPrim`, `DrawListAdd*`,
`GfxBlitTexRect`, ...) and the frame's memory stack allocator, keeps which call
site wrote which range of the frame's packet memory, and joins that with the EE
address the VIF1 list took each `DIRECT` packet from. VU1 kicks are named by
their program. Per origin it counts primitives, sprites and their pixels, the
targets drawn and the textures read (primitives counted as the GS assembles them), reads of a render target (feedback when
it is the target being drawn, reinterpreted when in another format family),
and transfers. `tools/bench/origins_names.py` names the call sites and sums
them by function; `RG_DRAW_ORIGINS_SEQ=N` writes one frame's order.
`tools/bench/origins-run.sh` runs it on the File 01 walk (1x, 30 fps mode, VU1
programs on the VU1).

Two scenes so far: the Salgin streets after the walk (`field2`, 900 frames)
and the scripted battle with the Hooded Man that `RG_FORCE_ENCOUNTER=1`
starts on the same walk (`battle2`, 1,560 frames: the event before it and
the battle). 99.8% and 99.7% of primitives attributed; what is left is the
game thread's own GIF transfer at the end of each frame (32 strips of the
frame onto itself, likely the flip's copy) and a few packets a frame.

Per frame, field / battle. Block addresses are the GS's (base pointer x 64
words): `00000` the frame, `02a00` the per-object buffer, `02c40` the glare's
192x192 buffer, `03800` the shadow buffer, all CT32 unless a format follows.

| Origin | Primitives | Sprite pixels (1x) | Target reads (feedback) | What it does |
|---|---|---|---|---|
| VU1 strips into `00000` | 16,200 / 7,600 | | 1,270 / 1,270 (none) | the scene; textures from uploaded pages and from TexAnime's targets |
| VU1 strips with depth and bounds | 7,900 / 7,900 | | 105 / 92 (none) | the characters and objects, drawn offscreen into `02a00` (and `02a40`-`02b00`) against the scene's depth |
| VU1 outlines | 590 / 510 | | | the toon outlines, mostly into `02a00` with their object |
| VU1 shadow volumes | 1,120 / 1,130 | | | into `02a00`, counted by add and subtract blends |
| VU1 overlay sprites | 94 / 93 | 219 K | 94 (none) | each object's bounds from `02a00`, read as CT24, blended into the frame |
| `ModelDrawOffscreenCompositeMaybe_16aac0` | 90 / 79 | 535 K / 471 K | | clears each object's region of `02a00` before it is drawn: about 20 objects a frame |
| `GlareFilterDrawMaybe_17a0d0` | 159 / 159 | 1.1 M | 154 (120) | the frame down to `02a00` and `02c40`, then 20 shifted adds of `02c40` onto itself (120 strips), then back onto the frame |
| `DrawListBegin/EndPlaneShadowPass` | 52 / 52 | 980 K | 50 (48) | the volumes' mask from `02a00` to `03800`, 48 feedback sprites of blur, then onto the frame |
| `TexAnimeTableApplyMaybe_162f50` | 174 / 176 | 123 K | none | animated textures: sprites from T8 and T4 pages into `02ae0`, `02b00`, `02b20`, `02b60`, `02a60`, which the scene then samples as CT32 textures; one local copy a frame |
| `DofFilterDrawMaybe_17d150` | 48 / 14 | 57 K / 16 K | 48 / 14 (none) | the frame to `02a00` and blended back |
| `GfxBlitTexRectPixels_141710` (and `...At`) | 18 / 18 | 233 K | 2 | the display buffer (`00e00`, read as CT24) copied into `02a40` |
| `CBlurDrawMaybe`, `CBlurTexBlurMaybe`, `CBlurTexClearMaybe` | 10 / 10 | 479 K | 7 (4) | a small blur in `03f80`, and a full-frame blend |
| `BloomExtractBlurMaybe_179360` | 0 / 4 | 0 / 186 K | 0 / 3 | the battle only, now and then |
| `CharaDrawTranslucentPass_1f8250` | 1 / 1 | 66 K | 1 | the frame copied to `03b80` (for refraction) |
| `MapDrawGroupRange_1e1710` | 12 / 12 | 49 K | | map pieces drawn into `02ac0` and `02a20`, and about 1 MB of texture uploads |
| `DrawListFrameBegin_1425d0` | 16 / 16 | 230 K | | the clear |
| UI: `MesWinDraw`, `FontDrawText*`, `MinimapDrawEntry` (into `02ca0`, then the frame), `CompassHudRender`, `UiDrawWidgets`, the gauges | 120 / 250 | 256 K / 159 K | 10 / 3 | |
| Effects drawn as geometry: `EffectEntryDrawNodes`, `SunLightDraw` (the battle), `BattleEffectBurstDraw`, `SwordTrailDraw` | 17 / 98 | | | |
| The game thread's GIF transfer | 32 / 32 | 229 K | 32 (32) | 32 strips of the frame onto itself at the end of it |
| All | 26,700 / 18,200 | | | about 4 MB of texture data uploaded a frame; no readbacks |

What it says for phases 3 and 4:

- **One function makes stage A.** `GlareFilterDraw` is the blur the audit
  found most expensive ([GS-AUDIT.md](GS-AUDIT.md), about 2.9 ms of the
  frame at 8x with its snapshot copies). Its 120 feedback sprites are 20
  shifted copies of one buffer in four diagonal directions (five rounds,
  weights 23 to 4 of 128), each reading the result of the last. It is the first
  native pass: a downsample and a separable blur at the buffer's own size.
- **Stage E is the per-object composite.** About 20 objects a frame go
  offscreen: a clear of the object's region of `02a00`
  (`ModelDrawOffscreenComposite`), its strips and outline drawn there against
  the scene's depth, then the overlay sprite program blends its bounds into
  the frame. Why: a soft edge around each object, the fade's use of it
  rare (answered later, see "Stage E, why offscreen"); done in object
  layers ("Stage E in object layers").
- **The shadow is the plane shadow pass**, not `ShadowMapRender` (not seen in
  these scenes): volumes into `02a00`, a 48-sprite feedback blur in
  `03800`, then onto the frame. Native: stencil volumes, a blur at the
  buffer's own size.
- **`TexAnimeTableApply` is render-to-texture** that the old table missed: 174
  sprites a frame build animated textures in GS memory that the scene then
  samples. Phase 4's address table must name these targets as textures, or
  the pass becomes its own native render-to-texture.
- **Textures are streamed, about 4 MB of image data a frame** in both
  scenes, as on the PS2, whose 4 MB of GS memory holds only what the frame
  draws. The native texture table has to recognize a page's content again
  (the `gs_replacement_hash` key) rather than decode it again. Uploads into
  pages a target last drew are likely the audit's "patches from local
  memory" (0.9 ms at 8x); not yet checked.
- **No readbacks** in either scene, and no local-to-local copies besides
  TexAnime's one a frame.
- **Not measured yet:** the GPU time and snapshot copies per origin (the
  origin is not yet carried on the backend's batches; the targets above tie
  them to the GPU-time labels in GS-AUDIT.md), and the town, menus, events
  with item messages and dissolves, and movies.

### Phase 1: native model geometry (output replaced)

**Done (2026-10-28/29), see [VU1-NATIVE.md](VU1-NATIVE.md).** Every program
the recordings hold is native and agrees with the VU1 at zero tolerance
(`check` on the walk: 1.5 million chunks, none different). It went another
way than planned below: the programs are recognized by the hash of each
piece (`src/vu1_native.cpp`), the strips go to the backend as `GSNativeMesh`
(object-space vertices and a transform record, merged per draw state and
culled on the CPU), and the oracle (`tools/vu1/oracle`) is the harness. In
`gpu` mode no VU1 program runs on the walk. Still packets, not meshes: the
outlines, shadow volumes, particles and the projected texture; the overlay
sprites too, though with the object layers their sprites become a
`kObjectTail` effect the renderer draws (phase 3). The plan as written:

- `NativeDrawSink`, a narrow interface on the SDL GPU backend that accepts
  `NativeMesh` and `NativePass` commands into the same target cache and pass
  queue as GS draws. Game-specific code lives in `src/render/`; the backend
  only gains the sink.
- Port families in order of cost from the ledger. Per family: a C++ reference
  decoder plus the shader. An offline harness feeds both the records in
  `build/vu1.cap` and the compiled program, then compares output vertices
  (XYZ in 12.4, Z, STQ, RGBA) within a tolerance for VU float rounding. GPU
  output is read back through a compute pass in the harness.
- Image check at 1x against `RG_RENDER=gs` on the reference frames, with a
  per-pixel tolerance.

**Done when** the families covering at least 95% of VU1 draw starts in the
reference scenes draw natively, with no visible difference at 1x and
measured GPU power within noise of the GS path.

### Phase 2: motion vectors and temporal upscaling

Phase 2 builds on the `RG_JITTER` Halton offsets and the `CameraRegistry`
from `0020-gs-source-camera.patch`, and is tracked step by step in
[FSR.md](FSR.md).

- Object identity across frames: the model matrix's EE source address plus
  the mesh shape, as `capture_report.py` matches them. Keep last frame's
  matrices per identity.
- A velocity target written by native meshes. Non-native draws get
  camera-only motion or are masked from history.
- A TAA / temporal upscale pass at the end of the scene, before post. The
  "scene done" point is the task boundary before the first post task, found in
  phase 0. 3D renders below output resolution, and the existing "internal
  resolution" setting becomes "render scale".

**Done when** TAA is on in the reference scenes with no ghosting on
characters or the camera, as an option in the F1 menu.

### Phase 3: native post and offscreen effects

Port the effect table one function at a time, parity first, in the census's
order: the glare, the per-object composites, the plane shadow's blur, then
the rest. Each hook records
a `NativePass` and a `MARK`, and skips the original. Anything the original does
besides drawing (timers, work-texture registration, the `GfxEndFrame` probe
bookkeeping) is reproduced in the hook. The diff of the function's
non-graphics side effects is part of the review.

**The glare (2026-10-29), behind `RG_NATIVE_GLARE=1`.** It went another
way than planned above: the game's function still runs, and the VIF1 thread
holds the packets it wrote (`src/native_effects.cpp`), builds a
`GSNativeEffect` from their sprites (3 buffers, 29 sprites) and sends the
packets on with their kicks withheld, so the GS registers end as before. The
packets are taken one DIRECT at a time as `GS::processGIFPacket` takes them:
the effect's first A+D tag counts 14 and its DIRECT holds 8, and the GS drops
the rest. On the 30 fps walk at 1x it is drawn every frame, the render
passes fall from 180 to 111 a frame and the draws from 716 to 632, and the
field's screenshots match the GS path's (the bright sky to the pixel).
In the 60 fps mode at 3x (`bw-perf.sh`, two pairs, the last 25 s standing
in the street): still 60 fps, passes 199 to 127 a frame, feedback copies 121
to 52, draws 644 to 569, GPU busy unchanged at 63-66%, the power 32.1-32.3 W
to 31.0-31.3 W. The threads' CPU time (`/proc`, 20 s) is the same (GS worker
43% and 42% of a core); the `[gfx]` line's "backend % of wall" rises from
47-49% to 58-60%, which is waiting, not work: `submitEffect` itself takes
about 6 ms a second (the `[gfx]` line's "ms flush, buffers, sprites").

**The plane shadow (2026-10-29), behind `RG_NATIVE_SHADOW=1`.** The same
way, from `DrawListEndPlaneShadowPass`'s packets; `DrawListBeginPlaneShadowPass`'s
go on as they are, and the registers they set (`XYOFFSET_1`, `SCISSOR_1`)
are where the shadow's sprites start from. Its five steps: `02a00` (the
volumes' counts) less 128; down to `03800` (192x192), read as CT24 with
HIGHLIGHT2; 36 feedback adds of blur; a channel copy through the CT16 view of
`03800` (12 strips of 8 pixels, R and G masked: alpha bits 2-7 from green's),
now one `kGreenToAlpha` op; and laid over the frame by that alpha with the
frame's alpha masked. The effect path gained CT24 and the HIGHLIGHT modes,
the subtract and the lay-over blends onto GS targets, and the alpha mask. On
the walk at 1x the field's screenshot matches the GS path's to at most 6 a
channel where the shadows fall (they are faint there; a control run without
the last step shows them). At 3x in the 60 fps mode, with the glare: passes
199 to 86 a frame, feedback copies 121 to 12, draws 644 to 527, still 60 fps,
GPU busy unchanged.

**The per-object composites (2026-10-30), `RG_NATIVE_COMPOSITE`, on by
default:** see "Stage E in object layers" below.

**Where it stands (2026-10-30):** with all three on, the walk's last 25 s at
3x and 8x take about 53 passes a frame (from about 199) and about 16
feedback copies a frame (8x, `bw-perf.sh`); GPU busy and power are
unchanged on the RX 6700 XT. Left from the census: the depth of field,
CBlur, the frame copies (`CharaDrawTranslucentPass`, `GfxBlitTexRectPixels`)
and the game thread's 32-strip transfer at the frame's end; which of them
make the remaining feedback copies is not yet counted.

**Done when** Salgin's first scene runs in at most about 25 render passes
with no snapshot copies or destination copies, and the GPU power measured
with `gpubusy.sh` falls by an amount recorded in the ledger.

### Phase 4: textures and 2D without local memory

- Uploads decode straight into native textures through the address table.
  Replacement packs load by the same key.
- The 2D batcher replaces the GS translator for every remaining packet. UI
  draws at output resolution, with the widescreen layout fix.
- Depth switches to reversed float once phase 0's census confirms that no
  GS-path draw tests against model depth any more.

**Done when** `gs_vram` is no longer allocated in a native run, and a
playthrough route (title, Salgin, the first battles, a boss, menus, a save
and a load, a movie) passes with the frame tools.

### Phase 5: remove the GS path

Delete local memory, page ownership, imports and patches, snapshots,
destination copies, round trips and readbacks from the backend, plus the
`DQ8_GFX_*` switches that exist only for them. Keep `RG_RENDER=gs` for one
release as a fallback, then remove it. The backend's device, window,
present, display filters and menu stay.

### Phase 6: further work (optional, each behind an option)

- Static meshes cached on the GPU, through the model-resource altitude.
- Per-pixel lighting from the same light slots.
- MSAA on the scene, anisotropic filtering, higher-resolution shadows.
- An HDR intermediate for bloom.

### Stage E, why offscreen (2026-10-29)

From `ModelDrawOffscreenCompositeMaybe_16aac0`'s code and
`RG_COMPOSITE_CENSUS` on the walk. Its only way in is
`ModelDrawOffscreenCompositeAtMaybe_16b8a0`, which jumps into it past the
function table (a hook on the inner function never runs). Its arguments: a0
the object's settings, f12 a glow alpha, f13 the alpha the object is laid
over the frame with.

- Nothing is drawn when the settings' +0x50 or +0x5C is 0. When +0x24 (a
  strength) is negative, or +0x20 (the offscreen buffer) is 0, the parts are
  drawn straight into the scene.
- Otherwise offscreen: the parts' screen bounds grown by 8 pixels are
  cleared, the parts drawn there, then laid back over the frame by
  `ScreenTexOverlayDrawSingle` with alpha `0x80 f13` (alpha tested >= 1),
  and, only when f13 is 1, by `ScreenTexOverlayDrawMulti`: copies of the
  object around itself at a radius of 16 `+0x24 min(f12, 1)^2` (1/16 pixel).
- Of 13,133 calls: 96% offscreen at f13 = 1 with a strength of 1.7 or 2 (the
  copies drawn); about 40 fading (f13 stepping from 0.1 to 0.9 as an object
  appears); 516 direct; 511 nothing.
- `RG_COMPOSITE_DIRECT=1` takes the direct path for all: the characters'
  insides are the same and only a thin rim along their silhouettes differs,
  and the passes fall from 180 to 142 a frame at 1x.

So the offscreen pass is for a soft edge around each object, and the fade's
use of it is rare.

What the soft edge is (`ScreenTexOverlayDrawMulti_16a510` and the walk's
frame log): `Multi(packet, rect, texture, rgba, mode, offset)` draws the
object's copy once unshifted (mode 1), twice at plus and minus 3/4 of the
offset (mode 2), or four times at plus and minus dx and dy (mode 3); the
mode is the settings' +0x64 unless a global (gp-0x7200) overrides it. On the
walk it is mode 2: per object, `02a00` laid over the frame twice, shifted
by plus and minus (3/16, 4/16) of a pixel, with vertex alpha 0x45, then once
unshifted at 0x80. Each copy reads `02a00` as CT24 with TA0 128 and AEM
(black is transparent), bilinear, blended by lerp(Cd, Cs, As) and alpha
tested As > 0. The two shifted copies smear the silhouette's colours about
half a pixel outward at about 54%, under the sharp copy: anti-aliasing of
the object's edge.

The native form first planned (superseded by "Stage E in object layers"
below, which says why): the objects drawn directly into the scene and also into
one object layer (colour and coverage, all of them); then one pass that,
outside each covered pixel, lays the layer's two bilinear samples at those
offsets over the scene, as the copies do. The overlap of objects differs
slightly from one pass per object (a later object's rim over an earlier
object). The few fading objects keep the offscreen pass, or take their
alpha in the layer. A temporal upscaler (phase 2) would also cover this
edge; until then the pass keeps parity.

### Stage E in object layers (2026-10-29, `RG_NATIVE_COMPOSITE`, on by default 2026-10-30)

A first try reordered what the GS gets (the objects' heads, then their
tails) and saved almost nothing (passes 180 to 177 a frame at 1x):
consecutive objects overlap, and each closed the batch. A merged layer with
an object index beside the colour would not be exact either: half the
objects' strips blend by alpha (a census of the draws into `02a00`), and an
object drawn over an earlier one in one layer blends with it, where the
game's blends with the black of its cleared box.

So each object gets a slot of its own. The VIF1 thread
(`src/vif1_thread.cpp`, `compositePacket`) sends a `kRouteLayer` with the
object's box before its head; the backend sends the head's draws into the
buffer to that slot instead (`ObjectLayers` in `sdlgpu_backend.cpp`). The
slots are 4 to 8 RGBA8 textures (`DQ8_GFX_LAYER_MB`, 96 MB unless set), all
bound to one pass as colour targets; the pipeline's write masks pick the
slot (`gs_draw.frag`'s `DQ8_LAYERS` variant), and the scene's depth is the
same. An object takes the first slot whose waiting boxes its own does not
meet, so each still reads only itself. Its tail becomes a `kObjectTail`
(the builder in `src/native_effects.cpp`, its kicks withheld), and the
waiting tails go in one pass, in order (`compositeLayers`), when anything
else reaches the GS or every slot is taken. Uploads and TEXFLUSHes between
objects go on: the layers are not GS memory, and the objects never draw
`02a00` (the shadow volumes and the other effects still use it).

What parity took:

- The halo copies read CT24 under AEM, bilinear: each texel's alpha from
  TEXA before the four are weighed (`gs_effect.frag`), as the GS path's
  `sampleTexture` does; filtering first gave every edge full alpha.
- TEXA is set before the tail (TA0 128, AEM 1): the GS frontend fills it in
  (`kTexaFromGs`, patch 0022).
- The tails write the frame's alpha (FBMSK 0): each lay over is followed by
  an alpha-only draw of As.
- Some objects' sharp copy reads the buffer as CT32, keeps the pixels whose
  alpha is at least 1 (a weapon drawn with FBA) and lays their alpha over
  the frame, which reads it later: a weapon's glow, alpha up to 0xE8, was
  10% darker when layers stored A/128 (the blend's factor) and saturated at
  0x80. A layer stores the GS's A/255; a blend by source alpha draws its
  colour with the factor and its alpha in the replay, as on hosts without
  dual-source blending (which more than one target rules out). No object
  blends by destination alpha, which would read A/255 as its factor.
- Objects wholly off the frame have tails with no sprites: dropped.
- Half-float layers were tried, for factors past one: a weapon's additive
  glow then went past one under its next lay over, where the GS clamps.

On the walk at 1x with the glare and the shadow native: 25,000 objects, all
in layers, no tail refused; passes 76 to 53 a frame. The logged frame (pad
3000, `tools/bench/vram2png.py`) matches the GS path's: no pixel more than 3
levels apart, the same noise as without the layers.

| 60 fps mode, last 25 s standing | fps | passes/frame | GPU busy | power |
|---|---|---|---|---|
| 3x, glare + shadow | 59.9 | 85.8 | 61% | 29.8 W |
| 3x, + object layers | 59.9 | 52.6 | 66% | 29.9 W |
| 8x, glare + shadow | 59.9 | 85.3 | 78% | 151.3 W |
| 8x, + object layers | 59.9 | 53.4 | 79% | 153.8 W |
| 8x, + object layers, exact alpha | 59.9 | unchanged | 80% | 152.4 W |

A first 8x run cleared every slot after each composite (64 MB textures, a
dozen times a frame) and had 2 slots: 87% busy, 159.7 W. Now a slot is
cleared once a frame (each object clears its own box), with at least 4.

## Sizes and order

Relative sizes from the shape of the work, not estimates in days. Phase 1 is
the long pole and needs phase 0's family count.

| Phase | Size | Can start |
|---|---|---|
| 0 Attribution and census | S-M | effects done for the field and a battle (2026-10-29); the town, menus, events and movies left |
| 1 Native meshes | L (about ten families, each a decoder, a shader and harness cases) | done (2026-10-29) |
| 2 TAA | M | groundwork built (jitter, camera registry, [FSR.md](FSR.md)); after phases 3 and 4, a fallback for low-end hardware |
| 3 Post | M-L (about ten effects, each S-M) | the glare, the plane shadow and the per-object composites (object layers, on by default) done (2026-10-30) |
| 4 Textures and 2D | M | after 3 |
| 5 Removal | S | after 4 |

Phase 2 was to run beside phase 3; it now waits (2026-10-29: effects stay
exact natively, and the upscaler is for hardware that cannot keep up
without it). Phase 3's scene/post split is still what it needs as its
input.

## Risks

- **VU1 side effects.** The next program can depend on what a program leaves
  behind, through data memory, VI registers across `MSCNT`, or the screen
  overlay sprite's jump into slot B. The output-first step and the
  state-diff mode exist for this. A family skips its run only once the diff is
  clean.
- **Coverage tail.** The table allows 785 compositions; the manifest shows far
  fewer in use. Unported compositions keep running the compiled program
  through the GS translator, so the tail costs speed, not correctness, until
  phase 4 needs it gone.
- **Mixed depth.** Until phase 4, native and GS-path draws share an integer
  Z. Any mismatch in the Z formula shows as z-fighting on decals and effects
  drawn over native models; the harness compares Z exactly.
- **Effect side effects.** A skipped original can leave game state behind
  (a fade's timer, a probe slot). Each hook's review lists what the original
  touched besides the chain.
- **A permanent fork of DQ8's backend.** Upstream fixes stop applying once
  phase 5 deletes the parts they touch. That is accepted: the GS parts are
  what goes.
- **Upscalers that need native API handles** (DLSS, XeSS) are out of scope
  while SDL GPU is the API. Temporal upscaling is our own pass.

## Open

- The families: their count, vertex formats, GIF tag templates, and which do
  lighting, skinning or clipping (phase 0, then the harness).
- ~~Why the game draws about 20 objects a frame offscreen~~ (stage E):
  answered 2026-10-29, mostly for a soft edge, see "Stage E, why offscreen"
  above.
- Whether the lens flare's occlusion is right today (the depth-probe question
  above).
- The 2D layer's end state: keep translating `GsPkt` packets, or hook the
  `GsPkt*` functions for direct native calls. The translator is enough for
  phase 5; direct calls are a later option.
