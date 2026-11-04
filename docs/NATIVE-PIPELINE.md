# The native pipeline: no GS when the flag is on

Written 2026-11-04. This is the goal and the plan, and it takes precedence
over the direction of [NATIVE-SUBMIT-PLAN.md](NATIVE-SUBMIT-PLAN.md) and
[GS-DECOUPLING.md](GS-DECOUPLING.md) where they differ. The history is in
[NATIVE-RENDERER.md](NATIVE-RENDERER.md). The camera and matrix facts are in
[VU1-RECON.md](VU1-RECON.md).

## Where it stands, and how to resume (2026-11-04)

**Done and checked on runs:** steps 1 to 4, 5a and 5b. The open items are
in each step's record.

**Built and committed, not yet run.** Test these first:

| Piece | Commit | What to check |
|---|---|---|
| 7a: clears and draw environments through the converter (patch 0035) | `81656ea` | no `GS::writeRegister` or `GS::drawRegisters`; the picture is unchanged |
| 7b: the depth probe from the renderer's depth | `81656ea` | the lens flare hides when something covers the sun; depth reads are answered, none unserved |
| 5b: CBlur green-to-alpha natively | `95b2ae7` | no `SubmitNativeEffect` on the walk; CBlur looks as before |
| 7c: colour captures from the renderer | `95b2ae7` | the menu and fades show the right frame; colour reads are served, none failed |
| 5c: object layers drawn by the producer | `7dc4a97` | no boxes around characters, which still draw whole |
| 7d: the pipeline's own present (patch 0036) | this commit | no `GS present (PMODE, DISPFB)`; window, screenshots and F1 menu as before; colour reads served with no marker on the GS queue |
| `tools/native-check.sh` | `1c52b19` | prints PASS/FAIL per check from a run log |

**To resume:**
1. Build: `cmake --build build/game -j$(nproc)`.
2. Run the walk: `RG_GS_VRAM_RESERVE=1 RG_NATIVE_PIPELINE=1
   tools/vu1/oracle/native-run.sh step7 gpu 110 3000`.
3. Check it: `tools/native-check.sh step7`. Screenshots are in
   `build/run/shots/step7`.
4. The lens flare, the menu and the fades are checked in play. The walk
   doesn't reach them.
5. If 5c misbehaves, `RG_NATIVE_COMPOSITE=0` takes it out (the boxes come
   back) to tell it apart from the rest.
6. Then fix what fails, and record the results in each piece's entry
   below.

**Step 6 (2026-11-04):** the leftover packets are absorbed, and the GS
tripwire is in. G1-G4 and G6 read 0 on the walk. The 2D pieces with an
unknown source (G5) are handled by host images; what's open is the local
copies. See step 6's record.

**Not built yet:**
- **The structural check proper:** not constructing the `GS` at all. The
  tripwire stands in for it until the unknown sources are handled.
- **Local copies and alpha-as-index textures** in the producer (step 6's
  record).
- **Step 8:** features.

**Known limits:**
- Targets clamp alpha above 0x80. This affects blends that read the
  destination's alpha.
