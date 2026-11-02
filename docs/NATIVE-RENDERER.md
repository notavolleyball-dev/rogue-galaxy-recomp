# A native renderer in place of the GS

A plan for retiring the GS emulation (local memory, page ownership, feedback
snapshots, readbacks, the register-level state machine) and drawing the game
with a renderer that knows what it is drawing: models with their matrices,
named effect buffers, and a 2D layer. Proposed 2026-10-28. Since then
phase 1 is done for every VU1 program the recordings hold
([VU1-NATIVE.md](VU1-NATIVE.md), 2026-10-29), phase 0's census of the
effects is in "Phase 0" below, and phase 3 has five effects: the glare, the
plane shadow, the per-object composites, CBlur's texture blur and the bloom
(2026-10-30; see "Phase 3"). All of it sits behind the F1 menu's "Native
renderer" switch. "Where it stands" is the summary for picking the work up;
the rest is the plan and its record.

See [GS-DECOUPLING.md](GS-DECOUPLING.md) for why the renderer is still shaped
like the GS and the plan that ends it.

## Where it stands (2026-10-31, with 2026-11-01 below)

**Done, behind the switch:** every recorded VU1 program native, the strips
drawn as GPU meshes (phase 1); the glare, the plane shadow, the per-object
composites (object layers, their lay overs one draw with dual-source
blending), CBlur (its texture blur and its lay over), the bloom, the
depth of field and the refraction's copy of the frame drawn by the
renderer (phase 3). Each is checked byte for byte at 1x against the GS path or its
replay, and at 1x and 3x in VRAM. Speed: 8x holds 60 fps on the RX 6700 XT;
"deck mode" (a throttled desktop: 4 cores at 2.9 GHz, the GPU's shader clock
at 500 MHz, memory at 674 MHz) went from 24.4 fps at 4x (`f53f358`, before
any of it) to 56.7 fps, the GPU about 17.6 ms a frame and limiting
(victor-takai/rogue-galaxy-recomp#10).

**Left before the GS can go, in the order to take it:**

1. **The field's remaining post and offscreen work** (phase 3, the census
   table in "Effects: hook the function"; what the effect builder makes of
   each is in "The rest, probed"): `TexAnimeTableApply`'s animated
   textures (render-to-texture the scene samples), the map pieces
   `MapDrawGroupRange` draws offscreen, the effects drawn as geometry
   (sword trails, the sun light, battle bursts, effect nodes), the fades,
   and whether the lens flare's depth probe is right. The frame clear and
   the game thread's 32 strips at the frame's end (the flip's copy into the
   display buffer) are plain sprites, for phase 4's batcher and the
   present ("The rest, probed").
   **Status (2026-10-31):** on the walk and the forced battle, every
   offscreen effect the builder can take is native. Of what is left,
   `TexAnimeTableApply` and `MapDrawGroupRange` draw from T8 and T4 pages
   with uploads among their sprites, so they wait for phase 4's texture
   table; the geometry effects are plain primitives into the frame,
   nothing offscreen; the fades, the focus filter, the lens flare and
   `ShadowMapRender` draw nothing on this route (probed), so item 3's
   censuses must find them. With every effect native, pad 3000 at 1x takes
   35 render passes a frame (37 before CBlur's lay over and the depth of
   field), no feedback copies; the GPU-time labels put most of the rest on
   GS-path draws, among them the shadow volumes' 230 or so blends into
   `02a00` a frame (item 2).
