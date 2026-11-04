# The native pipeline: no GS when the flag is on

Written 2026-11-04. This is the goal and the plan, and it takes precedence
over the direction of [NATIVE-SUBMIT-PLAN.md](NATIVE-SUBMIT-PLAN.md) and
[GS-DECOUPLING.md](GS-DECOUPLING.md) where they differ. The history is in
[NATIVE-RENDERER.md](NATIVE-RENDERER.md). The camera and matrix facts are in
[VU1-RECON.md](VU1-RECON.md).

## The goal

With the native renderer flag on, the GS does not exist as a concept on the
renderer's side. There is no GS local memory, no GS registers, no GS
addresses or pages, no GIF packets, and no `GSRasterBackend`. The renderer
gets scene data it understands: geometry with its transforms, cameras,
textures and render targets by id, materials as plain enums, and effects
described by what they are for. That pipeline is the foundation for:

- temporal upscaling (DLSS, FSR, a TAA of our own): depth, motion vectors
  and a jittered camera
- modern shadows: a shadow pass from the scene's geometry and lights
- ray tracing and path tracing: world-space geometry, materials, and
  eventually normals
- newer effects: a post chain that reads the scene's targets by name

With the flag off, the GS path stays as it is, as the fallback. Upstream
keeps it too.

**Not the goal:** making the GS translation cheaper. A change counts as
progress only if it moves something off the GS boundary below. Most of the
native submit work from 2026-11-02 was the other kind.

## Where the line is

The game is a PS2 game. Its code builds GS packets, names textures by GS
address, and sets display registers. That doesn't change, because we don't
rewrite the game's logic. The line is drawn here:

- **Producers** (game-side code in `src/`, on the game and VIF1 threads)
  may read the game's GS-shaped data in order to understand it: hooks on
  game functions, the VU1 program starts, and, as an interim step, the
  packet converter. Producers own the tables that turn the game's addresses
  into ids.
- **The renderer** (a new native renderer in `thirdparty/dq8-gfx`) takes
  only native records. It never sees a GS address, register value, page or
  packet, and keeps no GS memory.
- **The GS frontend and `GSRasterBackend`** are never entered.

The converter is a producer that decodes GS packets. It's allowed for now,
but it's counted separately (T below), so it stays visible as the next
horizon after the line holds.

## Definition of done: the boundary counters

Every way into the GS gets a counter. The renderer is off the GS when every
G counter reads 0 with the flag on, on every check route. They're printed
together as one `[gs-boundary]` line per stats window. Each counter is also
split by **source**: the thread-local source tag (VU1 program, hooked
function, or 2D task) and, for kernel stubs, the stub. That way a nonzero
count names the code that has to move.

This is feasible because the boundary is narrow. All of the following were
found in the code on 2026-11-04.