- 7b needs a D32_FLOAT depth format.
- 7c's capture is served from the last frame drawn.

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
   **Baseline (2026-11-04, one walk):** `RG_GS_VRAM_RESERVE=1
   tools/vu1/oracle/native-run.sh boundary-base gpu 110 3000`, with the
   native flag at its defaults. 30 fps, one 60-frame window on the field
   (pad 2900 onward), **per frame**. In that run the report printed site
   names containing a parenthesis as empty (fixed since). The sites below
   are identified from the window's `[gfx]` and `[decoupled]` lines, except
   where marked unresolved.

   | | per frame | what |
   |---|---|---|
   | G1 | 1,656 + 548 | `processGIFPacket` and image continuations, all from the game's GIF stage (2 from game hooks). GIF paths 1-3 carry nothing: the VIF1 thread routes every packet. |
   | G2 | 16 | `writeRegister` from kernel stubs (the draw environment) |
   | G3 | 340 + 1,096 | transfers begun, payload calls |
   | G4 | 2 | one site from an unmarked caller, unresolved (the debug snapshot, `drawRegisters`, a host read or the depth probe) |
   | G5 | 1,516 + 67 + 1,288 + 2 | native meshes with a `GSDrawState`, native effects, one site unresolved (GS `Submit`, `TEXFLUSH` or `FINISH`), presents |
   | G6 | ~16,400 | page-bookkeeping entries. The largest are `gsMarkPages` 3,785, `pendingDrawsTouch` 1,496, `gsMarkBlocks` 1,395, `pendingDrawsTouchOthers` 1,305, `notePending` 1,249, `gsMarkCoveredPages` 1,111, `canPatchHostWrite` 888, `transferConflicts` 887, `sourcePagesFor` 808, `markReadPagesCt32` 677, `GsTextureCache::invalidate` 565. 80% of converted draws (75,575 of 94,995) entered it. GS memory held: 4 MiB local (the backend's own) plus 4 MiB host writes. |
   | T | ~5,100 | packets decoded: two converter sites (456 and 4,650, unresolved which is `build2D`). 706 of them came from an unmarked caller. |

   G6 comes from the renderer thread, which no caller scope reaches, so it
   is attributed by function only. What this says for the order: meshes
   (G5) and uploads (G3) are the bulk of the traffic, so steps 3 and 4
   remove most of it. G6 goes with them, because it is the old backend's
   way of handling exactly those calls.
2. **The interface and an empty renderer.** Record types, the queue, and the
   renderer class presenting a cleared scene target. Behind its own switch
   inside the native flag, so the current native path keeps working until
   the new one draws the scene.
   **Built (2026-11-04), not yet run.**
   - The records are in `gfx/native/native_records.h`, and the queue,
     switch and renderer in `gfx/native/native_pipeline.{h,cpp}`. They form
     the library `dq8_gfx_native`, which links SDL3 alone.
   - **Checked at build level:** under that library's own compile flags, a
     GS header fails (`runtime/gs/gs_boundary.h` not found), so the renderer
     cannot see a GS type.
   - `RG_NATIVE_PIPELINE=1` takes effect only under the native renderer
     flag. It is decided once, at first use, through
     `native::setOuterSwitch(&rg::nativerenderer::enabled)` in
     `graphics.cpp`.
   - The first producer is the flip hook (`flip_pacing.cpp`). It declares
     the scene target once (640x448, `SceneColor`) and pushes a `Present`
     at each flip.
   - The backend's presenter (`acquirePresentation`) hands the frame to
     `Renderer::render` in place of the display registers. The renderer
     drains the queue, keeps targets and cameras, counts R1-R5 over mesh
     draws, and clears the scene into the backend's pending command buffer,
     which the present copy submits first.
   - The old backend still draws everything the GS path sends, unseen.
   - Without a window, the GS present path is kept. Whether screenshots
     capture the native scene has not been checked.
   - R1-R5 read 0 of 0 until step 4 sends mesh draws.
3. **Textures, palettes and targets by id.** The producer tables. Pass: G3
   at 0 for named uploads.
   **3a done (2026-11-04).** Split from 3b (announced): records first, the
   GS still sent the uploads.
   - **Producer** (`src/native_producer.{h,cpp}`, game side). It is called
     from `TexBuildUploadPkt`'s hook for each palette and mip level, and from
     both target declarations.
     - Decoding: CT32 as it is, CT16 to RGBA8, T8, T4 (widened to a byte a
       texel), pre-swizzled 8-bit art through the CT32-to-T8 layout, and
       palettes in CSM1 order.
     - Identity: content plus shape. A texture is sent once and named by id
       afterwards. The renderer's evictions (256 MiB budget) are drained so
       an evicted texture is sent again.
     - Tables for step 4: `textureAt(tbp, psm)`, `paletteAt(cbp)`,
       `targetAt(base)`.
   - **Renderer:** GPU textures (RGBA8, R8 indices, palette rows). Its first
     shader (`gfx/native/shaders/native_view.*`, SPIR-V only so far) draws
     the newest 48 textures as a grid on the scene: the debug view.
   - **Screenshots** capture the native scene.
   - **One walk** (`pipeline-step3a`, 110 s): per frame, about 180 texture
     levels arrive, almost all already known. 193 textures (5 MiB) and 186
     palettes were held, with none refused for format, size or layout.
     - The screenshot at frame 6002 shows the game's textures correctly
       decoded and paletted: character face sheets, the compass, HUD icons,
       the minimap, fonts and portraits.
     - **Found:** the producer hashes about 4.2 MB a frame on the game thread
       to recognize textures. The cost is unmeasured. Keying by
       `NameTableNodeSetImage`'s generation would avoid it.
     - **Found:** targets churn. About 46 target records a frame are
       re-declarations of the same base with another size. Step 4 has to
       decide whether a base with several shapes is one target or several.

   **3b done (2026-11-04).** With the pipeline on, a named upload the
   producer turned into a record no longer reaches the GS. The VIF1 thread's
   `sendToGs` walks each packet's GIF tags (`withholdUploads`) and drops:
   - the transfer's four registers (BITBLTBUF, TRXPOS, TRXREG, TRXDIR, host
     to local). Every such A+D block in the game also sets another register,
     so those go on in a block of their own.
   - its IMAGE data, in the packet and in the image continuations after it.

   It matches against the producer's own queue of uploads it made records
   of (`takeProduced`, in the game's order, the VIF1 thread its only
   consumer), not the backend's named queue. The VIF1 thread runs far
   enough ahead of the GS worker that the shared queue's 64-entry lookahead
   missed most uploads: the first try withheld 1,667 of about 22,900 a
   window.

   One walk (`pipeline-step3b`, per 60-frame window on the field):
   - **Withheld:** 11,361 textures and 11,241 palettes (265 MB), with 0
     uploads sent on for lack of a record. 22,842 A+D blocks were split.
   - **G3:** `BeginTransfer` fell from about 21,700 to 540 and
     `UploadImage` from about 66,000 to 600.
   - **Same game:** draw counts at windows 30, 45 and 60 match 3a's
     (302/304, 21,439/21,450, 13,552/13,584, the same jitter as earlier
     runs). The native grid shows the same textures as 3a. No errors.
   - **Not yet explained:**
     - The 540 transfers left. The site is now split by direction
       (host to local, local to host, local to local), so the next run
       names them; local-to-local copies from the effects are likely.
     - About 180 produced texture levels a window that were never taken.
       They were built but not sent, or sent by a path other than
       `sendToGs`.
     - 540 image continuations that arrived as packet starts. They are
       handled, but the shape is unconfirmed.
   - **Readbacks:** G4 shows no StoreImage reads on this route. The fade
     and menu captures, and the depth probe, were not reached, so they are
     unchecked with withholding on.