2. **Native draws for the VU1 programs still on packets:** outlines (about
   17% of the field's vertices), shadow volumes (5%), particles, the
   projected texture, the object set-up. They are computed natively but
   still kick GS packets.
   **Shadow volumes, first cut (2026-11-01):** a chunk's fans are grouped
   by blend (add, subtract) before they go out, so the GPU draws them as
   two runs a chunk where the program's order alternated: 277 runs a frame
   on the walk (pad 3000) became 43, 242 in the forced battle (pad 3450)
   became 47, and all of VRAM is byte-identical at 1x with
   `RG_NATIVE_VOLUME_GROUP=0` (the program's order). The mask clamps at 8
   bits after every blend, so this can differ where a pixel clamps between
   a subtract and an add; none did on these two frames, so the town and the
   censuses of item 3 should recheck it. The fans are still GS packets.
3. **Censuses not run yet:** the town, menus, events with item messages,
   battle dissolves, movies (`tools/bench/origins-run.sh`). Each may add
   effects to item 1.
4. **Textures without local memory** (phase 4): the address table (a page
   names its latest upload, decoded once and keyed by content, or the
   target that last drew there; CLUT loads through it), about 4 MB of
   uploads a frame recognized again rather than decoded again, the
   reinterpretations that survive the effects as explicit cases.
5. **2D and UI** (phase 4): the GS packets no hooked function or VU1
   program made (about 4,800 `GsPkt*` call sites) to a sprite batcher with
   the GS's integer colour arithmetic and no local memory behind it; then
   reversed float depth.
6. **Remove the GS path** (phase 5) once `gs_vram` is not allocated in a
   native run and the spot checks pass (walk and battle VRAM, title and movie,
   a Zerard tour); a full playthrough is not a gate, the feature flag is what
   keeps the GS path available.

**Since 2026-10-31, toward dropping the GS (2026-11-01; the record is in
"Phase 4", steps 1 to 3b):** item 2's shadow volumes are grouped by blend
(`RG_NATIVE_VOLUME_GROUP`), and the outlines turned out not to cost draws
(452 lines in 2 runs); item 4 is under way: the texture cache reads texels and
palettes from a memory only host writes reach when no draw or copy touched the
pages (`DQ8_GFX_UPLOAD_STORE`, on; checked on the walk, the battle, the title
screen, the opening movie and a menu), the frame no longer round-trips local
memory (the flip's CT24 view imports by GPU page copy), and the per-object
buffers skip importing what is under them. **Loose ends, in the order to take
them** (each was gated by byte-identical VRAM on the walk and the forced
battle; since 2026-11-02 that is a diagnostic, not a gate, see "Verifying"
below):

1. ~~*What reads the host writes patched into the object buffers*~~ Done
   2026-11-01 (step 5 below): the texture builds that read through the
   target for textures uploaded into its pages.
2. *The textures that still need targets* (the list `DQ8_GFX_WARN_LOCAL_READS=1`
   prints, with the owning targets): the frame (`00000`) read as T8/T4 views
   (64x64 to 128x256) with the CLUT in the frame too (palettes now copied
   out region-wise; the texels come from the target), the palettes at `03f80`
   (a 64x64 target), `03800` with no target left that owns its drawn blocks,
   and `02ba0` read 512 wide over four targets. In a title run only the 512x512
   `00000` read over the flip surface's tail rows remains (rows past 448). They
   are served by resolves into the backend's memory; sampling the targets on
   the GPU would remove the readbacks (`copyFromTarget` is the CT32 start).
3. *The 2D batcher* (item 5 above): kept as the translator the end-state
   section calls enough for phase 5 ("Shrinks": the packets keep arriving, a
   thin translator feeds the sprite batcher; the sdlgpu backend already draws
   them on the GPU from native textures). What it still carried of local memory
   is items 1 and 2; rewriting the 4,800 call sites for direct native calls
   stays the later option the "Open" list names, not done.
4. *Item 1's rest and item 3's censuses* (town, item messages, dissolves,
   other movies), now also with `DQ8_GFX_UPLOAD_AUDIT=1` on, which says
   whether the store holds for each.
5. *Stop allocating `gs_vram`*: done 2026-11-01 (Phase 4, step 4); what is
   left is the readbacks of loose end 2, then phase 5. No playthrough route:
   the native renderer stays behind its flag, and the GS path is the fallback.

**Verifying (since 2026-11-02).** The goal is off the GS: native pieces
replace the game's GS work at the function (RGDecomp's C says what each
function means to draw) rather than reproduce its packets and local memory,
so byte-identical VRAM is no longer the gate. A piece is checked by eye on
the routes that reach it, by the frame rate and the GPU times, and by the
readbacks (`resolves=` on the stats line) it adds or removes. `cmpvram.py`
stays a diagnostic: where a piece is meant to draw what the GS path drew
(the flip's copy), a diff still points at a bug; elsewhere a diff is
expected. The old `vg1`/`vb1` references predate later changes, so compare
a run with the piece's switch off from the same build instead.

The tools (`native-run.sh` keeps the game's local memory allocated,
`RG_GS_VRAM_RESERVE=0`, because the VRAM dump needs it; set
`RG_GS_VRAM_RESERVE=1` to run the reserved mode and compare screenshots
within a session instead.) Walk: `tools/vu1/oracle/native-run.sh NAME gpu 110 3000`, then
`python3 tools/bench/cmpvram.py NAME vg1`; battle: `RG_FORCE_ENCOUNTER=1`
`... NAME gpu 130 3450` against `vb1` (the reference logs are in `build/run`;
regenerate them from a commit before the change when they are missing). A
movie's frames follow the wall clock, so compare it by `DQ8_GFX_UPLOAD_AUDIT=1`
(its per-update SAME/DIFFERENT lines), not by screenshots. A title run is the
game started with no `RG_INPUT` for 150 s.

**The VU1 tail** (2026-10-31): every piece is native but the indexed
per-triangle renderer (A `8855d9e7` with E `56d6683a`, `c9e1cecc`), which
nothing reaches, the clipping Es (the GPU clips: with the native renderer the clipping chunks
go to it with E's near and far planes as clip distances, 2026-11-01; until
then they ran on the VU1, about half of what the VIF1 thread still
interpreted in a fight) and two stubs; the pieces
no recording holds were written from the code and checked against the
interpreter only, their programs named "unverified" ([VU1-NATIVE.md](VU1-NATIVE.md),
"Pieces written from the code alone"). Harvest mode (below) still turns
them into recordings when play reaches them. Programs whose draws go out as
packets rather than meshes (item 2) now include the affine and float-colour
Ds and the beam's.

**Capturing the rest** (harvest mode). Build `build/game` (the patches
apply on configure; after pulling a changed patch, undo them with
`git -C thirdparty/PS2Recomp checkout -- . && rm thirdparty/PS2Recomp/.rg-patches-applied`
and build again). Then play with your own card:

    tools/vu1/oracle/harvest-play.sh

or, to play the late-game save (a GameFAQs end-of-game save, Jaster Lv99,
132 hours, so everything is unlocked; the game saves into this copy, not
the real card):

    RG_MC0_DIR=build/run/mc0-late tools/vu1/oracle/harvest-play.sh

It samples each VU1 program it has no file for yet into `build/harvest`
(kept between sessions, so it only adds) and checks the native ones against
the VU1; the log, `build/run/harvest-<time>.log`, has a `[harvest] new
program` line for each new one, and `?` or `-` in its pieces is an unseen
piece, so note where you were. Afterwards `tools/vu1/census.py --recording
build/harvest/*.vu1r` ticks the pieces off. What to play, likeliest first,
is the "Harvest checklist" in [VU1-NATIVE.md](VU1-NATIVE.md): the Insectron
arena, battles with every character's skills and the combo finishers, a boss
or two, water, lava and waterfalls, each planet's first enemy area, save
points and the other warps, space and the planet select, menus with models,
story scenes, the first-person view.

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
- **RGDecomp's port branch** (`Dorgengoa/RGDecomp`, branch `port`,
  2026-10-28, CC0): the decompilation compiled as a native Windows game,
  D3D11 / Vulkan, a native-only build with no GS model (its
  `docs/NATIVE-RENDER.md`, "Checkpoint 10"). It still consumes the game's
  GIF packets and keeps GS local memory on the GPU, and is not exact by
  design (hardware clipping, float depth; its gate is 31-45 dB PSNR against
  its own emulated build). Useful as notes to check here, as RGDecomp's:
  VU1 semantics of row 6 (refraction) and row 9 (reflection map, the
  Insectron floors), row 7's UV wave (reads a VU memory word nothing
  writes), and the order it took off the GS (meshes, immediates, local
  memory on the GPU, CLUT loads on the GPU, then the model dropped).
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
| `GlareFilterDrawMaybe_17a0d0` | A: 159 sprites, 120 of them feedback adds in the 192x192 `02c40` | downsample + separable blur at the buffer's own size. Parity form done: the game's sprites drawn by the renderer (`RG_NATIVE_GLARE`, on with the native renderer; exact where the GS path is not, see "Phase 3") |
| `ModelDrawOffscreenCompositeMaybe_16aac0`, with the VU1's depth-and-bounds strips and overlay sprites | E: about 20 objects, each cleared, drawn into `02a00` and blended back | done: object layers, a slot per object and the tails in one pass (`RG_NATIVE_COMPOSITE`, on with the native renderer) |
| `DrawListBeginPlaneShadowPass_147b80`, `DrawListEndPlaneShadowPass_146cc0`, with the VU1's shadow volumes | F: volumes into `02a00`, 48 feedback sprites of blur in `03800` | stencil volumes, a blur at the buffer's own size. Parity form done as the glare's (`RG_NATIVE_SHADOW`, on with the native renderer) |
| `TexAnimeTableApplyMaybe_162f50` | 174 sprites into texture pages the scene samples | native render-to-texture into named textures |
| `DofFilterDrawMaybe_17d150` | D: the frame halved into `02a00` (16 sprites), laid back over the far scene by a 32-triangle strip depth tested GEQUAL | done: the strip's quads as sprites depth tested against the scene's depth buffer (`RG_NATIVE_DOF`, on with the native renderer) |
| `CBlurDrawMaybe_1784e0`, `CBlurTexBlurMaybe_1789e0`; `GfxBlitTexRectPixels_141710` (the display buffer into `02a40`) | small blur in `03f80`, full-frame blends | copy plus a sampled pass. Parity form done, both (`RG_NATIVE_CBLUR`, on with the native renderer); `CBlurDraw`'s copy into `02a40` is the census's `GfxBlitTexRectPixels` |
| `CharaDrawTranslucentPass_1f8250`, `RefractionCaptureFrame_13ffd0` | the frame halved into `03b80` (one sprite, in `RefractionCaptureFrame`) | parity form done: the copy drawn by the renderer into the GS's `03b80` (`RG_NATIVE_REFRACTION`, on with the native renderer); later the scene target sampled directly |
| `BloomExtractBlurMaybe_179360`, `FocusFilterDrawMaybe_17c000` | C, D: the battle now and then; the bloom also in the field (14 sprites a frame where the walk ends) | as the glare. Parity form of the bloom done (`RG_NATIVE_BLOOM`, on with the native renderer) |
| `ShadowMapRender_146680`, `ShadowMapBlur_145b60` | not seen in the field or the battle | a shadow pass at a chosen size; blur or PCF |
| `LensFlareDraw_2ed830`, `DepthProbeGetSlot_13fc80` (8x8 Z reads in `GfxEndFrame_141c20`) | not seen | **depth served from the GPU (2026-11-02):** the read (PSMZ32 at the sun, the minimum of 64 against the sun's depth) found nothing in local memory with the GPU backend, so the flare was never occluded; now the backend copies the block out of its depth target at each frame's end (`gs_depth_probe.frag`) and the read takes the last one, a frame or two late (`gfx/gs/gs_depth_probe.h`, `DQ8_GFX_DEPTH_PROBE=0`). Not yet seen on screen. Later: the flare itself native, its occlusion decided on the GPU |
| `ScreenColorFilterMaybe_179c60`, `ScreenFade*`, `ScreenFadeBlockCaptureFrame_177ed0` | fades (the battle's start) | **the tint native (2026-11-02, `RG_NATIVE_COLOR_FILTER`):** built from `(r, g, b)`, not its sprites (RGDecomp: the frame's alpha from its green through a CT16 view, then `rgb` times `Ad`), two ops into the frame taken over; the GS path's CT16 view cost a whole-frame readback each call. Only reached by the grayscale filter task (`GameSceneTintApply`), not on the walk; unverified on screen. The fades are plain full-screen sprites (untextured, textured wipes, a zoom of the display buffer), left to the 2D batcher; the capture is a `GsDownloadImage` readback the round trip already handles |
| `GfxFlipBuffersMaybe_143650`, and the game thread's 32-strip transfer at the frame's end | | **a native copy (2026-11-02, `RG_NATIVE_FLIP`):** the flip's draw environment (from `sceGsPutDrawEnv`, the runtime's stub, which sends no packet) and its sprite packet made one effect op, done as a GPU texture copy into the display buffer: 32 fewer GS batches a frame, no readbacks, the walk's VRAM and display buffer as before. The display buffer still exists because the presenter and `CBlurDraw` read it; presenting the scene target directly waits on those two |

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
fully playable. The GS path stays selectable until phase 5: the F1 menu's
"Native renderer" (`native_renderer` in settings.ini, `RG_NATIVE_RENDERER=0`
or `1` for a run; `src/native_renderer.h`) is the one switch, off unless set,
read once at start (a change takes effect at the next start). On, it sets
`RG_VU1_NATIVE=gpu` and every effect of `src/native_effects.h`; each of
`RG_VU1_NATIVE`, `RG_NATIVE_GLARE`, `RG_NATIVE_SHADOW`, `RG_NATIVE_COMPOSITE`,
`RG_NATIVE_CBLUR` and `RG_NATIVE_BLOOM` still overrides its own piece
([DEVELOPMENT.md](DEVELOPMENT.md)). Code that sets itself up once must read
the switch lazily, not in a static initializer: those run before the menu
hands its setting over. Upstream, the switch alone is victor-takai's PR #15
(branch `native-renderer-flag`), so native work can go behind it. A debug
key that flips native pieces live on the same frame is still planned.

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

**The glare (2026-10-29), `RG_NATIVE_GLARE`, on with the native renderer (2026-10-30).** It went another
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

**The plane shadow (2026-10-29), `RG_NATIVE_SHADOW`, on with the native renderer (2026-10-30).** The same
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

**The per-object composites (2026-10-30), `RG_NATIVE_COMPOSITE`, on with
the native renderer:** see "Stage E in object layers" below.

**CBlur (2026-10-30), `RG_NATIVE_CBLUR`, on with the native renderer.**
`CBlurTexBlurMaybe`'s packets (with the `GfxBlitTexRectPixelsAt` it calls)
are five ops on the 64x64 `03f80`, which keeps its state from frame to
frame: copied to `02a00`; its colour set to white with the alpha masked;
that times Ad (alpha 0x98, which reads the destination's alpha); the copy
added back tinted 0x79; and its alpha taken from its green (the CT16 channel
copy, as the shadow's). `CBlurDraw` later lays `02a40` over the frame by
`03f80`'s alpha, from GS packets as before. `03f80` is the GS's, so the
effect path gained a way to draw into a GS buffer what the pipeline cannot
blend: such a buffer is taken over (`GSNativeEffect::kTakenOver`). It is
drawn as an own buffer, through two textures at its surface's scale; the
first op reads the surface and the last writes it, so nothing is copied.
Own buffers honour the alpha mask in the shader now. At pad 3000 at 1x the
frame and `03f80` match the GS path's exactly. At 3x, 475 pixels differ
where `03f80`'s alpha is laid over the frame; the native frame there
matches the 1x frame and the GS path's does not (one is 93 against the GS
path's 50, and 93 at 1x). Over 60 frames at 1x it saves 5 passes, 2
feedback copies and 1 destination read a frame.

**Where the GS path is wrong: the glare (2026-10-30).** The logged frame
at pad 3000 (the save point and its sign in view) differs from the GS
path's in 8% of its pixels, up to 241 levels: the native glare shows on the
sign's emblem, the sword and the save point, and the GS path's shows
nothing. The glare's second step draws `02a00` (the frame's pixels whose
alpha is above 0x80) into `02c40` as Cs As, then takes Cs away: what is
left is Cs (As - 128) / 128, the glow. Here As reaches 243 (716 pixels
above 0x80; the two paths' `02a00` agree). The GS path blends with a
fixed-function factor, which clamps As / 128 at one, so it is left with 0
everywhere; the native effect does the GS's integer arithmetic in its
shader. With the glare's GS packets, `02c40` was black in the GS path's
memory just before the add onto the frame; by the GS's arithmetic on its
`02a00`, 706 pixels survive the second step alone. So the GS path is no
oracle where a blend's factor comes from a texture's alpha above 0x80 (its
destination copy takes only vertex alpha above it); the native glare is on
with the native renderer.

**Where it stands (2026-10-30):** with all three on, the walk's last 25 s at
3x and 8x take about 53 passes a frame (from about 199) and about 16
feedback copies a frame (8x, `bw-perf.sh`); GPU busy and power are
unchanged on the RX 6700 XT. Left from the census: the depth of field,
`CBlurDraw`, the frame copies (`CharaDrawTranslucentPass`,
`GfxBlitTexRectPixels`) and the game thread's 32-strip transfer at the
frame's end. With CBlur native too, the walk at 1x makes no feedback
copies and no destination reads (`DQ8_GFX_STATS_EVERY`, every 60 frames)
except a burst of about 150 copies where it starts (pad 2900) and the area
the last input walks into (pad 3400 on): there `BloomExtractBlurMaybe` runs
in the field, 14 sprites a frame on `02a00`, `02c40` and the frame, and
with it about 6 to 12 feedback copies and 2 to 8 destination reads a frame.

**The bloom (2026-10-30), `RG_NATIVE_BLOOM`, on with the native renderer.** The builder
took `BloomExtractBlurMaybe`'s packets as they were: `02a00` cleared, the
frame's alpha-tested pixels down to it, times their alpha (Cs Ad), then four
shifted adds into `02c40`, four back into `02a00`, and that added onto the
frame (14 sprites, 3 buffers). At pad 3450 at 1x the frame matches the GS
path's exactly. In that area at 1x, per 60 frames: passes 2845-3271 to
2347-2717, feedback copies 378-714 to 60-396.

**The rest there (2026-10-30): the scene's additive blends past one.** A
trace of every copy by surface put nearly all that was left on VU1 strips
textured from a page at `02c40` (T8), blended Cs As + Cd with vertex alpha
above 0x80: a fixed-function factor clamps at one, so the GS path read the
destination for each (a frame snapshot apiece). The shader now gives
Cs As / 128 on the GS's integers and the pipeline adds Cd
(`kFragFlagScaledAdd`); clamping before the add is the same as after, both
being positive. GS memory at pad 3450 is byte for byte the same, and in
that area 60 copies per 60 frames remain: the depth of field's
destination alpha test on the frame. The minimap's (on `02ca0`) comes
before the walk.

**The depth of field (2026-10-31), `RG_NATIVE_DOF`, on with the native
renderer.** `DofFilterDrawMaybe`'s packets: the frame halved into `02a00`
(16 sprites of 16 pixels, bilinear, UV 0-512 over 256 pixels), then
`02a00` laid back over the frame (lerp by As, vertex alpha 0x80, the
frame's alpha masked) by a triangle strip of 16 quads, 32 pixels each,
with ZTE and ZTST GEQUAL and Z not written: only the far scene is
replaced. The strip's Z is a sawtooth, 3907 at each quad's left edge and
3527 at its right, so the threshold ramps across the screen. The builder
takes a strip of axis-aligned quads (top and bottom vertices in turn, Z by
column) as sprites, and a depth test into a GS target;
`GSNativeEffect::Op` gained the depth buffer and the Z at the sprite's two
edges (framework patch 0024), and the backend attaches the scene's depth
surface and draws the sprite with a shader variant writing that Z
(`gs_effect.frag`, `DQ8_EFFECT_DEPTH`). The 16 quads, and any sprites in
a row into the same GS target, go in one pass (they were one each).

Bilinear reads in the effects now weigh the four texels with the GS's 4
fractional bits, as the GS path's `sampleTexture` does for UV draws (they
used the sampler's, except CT24 and layer sources): the depth of field's
222 texel rows over 448 land between sixteenths, where the sampler's finer
weights left it up to 9 levels from the GS path. The glare and the shadow
move by a few levels with it; against the GS path the shadow is closer (at
pad 2660 at most 3 levels apart where it was 4), the glare as far (the GS
path is wrong there, above). CBlur is unchanged and exact.

At 1x, each effect native alone against the GS path for all
(`tools/bench/fx-run.sh`, `tools/bench/pixdiff.py`): at pad 3000 the depth
of field differs in 2,422 pixels, by 1 level (29 by 2), all in its far
region; at pad 3450 and in the battle (pad 4400 with the forced encounter)
not at all. The other effects as before. At 3x it is within 2 levels with
`DQ8_GFX_EFFECT_SCALE=0`; at the default 2, its own `02a00` is drawn at 2x
where the GS path's is at 3x, and 376 pixels differ by more than 8 levels in
the blurred region, the blur buffers' trade-off. With every effect native,
the walk's last stretch (pad 3450) makes no feedback copies with or without
it (the 60 per 60 frames put down to it under "The rest there" no longer
show on this build), and it saves 2 render passes a frame there.

**CBlur's lay over (2026-10-31), with `RG_NATIVE_CBLUR`.**
`CBlurDrawMaybe`'s packets are three steps: the displayed frame (`00e00`,
read as CT24) copied into `02a40` (the census's `GfxBlitTexRectPixels`, 16
sprites); `02a40`'s alpha alone (FBMSK `0x00FFFFFF`) written from `03f80`
stretched over the screen; `02a40` laid over the frame by that alpha (vertex
alpha 0x40, so half of it) a pixel left and two up: a motion trail. The
builder gained the alpha-only write (`GSNativeEffect::kKeepColor`, patch
0024; into a GS target the `AlphaOnly` pipeline) and takes a CT24 read under
a TEXA set before the effect from the GS (`kTexaFromGs`, as the composite
tails did). At pad 3000 at 1x the frame matches the GS path's exactly; at 3x
the 1,409 pixels that differ are along the right and bottom edges, where
the shifted lay over reads past the copy (the GS path's 3x surface holds
whatever lies below and right, the native buffer clamps, as at 1x). It
saves 2 render passes a frame (37 to 35 with every effect native).

**The refraction's capture (2026-10-31), `RG_NATIVE_REFRACTION`, on with
the native renderer.** `CharaDrawTranslucentPass`'s copy of the frame is
`RefractionCaptureFrame`'s one sprite: the frame, bilinear, halved into
`03b80` (256x224, CT32), which the refraction material samples later (the
pass also runs CBlur's texture blur, an effect of its own). Two builder
rules made it: a buffer an effect draws and never reads is the GS's (its
result is for later; the builder had kept it as its own and lost it), and a
sprite may go from one GS buffer into another (not into itself). The
frame matches the GS path's at pad 3000 at 1x and 3x; `03b80` itself is 1
level above the GS path's in about a fifth of its pixels, where the 2x2
average lands on a half: the GS path's float modulate (128/255 then 255/128)
leaves those just under the half, the effect's integers do not (the GS's
own truncating arithmetic was tried and is further off). Nothing it
draws was refused on the walk or the battle.

**The rest, probed (2026-10-31).** `RG_NATIVE_EFFECTS_PROBE=1` hooks the
census's remaining functions, runs the builder over each one's packets with
tracing (the first two of each), logs what it would draw or why not, and
sends the packets on as they are. A probe takes only the packets between
the ranges of the effects and probes it calls, so each is named by the
innermost function. On the walk and the forced battle:

| Function | The builder's verdict |
|---|---|
| `CharaDrawTranslucentPass` (besides its capture and CBlur's blur) | transfers among the sprites; some calls draw nothing |
| `TexAnimeTableApplyMaybe` | a T8 or T4 texture (CLUT); transfers among the sprites |
| `MapDrawGroupRange` | transfers among the sprites; some calls draw nothing |
| `SunLightDraw`, `BattleEffectBurstDraw` | primitives other than sprites |
| `SwordTrailDraw` | a triangle strip of other than axis-aligned quads |
| `EffectEntryDrawNodes` | transfers among the sprites; some calls draw nothing |
| `GfxBlitTexRectPixels` (besides in `CBlurDraw`), `ScreenFadeBlockDraw`, `ScreenFadeBlockDrawWipeStrips`, `ScreenFadeBlockCaptureFrame`, `ScreenFadeCaptureFrame`, `ScreenColorFilterMaybe`, `FocusFilterDrawMaybe`, `LensFlareDraw` | draw nothing on this route (the battle's start included) |
| `GfxEndFrame`, `GfxFlipBuffersMaybe` | send nothing through the VIF1 lists |
| `DrawListFrameBegin` (the clear) | 16 untextured black sprites over the frame that also write Z 0 (ZTE with ALWAYS, Z not masked) under a draw environment set before it: a clear of colour and depth |

The game thread's own GIF packet each frame in the field (the census's "32
strips of the frame onto itself") is the flip's copy: 32 sprites of 16
pixels, DECAL with the texture's alpha, nearest, no blend, from the frame
(`00000`) through context 1, whose FRAME at that moment is `00e00` as CT24,
the display buffer (read from the GS's registers when the packet arrives;
the census took the target for the frame). `CBlurDraw` reads `00e00` the
next frame. On the title and the load screens it also uploads a 512x448
image into `02a00` every frame, in 896 tiles of 16x16. Both are plain
sprites and transfers: phase 4's batcher, and the present ("a present from
the scene target").

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

**Step 1, the upload audit (2026-11-01, `DQ8_GFX_UPLOAD_AUDIT=1`):** the
texture cache keeps which upload, draw or copy last wrote each 8 KiB page and,
for each texture and palette it builds, says whether an address table keyed
by the uploads could supply it (`[upload-audit]` lines on the stats report,
cumulative). The walk (pad 3000) and the forced battle (pad 3450) gave, in
builds: texture `exact` (same base, width and format as one upload) 14 and
18; `reinterpreted upload 00 as 13` (a CT32 upload read as T8, the game's
way of sending 8-bit art) 54 and 62, the largest class by texels; `partial-
page` (an upload that shares a page with another writer) 25 and 33, among
them a T4 read of the object buffer at `02a00` (an effect, not an upload);
`draw-or-copy` 7 and 8; `several-uploads` 0 and 1. Palettes: `partial-
page` 87 and 108 (a CLUT is under a page, so page granularity cannot judge
it), `draw-or-copy` 12 and 13. What it means for step 2: a table of
rectangles is not enough, because the common case reads an upload through
another format; an upload has to be kept as the words the GS would hold (the
GS's own layout for the format it was sent in), and a read in any format maps
onto those words. That is a sparse local memory by upload, and it can be
the first step of dropping the dense one. Block-level (256 byte) coverage
would settle the `partial-page` classes; the audit does not have it yet.

**Step 2a, the host writes' memory (2026-11-01, same switch):** the audit
also feeds every host upload to a second local memory that nothing else
writes, and at each build compares the texels' indices and the palette window
read from it with those read from `gs_vram`. The rule to serve a build from
it: no page it reads was touched by a draw, a copy or a write outside the
last upload since an upload covered the whole page. On the walk and the
battle every build the rule serves (about 70 textures and 90 to 110 palettes
a run, among them the `reinterpreted 00 as 13` and `partial-page` classes the
first audit could not judge) read the same texels from it, 0 differences; the
rest (7 to 8 textures of the frame buffer at `00000`, 11 of the object buffer
at `02a40`, one of `03800` written outside a recorded upload, 12 palettes) is
what a draw or a copy wrote and needs the targets. So the sparse store is
the uploads' own words in the GS's layout, and it already serves the
uploaded art; what is left for step 3 is naming the targets those 20 or so
builds come from.

**Step 2b, serving from it (2026-11-01, `DQ8_GFX_UPLOAD_STORE=1`):** the
texture cache reads texels and palettes from the host writes' memory when no
page they read was touched by anything else (the rule above), and from
`gs_vram` otherwise (`SourceScope` in `sdlgpu_textures.cpp`; the cache
builds, updates, palette refreshes, and the region identities and
replacements the draws ask for). The walk (pad 3000) and the forced battle
(pad 3450) end with all of VRAM and the display buffer byte-identical to the
runs without it; 173,560 reads came from the host writes' memory and 27,884
from local memory on the walk (246,475 and 36,011 in the battle). **Censuses (2026-11-01):** the audit also checks every update of a cached
texture (the movie writes its frames this way). The title screen and the
opening movie (150 s: about 6,000 updates, 1 M texels) and the walk with a
menu opened and closed (about 140,000 updates, 3.7 M texels) found no read
the rule served that differed from local memory (the 2,552 that differed were
all `needs-targets`: textures of the frame buffer). So the store is on
(`DQ8_GFX_UPLOAD_STORE=0` turns it off), and the walk is byte-identical with
it. It still keeps `gs_vram` for the reads that need the targets and for the
writes draws make.

**Step 3a, the frame no longer round-trips (2026-11-01):** the walk's
`resolves=60 (13440 Kpx)` per 60 frames, a readback of the whole 512x448
frame a frame, was one cause: the flip draws the frame into the display
buffer `00e00` as CT24, and CBlur's read of it as CT32 made a CT32 view of
the same pages, which imported the CT24 surface through local memory (a
`view-resolve`, `DQ8_GFX_TRACE_TARGETS=1`) because only CT32 sources went by
GPU page copies. A CT24 surface now counts as one for that import
(`DQ8_GFX_NO_CT24_IMPORT=1` undoes it); the alpha it carries is the draws'.
Also in: a CT32 texture's texels from the CT32 target that drew its pages,
texel for texel on the GPU (`gs_target_copy.frag`, `copyFromTarget`; the
way `expandIndexed8` does 8-bit ones; `DQ8_GFX_NO_DIRECT_TARGET_TEXTURES=1`
undoes it; `DQ8_GFX_CHECK_GPU_TEXTURES=1` compares it with the CPU's
expansion), which the walk reaches only for small ones: the frame's
texture (`tbp 00000`, 512x512) is dirtied by the flip's writes to rows 448 on,
and the object buffer `02a00` read as 512 wide is not one target's. Walk
and battle: `resolves=0`, VRAM and display buffer byte-identical to before.
What still goes through local memory: the imports into targets (`refreshes`:
225 Kpx a frame in the battle), the textures over several targets' pages,
and the palettes draws wrote.

**Step 3b, imports nothing reads (2026-11-01):** the per-object offscreen
buffers (`02a20` up to `02c40`; the game clears an object's box before it is
drawn into them) no longer import what local memory holds under them when
the native renderer is on (`DQ8_GFX_KEEP_OBJECT_IMPORTS=1` imports):
walk and battle byte-identical, the walk's `refreshes` from 2,979 Kpx to 579
per 60 frames, the battle's from 13,499 to 10,063. Two things this did not
get: the buffer at `02a00` (512x448) and the host writes patched into the
small buffers (`cpu-patch`, the textures uploaded to pages the buffers share:
2.5 M pixels in 140 s of the battle at `02ac0` alone) are read, since
dropping either changes the picture (100,512 bytes of VRAM differ on the
walk). Those patches are the aliasing itself: something draws or samples
the uploaded words through the target. Finding what is the next step.

**Step 4, no game-side local memory (2026-11-01):** with the native renderer
and the GPU backend, the game's 4 MiB of GS local memory is no longer
allocated: `PS2Memory` reserves the address range `PROT_NONE` (nothing
committed; a stray read or write faults, which is how the readers were
found), `GS` hands its backends a null pointer, and the sdlgpu backend keeps
a 4 MiB memory of its own (`GsTextureCache`'s host writes plus whatever a
readback asks for). `RG_GS_VRAM_RESERVE=0` allocates it as before; with the
native renderer off nothing changed. Checked on the walk, the forced
battle (shadow volumes, effects, HUD), the title screen and the opening
movie, the file-load screen: the same pictures as the allocated run, and no
fault. Two parts of the backend changed for it. The drawn-page rule became a
drawn-*block* rule (`gsMarkBlocks`, `GsTextureCache::noteDrawnBlocks`,
fed by `GsTargetCache::setDrawObserver`): uploaded textures share pages with
render targets (the transporter glow, the HUD) without sharing blocks, and
under the page rule they were read from the stale memory; the audit
(`DQ8_GFX_UPLOAD_AUDIT=1`, memory allocated) shows no uploads-only
DIFFERENT on the battle. The few builds that still read drawn blocks
(`DQ8_GFX_WARN_LOCAL_READS=1` lists them: the frame at `00000` as T8 with
its CLUT, `03800`, a 512-wide `00e00`, palettes at `03f80`, `02d20`) are
served by resolves into the backend's memory (`resolves=0` per 60 frames on
the walk, a few hundred `texture` resolves in a battle). So the honest
state: the game keeps no GS memory, but the backend's own still takes those
readbacks. `DQ8_GFX_DROP_RESOLVES=1` drops them and `DQ8_GFX_POISON_DRAWN=1`
makes a read of drawn blocks return garbage, so each one that matters shows
in the picture (it does: the glow and the HUD under the page rule, now only
those rare builds); getting them off the CPU is the target-sampling work of
loose end 2. The movies' frames and every other pad-timed screenshot differ
between runs by timing, so compare runs of one session, or use the audit.

**Step 5, textures uploaded into a target's pages (2026-11-01):** a texture
build or update whose blocks no draw wrote is read from the host writes'
memory and no longer through the target that shares its pages
(`expandFromTarget`, `resolveSource`; `DQ8_GFX_NO_BLOCKWISE_TARGETS=1` off),
and the host writes patched into the object buffers (`02a20` to `02c40`) are
not recorded any more (`DQ8_GFX_KEEP_OBJECT_PATCHES=1` records them): they
were the way those textures reached the target. Walk and battle byte-identical
to `vg1`/`vb1`; 254,352 target reads skipped in the battle, its `refreshes`
from 10 Mpx to 3 Kpx per 60 frames. Palettes inside a target (the frame at
`00000`, `03f80`) are copied out region-wise (`GsTargetCache::copyOut`, the
target stays the owner) instead of resolving everything the GPU holds of it.

**What the effects cost (2026-11-01, measured, not guessed).** Per frame the
effects run in this order, each reading and writing the frame after the one
before: the plane shadow, CBlur, the frame captured into `03b80`, a small
palette blur, the 16-strip resample through `02a00`, the glare. Skipping every
op that writes the frame (`DQ8_GFX_SKIP_FX_FRAME=1`) at 4x and 60 fps, not
throttled: power 41.2 to 36.4 W (12%), the GS backend's share of the wall 46%
to 39%. The 20 or so small blur passes on the 192x192 buffers cost 3-5 ms/s
each under `DQ8_GFX_GPU_TIMES=1` (deck mode); the scene's own draws are
about 40% of the GPU time and the effects' frame lay-overs about a fifth. So
fusing the lay-overs into one pass would save perhaps 8%, not a doubling; the
larger lever is the scene's offscreen work (the object buffers at the
internal scale, `02a00 512x448 3D blend` at 1,600 draws a second).

**Done when** `gs_vram` is no longer allocated in a native run (met) and the
spot checks above pass; a full playthrough is not required, since the flag
keeps the GS path for anything the checks miss.

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

### Stage E in object layers (2026-10-29, `RG_NATIVE_COMPOSITE`, on with the native renderer)

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
  The tails go into the frame alone, so where the device has dual-source
  blending (2026-10-30) a lay over is one draw: `gs_effect.frag`'s
  `DQ8_DUAL_SOURCE` variant gives C/128 as the blend's second output and
  As as the alpha (`EffectBlend::OverExact`); the replay stays the
  fallback. The walk's tails fall from 6 draws an object to 3. The frame at
  pad 3000 at 1x is byte for byte the replay's (`DQ8_GFX_NO_DUAL_SOURCE=1`);
  with the throttled box of the issue #10 comment at 4x, the composite
  layers take 1.41 ms a frame of GPU instead of 2.31 and the walk runs at
  56.7 fps instead of 55.5.
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
| 3 Post | M-L (about ten effects, each S-M) | the glare, the plane shadow, the per-object composites (object layers), CBlur's texture blur and the bloom done (2026-10-30); the depth of field, CBlur's lay over and the refraction's capture (2026-10-31); the rest in "Where it stands" |
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