| # | Counter | Where it sits | Who crosses it today |
|---|---|---|---|
| G1 | GIF packets into the GS frontend | `GS::processGIFPacket`, `processNativePackedGIFPacket`, `processImageContinuation` | the GIF arbiter (the game's DMA: 2D `GsPkt*`, about 4,750 packets a frame on the walk), compiled VU1 `XGKICK` (`ps2_vu1_core.cpp`), the VIF1 GIF stage (`vif1_thread.cpp`: packets the converter refused, the register and transfer packets it still sends) |
| G2 | GS register writes outside packets | `GS::writeRegister`, `clearFramebufferContext`, `clearActiveFramebuffer` | kernel stubs: `sceGsPutDrawEnv`'s clears (`Stubs/GS.cpp`), `Helpers/Support.h` |
| G3 | Uploads into GS memory | `GS::uploadImageNative`, the backend's `BeginTransfer` and `UploadImage` | texture and palette uploads (about 350 a frame), named by GS address even when held (`held_uploads.cpp`) |
| G4 | Reads of GS memory | `GS::consumeLocalToHostBytes`, `ReadVram`, `WriteVram`, `SnapshotVram`, `hostreads` by GS address, the depth probe by GS address | `sceGsExecStoreImage` (fade and menu captures), the lens flare's depth probe, debug tools |
| G5 | GS-typed renderer calls | `GSRasterBackend::Submit`, `SubmitMany`, `SubmitNativeMesh` (a `GSDrawState`), `SubmitNativeEffect` (GS bases, ALPHA/TEST/ZBUF values), `Present` (PMODE, DISPFB), `Flush`, `TextureFlush`, `Sync` | everything that draws today, converted or not |
| G6 | The GS memory model entered | `GS_NOTE_PAGE_BOOKKEEPING()` anywhere on the renderer thread, not just inside converted draws; GS memory allocated (bytes) | the sdlgpu backend's page sets, target cache by address, invalidation, `m_shadow` |

Notes:

- G5 and G6 are the general form of the a/b/c counters committed in
  `74e3e74`. Those stay as the finer split inside G5 and G6.
- **The last check is structural, not a counter.** Once every G counter
  reads 0, the flag-on build stops constructing the `GS` frontend and its
  backend. The game then either runs or faults on the first GS call, which
  is the most complete test there is. At that point the counters have done
  their job.
- **T, tracked but not gating (decided, see Open 1):** packets decoded by the converter
  (`build2D` and the effect builder), by source. These are producers reading
  GS packets. T at 0 means every 2D family is a function hook. That's the
  later horizon, and it's an open question below.

### What the counters can't prove

They only see what was played. A route nobody ran can still cross the
boundary. Two mitigations:

1. Print the first crossing per source and counter (`DQ8_GFX_WARN_GS=1`), so
   any crossing in ordinary play names itself. That's the same idea as the
   harvest mode for VU1 programs.
2. The structural check above: without a GS object, a crossing is a crash
   with a stack trace instead of a wrong picture.

**Check routes:** the walk, a forced battle, the title and opening movie, a
menu, the Zerard tour (`build/run/zerard-batch.sh`), and the town. These are
the routes the earlier steps were checked on, plus the town census that
NATIVE-RENDERER.md still lists as open.

## Readiness counters: what the pipeline gives the features (not gating)

The G counters measure removal of the GS. These measure whether the
pipeline is good enough for the features. They're per frame, over the
scene's draws, and printed as `[native-scene]`:

| | What it means | Needed by |
|---|---|---|
| R1 | draws with a camera id and their own model matrix (not one combined clip matrix) | motion vectors, upscaling, shadows, RT |
| R2 | draws with an object identity stable across frames | motion vectors for moving objects |
| R3 | draws into the scene target by id, with depth written as linear or reversed float depth | depth-based post, upscaling |
| R4 | draws whose material is fully known: texture id, blend enum, alpha test, fog | RT and path tracing materials, a deferred or forward+ pass |
| R5 | draws with normals or a lighting source we can rebuild | modern lighting, RT |

They aren't a definition of done. They show what a feature can rely on
before it's started, so features don't get built on guesses.

## The native interface

All records go through one ordered queue, the one the backend has today,
in the order the game built them. Every reference is an id. No GS types
appear.

- **Texture** `{id, format, width, height, mips, palette id or none}`. Made
  by the upload producer: `TexBuildUploadPkt` hook plus held uploads (exists,
  keyed by content). The producer keeps the address-to-id table: a page names
  its latest upload or the target that drew there. Indexed formats are kept
  as indices plus a palette id, and the renderer expands them in a shader.
- **Palette** `{id, entries, format}`. From the same hook.
- **Target** `{id, role, width, height, format}`. Roles: scene colour, scene
  depth, display, and offscreen (glare buffer, shadow buffer, object layers,
  and so on). Declared by the `DrawEnvSetFrameFromDesc` hook (exists,
  `gs_named_targets`, but by base address today). The renderer picks each
  target's resolution, which is what makes upscaling possible.
- **Camera** `{id, view, projection, viewport, jitter}`. From VU1 data memory
  at the program start: projection at quads 8-11, view at 24-27 (verified;
  `CameraRegistry` already exists).
- **Mesh draw** `{target id, camera id, model matrix, object id, vertices in
  object space, material, scissor in target pixels, pass tag, source tag}`.
  From the VU1 program starts (exists, `GSNativeMesh`). It changes from one
  combined clip matrix to model plus camera, and from `GSDrawState` to a
  material.
- **Material** `{texture id, sampler (filter, clamp), blend (an enum of the
  modes the game uses), alpha test, depth test and write, cull, fog}`.
  Resolved by the producer from the game's registers, once per state change.
- **Sprite draw** `{target id, rect, uv, colour, texture id, blend, layer:
  scene or UI}`. From the converter now and from `GsPkt*` hooks later. UI
  draws at output resolution.
- **Effect** by purpose: `Glare`, `Bloom`, `DepthOfField`, `PlaneShadow`,
  `ObjectLayer` (route, tail, composite), `CBlur`, `ColorFilter`, `Fade`,
  `RefractionCapture`, `TexAnime` (render to texture). Each has its own
  parameters from the hooked function and targets by id. The renderer
  implements each one as it sees fit, not pass for pass.
- **Frame and present** `{begin, end, present: which target, display rect,
  vsync tick}`. Replaces PMODE and DISPFB and the flip's copy.
- **Readback** `{target id, rect, format, destination in EE memory}` and
  **depth query** `{target id, point}`. These replace StoreImage's G4 path
  and the depth probe.

**The renderer** is a new class next to `SdlGpuBackend`. It shares the
device, window, presenter, shaders and pipeline helpers, but not the GS
model: no `GsSurface` by base and PSM, no page sets, no `m_shadow`. Its
frame is a small fixed graph: scene (colour, depth, velocity) →
TAA/upscale → post (the effects) → UI → present. Features slot into that
graph.

This is a new renderer, not a cleanup of the old backend. I recommend that
because the old backend's draw path is built around finding targets and
textures by GS address and page. Taking that out piece by piece is how the
last week ended up "shaped like the GS". The GS backend is left untouched
as the fallback.

## Order

One step per request. Each step lists its counters and states what pass
means before the code is written. Pass/fail is checked on the routes. A
build that compiles is not "done".

1. **Counters.** G1-G6, T, R1-R5, per source, with `DQ8_GFX_WARN_GS`.
   Pass: builds, and one walk prints a line in which every place listed in
   the table shows up as nonzero, or is explained. This is the baseline.
   **Built (2026-11-04), not yet run.** The counting is in `runtime/gs/gs_boundary.h` (patch 0034): a static site per
   crossing, counted per caller. Callers name themselves with a
   `gs_boundary::FromScope`: the GIF arbiter per packet (path1, path2,
   path3), the game's VIF1 thread and GIF stage, kernel stubs, and game hooks.
   A count under `?` came from an unmarked caller. Each stats window prints a
   `[gs-boundary]` summary line, one line per site hit with its callers, and
   the GS memory the backend holds (G6). The sites:
   - G1: `GS::processGIFPacket`, `processImageContinuation`,
     `processNativePackedGIFPacket`.
   - G2: `GS::writeRegister`, `clearFramebufferContext`,
     `clearActiveFramebuffer`.
   - G3: `GS::uploadImageNative`, and the frontend's `BeginTransfer` and
     `UploadImage` calls.
   - G4: `consumeLocalToHostBytes`, `ReadVram`, `WriteVram`, `snapshotVRAM`,
     `getDebugSnapshot`, `drawRegisters` (the converter's mirror reads the
     register file), `hostreads::begin`, `depthprobe::read`.
   - G5: the frontend's `Submit`, both `SubmitNativeMesh`,
     `SubmitNativeEffect`, the present, `TEXFLUSH`, and `FINISH`'s flush.
   - G6: every `GS_NOTE_PAGE_BOOKKEEPING()`, which replaces
     `gsNotePageBookkeeping()`, one site per function.
   - T: `build2D` and the effect builder, in packets.

   Not counted: `Initialize`, `Reset` and the backend swap (setup, left to
   the structural check), the debug-history internals, and the
   `SupportsNativeEffects` query. **Deviation, announced:** R1-R5 move to
   step 2. Today they are 0 by construction, because the fields they count
   only exist in step 2's record types.
2. **The interface and an empty renderer.** Record types, the queue, and the
   renderer class presenting a cleared scene target. Behind its own switch
   inside the native flag, so the current native path keeps working until
   the new one draws the scene.
3. **Textures, palettes and targets by id.** The producer tables. Pass: G3
   at 0 for named uploads.
4. **VU1 meshes as mesh draws** with camera and model split. Pass: G5's mesh
   share at 0, and R1 and R3 near 100% of scene draws. **This is the step
   that unlocks motion vectors and upscaling.**
5. **Effects by purpose**, as the profile ranks them. Pass: G5's effect
   share at 0.
6. **2D** through the converter as sprite draws, then the remaining VU1
   programs on packets (outlines, shadow volumes, particles). Pass: G1 and
   G5 at 0.
7. **Present, clears, readbacks, depth probe.** Pass: G2, G4 at 0. Then the
   structural check: no `GS` object.
8. **Features**: TAA/upscaling first, then shadows, then RT, each on the R
   counters it needs.

Steps 3 to 7 can be split differently once step 1's numbers show where the
crossings really are.

## Open questions

1. ~~Does T have to reach 0?~~ **Decided (user, 2026-11-04):** the
   converter is an acceptable producer, as long as it stays easy to replace
   if it turns out to be a bottleneck. What that requires:
   - The converter emits the same sprite and mesh records a function hook
     would. Nothing downstream knows which producer made a record.
   - Its packet decoding stays inside the converter. No GS state leaks into
     the records or the producer tables, beyond the address-to-id lookups
     every producer shares.
   - Replacement works one family at a time, by source tag. A `GsPkt*`
     family hooked natively stops sending packets, and the converter simply
     never sees them. T by source shows which families are left and what
     each one costs.
2. **Lighting for R5.** The VU1 programs light vertices themselves; the
   meshes arrive with lit colours and no normals. For RT, either the programs'
   inputs (normals, light slots in VU1 memory) go into the mesh draw, or the
   model resources are read directly (NATIVE-RENDERER.md's "model-resource
   altitude"). Not needed before step 8.
3. **FSR/DLSS under SDL GPU** (VU1-RECON item 6): whether native Vulkan
   handles are reachable. A TAA of our own is the fallback and proves the
   vectors either way.
