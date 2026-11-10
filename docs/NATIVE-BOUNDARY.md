# The native CPU / native GPU boundary: structural issues and the plan

Written 2026-11-09, to be picked up by whoever works on it next. The native
CPU side and the native GPU side were each made fast on their own. What is
left between them is work the PS2 needed and we do not: the game still
speaks GS (uploads, register packets, VU1 memory), and we translate it back
every frame. This page lists those issues in order of size, with a plan for
each. Read [NATIVE-CPU-HANDOFF.md](NATIVE-CPU-HANDOFF.md) for the benchmark
method and [NATIVE-PIPELINE.md](NATIVE-PIPELINE.md) for the pipeline's steps.

The frame, for reference: game thread (guest code) -> `sceDmaSend` copies the
DMA list (`gatherChain`, `src/vif1_thread.cpp`) -> VIF1 thread (UNPACK, VU1
programs as native code) -> Draw thread (`DrawStage`: 2D converter, draw-state
mirror, `rg::nativeproducer`) -> records -> presenter. The game waits for the
whole chain every frame, and the game thread is the limit (~3.1 ms a frame in
the field, unthrottled).

## Issue 1: textures are sent again every frame

**What happens.** The PS2 has 4 MB of video memory, so the game keeps its
textures in groups and swaps a group in whenever it draws with it:
`DrawListAddTexLoad_1418c0` (105 callers) -> `TexGroupAllocVram_157e70(mgr,
group, dst)`, which returns at once when the group is the current one and
otherwise gives every texture of the group a GS address (written into the
node's TEX0, +0x38) and calls `TexBuildUploadPkt_158610(mgr, dst, node, 0)`
for each. In the field that is ~200 textures and ~200 palettes a frame,
~4.4 MiB.

Our renderer already holds every one of them. Yet each frame:

1. the guest builds the upload packets (`GsBuildImageUploadRefPkt_158390`:
   0x70 bytes of registers and 0x30 a chunk, `ref` tags to the pixels);
2. our hook on `TexBuildUploadPkt` (`src/native_capture.cpp`) hashes or
   looks up each level and palette and queues a "produced" entry;
3. `gatherChain` copies the 4.4 MiB into the list (game thread);
4. the VIF1 thread copies them again to the Draw stage;
5. the Draw thread matches each upload to its "produced" entry
   (`takeProduced`, a lock and a search each), hashes the small ones again
   to catch records made too early (`seeWithheld`), and drops the bytes.

All that survives is "texture id X is now at GS address Y".

**The fix: send the binding, not the upload.** The hook writes a small
marker at `dst` in place of the upload packets and does not run the guest
function. The marker is itself a valid piece of the list:

- a DMA `cnt` tag (QWC = 1 + k) with a magic value in its unused bits 16-25
  and the VIFcodes NOP and DIRECT (1 + k);
- one GIF tag: A+D, NLOOP = k, EOP;
- k entries of 16 bytes, each an A+D write to register 0x7E (which the GS
  does not have). In guest memory an entry describes the source (address,
  size, format, GS address); see `BindEntry` in `src/native_producer.h`.

At send time `gatherChain` (game thread) sees the magic, and resolves each
entry in its copy of the list to a texture or palette id
(`rg::nativeproducer::bindTexture` / `bindPalette`): one thread-local lookup
and a write-watch check when the memory is unchanged, a hash and a record
when it is new. The entry in the list becomes {id, GS address, format}. This
is the moment the PS2's DMA would read the pixels, so a texture the game
animates between the build and the send (`TexAnimeTableApplyMaybe_162f50`
runs right after the packets are built: the "stale record" case) is read
right, with no second hash downstream.

The marker then travels as an ordinary GIF packet, in order with the draws,
through everything that holds packets back (effects, object composites, the
2D converter), and `withholdUploads` on the Draw thread applies it
(`rg::nativeproducer::bound`: the resident tables, one lock a packet).

Nothing is built by the guest, copied, hashed twice or matched by search.

**What must treat the marker as an upload** (each tests for the transfer
registers 0x50-0x53 today; `kBindRegister` joins them):

- `rg::native2d::quick` (`src/native_2d.cpp`): `transfer`, so the packet
  goes to `sendRaw` and not to the mirror;
- `sendWrites` (`src/vif1_thread.cpp`): not `direct`;
- the sprite effect builder (`src/native_effects.cpp`, "a transfer among the
  sprites");
- `uploadOrFlush` (`src/vif1_thread.cpp`, the object composites).

**What keeps the old path** (the guest function runs, the upload is withheld
as before): a node whose palettes go through the manager's callback
(`mgr+0x1F4` set with mode 0), mode != 0, a format or size `texture()`
refuses, and any run where the lists are read whole or the Draw stage is off
(`RG_VU1_CAPTURE`, VU1 replay, `RG_TEXTURE_BIND=0`).

**Steps.**

1. [done 2026-11-09, uncommitted] The producer's `bindTexture` /
   `bindPalette` / `bound`, the marker in the hook, its resolution in
   `gatherChain`, its application in `withholdUploads`, and the four
   classifiers above.
2. [done] Field walk: uploads withheld 0, ~200 textures and ~200 palettes
   bound a frame, no texture or palette misses, no stale records,
   screenshots right.
3. [done] Field walk, unthrottled, 16 cores, two runs each:
   `RG_TEXTURE_BIND=0` 2.91 ms a frame (343 fps), bindings 2.28-2.36 ms
   (425-439 fps). Still to do: a battle and the menus in the user's QA
   (effects and object composites hold packets back, the paths most likely
   to differ), deck mode numbers, commit.
4. Clean up: the partial "elided upload" path (the `Hole` machinery in
   `src/vif1_thread.cpp`, `sourceUnchanged`, `noteElided`,
   `RG_UPLOAD_ELIDE`) is uncommitted and obsolete once bindings work: remove
   it rather than commit it.
5. Later, if the remaining cost shows in a profile: `TexGroupAllocVram` is
   called ~N times a frame for groups whose bindings do not change from one
   frame to the next; a group's whole marker could be cached by (group,
   allocation) and re-emitted. Only worth it if step 3 leaves the per-node
   hook visible.

## Issue 2: geometry is derived again every frame

**What happens.** Every vertex of every mesh, moving or not, takes this path
each frame: copied into the list (game thread), UNPACKed into VU1 memory,
decoded and transformed by the native VU1 program (`readChunk`,
`buildMesh`), copied to the Draw stage, copied into `m_mesh`, copied into
`draw.raw`, packed for the GPU. It is also projected three times on the CPU
(depth, the bounds the game reads back from VU1 quads 98/99, the presenter's
cull) before the GPU projects it. ~800-880 meshes and ~20k triangles a
frame in the field.

**The fix: static meshes stay on the GPU.** A mesh whose source memory has
not changed (the write watch in `src/guest_write_watch.*` already answers
this for textures) is the same vertex buffer as last frame; only its
matrices differ. Such a mesh should be sent once, kept by the renderer under
an id, and drawn afterwards from {mesh id, matrices, draw state}, the same
shape the texture fix has.

**Measured 2026-11-09** (`RG_MESH_REPEAT=1`, field walk; the counter is in
`src/vif1_thread.cpp`, `[mesh-repeat]` lines every 600 lists, one list a
frame):

| a frame | meshes | vertices | built identical to a frame before |
|---|---|---|---|
| not skinned | 428 | 15,755 | 79.7% of vertices (plus 4% repeated inside the frame) |
| skinned | 352 | 8,295 | 0.2% |

"Identical" is the object-space vertex (position, UV, color), not the
depth. So about half of all vertices (12,500 of 24,000) are static and half
are not (skinned characters, and a fifth of the unskinned: scrolling UVs,
particles, lit objects that turn). 61% of the repeats had the same header
too (the camera stood still). The list is ~930 KiB a frame, nearly all of
it vertex data; whether the static part comes from memory the game does not
rewrite (prebuilt chains it `call`s) is not counted yet: the counter's
inline/reference split counts a called chain's own data as inline.

That is over the "one third" bar: retained static meshes are worth
building. It does not help the skinned half; that needs skinning in the
vertex shader, a separate job after this one.

**Correction (same day): the depth was not in the way.** The renderer
already makes a 3D mesh's depth on the GPU (`depthRow`, made in double, with
`invariant gl_Position`); `GSNativeMesh::Vertex::depth` is read only for 2D
pieces, so the per-vertex depth loop in `buildMesh` is dead work for meshes
in GPU mode. What tied a mesh to the CPU each frame on the renderer's side
was the program's cull (`cullMesh` on the presenter: which triangles the VU1
program drops, guard band and backface) and the repack into the frame's
vertex buffer.