4. **VU1 meshes as mesh draws** with camera and model split. Pass: G5's mesh
   share at 0, and R1 and R3 near 100% of scene draws. **This is the step
   that unlocks motion vectors and upscaling.**

   Split (2026-11-04), each part with its own pass:
   - **4a. Records alongside the GS.** Each VU1 program mesh that goes to
     the GS (`deliverMesh` with the program's VU1 data) also becomes a
     `MeshDraw`. It carries:
     - the camera (view 24-27, projection 8-11, viewport 4-7 with XYOFFSET
       folded in) as its own record;
     - the model (60-63), checked against the program's own matrix (68-71):
       on a mismatch the draw keeps the whole matrix with no camera;
     - the material from the converter's mirrored draw state, textures and
       palettes by the producer's tables;
     - indices with the program's guard band and backface skips applied, so
       the renderer needs no GS rule.

     The GS still draws everything. **Pass:** mesh records about equal to
     the VU1 share of G5's `SubmitNativeMesh`; R1 near 100% of them (the
     model check holds); texture and target misses named and counted.

     **4a done (2026-11-04).**
     - `rg::nativeproducer::mesh` (`src/native_producer.cpp`) is called from
       `deliverMesh` for the meshes `buildMesh` makes. It builds the record
       under the producer's lock, then pushes it.
     - The model check compares model x view x projection x viewport with
       68-71, per column, within 2e-3 of the column's largest entry.
     - Records gained what the material needs: `TextureFunction`,
       `textureAlpha`, `AlphaFail`, `colorWrite`, and the fog's parameters
       and color on the mesh. Depth compares are reversed (Greater is
       nearer), as the game's are.
     - Material unknowns are counted by reason: blend equations outside the
       five, region clamps, partial write masks, and DATE or FBA. A CLUT
       offset (CSA) counts as a palette miss.
     - Depth buffers get ids of their own, by ZBUF base, sized like the
       color target they're first drawn with.
     - The `[native-producer]` line now has a `meshes` part with all of the
       above.

     Found on the way, both fixed:
     - **Quads 4-7 are not a viewport.** They are the projection straight to
       the window: 8-11 times the screen's scale (256, -224) and offset
       (2048). The program's matrix is model x view x (4-7), so the camera
       keeps 8-11 as its clip space, and its viewport is inverse(8-11) x
       (4-7). VU1-RECON's table had it as a viewport after 8-11.
     - **Textures resolved in stream order.** The game thread's tables run
       a frame of uploads ahead, and the game reuses its texture addresses
       within a frame: 38% of meshes found the wrong format there, and the
       hits could have been the wrong texture. Meshes now look up what the
       VIF1 thread last sent to each address (`residentTexture` and
       `residentPalette`, set in `takeProduced`).

     One walk (`pipeline-step4a`, per 60-frame window on the field):
     - 64,466 mesh records (about 1,070 a frame), all from VU1 program
       meshes. G5's `SubmitNativeMesh` shows 120,311; the rest are the
       converter's 2D pieces (`build2D`), which are step 6.
     - **R1** 100%, with one camera a frame. The model check never failed.
     - **R3** 85%. The rest draw to offscreen targets.
     - **R4** 95%. Of the misses: 2,340 a window are one texture (0x2a60,
       sampled as a 64x64 CT32 where an 8-bit texture was uploaded,
       probably one the GPU writes), and 2,580 use DATE or FBA.
     - About 25,000 triangles a frame are drawn, and 5,400 are culled by
       the program's rules.
     - The renderer has no evictions at 5 MiB, so a resident entry pointing
       at an evicted id isn't handled yet.
   - **4b. The renderer draws them.** One pipeline: model x view x
     projection, float depth (reversed), texture and palette sampled, the
     game's texture function, blend, alpha test, fog. The debug grid goes.
     **Pass:** a screenshot shows the field's world and characters in
     place; R3 at 100% of scene draws.

     **4b done (2026-11-04).** `native_meshes.cpp` in `dq8_gfx_native`, and
     the `native_mesh` shaders.
     - **Frames:** the draws between two Present records are one frame. The
       scene keeps the last frame until the next one closes, because the
       host presents more often than the game.
     - **Per frame:** one vertex buffer and one index buffer, and a pipeline
       per blend, depth and write-mask state.
     - **Depth:** a D32 float target, reversed (cleared to 0). Depth is
       clamped rather than clipped at the far plane, because the sky sits
       past it.
     - **Fragment shader:** the game's four texture functions in its units
       (0x80 is 1.0). Index8 textures are fetched and looked up in their
       palette, not filtered.
     - **Not drawn here:** draws into other targets (the effects' buffers)
       are counted and skipped, for step 5. `RG_NATIVE_TEXTURE_GRID=1`
       keeps 3a's grid over the scene.
     - **Known approximations:** the blend's alpha is written as As / 0x80,
       clamped, so a source alpha over 0x80 doesn't amplify and the stored
       alpha isn't the game's. A failing alpha test with a fail mode other
       than "nothing" passes. Indexed textures are never filtered.

     One walk (`pipeline-step4b`): about 870 mesh draws a frame into the
     scene, about 150 a frame offscreen, with none refused (no camera,
     texture or pipeline missing). The screenshots show the field's world,
     the character, the beam and the compass from the native renderer.
     Where a character is drawn through an object layer (frame 6001), only
     its directly drawn parts (weapon, hair) show, as expected until step 5.
   - **4c. Withheld from the GS.** A mesh made a record no longer reaches
     the GS. **Pass:** G5's VU1 mesh share at 0, same draw counts as 4a.
     The depth probe reads GS depth, so it goes wrong here until step 7.

     **4c done (2026-11-04).** `rg::nativeproducer::mesh` returns whether it
     made the record, and `deliverMesh` returns right after when it did.
     One walk (`pipeline-step4c`): the same 64,466 mesh records a window as
     4a, all made, so all withheld. G5's `SubmitNativeMesh` fell from about
     120k to 56k a window, and what's left is the converter's 2D pieces
     (step 6).

     Found: with composite batching on (`RG_NATIVE_COMPOSITE`), a
     character's head is routed to a layer and its tail's programs send GS
     packets instead of meshes (`inCompositeTail`). The native renderer
     never sees the tail, so characters were drawn in part, in 4b too. One
     walk with batching off (`pipeline-step4c-nocomp`) drew them whole, so
     batching is now off under the pipeline (`compositesBatched`;
     `RG_NATIVE_COMPOSITE=1` still forces it on).

     **Left for step 5:** where the game draws characters into its own
     object layer and composites it (frame 6001 of each walk), only their
     directly drawn parts show. Those draws are the "offscreen" count,
     about 190 a frame.
5. **Effects by purpose**, as the profile ranks them. Pass: G5's effect
   share at 0.

   Split (2026-11-04). With composite batching off, the effects that reach
   G5 are about 8 a frame, each a list of sprite ops between GS buffers.
   The object layers' models are already mesh draws into offscreen targets
   (4b's "offscreen" count). So the renderer first draws into any target,
   and then the effects become sprite records between targets: the
   converter as a producer, as decided in open question 1.
   - **5a. Every target drawn.** The frame's mesh draws run in the game's
     order into the targets they name. Offscreen targets keep their
     contents across frames (the game accumulates into some), only grow,
     and are drawn at the target's current size. Depth targets are
     cleared on their first use in a frame. Also counted: where the
     effects and the converter's 2D pieces that still go to the GS draw.
     **Pass:** no mesh draw refused for its target; the scene unchanged.

     **5a done (2026-11-04).** `Renderer::drawFrame` runs the frame's mesh
     draws in order, one render pass per run of draws into one target.
     - **Targets:** offscreen targets are GPU textures kept by id
       (`m_gpuTargets`), sized only upward and drawn at the target's
       current size through the viewport, so the ~46 re-declarations a
       frame cost nothing.
     - **Depth:** each depth target is cleared at its first use in a frame.
     - **Pipelines:** whether a depth target is attached is part of the key.

     One walk (`pipeline-step5a`, per window): 61,389 mesh draws drawn,
     10,601 of them offscreen, in about 20 passes a frame. None refused
     (target not declared, depth as color, camera, texture, pipeline: all
     0). The scene is the same as 4c's.

     The survey for 5b, per window:
     - **Effects:** 480 effects, all sprite lists (8 a frame), with 4,800
       ops. Into: the scene 1,353, declared offscreen targets 3,387,
       undeclared 60. From: nothing (fills) 720, a declared target 4,080.
       Every op names targets the producer already has ids for, except
       the 60 into an undeclared buffer.
     - **2D pieces:** the converter's 2D pieces still going to the GS as
       meshes are 39,299 (655 a frame). Into: the scene 3,781, offscreen
       targets 35,518. Textured from a target 2,137, from a texture 858,
       the rest untextured.

     So the offscreen targets' contents come mostly from 2D pieces, and the
     effects read them back. 5b needs both, which brings part of step 6
     forward.
   - **5b. Effects as sprite records.** The converter's effect sprites
     (and the 2D pieces the effects' buffers need, such as their clears)
     become sprite draws, naming targets as sources. **Pass:** G5's effect
     share at 0; characters drawn through object layers show whole.

     **5b in progress (2026-11-04), paused for review.**

     Built:
     - **Window-space mesh draws.** A MeshDraw without a model is in the
       target's pixels. When it uses depth, its GS Z is mapped to the
       renderer's depth through the newest camera: Zgs is linear in the
       projection's z / w.
     - **2D pieces.** Every converter 2D piece becomes one
       (`rg::nativeproducer::piece`) on all three paths: `deliverMesh`,
       GsPkt blocks, and whole converted packets. Game-thread pieces wait
       for the VIF1 lists in flight first.
     - **Effects.** An effect's sprite ops become one window-space draw
       each (`rg::nativeproducer::effect`), all or nothing per effect.
     - **Targets as sources.** `Material::sourceTarget` names a target to
       sample. A draw reading its own target reads a copy, taken just
       before it in a pass of its own.
     - **Blending in general.** The GS's (A - B) * C + D is Cs * Fs + Cd * Fd
       with factors 0, 1, C, 1 - C or -C. The records now carry a GPU-style
       blend (`BlendFactor`, `BlendOp`) instead of five named ones, and a
       fixed factor goes through the blend constant.
     - **CT24 reads** take TEXA from the converter's mirror when the effect
       left it to the GS.
     - **Targets:** the scene is the first buffer named as the frame;
       another named so (CBlur's 02a40) gets its own id. A target that isn't
       the scene never shrinks, and grows to the scissor of what draws into
       it. One the game never names is declared from its draws.
     - `RG_NATIVE_FRAME_DUMP=N` prints flip N's draws, one line each.

     One walk (`pipeline-step5b`, per window):
     - **2D pieces:** all 58,900 made.
     - **Effects:** 360 of 480 made. The 120 left are CBlur's green-to-alpha
       (the GS's 16-bit view of a 32-bit buffer), the only effect left on
       the GS.
     - **G5:** `SubmitNativeMesh` doesn't appear; `SubmitNativeEffect` is at
       120.
     - **Renderer:** 124,175 draws (73,842 offscreen) in about 93 passes a
       frame, 20 of them for copies. None refused.

     **Green-to-alpha (built 2026-11-04, not yet run):** CBlur's op, and
     the colour filter's, is now a native draw. It reads a copy of its own
     target and writes only alpha: alpha = (g & 0xFC) | (a & 3), the same as
     the GS backend's effect shader (`Material::shuffle`,
     `ChannelShuffle::GreenToAlpha`). **Pass:** the producer's "made green
     to alpha" counts 120 a window, G5 `SubmitNativeEffect` reaches 0 on the
     walk, and CBlur looks as before. Alpha above 0x80 is still clamped in
     targets.

     Bugs found on the way, each one blacked out the picture:
     - CBlur's buffer aliased to the scene, so its copy replaced the scene.
     - The glare's blend wasn't one of the five, and fell back to alpha, so
       laying the glare over the scene replaced it.
     - Layers were drawn past their declared height, so composites read
       nothing.

     **Characters, found (2026-11-04):** the scene reads the character
     layers as **PSMCT24**, so the alpha comes from TEXA (TA0, and 0 for
     black texels with AEM), not from the layer. Effect ops already did
     this; 2D pieces now do too (1,957 a window). The Hooded Man now shows
     solid.

     The **destination alpha test** (DATE/DATM) is now drawn too
     (`Material::destinationAlphaTest`). The draw reads the target's alpha
     at its pixel from a copy taken just before it, as draws reading their
     own target do. 60 a window.

     **Also fixed (2026-11-04):**
     - **Textures uploaded over targets.** The game uploads textures into
       memory some buffers also use (02a00, 02a40). A draw sampling there
       read the target even when a texture had been uploaded since. The
       producer now tracks which wrote each address last, in stream order
       (`uploadedSeq`, `drawnSeq`), and samples that: 13,530 samples a
       window. This fixed the minimap's dark panel and gave the compass
       its N/E/S/W letters.
     - **Fog on 2D pieces.** Decoded from build2D's form (w = 1 + (F +
       0.25) / 255) into a per-vertex `Vertex::fog`. The vertex shader uses
       it for window-space draws.
     - `RG_NATIVE_UNKNOWN_LOG=1` prints, once per combination, why a 2D
       piece's material is unknown, with its raw registers. Left now: FBA
       on draws into the layers, which writes alpha 0x80 anyway.

     **Boxes around characters drawn through layers: cause found, fix
     deferred (2026-11-04).** The GS path shows the same boxes with
     composite batching off (`RG_NATIVE_COMPOSITE=0`, run `gs-layers`) and
     none with it on (`gs-layers-batched`): the scene at frame 3000 of each,
     decoded from their VRAM dumps. So the native renderer reproduces the
     GS. The boxes come from running the game's object layers unbatched,
     which 4c chose because batching sends each object's tail to the GS as
     packets. Unbatched, the layer buffer (02a00) is memory the shadow
     volumes and texture uploads also use, and what's left in an object's
     box isn't the black the CT24 + AEM composite needs.

     **The fix** is batching done natively: each object drawn into a layer
     of the renderer's own, cleared to transparent, with its tail
     composited from it (`kRouteLayer`, `kObjectTail` and `kComposite` as
     records: the plan's `EffectKind::ObjectLayer`). It's a piece of its
     own: **5c**.

     **5c, built (2026-11-04), not yet run.** Composite batching is back on
     under the pipeline (`RG_NATIVE_COMPOSITE=0` turns it off). The producer
     handles the layer commands itself:
     - **`kRouteLayer`:** the object's draws into the routed buffer (02a00)
       go to one layer target of the producer's own. That target is
       cleared to transparent black over the object's box when routing
       starts. `kRouteEnd` stops the routing.
     - **`kObjectTail` with sprites:** the tail's ops that read the layer
       buffer read the layer target. They're drawn in stream order, so
       `kComposite` has nothing left to do and one layer target serves
       every object.
     - **`kObjectTail` without sprites, naming the buffer:** the tail stays
       packets, which read the buffer, so the layer's box is first copied
       back into it.
     - Routed draws don't count as writes to the buffer in the last-writer
       rule.

     **Pass:** no boxes around characters on the walk. The producer's
     `object layers` count goes up with `tails made`, G5 has no
     `SubmitNativeEffect` for route, tail or composite, and characters
     still draw whole. That last one was 4c's failure.

6. **2D** through the converter as sprite draws, then the remaining VU1
   programs on packets (outlines, shadow volumes, particles). Pass: G1 and
   G5 at 0.

   **6, the remainders (2026-11-04, `step6b` walk).**
   - **The packets left over are absorbed.** Under the pipeline (and the
     converter's own draw state), what is left of a packet after the
     converter and the upload withholding no longer goes to the GS
     (`absorb` in `src/vif1_thread.cpp`). Nor do image continuations.
     SIGNAL, FINISH and LABEL are set in the privileged registers (CSR,
     SIGLBLID) as the GS front end would. The rest is counted on a
     `[native-absorb]` line by kind. `RG_NATIVE_ABSORB=0` turns it off.
   - **Result on the walk:** G1, G2, G3, G4 and G6 read 0 in every window.
     Packets absorbed are about 69k a window, mostly TEXFLUSH (85k). There
     are about 300 local copies, 2.2 MiB of image continuations and no
     draw kicks, so nothing drawn was lost. FINISH, SIGNAL and LABEL never
     came on the walk. The picture is unchanged.
   - **The tripwire** (`src/gs_tripwire.cpp`, outermost on the GS backend
     chain) is the structural check in all but name. Under the pipeline any
     call into the GS backend is logged once per kind with a backtrace.
     Draws, transfers, flushes and syncs are dropped. Reads and
     presentation still go through. `RG_NATIVE_GS=abort` stops at the first
     crossing; `pass` sends everything as before. On the walk the only
     crossing is `SubmitNativeMesh` from the GIF stage: 96,552 of them,
     each a 2D piece whose source the producer doesn't know. G5 shows the
     same.
   - **2D pieces with an unknown source: handled (host images).** Both
     sources were host uploads (TRXDIR 0) that skip the texture hook, so
     the producer never made a record and the absorber dropped them:
     - `02a00` CT32 is the boot movie (JAPAN Studio logo and the rest).
       Each decoded frame arrives as about 880 16x16 uploads into a
       512-wide buffer, about 53 MiB a second.
     - The title screen is four 16-colour images uploaded once (frame
       ~1700, and once at boot), two to each 32-bit buffer: `1c00` T4HL
       512x416 and T4HH 512x384, `0e00` T4HL and T4HH 512x384. Their
       palettes are 8x2 CT32 uploads at `3fc0`, `3fd0`, `3fe0`, `3ff0`.
       (The PSM `0x24` first noted as T4HH is T4HL.)

     The absorber now gathers each such upload's pixels, including image
     data that continues past its packet, and gives them to the producer
     (`hostUpload`). The producer keeps one linear image per base and
     format, written rectangle by rectangle. An upload erases images of
     another format whose memory and bits it overlaps; T4HL and T4HH share
     words but not bits, so they coexist. A 2D piece that reads an image
     in its own format and buffer width samples it (`sourceOf`): the GS
     reads back what it was written with, so no address swizzle is
     needed. A changed image becomes one texture record when a draw reads
     it, deduplicated by hash; the movie costs one 1 MiB record a frame.
     Palettes come from host images at the CBP the same way (8x2 for 16
     entries, 16x16 with the CSM1 swap for 256). A host image and a
     produced texture at the same base: the later one in the stream wins,
     and likewise against a target drawn there.

     On the walk (`hostimg`) pieces with an unknown source went from
     96,552 to 0, the tripwire is never entered, and every check passes.
     The boot logo and the title screen (logo, swirl) show. At frame 1802
     the menu and copyright text were not up yet; the reference has them
     at 1835, so the shot may be before their fade. Check it in game.

     Not handled: a read in another format or buffer width than the
     upload's (counted as `reads refused`), CT24 host images (alpha is
     TEXA's), overlap between images at different bases is approximate
     (a byte span, not pages), and 3D meshes (`materialOf`) don't look at
     host images yet.
   - **Local copies (TRXDIR 2), absorbed, not mirrored.** These are the
     shapes seen:
     - `2a20` T8 into `2a00` T4, in tiles with wraparound offsets: a
       scrolling texture animation, done by copying GS memory between
       formats.
     - `3840` T8 into `3800` T8, 127x39.

     Mirroring these needs the producer to model GS memory layout across
     formats. That is a design decision, not a fix. Until then the
     textures they make are stale or missing in the native picture.
   - **Produced levels never taken:** the ones skipped are all `DBP 2aa0
     DBW 1 CT32 64x64`, inside the `2a00` area above. Its upload never
     reaches the walker as a transfer of that shape. The queue is bounded
     (8192, with a lookahead of 64), so nothing grows.
   - **Target churn** is gone: targets keep their largest size, and
     `targets new` is 0 in steady windows.
   - **Hashing** is about 4.4 MB a frame with a 4-lane 64-bit hash, roughly
     1 ms on the game thread. Cutting it means knowing which guest memory
     changed. Left as is.
7. **Present, clears, readbacks, depth probe.** Pass: G2, G4 at 0. Then the
   structural check: no `GS` object.

   On the walk (`pipeline-step5b`) G2 and G4 come from one place each:
   - **G2:** `GS::writeRegister` from kernel stubs, 960 a window (16 a
     frame). These are the libgraph stubs' draw environments and clears
     (`sceGsSwapDBuff`'s register pairs and clear sprite).
   - **G4:** `GS::drawRegisters`, 120 a window. This is the converter
     reading the register file back after those stubs (`gsWaiter`).

   Neither the depth probe nor the colour captures were reached on the
   walk. They are G4 wherever they run.

   **7a, clears and draw environments (built 2026-11-04, not yet run).**
   - **Pass:** under the pipeline, G2 has no `writeRegister` and G4 has no
     `drawRegisters`, and the picture is unchanged on the walk.
   - The GS front end gains a per-thread register sink (patch 0035,
     `GS::setRegisterSink`). While a thread has one set, its
     `writeRegister` calls are collected instead of applied, and its
     framebuffer clear shortcuts are dropped. The clear sprite that follows
     them is among the collected writes.
   - `gsWaiter` sets the sink around the stub. It then sends the writes
     through the converter as one A+D packet, in order with the lists it
     has just drained.
   - So the mirror follows the stub's registers with no read-back, and the
     clear sprite becomes a 2D piece like any other.

   **7b, the depth probe (built 2026-11-04, not yet run).**
   - **Pass:** at a lens flare (the sun in view), the producer's depth
     reads count up with answers. The renderer's line shows them copied
     and published with none unserved. The flare is hidden when an object
     covers the sun, and there is no G4 `depthprobe::read`.
   - The native library has a GS-free mailbox (`native::readDepth`,
     `DepthBlock`), modelled on the GS backend's probe: reads are told
     apart by their order, and answers arrive a frame or two late.
   - At each `render`, the renderer publishes the copy that has finished.
     It then copies the asked-for blocks out of its D32 depth targets, from
     the last frame drawn, in a command buffer of its own with a fence.
   - Each block is reduced to game pixels, keeping the nearest value.
   - The producer (`readDepth`) maps the read's SBP to the depth target by
     its ZBUF base. It turns reversed depth d back into GS Z through the
     last 3D camera's viewport: Z = v22·(1 − 2d) + v32, with 0 where
     nothing was drawn.
   - The `sceGsExecStoreImage` hook serves it ahead of the GS probe.
   - Only a D32_FLOAT depth is served. On D24 the reads count as
     unserved.

   **7c, colour captures (built 2026-11-04, not yet run).**
   - **Pass:** opening the menu and a screen fade under the pipeline show
     the right frame behind them. The renderer's line counts `colour
     reads: served` with none failed, and there's no "a colour read was not
     served" line.
   - The approach is option 1, answering from the last frame drawn. The
     native library has a GS-free mailbox (`beginColorRead` and
     `waitColorRead`, by target id, in game pixels).
   - The game thread opens a read, then sends a marker
     (`native::kServeCommand`) down the old backend's effect queue. The
     backend's worker reaches the marker in stream order, submits whatever
     is pending, and calls `Renderer::serveColorReads`.
   - That copies the region out of the scene, or out of an offscreen
     target, waits for the GPU, and averages each game pixel's
     scale × scale block. Alpha goes back to the game's units.
   - The producer maps the read's base to a target (`captureTarget`). The
     display buffer, or a buffer never drawn, maps to the scene. The hook
     packs the result as CT32, CT24 or CT16.
   - The marker still rides the GS backend's queue (G5). It goes away with
     7d.

   The analysis this was built from: The fade's and the menu's
   frame captures (`sceGsExecStoreImage` of colour) still take the GS's
   host-read path. Under the pipeline the GS targets hold nothing, so these
   captures are wrong there.

   The blocker is that the renderer runs only when the game's flip makes
   the GS present. A capture the game thread waits on can't be answered
   until the next flip, which the waiting thread is holding up, so it
   deadlocks. The options:
   1. Answer from the last frame drawn.
   2. Give the pipeline its own present driver, which the structural check
      needs anyway (7d).
   3. Keep the capture on the GPU: note which main-memory range holds a
      capture of which target, and when that range is uploaded as a
      texture, copy target to texture. This is the upscale-friendly one.

   **7d, present (built 2026-11-04, not yet run).**
   - **Pass:** under the pipeline, `tools/native-check.sh` shows no `GS
     present (PMODE, DISPFB)`. The window, the screenshots and the F1 menu
     look as before, also with the game paused. The menu capture and fades
     still show the right frame, with colour reads served and none failed.
   - Patch 0036 gives the runtime's presenter loop a host hook
     (`ps2SetHostPresentHook`, its own header). At a new vsync, a hook that
     returns true has shown the frame, and the GS latch is skipped.
   - The game's hook (`presentNative` in src/graphics.cpp) presents when the
     flip counter moved. The flip hook pushes the Present record before it
     counts the flip. Without a flip for 200 ms it presents at every vsync.
   - The backend's `presentNativeFrame` is `PreparePresentation` with no
     GS request: the renderer's scene into a presentation slot, then the
     usual window display, screenshot download and capture.
   - Under the pipeline the flip no longer prepares a GS presentation on
     the game thread.
   - Colour reads: the hook calls `serveNativeReads` at every pass of the
     presenter loop. The 7c marker (`kServeCommand`) is gone, and so is its
     G5 `SubmitNativeEffect`.
   - The hook writes the screenshots itself, in the GS latch's format and
     naming.
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
