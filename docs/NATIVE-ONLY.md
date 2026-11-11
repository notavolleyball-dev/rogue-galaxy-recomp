# Native-only rendering: removing the GS and VU1 coupling

The work plan for drawing the game without the emulated GS and without
deriving the renderer's input from GS packets or VU1 data memory. Written
2026-11-11. This page is what to work from. [NATIVE-RENDERER.md](NATIVE-RENDERER.md),
[NATIVE-PIPELINE.md](NATIVE-PIPELINE.md), [NATIVE-BOUNDARY.md](NATIVE-BOUNDARY.md)
and [VU1-NATIVE.md](VU1-NATIVE.md) stay as the record of what exists and
why. Analyses of individual game functions go in
[NATIVE-ONLY-NOTES.md](NATIVE-ONLY-NOTES.md), one section per function.

## Read this first: this is a large structural change

**This plan rebuilds how the game is drawn.** It deletes the emulated GS (about
17,600 lines of the backend, plus the GS front end on the draw path). It
deletes the packet plumbing in `src/` that turns GS data back into records
(the draw-state mirror, the 2D converter, effects rebuilt from sprites,
upload withholding, the absorber, most of the Draw stage). It replaces what
feeds the renderer with hooks on the game's own functions: effects, 2D blocks,
fonts and model draws. Tens of thousands of lines change hands. That is the
point of the work, not a risk to be minimized.

The owner of this project has seen earlier large efforts shrink, mid-way,
into small fixes at the edges because the full change looked too hard. That
is not acceptable here. Whoever works on this, human or model, works to these
terms:

- **Progress is measured by milestones closed**: their "Done when" met and
  their listed code deleted. Optimizations, cleanups, new switches or fixes
  outside the current milestone are not progress on this plan, however
  useful they look. Note them at the end of this page and carry on.
- **Hard is expected.** The model format (M5), the effect functions (M3)
  and the 2D builders (M4) are large, poorly documented pieces of a
  commercial game. When a step is hard, do the static work and write down
  what was found and what is blocking in NATIVE-ONLY-NOTES.md, then ask.
  Do not quietly narrow the step's scope, swap in an easier target, or stop
  at analysis.
- **No fallbacks left in place.** A milestone is not done while the old
  path still handles part of its scope ("refused, sent on as packets", "the
  rest keeps running the VU1"). A case that cannot be ported yet is listed by
  name in the milestone with the reason, and the user agrees to it. A
  fallback is never left in silently.
- **Deleting is part of the deliverable.** Every milestone lists the code it
  deletes. A milestone that adds the new path but leaves the old one is half
  done.
- **No permanent switches back to the old way.** The `gs-path-final` tag
  is the comparison. An env switch that brings
  back deleted behavior is not added.
- **Scope changes are the user's decision.** If the plan is wrong (a hook
  point does not exist, a function cannot be skipped), say so with the
  evidence and propose the change. Do not re-scope on your own.
- **At the start of every session**, read this section and the current
  milestone. Then state which milestone item the session advances.

## Now

The only place the current state and the next step are written. Rewrite it
(do not append) at the end of every session.

**State (2026-11-11).** The user's instruction for this run: work through
the milestones without waiting on play tests, commit often (each commit
compile-checked, so a breakage can be bisected), push often, and stack the
branches (each milestone's branch starts from the previous one's, for
stacked PRs at the end).

- **M1 is done in code** on `native-only/m1-gs-backend` (from
  `native-only/m0-plan`): the native pipeline is the only path; the GS
  backend, its caches, `dq8_gfx_gs`, the GS shaders, the dual-source SDL
  patch, the tripwire (now `src/gs_null.*`), `depth_reads`, the GS half of
  flip pacing, `gs_census`, `gs_timing` and `RG_SELFTEST` are deleted; the
  presenter (`sdlgpu_presenter.*`) replaces the backend; whole-texture packs
  work from the producer (`src/pcsx2_hash.*`). Not yet play-tested: see
  "Handoff" in M1 below for what to check.