**Done 2026-11-09: the renderer half ("retained meshes").**

- VIF1 thread (`retainOf`, `src/vif1_thread.cpp`): an unskinned mesh is
  known by a hash of its object-space vertices. Built the same in 8 lists,
  it gets an id and is sent once (a `RetainedMesh` record); after that only
  the id and the chunk's header travel to the draw stage and the producer.
- Renderer (`thirdparty/dq8-gfx`, `native_pipeline` / `native_meshes`):
  the mesh lives in one persistent vertex buffer as whole triangles (each
  vertex carries its triangle's three corners), drawn by
  `native_mesh_static.vert`, which judges the program's cull rule on the GPU
  with the CPU's arithmetic (`precise`, same order of operations). One draw
  call a mesh, its row of the draw table the call's first instance. When the
  buffer (2^19 vertices) fills, everything is dropped and `retainedEpoch`
  tells the VIF1 thread to start again.
- `RG_MESH_RETAIN=0` is the old path. The stats line has `retained meshes N
  (...)`.

Field walk: 869 meshes retained (91k buffer vertices), no resets, none
missing, screenshots right. **Speed: no change** (2.32 ms a frame off, 2.35
on, two runs each, 16 cores). The work it removes (the draw stage's and the
producer's vertex copies, the presenter's cull and repack) was not on the
frame's critical path: the game thread is. It should lower total CPU, which
matters on four cores; not measured (needs deck mode). Not tested: battles,
menus, cutscenes.

**What makes it pay: the input half.** The game thread still copies every
vertex into the list, and the VIF1 thread still UNPACKs, decodes and now
hashes it, only to find the id it had last frame. The identity has to come
from the input, before that work:

1. Find where a static chunk's bytes come from in guest memory. `listAddress`
   of the UNPACK's data gives the EE address; the question is whether it is
   memory the game leaves alone (a prebuilt chain it `call`s: the write
   watch can vouch for it) or a list it rewrites each frame. Count the
   retained meshes' vertices by which.
2. If it is stable memory: key the mesh by (source address, size, write
   watch stamp, the VU1 program, a hash of the constants `readChunk` reads:
   quads 77-79, 81-83, 42, 43, 87, the lights). On a hit the VIF1 thread
   skips `readChunk` and the hash, and needs only the bounds the game reads
   back (quads 98/99), which can be kept with the mesh as a box in object
   space (8 corners transformed, not every vertex).
3. Then the list copy leaves those bytes in guest memory and the VIF1
   thread does not UNPACK them (the `Hole` idea, for geometry; a patch to
   the VIF1 interpreter). This is the part the game thread feels.
4. If the game rewrites the chunks each frame instead, hook where it does
   (as the texture hook does for uploads) so the chunk is named, not built.
5. Skinned meshes (a third of the vertices, never repeated): skinning in the
   vertex shader, bones as a per-draw matrix palette. After the above.
6. Drop the dead depth loop for GPU-mode meshes (small; `depthsAndBounds`
   still needs the bounds).

## Where the frame's limit is (measured 2026-11-09, after issues 1 and 2a)

Field walk, unthrottled, 16 cores, 2.33 ms a frame. From a 12 s profile and
the wait log (`RG_EE_WAITLOG=1`, which now also prints each list's latency:
`a list, from its send: VIF1 done N us, draw stage done N us`):

- The game thread is busy 87% of the time; Draw 77%, VIF1 64%, presenter
  59%.
- The game waits on the chain once a frame. Over most of the walk that wait
  is 2-3% of the frame: **the game thread is the limit.** In the walk's last
  scene it is 11% (0.26 ms): there the Draw thread is, a list clearing it
  1.95 ms after its send, 0.5 ms after the VIF1 thread finished it.
- The game thread's time: translated guest code 63%, scheduler and
  native/runtime boundary 13%, the list copy (`send`, `appendBytes`,
  ~930 KiB a frame) 6.6%, the texture hook and bindings 4% (most of it
  `sourceHash` on ~650 KiB a frame of textures and palettes whose pages the
  write watch gave up on), VU0 3%.
- The Draw thread's time: the 2D converter and GIF decode 28%, the stage's
  packet plumbing 13.5%, `piece` 8%, the mirror 3.5% (together, the packet
  path: over half); `mesh` and its material 16%; memcpy 15%; locks and
  malloc 7%.

So what shortens a frame from here, in order: guest work on the game thread
(the native CPU work: [NATIVE-CPU-HANDOFF.md](NATIVE-CPU-HANDOFF.md)), the
list copy, then the Draw thread's packet path where a scene is heavy. Work
moved off the presenter or the VIF1 thread does not show in the frame time
on 16 cores (issue 2's renderer half did not); it should on four.

## Issue 4: what is not a mesh still travels as GS packets

~4,300 GIF packets a frame reach the Draw thread, ~3,400 of them register
writes only, and ~830 2D pieces a frame are made from the rest. Each is
copied to the stage, placed (effects and composites look it up by address),
classified, decoded into events and applied to the mirror.

**Done 2026-11-09: a mesh's own writes.** The native VU1 program encoded its
fog colour, PRIM and PRMODE as two GIF packets ahead of every mesh, for the
Draw thread to decode again (~1,500 packets a frame). They now ride with the
mesh as (register, value) pairs (`packetsAsWrites`, `t_meshWrites` in
`src/vif1_thread.cpp`) and go straight to the mirror. `RG_MESH_WRITES=0` is
the old path. Field walk 2.33 -> 2.26 ms a frame (two runs each), pictures
right.

**Left, in order of size:**

1. The game's own per-mesh state packets (TEX0, ALPHA, TEST, ... sent by
   DIRECT from model data, ~1,900 a frame): the same idea needs them
   recognised at the VIF1 thread (register-only DIRECTs) and decoded there
   once, or cached by their list address when it is stable memory.
2. The ~830 2D pieces a frame (particles, effects, HUD): which programs and
   hooks make them, and whether the native VU1 programs that produce
   packets (`buildChunk`: particles, outlines, shadows) can hand the
   producer meshes instead of packets to convert back.
3. `piece` and `mesh` each take the producer's lock and build a material per
   draw: consecutive draws under an unchanged mirror could reuse the last.

## Issue 3: the game waits for the whole chain every frame

The game syncs with VIF1 and Draw each frame, so the three threads take
turns more than they overlap. Letting the game run ahead was tried on
2026-11-08 and was slower (see the handoff). Not a fresh lead; listed so it
is not rediscovered. Issues 1 and 2 shorten the chain instead, which is what
the sync is waiting on.

## The working tree, 2026-11-09

Uncommitted on `perf/native-cpu` (at `7a1e509`), besides the texture work
above. Small trims, none measured on their own; finish, measure, and commit
or drop each:

- patch `0046-vif1-unpack-fixed-shapes` (VIF1 UNPACK with compile-time
  shapes; registered and in `patches/README.md`);
- `listAddress` stepping from the last piece found (`src/vif1_thread.cpp`);
- the elided uploads (see Issue 1, step 4: remove);
- depth and bounds in one pass (`depthsAndBounds`, `src/vu1_native.cpp`):
  built, never run.

Profile it was all read from (field walk, unthrottled, 73e40f9): game thread
30% of the CPU samples, Draw 26%, VIF1 25%, presenter 19%.