- **M2 is done in code** on `native-only/m2-contract` (from M1's branch;
  see M2's status for what changed and the points kept differently from
  the draft contract, for the user's review).
- **M3 is done in code** on `native-only/m3-passes` (from M2's branch):
  every effect is a `Pass` built from its function's arguments, the
  sprite builder is deleted, CT16 decoding and fade captures moved to the
  GPU. See M3's status for what is carried over (step 2, TexAnime to M4,
  lens flare and depth probe).
- **M4, first part** on `native-only/m4-2d` (from M3's branch): layer tags
  for 2D from the running draw task. The rest of M4 waits for M5 (see
  "Decisions").
- **M5 is done in code, but for the declines named in its status,** on
  `native-only/m5-geometry` (from M4's branch), with M4's core:
  - every VU1 chunk the native programs know is drawn by the renderer's
    model vertex stage: strips (projector, float-input, affine and
    float-color programs too) with skinning, lighting and UV generation;
    outlines, shadow volumes and particles made there too;
  - known chunks no longer travel in the list (step 2), and the game
    thread's gather draws them (step 3);
  - the CPU mesh path, the retained static meshes and every switch back
    (`RG_MODEL_GPU`, `RG_VU1_CLIP_ON_GPU`, the mode switch) are deleted;
  - M4's core: 2D blocks are made on the game thread (src/native_blocks.*)
    and sent in the stream's order;
  - M6, early: the GS front end gets nothing any more (see M6).
- **Now: M4's rest and M6** on `native-only/m4b-2d` (from M5's branch).
  - In code: no GsPkt block is written (every segment over scratch, the
    sprite composites followed); composites and effects by markers only;
    every model draw from a record or a captured stream, under the game
    thread's draw state; nothing runs the VU1 for a draw and its programs
    are not compiled in; the list's mirror, its 2D converter and
    gif_events are deleted; no runtime patch of the plan's list is left
    (M6's status has the list, and what is left by what it waits on).
  - **For the play test of this branch**, the lines that say how far it
    got: `[native-blocks]` (blocks drawn from the builder's calls; lost
    should stay 0), `[gs-packets]` (packets that still draw, not drawn, by
    draw task: what 2D is left outside the builder), `[model-emit]`
    (calls from records, written by why, captured streams and those that
    could not be), `[vu1native]` (native chunks; not drawn, with why;
    chunks outside a record or stream), `[composite]`, `[nativefx]`
    (passes drawn and not), `[native-env]`, `[uploads]` (the list's
    uploads by path, withheld / host / local copy, and by draw task: the
    uploaders to hook next). Also the game thread's frame time (lists run
    on it).
  - **Bisect note:** 906c33d does not build (fixed by f3bee42).
  - CSA palettes are drawn now (they were refused outright): a T4 whose
    TEX0 loads its CLUT reads its CBP's first 16 entries; one that does
    not reads the CBP CLUT's entries from 16 CSA on (a derived palette
    record); region packs name them by that window. A T8 reads its 256
    entries by the same rule.
  - All of M4's and M6's code items are in (2026-11-11): uploads by
    marker, fonts as glyph records, the view, camera and light state
    carried by the records. What is left is the play test (each
    milestone's runtime "done when") and the user's agreement to the
    cases listed by name (M5's declined chunks, which are counted and not
    drawn; the lens flare's depth read answered a frame late).
- **Earlier on this branch, M6 steps 1-4** (they needed no play data): the
  lists run on the game thread as they are sent (the VIF1 thread is gone);
  the draw path's types are src/'s own; the GS-only runtime patches
  without a user (0019, 0020, 0021, 0028, 0030, 0031) and the overlay's
  packet builder are deleted; the draw stage is deleted (records go
  straight to the producer in the game's order). **After pulling, check
  the submodule out again** (patches/README.md) so the deleted patches
  (now also 0002, 0013, 0015 and 0034) leave the checkout.

**Working setup** (development machine: macOS, arm64):
- The game cannot be built or run here. Only `tools/docker/compile-check.sh`
  (all; `build` for a `src/` change) checks compilation. The user tests on
  Linux.
- `tools/fninfo.py` works from `disc/` (the extracted game, gitignored) and
  `build/rgdecomp`.
- `origin` is the user's working repository on their Linux machine. A push
  to a branch that is checked out there is refused: ask the user to detach.

## Decisions (2026-11-11)

- **The emulated GS path is removed**, not kept as a fallback. This
  supersedes the direction of issue 10 (`aaec68f`), which kept the GS path as
  the default and the fallback (NATIVE-RENDERER.md, phase 5's status note).
- **Snapshot: a tag.** The last commit with the GS path is tagged
  `gs-path-final`. When something looks wrong, the same save is loaded on a
  build of the tag and compared by eye. There are no recorded reference
  frames: input replay is not deterministic enough for pixel comparisons,
  and chasing replay noise would cost more than it finds (decided
  2026-11-11).
- **Structure before features.** TAA, motion vectors, MSAA, UI at output
  resolution and ray tracing wait until this plan is done. The record
  contract (M2) carries what they will need (object identity, the previous
  transform, normals, jitter), so none of them forces a rewrite later.
- **Static analysis first.** The executable and overlays are in `disc/`
  (gitignored). Function bodies are read with `tools/mipsdis.py`, RGDecomp's C
  (the commit pinned in `config/SCUS_974.90/funcmap.toml`), and
  `tools/rgcpu`'s dataflow analysis for arguments. The running game is used
  to confirm, not to discover.
- **One branch per milestone**, `native-only/mN-<name>`. Claude writes the
  code; the user builds, plays and reports (see "Handoff").
- **Computation goes where it belongs, now** (the user, 2026-11-11). This
  restructure is the time to move work, not only to port it: whatever the
  GPU will need later (effects, upscaling, lighting) or whatever is easier
  next to the GPU's other work moves there as each piece is touched. The
  records carry the game's own data, and the shaders decode it:
  - CT16 texels and CLUTs with the draw's TEXA (done in M3);
  - captured frames kept at the renderer's resolution (done in M3);
  - the M5 transforms, skinning, lighting and UV generation.
  A CPU fallback, where a device lacks a format, lives in the renderer and
  not in the producer.
- **M5 before the rest of M4** (the user, 2026-11-11). 2D records made on
  the game thread would be ordered against meshes that still come from the
  VIF1 stream a frame later: the producer's choice between an upload and a
  drawn target at the same address (render-to-texture read by 2D) could
  read the wrong one. So geometry moves to game-time records first (M5);
  M4's blocks then make records directly from the `GsPkt*` setters. M4's
  branch holds its first part (layer tags); M5's branch stacks on it, and
  the rest of M4 follows on its own branch after M5.
- **Platforms:** Windows, Linux and later Android, all through SDL GPU with
  SPIR-V. macOS is not a target: the native shaders are SPIR-V only, and
  the guest code assumes x86 rounding.

## Where we start

Measured or read 2026-11-11 at `8431766`.

**Renderer library** (`thirdparty/dq8-gfx/src/gfx/native`, 3,811 lines). It
has no GS types, and the build enforces that. It does still encode GS
semantics:

- alpha with 0x80 as 1.0, stored in RGBA8 targets, so anything above 0x80
  saturates;
- integer-style color math (`floor` in the texture function and fog);
- the GS integer Z (`depthScale`, `depthMax`, `depthRow`, `gsMatrix`);
- the destination-alpha test (done with a whole-target copy);
- CLAMP region modes, with no filtering when a region is used;
- TEXA's fixed alpha and AEM;
- the CBlur channel shuffle;
- texture sizes taken from TEX0;
- fog as an F value from 0 to 255;
- the raw VU1 chunk path, which replays the program's cull on the GPU
  (`rawHeader`, `setMeshCull`, `StaticVertex`, `native_mesh_static.vert`);
- 2D in the GS's window coordinates;
- feedback handled by copying the whole target once per self-reading draw.

The `SpriteDraw`, `Effect`, `FrameBegin`, `Readback` and `DepthQuery`
records are never pushed. `Present.target` is ignored.

**Backend** (`thirdparty/dq8-gfx`). About 17,600 lines are GS-only:
- the GS draw path in `sdlgpu_backend.cpp` (about 4,300 lines);
- `sdlgpu_targets.*` (2,297) and `sdlgpu_textures.*` (1,777);
- `gfx/gs/*` (1,972);
- the GS shaders;
- `xxhash.h` (6,773).

About 8,500 lines are shared: the presenter, window, input, FSR, and the F1
menu (5,041). The native renderer depends on the GS side in five places:
- `SdlGpuDevice::create` fails without the GS shaders;
- the backend is a `GSRasterBackend`;
- scale changes go through the GS target cache;
- FSR and the sides use `gs_reinterpret.vert`;
- the raw path carries the runtime's `GSNativeMesh::Header`.

**Producer** (`src/`). Under `RG_NATIVE_PIPELINE=1` nothing reaches the GS
backend; it sits behind the tripwire. But every input the renderer gets,
except textures, targets and the present, is still derived from GS-shaped
data:

| Input | Comes from today |
|---|---|
| Every material (blend, tests, texture, clamp, TEXA, fog color, scissor, target) | The draw-state mirror (`src/draw_state_mirror.*`), fed by GIF packets and libgraph's register writes |
| 2D (HUD, menus, text, fades, particles' packets) | The GIF converter (`src/native_2d.*`) |
| Effects | Their sprites, rebuilt from the packet ranges the hooks recorded (`src/native_effects.*`). Only the color filter is built from its arguments |
| Object layers (stage E) | Clear boxes and tail sprites read from packets |
| Meshes and cameras | VU1 data memory filled by VIF1 UNPACK (`src/vu1_native.cpp`, `nativeproducer::mesh`) |
| Texture bindings | GIF A+D markers applied in stream order (`withholdUploads`) |

The plumbing for these is in `src/vif1_thread.cpp`: the Draw stage, upload
withholding, host captures, the absorber, held effects and the object
composites, about 1,500 of its 3,823 lines.

**Features that live in the GS backend and would be lost with it:**
- **Texture packs** (`src/texture_pack.cpp`). Both kinds are answered by
  `sdlgpu_textures.cpp`: whole-texture packs, in PCSX2's format, keyed by an
  XXH3 hash of the GS blocks and the palette; and region packs, for UV-sprite
  regions such as font glyphs and labels.
- **`src/hud_size.cpp` and `src/widescreen.cpp`.** These rewrite packet
  vertices, so they stop working once 2D no longer comes from packets.

**Counts from the executable** (main ELF; overlays not counted):

| What | Count |
|---|---|
| `GsPkt*` functions | 65 |
| Calls to them | 3,394, from 161 functions |
| Calls to `DrawListBeginPrim` | 126 |
| Calls to `ModelPrepareDrawMaybe` | 7, from 6 functions |
| Calls to `ModelEmitDrawPacketsMaybe` | 5 |

`ModelBuildSkinMtxPacketMaybe` is called only from `ModelPrepareDrawMaybe`,
so skinning matrices are built per draw on the EE. `MeshMaterialBuildPkt` is
called at load (from `ModelBuildMeshes`, `EffModelResInitFromMds` and
`EffQuadResInitFromMds`), so material packets are prebuilt. This suggests the
static mesh packets are prebuilt as well; M5 confirms it.

## The end state

```
game (recompiled, unchanged logic)
 ├─ model draws ─ hook ModelPrepareDraw / ModelEmitDrawPackets ─► MeshDraw {mesh id, model, previous model,
 │                                                                skin palette, material id, camera}
 ├─ effect functions ─ hooked, original skipped ─────────────────► Pass {kind, parameters, targets by role}
 ├─ 2D blocks, GsPkt builders, fonts ─ hooked ───────────────────► Draw2D {layer tag, virtual 640x448 space}
 ├─ textures, targets, flip ─ hooks (as today) ──────────────────► Texture, Palette, Target, Present
 └─ DMA chains ─ still built by the game; nothing on them is drawn from
                                  │
                       native::push (in game order)
                                  ▼
       renderer: scene → passes → 2D → present (FSR, filters, menu)
```

**Gone:**
- on the backend side: the GS backend, `dq8_gfx_gs`, the GS shaders and the
  tripwire;
- in the producer: the GS front end on the draw path, the GIF decode
  (`gif_events`), the draw-state mirror, the 2D converter, effects rebuilt from
  sprites, upload withholding and bind markers, the absorber, and the Draw
  stage's packet path;
- on the VU1 side: the compiled VU1 for draws, VIF1 UNPACK of geometry, and
  the raw chunk path;
- runtime patches 0002, 0013, 0015, 0019, 0020, 0021, 0028, 0030, 0031 and
  0034 (each checked as it goes).

**Stays:**
- the recompiled game;
- SDL GPU;
- the presenter, FSR, display filters and the F1 menu;
- the write watch;
- the texture, target and flip hooks;
- the readbacks the game makes (lens-flare depth, screen captures), answered
  by the renderer;
- a stub that answers the game's SIGNAL, FINISH and LABEL waits.

## The record contract v2 (draft for review, 2026-11-11)

What the renderer receives once M2 lands; M3 to M5 only fill it in. It is
designed whole now, so later milestones and later features (TAA, ray
tracing) add producers, not fields. It replaces `native_records.h`'s
records. Everything still goes through the one ordered queue
(`native::push`), in the order the game built it, and every reference is
an id.

**Units and conventions:**
- **Color** is float, linear in the game's terms, with 1.0 the game's full
  intensity (its 0x80). Values up to 2.0 are legal: the game's colors and
  alphas reach 0xFF.
- **Matrices** are row vectors, as the game keeps them.
- **Projections** are standard: they map to clip space with x and y in
  [-1, 1] and z in [0, 1], reversed (1 is near). The producer turns the
  game's projection-to-GS-window chain into this. No record carries a GS
  window coordinate.
- **Scissors and rectangles** are in the target's own pixels at the game's
  size (640x448 for the scene). The renderer scales them.

**Resources** (sent once, kept by id until replaced or evicted):

| Record | Fields |
|---|---|
| `Texture` | id, format (`Rgba8`, `Index8`), width, height, every mip level, its own palette, `alphaScale` (what a texel's alpha byte is worth; 255/128 for the game's art). Texels are already decoded: no TEXA, no CSM, no swizzle |
| `Palette` | id, 16 or 256 colors (`Rgba8`), `alphaScale` |
| `Target` | id, role (`SceneColor`, `SceneDepth`, `Display`, `Offscreen`), the game's width and height, format (`Color`: RGBA16F; `Depth`: D32 with stencil), `persistent` (content kept between frames, as for CBlur's state) |
| `Camera` | id, view, projection (standard, as above), viewport in the target's pixels, jitter |
| `Mesh` | id, a vertex buffer (position, normal, uv, color, up to 4 bone indices and weights; each attribute optional) and an index buffer of triangles. Retained by the renderer: static geometry is sent once |
| `LightSet` | id, up to 3 directional lights (direction, color), ambient, up to 4 point lights (position, color, radius), as the VU1's lighting pieces use them |

**`Material`** is a value, not a resource; the renderer deduplicates it:
- texture and palette, or `sourceTarget` (a target's color as drawn so far);
- sampler: filter (nearest or linear), mip mode, wrap per axis (repeat or
  clamp);
- combine: `Modulate`, `Decal`, `Highlight`, `Highlight2`; whether the
  texture's alpha is used; `opaqueSource`;
- blend: source and destination factors, op, a float constant;
- alpha test (compare, a float reference) and what a failing pixel still
  writes;
- stencil test and write (the destination-alpha test becomes a stencil bit
  set where alpha ≥ 1);
- depth test and write, and `decalBias`;
- color write mask, cull mode, fog on or off.

**Draws and commands:**

| Record | Fields |
|---|---|
| `MeshDraw` | target, depth target, camera, `Mesh` id (or inline vertices for geometry built each frame), model, previous model, object id (stable across frames), a skin palette (bone matrices), `LightSet` id, material, scissor, fog (start, end, color), UV generator (scroll, sphere map, reflection, wave, ramp, beam, with parameters), source tag |
| `Draw2D` | layer (`Scene`, `Hud`, `Menu`, `Text`, `Fade`, `Overlay`), target, positions in a virtual 640x448 space, uv, color, indices, material, scissor, an optional depth (a 2D draw that tests against the scene, its z in the scene's depth convention), source tag |
| `Pass` | kind (`Glare`, `Bloom`, `DepthOfField`, `PlaneShadow`, `CBlur`, `ColorFilter`, `Fade`, `Capture`, `TexAnime`, `Focus`, `ShadowMap`, `Flare`), the targets it reads and writes, and a parameter struct per kind, filled from the hooked function's arguments (M3 notes define each) |
| `LayerBegin`, `LayerEnd` | an object layer (stage E): the layer target and box at the start; the composite parameters (glow and lay-over alpha, halo mode) at the end. The draws between go to the layer |
| `Clear` | target, rectangle, color, depth or stencil, each optional |
| `FrameBegin`, `Present` | the frame number; the target and rectangle shown |
| `ColorRead`, `DepthRead` | a request the game is waiting on, answered through the producer's mailbox (as `beginColorRead` and `readDepth` today) |

**Not in v2:** any GS register, address, page, format code, integer Z,
window coordinate, CLAMP region, TEXA or channel shuffle; `RawVertex`,
`rawHeader` and the program's cull (the raw path goes in M5; until then it
stays as it is, the only v1 holdover, listed in M2's done line).

## Rules for every milestone

1. **The game stays playable** at the end of every milestone, on the native
   path. From M1 on there is no other path.
2. **Two steps per hook** (the VU1 plan's method). First, *replace the
   output*: the original runs and its packets are discarded. Then, *skip the
   original*, only once its side effects are listed and reproduced. A side
   effect is any store the function or its callees make outside the frame's
   packet stack (`DrawCtx` at 0x4918B0, stacks at ctx+8).
3. **Notes before code.** Each hooked function gets a section in
   NATIVE-ONLY-NOTES.md before its hook is written. Use this template:
   - address and size;
   - arguments (RGDecomp's signature and the disassembly, from
     `tools/fninfo.py`);
   - structures read and written, with offsets;
   - callees;
   - side effects;
   - what the hook emits;
   - open points for a running game to settle.
4. **Checked by counters and by play.** Every milestone states its checks
   as log counters (tripwire hits, refusals, packets still converted,
   chunks still run on the VU1), which do not depend on timing or input,
   and lists the visible differences it expects. The user plays the
   milestone and marks faults with F8. A visible difference not listed is a
   bug until explained.
5. **Delete as you go.** Each milestone deletes the code it makes dead,
   along with its env switches. No permanent A/B switches: the tag is the A.
6. **The renderer stays clean.** From M2 on it receives no GS semantics
   either (no 0x80 alpha, no integer Z, no region clamp). The library
   boundary stays enforced by the build.

## Handoff

For each milestone, Claude provides:
- the branch;
- what changed and what was deleted;
- build notes;
- what to play (the scenes and effects the milestone touched);
- the expected `[native-pipeline]` and `[native-producer]` counters.

The user reports back:
- build errors, verbatim;
- `tools/native-check.sh NAME` output and the stats lines;
- F8 bug marks for anything wrong ([BUG-MARKS.md](BUG-MARKS.md)), with a
  note when the tag draws the same scene differently.

A milestone is merged when its "Done when" holds.

## Milestones

| | Milestone | Needs the game running for | Size |
|---|---|---|---|
| M0 | Groundwork: tag, tools, compile check, contract | nothing | S |
| M1 | Delete the GS backend; port texture packs | testing | M |
| M2 | Record contract v2: no GS semantics in the renderer | testing | M-L |
| M3 | Effects as renderer passes, from their parameters | testing, including reaching the rare effects | L |
| M4 | 2D at its source | testing | L |
| M5 | Geometry off VU1 memory | testing | XL |
| M6 | Remove the GS front end and the packet path | testing | M |

M1 to M6 run in order. Within M3 and M4, items are independent.

### M0: groundwork (no change to the game)

**Claude, without the game** (branch `native-only/m0-plan`):
- [done] RGDecomp at its pinned commit in `build/rgdecomp`, and
  `tools/fninfo.py NAME_OR_ADDRESS`, which prints a function's map entry,
  callers and callees (main executable), RGDecomp's C and, with `--dis`, its
  disassembly. This is the basis of every note. rgcpu's IN set needs the
  recompiler's generated code, which only the compile check has; arguments
  come from RGDecomp's signatures and the disassembly.
- [done 2026-11-11] The compile check, `tools/docker/compile-check.sh`: an
  x86_64 Arch Linux container on the development machine builds the
  checkout (`setup.py tools`, `recompile`, `build`) without running it. The
  build trees live in the Docker volume `rg-build`.
- [done, for review] The record contract v2, above.

**User:**
- [done] The tag `gs-path-final` (`8431766`) is on origin; a build of it is
  kept for side-by-side checks.
- Review the record contract v2.

**Done when** the compile check builds the branch and the user has reviewed
the record contract v2.

### M1: delete the GS backend

The native pipeline becomes the only path:
- `RG_NATIVE_PIPELINE`, `RG_NATIVE_RENDERER` and the F1 switch are removed;
- `RG_VU1_NATIVE` defaults to `gpu`, and the user-facing modes are removed;
- `check` and the oracle stay until M5.

**Texture packs first.**
- Whole-texture packs: the producer computes PCSX2's hash (XXH3 over the GS
  block layout and the palette) when it makes a texture record. It already
  knows the layout from its unswizzle code; this needs a hash of the guest
  bytes in GS block order. The replacement texels are sent as the record's
  texels at a larger size. Move `xxhash.h` to the game or the native
  library.
- Region packs (UV-sprite regions: font glyphs, labels): see question 1.

**Deleted:**
- the backend's GS sections;
- `sdlgpu_targets.*` and `sdlgpu_textures.*`;
- `dq8_gfx_gs`;
- the GS shaders, keeping `gs_reinterpret.vert` under a new name;
- the dual-source SDL patch;
- `src/gs_tripwire.*`, replaced by a null `GSRasterBackend` that keeps the
  tripwire's log line, since anything that reaches it is a bug;
- `depth_reads.*`;
- the GS half of `flip_pacing.*`;
- `debug/gs_census.*` and `debug/gs_timing.*`;
- `RG_SELFTEST=gs`;
- the GS-only `RG_GS_*` and `DQ8_GFX_*` switches (the list is in the
  producer inventory of 2026-11-11; recheck at the time).

**Reworked:**
- the presenter, as an interface of its own, not a `GSRasterBackend`;
- `SdlGpuDevice::create` without the GS shaders;
- scale changes;
- the stats line, with a home outside `reportStats`;
- the F1 stats page.

**Done when:**
- the game plays as on the tag's native pipeline (nothing the native path
  draws has changed), with the tripwire's replacement never hit;
- whole-texture packs load (one check on the user's pack);
- `native-check.sh` is clean;
- the GS memory is no longer allocated (the log says so).

**Status (2026-11-11): done in code, not play-tested.** Commits on
`native-only/m1-gs-backend`, each compile-checked:
1. the native pipeline is the only path (the switches, `native_renderer.*`,
   the VU1 modes but `gpu` and `check`; the null GS backend);
2. the presenter replaces the GS backend, which is deleted with its
   caches, `dq8_gfx_gs`, the GS shaders, the dual-source SDL patch, the
   selftest, the software fallback (`RG_GS_SDLGPU`, `RG_HAS_SDLGPU`), and
   `RG_GS`, `RG_GS_NO_WINDOW`; `RG_GS_SCALE` is `RG_SCALE`;
3. whole-texture packs from the producer;
4. scripts, launchers and docs.

The log says `[rg] GS backend: none; GS local memory is not mirrored`. The
runtime itself still allocates its 4 MiB of GS local memory (`PS2Memory`);
that goes with the runtime's GS patches in M6.

**Handoff (what to play and expect).** Build the branch, play any save for
a few minutes through a town, a battle and the menus, with
`DQ8_GFX_STATS_EVERY=600`:
- the picture as on the tag's native pipeline (`RG_NATIVE_RENDERER=1
  RG_NATIVE_PIPELINE=1` on `gs-path-final`); nothing the native path draws
  changed;
- no `[native-gs] the GS backend was entered` line, and at exit
  `[native-gs] GS backend calls: none`;
- a `[gfx] ... fps (backend ...% of wall) ... draws=` line, the
  `[native-pipeline]` line and the `[native-producer]` line, which now ends
  with `texture packs: pairs looked up N, pictures made N, none N, draws
  from a picture N`;
- with a PCSX2 pack in `textures\packs`: the 3D drawn from it (pictures
  made and draws from a picture above 0), the menus and icons not;
- the menu (F1): Display > Internal resolution changes the picture's
  sharpness at once; the Renderer statistics page shows meshes, GPU draw
  calls, passes, copies and textures;
- `tools/native-check.sh NAME` clean.

Expected differences from the tag: packs of 2D art (by region) are not
drawn (until M4); the "Sharper picture" switch is gone.

Left for later milestones, by name: the VIF1 thread's program tags and
camera registry (`g_tagPrograms`, `t_vu1Tag`, `t_vu1Camera`), which fed the
GS backend's jitter and camera log and still ride the GIF stage's headers,
go with the packet path in M6; so does `uploadsTaken()`, now a constant.

### M2: record contract v2

The renderer receives plain, modern data. Each GS semantic moves to the
producer, or into a renderer feature with a normal name:

| Today | v2 |
|---|---|
| Alpha with 0x80 as 1.0; targets clamp above it | Normalized float alpha. Scene and offscreen targets RGBA16F, so values up to 2.0 survive (this fixes glare where alpha > 0x80). Textures stay 8-bit, with the scale applied at sampling |
| `floor` in the color math | Float math. The texture functions as a normal combine enum |
| Integer Z, `depthScale`, `depthMax`, `gsMatrix` | Reversed float depth from the projection. Decals carry a `decalBias` material field. `gsMatrix` and the depth log go |
| Destination-alpha test via a target copy | A stencil bit, written where alpha ≥ 1, then tested |
| CLAMP region modes | The producer cuts the region into its own texture and uses ordinary wrap |
| TEXA, AEM | Baked into the texels at decode. A target read without alpha gets an `opaqueSource` flag |
| Fog F from 0 to 255 | A float fog factor and color |
| 2D in GS window coordinates, `hasModel=false` | Every draw has a camera. 2D gets an orthographic camera over a virtual 640x448 space with a layer tag (scene, HUD, menu, fade, text) |
| `Present.target` ignored | Honored. Clears become explicit records |
| Unused records | `Effect` becomes `Pass` (designed here, built in M3). The rest are deleted |
| Missing | `MeshDraw.object` and `previousModel` filled where known; `Camera.jitter`; normals; mip levels. Carried now, so later features add no fields |

The raw chunk path is left as it is until M5, which replaces it.

**Done when** the game plays as before, apart from the differences listed
here in advance (among them glare and bloom where alpha was over 0x80,
which should now match the tag's GS path), and no `0x80` remains in the
renderer library.

**Status (2026-11-11): done in code, not play-tested**, on
`native-only/m2-contract` (from M1's branch), each commit compile-checked:
1. the v2 record set: `Draw2D` (the producer's window-space draws, with a
   layer tag, Scene until M4), `Pass` (designed; M3 fills it), `Clear` (the
   producer clears the scene at each frame's start and a depth target at
   its first use in a frame; the renderer no longer does on its own),
   `Present`'s target and rectangle honored; `SpriteDraw`, `Readback`,
   `DepthQuery`, `gsMatrix` and the depth log (it compared with the GS's Z)
   deleted;
2. float units: RGBA16F targets, `Texture::scale`, float material fields,
   float fog, a resolve pass at `Present::scale`; colour reads in floats;
3. standard cameras (reversed-Z projection over a viewport rectangle) and
   the depth terms (`depthScale`, `depthBias`, `depthCap`, `decalBias`)
   made by the producer; 2D depth made by the producer;
4. tiles of sheets (region clamp and repeat) cut into textures of their
   own by the producer; plain clamp or repeat and a texel offset in the
   renderer.

`grep 0x80` over `thirdparty/dq8-gfx/src/gfx/native` finds nothing.

**Kept differently from the draft contract, for the user to decide** (see
"Outside this plan" for the reasoning):
- The destination-alpha test stays a read of a copy of the target (a
  stencil bit needs the shader to write stencil, which SDL GPU does not
  offer, or the same copy to make it).
- A source read without its alpha (TEXA, AEM) stays a material field
  (`sourceAlpha`, `alphaZeroOnBlack`) for textures and targets alike:
  baking it into textures would remove no renderer code, as targets need
  it anyway.
- `Draw2D` positions are the target's pixels (the scene's are its 640x448)
  rather than a virtual space with an orthographic camera.
- The channel shuffle (CBlur's green-to-alpha) stays until M3 makes CBlur
  a pass.
- `MeshDraw::object` and `previousModel` are not filled yet: the producer
  has no object identity before M5's model-level hook (a retained chunk's
  id is shared by every instance of the mesh).

**Expected differences from M1** (to look for when play-testing): glare,
bloom and anything blended by an alpha or a fixed factor over 0x80 is
brighter where the GS was brighter (it was clamped at 0x80 before); colors
no longer truncated to whole bytes between steps (smoother gradients);
tiles of sheets drawn with filtering when the game asks for it.

### M3: effects as passes

For each effect: a note, then a hook that emits a `Pass` and discards the
original's packets (step 1), then a hook that skips the original once its
side effects are reproduced (step 2), then a renderer pass at parity. Order
by cost (GS-PERFORMANCE.md) and by how often each effect appears:

1. Flip copy (`GfxFlipBuffersMaybe`): present the scene; no copy.
2. Glare (159 sprites a frame).
3. Plane shadow: the begin and end hooks; volumes with add and subtract,
   then 36 blur adds. The volumes come from a VU1 packet program; their pass
   takes them as geometry.
4. Bloom.
5. Depth of field.
6. Object layers (stage E). Hook `ModelDrawOffscreenCompositeAtMaybe`, whose
   arguments are already known (a0 settings: +0x20, +0x24, +0x50, +0x5C,
   +0x64; f12, f13). The layer becomes a pass with its box computed from the
   object's bounds, not from a clear sprite.
7. CBlur. It keeps 64x64 state between frames as a renderer-owned
   persistent target, and needs its work-texture registration reproduced.
8. Refraction capture.
9. Color filter, which is already built from its arguments.
10. Fades and screen captures. The capture stays on the GPU when the only
    reader is the next frame's draw; otherwise `sceGsExecStoreImage` keeps
    being answered.
11. TexAnime: a render-to-texture of 174 sprites a frame. It becomes a
    texture-animation pass, or a CPU compose into the texture record.
12. Focus, shadow map and its blur, lens flare and the depth probe. These
    need routes first: none of the probed routes reached them.

The `RG_NATIVE_<EFFECT>` switches, `native_effect.h`, the sprite builder and
the held-effect plumbing in `vif1_thread.cpp` are deleted as their last user
goes.

**Status (2026-11-11): done in code, not play-tested**, on
`native-only/m3-passes` (from M2's branch), each commit compile-checked.
Every effect the sprite builder drew is now a pass the renderer expands
(`native_passes.cpp`), built from the hooked function's arguments (notes for
each in NATIVE-ONLY-NOTES.md):
- glare, bloom, DoF, focus, colour filter, refraction capture, the flip's
  copy, the shadow map's blur;
- the plane shadow;
- CBlur (`CBlur`, `CBlurDraw`), its buffers found by a port of the game's
  name-table lookup;
- object layers' tails (`ObjectLayer`, from the VU1 overlay calls'
  arguments and the composite's settings).

What else changed:
- The sprite builder (`build`, `native_effect.h`, the producer's
  `effect()`), the shadow setup's register mirror and the
  `RG_NATIVE_<EFFECT>` switches are deleted. Object layers are routed by
  `routeLayer`/`routeEnd`.
- A pass's packets go on with their kicks withheld, so their uploads and
  register writes still happen.
- Two moves to the GPU (see "Decisions"):
  - CT16 textures and CLUTs are `Ct16` records of the game's bytes, and the
    shader applies the draw's TEXA;
  - colour reads also keep their pixels on the GPU, and a fade's captured
    frame is drawn from that copy at the renderer's resolution.

**Step 2 (skipping the original), in part** (on `native-only/m5-geometry`):
- A pass can stand at a marker the hook writes into the list (a cnt tag and
  a DIRECT A+D write of its unit to `kPassRegister`), with the function not
  run.
- The glare, DoF, the colour filter and the bloom go so; the bloom's one
  write outside its packets is made by the hook.
- The others keep running for what they do outside their packets:
  - refraction: its node and the allocator behind it;
  - the shadow map's blur: the view pop and the light packet;
  - the plane shadow: camera and light packets;
  - CBlur: its texture loads;
  - focus: the frames its call sets;
  - the object layers: the models drawn into them.

**Not done, carried over:**
- **TexAnime** is plain 2D sprites into a texture target, so it moves with
  M4's 2D at its source.
- **Lens flare and depth probe** (item 12): no route reached them. They
  stay a depth read answered a frame late.
- ~~**Object layers' box** still comes from the head's clear sprite~~
  (M6: from the head's clear block made from the builder's calls on the
  game thread; the game makes that clear from the parts' bounds, united,
  clamped to the viewport and padded by 8 pixels, so no packet is read for
  it).

**Handoff (what to play and expect).**
- Every post effect is the renderer's own now. Look at:
  - glare and bloom (bright areas);
  - DoF and focus (cutscenes);
  - the colour filter (tinted areas);
  - refraction (water, glass);
  - the plane shadow under characters (blurred or hard);
  - the character blur (afterimages in fast moves, battle specials);
  - characters with glow or outline (object layers);
  - fades (wipes and cross-fades over a captured frame should now be as
    sharp as the game at the chosen scale).
- `tools/native-check.sh` lines 5 and 5c count effects whose pass was not
  sent.
- Bisect by commit: each effect is its own commit.

**Done when:**
- Salgin's first scene draws in at most about 25 render passes, with no
  feedback copies (stats line);
- the producer's stats show no effect built from sprites;
- each ported effect, played, looks as on the tag.

### M4: 2D at its source

- **Hooks.** Hook the `DrawListBeginPrim` and `DrawListEndPrim` blocks
  (126 call sites) and the `GsPkt*` state and vertex setters, so a block
  produces `Draw2D` records directly, with no GIF encoding. Two facts set the
  approach:
  - the builder's fields the game reads back (+0x70 PRIM, +0xF4 write
    pointer, and any others the notes find) are kept consistent;
  - `hud_size.cpp` and `widescreen.cpp` become transforms of the 2D layer
    (anchoring and 4:3 safe areas) instead of vertex patches.
- **Fonts.** `FontDrawGlyph`, `FontDrawGlyphOutline` and the gaiji glyphs
  become glyph records. Region packs return here, keyed by glyph and region
  rather than by a texel hash.
- **Layer tags** come from the block's owner and the running `TaskDraw*`
  (62 of them).
- **libgraph.** `sceGsPutDrawEnv`, `sceGsSwapDBuff`'s clear and the
  `sceGifPk*` paths become target bindings and clears.
- **The title screen's** 2D pieces of unknown source are found here.

**Status (2026-11-11): its core in code, not play-tested** (layer tags
on `native-only/m4-2d`; the rest on `native-only/m5-geometry`, after M5's
first steps, as decided).

Done:
- **Layer tags.** Each 2D draw carries the layer of the draw task that
  wrote it.
- **TexAnime** (`TexAnimeTableApplyMaybe_162f50`) draws through GsPkt
  blocks and VRAM copies (`DrawListAddVramCopyFixed`): the blocks are made
  on the game thread like any other, and the copies are the producer's
  local-copy mirror.
- **2D blocks at their source** (`src/native_blocks.*`). Each GsPkt block
  is read on the game thread when it ends, under a draw state the game
  thread follows itself (`rg::drawstate::Tracker`: the environments'
  packets and the blocks). Its pieces are kept by the block's packet range
  and sent by the VIF1 thread in the stream's order.
  - `[native-blocks]` lines count the blocks made, drawn, not found and
    not read, and how often the VIF1 thread's mirror differs from the game
    thread's at a block's end (should stay 0).
  - `RG_NATIVE_BLOCKS=0` turns it off.

- **The census for "Done when":** `[native2d] pieces the converter made,
  by draw task` lines (every 20000 pieces) name the draw tasks
  (`TaskDraw@address`, `tools/fninfo.py` names them) whose packets the
  draw stage's converter still turns into 2D, and `no task@0` for
  packets outside any task. Each named one is a source left to move to
  the game thread; the VU1 packet programs' are expected to stay.

- **The builder's calls, step one** (`src/native_gspkt.*`): the builder's
  framing functions and emitters (25 of them, from the map in
  NATIVE-ONLY-NOTES.md) are hooked, and each block's GIF data is made on
  the game thread from their arguments and the builder's fields, never
  from the bytes they write. It is compared with the block's bytes:
  `[native-blocks] … the builder's shadow: the same N, differing M, not
  followed K` (the first differences in full). Not followed: blocks with
  the PACKED sprite composites that write their data themselves
  (`GsPktDrawSprite`, `GsPktAddTexSprite`), raw writes or a DMA call (the
  overlay's VU1 call), and outer blocks with one nested in them. The 2D is
  still read from the bytes.
  - **Step two, in code, unconditional:** every block outside an effect's
    call is drawn from its shadow and the game writes none of it: each
    GsPktOpen opens a segment over scratch above the memstack's top (the
    setters still run and keep the builder coherent; nothing is
    allocated, no CALL written), the 2D is made from the shadow at
    GsPktEnd, and a marker where the CALL would have gone brings it, with
    the block's register writes, to the list's run. The byte reading of
    blocks, the per-owner comparison and the Block2D ranges are deleted.
    A block written through something the shadow does not follow (a raw
    write; an outer block an inner one reset) loses what was skipped of
    it (`lost`, counted). A pass effect's blocks (the effects that still
    run their originals: plane shadow, CBlur, CBlurDraw, focus, shadow-map
    blur, refraction) are not written and not drawn: each such effect's
    pass now comes at a marker where its call's output ends, as the
    skipped effects' do (`effects' blocks left to their passes`). A
    composite's blocks are still written (its head's and tail's ranges
    route them).
- **Fixed:** blocks were never read before (their range ended in RET
  tags; see the commit). The counts above are the first real ones.

Not done:
- ~~fonts as glyph records~~ **in code:** `FontDrawGlyph` and
  `GsPktSpriteUVRect` (its and its outline's sprites) are hooked; each glyph
  sets its texture before its sprites, so they are a piece of their own,
  and the piece whose texel rectangle is a glyph's carries the glyph's code
  to its Draw2D record (`source` = `kSourceGlyph | code`, named in the
  renderer's draw labels). Region packs look each glyph up by its own
  rectangle (as before); keying packs by the code instead would rename
  them, and is left for when a pack is made that way;
- libgraph, **in code:** `sceGsPutDrawEnv`'s packet and every libgraph
  stub's register writes (`sceGsSwapDBuff`, `sceGsSetDefDBuff`,
  `sceGsResetGraph`, ... through the register sink) go to the draw state
  directly, and each clear sprite becomes the producer's `Clear` (frame,
  and depth to the far plane when the sprite's z is 0): no GIF DMA, no
  converter (`takeEnvironment`). `[native-env]` says when it is first
  taken. Writes of another shape (a textured sprite, a transfer) go as
  before. Left: the `sceGifPk` paths of the flip (M3's
  Capture pass already draws it) and of the uploads (images, not 2D);
- ~~HUD size and widescreen as layer transforms~~ **in code:** neither
  writes the game's vertices or arguments any more. A block's 2D records
  are reshaped at its end by its owner (`native_blocks`'s `BlockMesh`,
  `transform`): `hud_size::reshape` scales the minimap's view and arrow
  toward the view's corner and each compass letter about its centre;
  `widescreen::reshape` moves the movie's picture to the wide frame's box
  (its bars and size read, not changed, at its top left's vertex). The
  compass dial's screen matrix and the cameras' aspects stay parameter
  changes. To check: the options as on the tag;
- **region packs, in part.**
  - Drawn again (`replaceRegion` in the producer): a UV sprite's texel
    rectangle of an indexed texture is named as gs-path-final's
    `GsTextureCache` named it, from the indices and palette entries the
    producer holds, and drawn from the pack's picture.
  - Not done:
    - ~~the border and shadow versions of glyphs~~ (in code with the glyph
      records: `FontDrawGlyphOutline` draws them as offset copies of the
      glyph's own texel rectangle in black or grey, sprites of the glyph's
      piece, so the region's picture draws them, tinted as the game tints
      them);
    - ~~a region whose CLUT is offset by TEXCLUT~~: no code of the game
      writes TEXCLUT (no decompiled function names it, and the builder's
      arbitrary register writes are 0x40, 0x3B and 0x3F only), so there is
      no such region; still counted as not hashable if one appears;
    - ~~a T8 with a CT16 CLUT~~ **in code:** its 512-entry window (CLUT rows
      0-31) is its own palette's 256 entries and the 256 of the palette a
      16x16 upload put where row 16 starts (CBP + 4, PSMCT16's block
      table), from the producer's palette records; counted as not hashable
      when that palette is not one.
  - The stats line counts `regions looked up`, `pictures made`,
    `draws from one` and `not hashable here`.
  - To confirm with a pack collected under the tag.

**Done when** (2026-11-11: the first in code, stronger than asked: there
is no converter on the list, and a packet that still draws is counted by
`[gs-packets]`; the other two wait on play; fonts as glyph records are not
among them, and need the pack naming decided, as they change it):
- the 2D converter receives no packet except those of the VU1 packet
  programs (counter, over a play session with menus, battles and events);
- the HUD size and widescreen options behave as on the tag;
- region packs load.

### M5: geometry off VU1 memory

The largest piece. It mostly needs the model format, which the static work
recovers.

1. **Notes:**
   - `ModelMeshBuildFromResMaybe`, `ModelMeshBuildUnpackPktMaybe`,
     `MeshMaterialBuildPkt`, the `ModelRes*` getters and the
     `VertexDecode*`/`UvDecode*` decoders (the resource format);
   - `ModelPrepareDrawMaybe` and `ModelEmitDrawPacketsMaybe` (what a draw
     sends);
   - `ModelBuildSkinMtxPacketMaybe` (the bone palette);
   - `DrawListAddLightPacket` (the lights).

   Check each against the piece decoders in `src/vu1_native.cpp`, which
   already know the VU1's side of the format.
2. **Meshes by identity.** Key meshes at their source (the prebuilt packet's
   address and write-watch stamp), so the VIF1 thread skips UNPACK, decode
   and hash. This is NATIVE-BOUNDARY.md issue 2, steps 1-3: the part the game
   thread feels.
3. **The model-level hook.** A draw becomes {mesh id, model matrix, previous
   model, camera, material id from the prebuilt material, skin palette}, and
   no geometry rides the DMA chain. Two things are reproduced:
   - the VU1 memory side effects the game reads, chiefly the bounds in quads
     98/99, computed from an object-space box;
   - the base slot's per-frame matrix products, if anything reads them.
4. **Skinning in the vertex shader** (1, 2 or 4 bones; the palette that
   lands at VU1 quads 900+ today).
5. **Lighting and UV generation on the GPU.** Lighting from the C pieces:
   directional, point and ambient light. UV generation from the F pieces:
   scroll, sphere map, reflection, wave, ramp and beam.
6. **The packet programs as records:** outlines as lines, shadow volumes for
   M3's plane-shadow pass, particles as instanced quads, overlay sprites.
7. **Clipping on the GPU**, so no chunk falls back to the compiled VU1.

**Status (2026-11-11): step 1 in code, not play-tested.**
- Done:
  - `[model-census]` lines count the model emitters' items by mesh class;
  - T4 textures are packed on the GPU;
  - `vu1_native`'s `modelChunk`, `modelChunkHash`, `modelRow` and
    `modelHeader`;
  - the renderer's `native_model.vert` and its buffers;
  - the VIF1 thread's model table (a chunk gets an id the second time its
    inputs are seen) and the Model record through the draw stage.
- Handoff:
  - The stats line's `model chunks: defined so far N, draws M` should
    grow and settle in the field.
  - Characters, scenery and effects models should look as before:
    skinned characters, lit surfaces, sphere-mapped and reflective
    materials, point-lit areas.
  - Faults to look for:
    - a model drawn in its bind pose or exploded (the palette);
    - wrong lighting (the C variants);
    - wrong UVs (F);
    - missing triangles (the backface rule).
  - (`RG_MODEL_GPU=0`, the CPU path, is deleted: see below.)
- **Since step 1, also in code:**
  - every strip chunk the model stage can draw goes to it (a chunk with
    no id yet, or whose inputs change every frame, with its inputs);
  - the float-input and projector programs, too, and the affine D (flag
    128: the UV goes times w with a divisor w, so the GPU's interpolation
    gives the GS's screen-linear ST with Q 1) and the float-color D (flag
    256);
  - the particles in the model vertex stage (`particle()`, flag 32): six
    inputs a particle (position, shape, corner offsets, texel, color); the
    shader turns each to face the eye (buildParticles' arithmetic, its sine
    polynomial included) in world space;
  - the shadow volumes in the model vertex stage (`volume()`, flag 64): the
    chunk's triangles as inputs, skinned there, drawn twice. Each draw
    follows its blend's packet (quad 38's or 35's) and is colored by that
    packet's RGBAQ; the shader makes each lit triangle's volume (corners
    and the same pushed from the light, skipped when a corner is outside
    its own w), and the backface rule keeps the facing triangles in one
    draw and the others in the other. A packet that sets no RGBAQ keeps
    the old window packets;
  - the outlines in the model vertex stage (`outline()` in
    `native_model.vert`, flag 16 of the row): each edge's points go as
    inputs (skinned there), 24 vertices an edge; the shader makes
    buildOutline's guard band and silhouette tests, its fade and color,
    and its four lines offset by +-h, each a quad half a game pixel either
    side (so they scale with the output, as the 2D path's lines);
  - the retained static meshes (`native_mesh_static.vert`, `StaticVertex`,
    `RG_MESH_RETAIN`) deleted.
  - **The CPU mesh path is deleted:** `buildMesh`, `readChunk`, the
    projector's, particles' and strip programs' packet builders,
    `RawVertex`, `MeshDraw::raw`/`rawHeader`, `setMeshCull`/`cullMesh`,
    the bounds kept in quads 98/99 (only the overlay B read them, and M3's
    ObjectLayer pass replaced it), `RG_MODEL_GPU`, `RG_MESH_REPEAT`,
    `RG_VU1_CLIP_ON_GPU` and the VU1 mode switch. Every VU1 mesh the producer takes is the model
    stage's; `buildChunk` is left for the overlay, the base piece, and the
    outline and shadow fallbacks.
- **Step 3 in code too:** every list now runs on the game thread as it is
  sent (M6's first step), so every chunk's draw is made at game time from
  VU1 memory as the list leaves it. The game thread's second reader of the
  stream (`src/native_models.*`, its `[model-game]` count and
  `RG_MODEL_GAME_TIME`) went with that: it duplicated what the list's own
  run now does on the same thread.
- **Left, by name (the user's to agree; `[vu1native]` lines count each as
  "left to the VU1"):** chunks `declineChunk` refuses still run the VU1
  interpreter: a chunk whose A enters a plain B past its first entry
  (quad 76.z and TOP+1.w both set; A jumps to pair 300 + TOP+1.w), a
  skinning B entered at 7 or past its table (7 is a JR: the VU decodes
  nothing; 1, 3 and 5 fall into the next branch and are drawn), and PRIM neither strip nor list (a fan: the D
  pieces' strip window of ADC over a fan's triangles). (Since: chunks of
  no whole triangle, edge or particle, and those of fewer vertices than a
  pipelined F piece reads, are the model stage's: it draws the whole ones
  they have, the VU would run past the chunk's end.)
  (Since on `native-only/m4b-2d`: the scroll F, quad 88.xy, and the
  vertex-color C pieces' additive rule, 87.y & 0x100, are the model
  stage's: row 42 and flag 2.) Each needs its VU code read (tools/vu1) before
  it can be drawn; none is seen to be frequent, but a play session's
  counts will say. Unknown programs (identify finds no pieces) run the VU1
  too.
- **Step 2 deleted (M6):** the learning of REF payloads and the MSCNT marks
  that stood for them in the list (`RG_MODEL_ELIDE`, `[model-elide]`) went
  once the model draw calls were drawn from their records: the list no
  longer carries those chunks at all.
- Ordinary meshes, skinned ones too, are VIF packets built once at load and
  only CALLed each frame (`ModelMeshLinks`).
- No EE code reads VU1 memory back. Quads 98/99 only feed the overlay
  sprites, which M3 already draws from their arguments.
- Every input a chunk's per-vertex program reads is in VU1 data memory by
  its MSCNT: the chunk itself, then the matrix, header, palette, material,
  view and light packets the list carried before it.

So M5 goes in this order:
1. **Chunks by identity, on the GPU.**
   - The VIF1 thread hashes a strip chunk's input sections at its MSCNT. The
     first time it sees a hash, it decodes the chunk's raw inputs once
     (positions, normals, UVs, colors, bone slots and weights, as int
     values) into a `ModelChunk` record the renderer keeps. Each triangle's
     vertices carry its three corners, for the program's backface rule.
   - Each draw is then a `MeshDraw` naming the chunk, with a snapshot of the
     VU1 constants the program reads (`ModelConstants`) and the bone
     palette.
   - The renderer's model vertex stage (`native_model.vert`) runs B
     (dequantize and skin), F (UV generation, wave, swell), C (the lighting
     variants) and the existing transform, depth and fog. The CPU no longer
     runs `readChunk` or `buildMesh` for these chunks.
   - The guard-band skip goes, as planned; backfaces stay, judged on the
     GPU from the corners.
   - `RG_MODEL_GPU=0` was the CPU path, for triage, until it was deleted.
2. **The list stops carrying geometry.** The game thread's gather leaves a
   known chunk's bytes in guest memory (as uploads are elided), and the VIF1
   thread neither copies nor UNPACKs them. It needs the chunk's identity
   from the REF tag's address and the write watch.
3. **Model-level records at game time** (M6, with the rest): the snapshot
   comes from the Emit hook instead of VU1 memory, once nothing else needs
   the VIF1 stream.

Replaced and deleted: the raw chunk path (`RawVertex`, `rawHeader`,
`setMeshCull`, `StaticVertex`, `native_mesh_static.vert`), giving standard
indexed meshes and GPU culling. The PS2's guard-band skip goes; edges may
differ slightly, and those differences are listed.

**Done when:**
- no draw runs the compiled VU1 (counter, over a play session);
- the VIF1 thread unpacks no model geometry (counter);
- the field walk's frame time is measured against M4 (the game thread's
  list copy is the expected gain).

### M6: remove the GS front end and the packet path

**Step 1 in code (2026-11-11, on `native-only/m4b-2d`): the lists run on
the game thread.** `send()` runs each VIF1 list where the game sends it
(`Vif1Thread::runList`; `RG_VIF1_THREAD` is gone): VIF decode, the native programs and the VU1
interpreter for what is left to it, at the default floating-point
environment, with its records going to the draw stage in the game's order
among the records the game's hooks make. The VIF1 worker thread is gone;
the draw stage still consumes on its own thread and finishes each list
(the game's DMA completion). To watch: the game thread's frame time (the
list's decode is now on it; `vif1 busy` in the stats is now the game
thread's share).

**Step 2 in code (on `native-only/m4b-2d`): the draw path's types are
src/'s own.** `src/native_types.h` (`rg::NativeMesh`, `rg::DrawState` and
its registers, `rg::nativeFtoi`/`nativeReciprocal`) and `src/gs_swizzle.h`
(GS memory's block and column tables, for the producer's and the packs'
addresses) replace the runtime's `gs_types.h` and `ps2_gs_psm*.h`; the
draw-state mirror keeps its own registers (patch 0031's
`GS::DrawRegisters` and its `readBack` are no longer used); the `GS &`
the delivery functions and the draw stage carried, and the program and
camera tags for the GS backend's profiles, are gone. What of the
runtime's GS `src/` still uses: the VU1 interpreter's own `GS` (its
XGKICK), `GSRegisters` for SIGNAL, FINISH and LABEL, the libgraph stubs'
register sink (patch 0034), the null backend (`gs_null.*`) and the
presenter's front end (`graphics.cpp`).

**Step 3 in code: the GS-only runtime patches without a user are
deleted:** 0019 (source tag), 0020 (source camera), 0021 (`GSNativeMesh`),
0028 (its clip planes), 0030 (its backface lists) and 0031 (the draw
registers); `gs_null`'s `SubmitNativeMesh` override with them. The rest of
the series applies to a clean checkout of the submodule without them
(checked 2026-11-11). **An existing checkout keeps them applied** until
the submodule is checked out again: `git -C thirdparty/PS2Recomp checkout
-- .`, delete `thirdparty/PS2Recomp/.rg-patches-applied`, then `cmake -S .
-B build/game` (patches/README.md). **Since:** 0002, 0013 and 0015 are
deleted too: they patch the runtime's `GSThreadedBackend`, which nothing
creates (the null backend is installed directly); the series applies to a
clean checkout without them (checked 2026-11-11). **And 0034** (the
libgraph stubs' register sink): the stubs run unsinked, keeping what they
do to the privileged registers (the display) exactly, and the draw state
they set is taken from their arguments instead (`sceGsPutDrawEnv`'s
packet, `sceGsSwapDBuff`'s buffer's draw environment); their own writes
into the runtime's GS front end, which draws nothing, are not read. The
series applies without it (checked). None of the plan's runtime patches
is left.

**Step 4 in code: the draw stage is deleted.** A list runs on the game
thread and what it makes goes straight to the producer (packets to the
converter and the mirror, meshes, uploads, an elided upload as what the
producer made of it), in the game's order; the producer's records go to the
renderer's queue. Gone: the `DrawStage` class and its thread, its record
encoding (packet, image, mesh, model records and the VU1 quads copied with
each mesh), `RG_DRAW_STAGE`, and `RG_MESH_WRITES` (a mesh's state packets
always ride as register writes). To watch: the game thread's frame time
(the converter and the producer's meshes are on it now: `lists busy` in
the stats).

**Model draws at the emit call, step 1 in code** (`src/native_emit.*`):
`ModelEmitDrawPacketsMaybe_14ec20` is hooked; at each call (outside an
effect's) each item's and group's inputs are read from the game's
structures (NATIVE-ONLY-NOTES.md, "Building model draws at Emit time"): the
model matrix and mesh header from the item's packets p14/p18, the
material's quads 82-87 from its packet (82 from the material when an
animator set `t+6`). Markers on each side of the call's list output tell
the list's run which call its chunks belong to (items by their base
MSCAL 0, groups by their MSCAL A), and every model chunk compares its VU1
memory with them: `[model-emit] ... the same N, differing M` (the first
twelve in full). Compared so far, all read at the call: the model matrix
and mesh header (60-63, 76-79, 81, 42), the material's quads (82-87), the
program (the microcode table's six packets, identified as the list's run
identifies the VU1's code), the GS state (ALPHA, TEST, ZBUF's mask, FBA
from the material's GsEnv; TEX0 from its texture node; TEX1, CLAMP from
its packet), and each chunk's header from the group's chain of spans in
EE memory.

**Step 2 in code, unconditional:** a call whose every item's mesh is a
`ModelMeshLinks` (static chains) and every group a strip or projector
program the table names is not written: the hook leaves a marker in the
list with the call's record and returns 0 quadwords (the emitter's caches
left as they are). Where the list's run reaches the marker, each item is
drawn from the record: its model matrix put in VU1 memory and the base
piece run (as its p14's MSCAL 0 would), its header and palette put (p18,
p1C), each group's material quads and texenv register writes (the
material's GsEnv with the draw environment's frame and depth base, TEX1,
CLAMP, TEX0) applied, each chunk unpacked from its span in EE memory to
the item's VIF BASE and drawn as any model chunk. The camera, view and
light packets still come from the list. The comparison (step 1's markers
and per-chunk checks, the per-mesh trust) is deleted; `[model-emit]`
counts the calls drawn from the game's structures and those the game
still writes. The shadow casters
(`ModelEmitShadowCasterPacketsMaybe`, one material for every group) go
the same way. **The sorted path too:** `ModelQueueSortedPackets` keeps a
record of each queued node (by its item's p14 and its chain), and
`DrawListEmitBucket` is reimplemented as the game writes it, each node it
knows compared between markers or, its mesh trusted, drawn from its record
(its header's changed CALLs still written; the next written node CALLs
its texenv again). The ModelMesh class (cloth and other meshes whose chunks the game
rebuilds into the frame's buffer at every emit) is drawn from records
too: the hook runs the mesh's own `GetGroupPkts` (through
`rg::hooks::runToEnd`, its arrays below the guest's stack), allocates the
quadwords it wrote in the frame's buffer as the emitter would, and reads
the record from the rebuilt chains. The sorted path (`ModelQueueSortedPackets`,
drawn at the buckets' replay) and the shadow casters come after.

**Nothing runs the VU1 for a draw (in code):** an MSCAL or MSCNT is drawn
by the native programs or not at all. The chunks `declineChunk` refuses
(M5's status names them), those of programs `identify()` does not know and
shadow chunks whose packets set no RGBAQ are counted under `[vu1native]`'s
"not drawn" and dropped; `buildShadow`, the last GS packet builder but the
base piece's FOGCOL, is deleted. VPU_STAT's D/T bits stay clear.

**Composites by markers (in code):** a composite's blocks are skipped like
every other: its head's are drawn from their shadows, the first clear of
untextured CT32 sprites giving the object layer's box and a marker (kind 6)
before it; its tail's are discarded (its ObjectLayer passes draw them). A
marker at the call's end (kind 7) ends the layer and sends the tail's
passes. The VIF1 side's batch of head and tail packets, `clearBox` from
packets and the tail chain's ranges are deleted.

**Effects by markers only; no block is written (in code):** every pass is
sent at its marker (Glare, Bloom, the colour filter and DoF write it at the
call's end when they could not before it); the packet ranges, the holding
of an effect's packets and their withheld kicks are deleted. `native_gspkt`
opens every segment over scratch, an effect's too: a block inside a pass's
call is discarded, and `DrawListBeginPlaneShadowPass` is no longer an
effect (its mask's clear is an ordinary block).

**Records under their own draw state (in code):** the mirror's register
file is a value (`rg::drawstate::Registers`); the game thread's tracker
(`rg::drawstate::game()`: the draw environments' packets, the blocks,
libgraph's environments, the model calls drawn from records) gives each
group of a record its registers where its marker goes, and its chunks are
drawn under them, not under the list's mirror. The list's mirror is left
only for what the list still draws: model calls the game writes,
outlines, particles, shadow volumes, the converter's pieces (blocks the
shadow does not follow, 2D outside the builder) and uploads.

**Since (all in code, on `native-only/m4b-2d`):**
- **Outlines, particles and shadow volumes from records:** the list's run
  and a record's chain draw a chunk alike (`Vif1Thread::drawChunk`,
  `declineChunk` first for records too); `drawable()` admits every program
  the native path draws.
- **The sprite composites' blocks followed:** `GsPktDrawSprite` and
  `GsPktAddTexSprite` no longer spoil a block: where a tag ends
  (FinalizeTags, Close) the shadow takes the tag's data as the game wrote it
  in the scratch segment (inline PACKED qwords, AddTexSprite's RGBAQ).
- **The list's packets are register state:** a small GIF register reader
  (`src/gs_writes.*`) reads what the list still carries (draw environments,
  a mesh's state, uploads' registers); the 2D converter is off the list
  (native_2d is the blocks' shadow builder alone), and a packet that still
  draws is counted by draw task (`[gs-packets]`) and not drawn.
  `gif_events` is trimmed to native_2d's decoder.
- **vu1_native's builders give register writes,** not packets.
- **Fewer written model calls:** Mode1 items skipped (they draw nothing),
  an empty program slot resolved from the microcode cache at the emit call;
  `[model-emit]` counts the written calls by why.
- **Captured streams (marker kind 8):** what every other model emitter
  writes (calls the game still writes, those inside effect calls such as
  the composite's parts, the sorted path's written nodes,
  `EffModelDrawNodes`, `EffQuadDrawNodes`) is taken out of the list where
  the emitter returns, as the VIF stream the list would have carried, and
  replayed at its marker through the runtime's VIF1 under the game
  thread's registers there and its own writes.
- **The list's mirror is deleted:** nothing draws under the list's state.
  `draw_state_mirror` keeps the register file records carry (`Registers`)
  and the game thread's tracker (`game()`); a chunk from neither a record
  nor a stream is counted (`[vu1native] chunks in the list outside ...`).
- **gif_events deleted:** native_2d reads the blocks' shadows through
  `src/gs_writes.h` (with PACKED's RGBA-without-Q and ST's Q as two
  pseudo-registers) and keeps the vertex state itself.
- **The VU1 programs are not compiled into the game** (nothing runs the
  VU1); `build/vu1-manifest` is still expanded for tools/vu1.
- **Runtime patches 0002, 0013, 0015 and 0034 deleted;** the texture
  binding and upload elision switches (`RG_TEXTURE_BIND`,
  `RG_UPLOAD_ELIDE`) and dead lookups with them.
- **Blocks:** a discarded block (a pass's) is never written even once its
  shadow spoils (the composite tail's overlay calls); a block inside
  another (TexAnime's) no longer spoils the outer one, unless the outer
  one has a tag open. `[uploads]` counts the list's uploads by path and
  draw task.

**Left of M6, by what it waits on:**
- **Uploads, in code since:** every texture node becomes records at the
  hook and a marker in the list (bound; or, with records the hook made, a
  take marker, kTakeRegister; the palette callback's palettes made records
  from where the callback put them); every upload by reference the game
  builds (GsBuildImageUploadRefPkt) is a host marker (kHostRegister: the
  hook's record of the transfer if queued, else a host upload of its
  source at the marker). Its inline twin (GsBuildImageUploadPkt) has no
  caller that runs (TexBuildUploadPkt's mode is always 0). Local copies
  (TexAnime's DrawListAddVramCopy) are A+D register writes the list's run
  mirrors in order, as a marker would. The movie's frames
  (GsUploadImage16Blocks, a GIF-channel packet of macroblock uploads) are a
  host marker too, the frame assembled at it. Withholding and the list's
  uncopied payloads are deleted (no record waits for a transfer).
  **Host capture and the local-copy mirror stay, by the plan's own rule
  ("if the census shows no user"): each has a user found in the code** —
  the font pages' load (MesFontLoad → FixedTexUploadImpl: its own
  GIF-channel packet of uploads by reference, sent by sceDmaSend at load)
  and TexAnime's VRAM copies. `[uploads]` names any other.
- **The playthrough route** (the user's).


**And:** the overlay B's packet builder (`runOverlay`) is deleted: its
sprites are only drawn inside a composite's tail, by M3's ObjectLayer pass,
with the tail's packets withheld, and the bounds it read (quads 98/99) are
no longer kept. Its MSCAL is still recognized, so the VU1 does not run it.

**Done early (2026-11-11, on `native-only/m5-geometry`):**
- The VU1 parity tooling went with M5: check mode, `RG_VU1_REPLAY`,
  `RG_VU1_HARVEST`, `RG_VU1_CAPTURE` and the oracle.
- Every path into the runtime's GS front end went: the GIF stage
  (`GifStage`, `RG_GIF_THREAD`), `meshToGs`, `sendRaw`'s and
  `deliverImage`'s GS tails, `RG_NATIVE_ABSORB` and `RG_NATIVE_2D`.
- Packets still feed the draw-state mirror and SIGNAL, FINISH and LABEL.

The cleanup that M1 to M5 make possible. What goes:
- the Draw stage's packet path;
- the draw-state mirror and `native_2d`;
- `gif_events`;
- upload withholding and bind markers (bindings become direct calls in
  order);
- the absorber, reduced to the SIGNAL/FINISH/LABEL stub;
- the host-capture and local-copy mirroring, if the census shows no user;
- the GS-only runtime patches;
- `vu1_native`'s packet builders, the compiled VU1 programs for draws, the
  `.vu1r` oracle and the harvest mode;
- the GS-related `include`s in `src/`.

What the VIF1 thread still does is whatever the notes find the game still
sends; for graphics, that should be nothing.

**Done when:**
- `src/` includes no GS header except the runtime's stubs that the game's
  waits need;
- the user's playthrough route passes: title, Salgin, the first battles, a
  boss, menus, a save and a load, a movie.

## After this (not in scope)

TAA and motion vectors (object identity and previous transforms exist from
M2 and M5), UI at output resolution (layer tags from M4), MSAA, HDR bloom,
shadow-map resolution, and ray tracing (world-space meshes with normals and
materials from M5).

## Risks

- **No live oracle.** With the GS path gone, faults are found in play.
  Mitigations: counters that catch whole classes of fault (anything refused
  or left over), F8 marks, and the tag built to compare a scene by eye.
- **Skipped originals with side effects that affect game logic**, not just
  the picture. Rule 2 (the two steps) and the store review in each note are
  the guard.
- **The model format** may take longer than the rest put together. M5 comes
  after M1 to M4, which deliver most of the cleanup and the GPU and power
  gain without it.
- **Turnaround.** Without a running game here, every behavior check is a
  round trip. The compile check in M0 keeps build errors off that loop.
- **A permanent fork of dq8-gfx.** Accepted already (NATIVE-RENDERER.md,
  "Risks").

## Settled (2026-11-11)

1. **Region texture packs** (font and UI regions) are off from M1 until M4
   brings them back on glyph and region records. Whole-texture packs are
   ported in M1 and keep working throughout.
2. **The compile check** is an x86_64 Linux build in Docker on the
   development machine, with the PS2Recomp and imgui submodules checked out.
3. **Testing is on Linux.** The run scripts stay Linux-only.
4. **The tag** `gs-path-final` is at `8431766` (`origin/main` when the plan
   was written; the plan's own commit only adds docs) and is pushed to
   origin.

## Outside this plan

Ideas and fixes that came up during the work but belong to no milestone.
They wait until the plan is done, or until the user moves one in.

- **Destination-alpha test as stencil (M2, deferred to the user).** The
  draft contract has the destination-alpha test become a stencil bit set
  where alpha >= 1. Setting it per pixel needs the fragment shader to
  write the stencil value (shader stencil export), which SDL GPU does not
  expose; without it, each alpha-writing draw would need a second pass
  that reads a copy of the target to set the bit, which is the copy the
  renderer takes today. Kept as the copy read. Proposal: keep it; revisit
  if SDL GPU gains stencil export.
- **TEXA baked into texels (M2, deferred to the user).** Kept as the
  material's `sourceAlpha` and `alphaZeroOnBlack`, which a target read
  needs anyway (a buffer read through the GS's CT24 view); baking it into
  texture copies would add textures and remove no renderer code. M3 went
  further the same way: CT16's TEXA (`texaLow`, `texaHigh`) is applied by
  the shader too.
- **More decoding to the GPU (candidates).**
  - T4 textures: done (M5's branch). They are `Index4` records of the
    game's bytes, and the shader takes the nibble. The tile cutter and the
    packs' hash widen a copy where they need one.
  - Pre-swizzled T8 uploads are still unswizzled on the CPU.
