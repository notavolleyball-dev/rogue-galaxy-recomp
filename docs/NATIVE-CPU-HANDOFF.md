# Native CPU: handoff

Branch perf/native-cpu, worktree ../rg-wt-nativecpu. The goal is
docs/NATIVE-CPU-GOAL.md (we own the recomp, no emulated shortcuts); the
design is docs/NATIVE-CPU.md.

**How this file works.** "Now" below is the only place the current state and
the next step are written down. Rewrite it at the end of every session; do
not append to it. Everything under "History" is the dated record, oldest
first: add a section there for what a session found, and read one only when
"Now" points at it.

## Now (2026-11-10, at 9cc364f)

**State.** The whole game runs on our translator under RG_NATIVE_CPU=1
(tools/rgcpu: 17,303 of 17,733 functions native, the rest through the
oracle), with ThinLTO, patches 0001-0046 applied. With the native pipeline:

- Field walk, unthrottled, 16 cores: 2.26 ms a frame at 7145858 (was 2.91
  before the boundary work in [NATIVE-BOUNDARY.md](NATIVE-BOUNDARY.md)).
- Deck mode, 1x, at 73e40f9 (before the boundary work, not measured since):
  189-203 fps, game thread 4.4-4.7 ms a frame, Draw 3.8-4.0, VIF1 3.6-3.8.
  Deck mode's CPU is ~1.5x a real Deck's, so read those as ~7 ms on one.
  2x and 4x are GPU bound there ("Deck mode results").
- The limit is the game thread (87% busy): translated guest code 63% of it,
  scheduler and native/runtime boundary 13%, the list copy 6.6%
  (NATIVE-BOUNDARY.md, "Where the frame's limit is").

**Next, in order.**

1. Devirtualize the hot jalr sites: record targets per site, emit a compare
   and a direct native call before the boundary. Sites and per-frame counts
   are in "Scheduler and boundary counts".
2. A cheaper call form for host leaf hooks and stubs (GsPktSetXYZ/f, memcpy,
   MtxMulVec4).
3. On the boundary side: the "Left" lists under issues 2 and 4 of
   NATIVE-BOUNDARY.md (the input half of retained meshes; the game's own
   per-mesh state packets).

**QA.** The user played the head (e8bd6d3) with tools/run/play.sh (defaults,
boundary switches on), covering "For the in-game QA pass".

**Upstreaming.** The branch is being split into PRs: docs/NATIVE-CPU-SPLIT.md.

**Run and measure.**

    tools/run/play.sh [late] [cpu0] [gs] [rdoc] [VAR=value]   # play this build
    tools/rgcpu/bench/cpuframe.sh NAME      # per-thread CPU ms/frame, unpaced field walk
    RG_BIN=/scratch/old/roguegalaxy tools/rgcpu/bench/cpuframe.sh old   # A/B: alternate, two pairs
    RG_EE_WAITLOG=1                         # main thread share, list latency, the game's wait

One walk of one build swings ~3%, so never compare single runs. A thread's
"% busy" includes blocked time (`perf stat -t TID` task-clock to check).
Translate and build: "Commands". After a change to tools/rgcpu or the hook
list, regenerate build/generated/native before building.

**Tried, do not try again without a new reason.** Releasing the vsync hold
earlier (13 vsyncs a frame, slower); mesh prep moved onto VIF1 (slower);
letting the game run ahead of the chain; one host mapping for RAM and
mirrors (nothing left to win, "ThinLTO and the memory slow path"); polling
the clock instead of yielding; a GPU compute cull (rounding differs from the
game's skip rule); the register-caching pass over PS2Recomp's output.

# History

## Where it stands (2026-11-07)

- tools/rgcpu translates 17,303 of 17,733 main-ELF functions to native C++
  (registers as locals, owned stack frames in 10,214 functions, direct calls
  with whole-program signatures). Differential harness: the hot trees and
  2,400 random functions match the oracle (see NATIVE-CPU.md, "Status ...
  later" for the one explained stale-stack case and UNSURE runs).
- Runtime patches 0039 (waits/invokes suspend in place, no unwinds on a
  fiber) and 0040 (nativeCheckpoint / nativeBoundary). src/native_cpu.cpp
  swaps table slots to NAME_nc at boot under RG_NATIVE_CPU=1.
- Hooked functions (config/SCUS_974.90/native-hooks.txt, 126) are called as
  boundaries; the game warns at boot about a hooked slot not in that list.
- The game builds with it (build/generated/native is generated, see below)
  and boots with RG_NATIVE_CPU=1: title overlay loads, the screen renders.

## Will this be faster? What is gone, what still costs

No in-game number yet. Structurally, the costs we
kept tuning around before are gone in native code:
- registers are C++ locals, not loads/stores of R5900Context per instruction;
- calls are direct C++ calls with per-function signatures plus one compare
  of the table slot (directGuestCall and its dispatch were 7.8% of game
  time, eeCheckpointDue 3.3%);
- no unwinding and no re-entry through switch(ctx->pc): threads suspend in
  place;
- ~79k stack accesses in 10,214 functions are locals, not guest memory.

What still costs, all ours to remove, roughly in order of expected size:
1. Memory bounds check on every guest load/store (compare + cold slow
   path; also causes spills). Fix: direct host mapping of the PS2 address
   space, no check (NATIVE-CPU.md plan step 3).
2. Checkpoints: an out-of-line call per back edge and per call. Fix: an
   inline cycle counter decrement, call only when due.
3. No cross-function inlining (each nc_NNN.cpp sees declarations only).
   Fix: per-call-tree TUs or LTO.
4. Very wide signatures: functions whose analysis did not narrow (a jalr
   or unknown target makes everything live) take 100+ parameters; their
   calls are expensive. Measure how hot they are first; tighten the
   analysis (e.g. ABI assumption at jalr) where it matters.
5. Boundary calls (hooks, untranslated code, jalr): full flush/reload of
   the context. Fine if rare; check the hot paths.
6. VU0 registers still in gctx; FPU still through the exact-PS2 helpers.

Expectation: the first A/B should show a clear gain (leaves alone were
1.41x weighted before owned frames, and leaves are ~19% of game time; the
call/dispatch/context savings live in the trees, which the harness cannot
time). The large step toward ~1 ms/frame comes from 1-3. If the first A/B
is small, look at 4 first.

## The memory card hang (fixed 2026-11-08)

The walk stopped at "Checking memory cards...". Bisected in the game
(tools/rgcpu/bench/gbisect.py, 15 boots) to SaveModeDispatch (0x375df0). It
calls the card state's handler through PtmfScall (0x100e70, a
pointer-to-member call: `jr $t9`). PtmfScall is not translated, and the
analysis took an unknown `jr reg` as reading everything but writing nothing
(maydef_local), so its OUT was {r24, r25}: the handler's v0 never came back
and the dispatcher tested a stale v0 forever. Fix: a register tail jump may
define ABI_CLOBBER, as a call does (183 functions have one). With it the
walk reaches the field with all 17,177 functions native.

Tools that came out of it: RG_NATIVE_CPU_ONLY=LO-HI and
RG_NATIVE_CPU_ORACLE=FILE (src/native_cpu.cpp) keep slots on the oracle;
`pkill -USR1 roguegalaxy` prints the stall report (as F10) for a hang that
keeps flipping.

## First in-game A/B (2026-11-08)

cyc.sh (unpaced walk on the field, game thread, perf stat 40 s), one binary,
flag off/on alternated:

    off  32.40  32.54 Mcycles/frame   71.6  73.7 Minstr/frame  IPC 2.21 2.26
    on   35.05  30.58 Mcycles/frame   61.5  59.8 Minstr/frame  IPC 1.75 1.96

Native runs ~16% fewer instructions but IPC falls as much: no net gain yet.
Noise between runs is ~10% (VIF1, unchanged code, moves as much), so take
several runs per side. perf record (native on, game thread):

    49%  native functions; of those (before the jr-reg ABI change) 23% in
         functions with 100+ parameters, 15% with 40-99, 11% under 40.
         Hot samples are `mov %rax, N(%rsp)`: arguments spilled to the stack
         around every call (SoundStreamProcessCmds 0x1c05f0, SoundStream-
         SlotUpdate 0x1bea70, FrameUpdateWorld 0x149680, MeshRaycastTris).
    8.5% PS2Runtime::nativeCheckpoint (out-of-line call per back edge/call)
    8.3% libc (unsymbolized; likely memcpy/memset -- record with call graph)
    9.5% renderer/VU work on the game thread, 5.6% scheduler, 5% oracle
         functions, 4.6% boundaries/slow memory/dispatch

Done: an untranslated `jr reg` tail jump now reads ABI_IN like a jalr instead
of everything; that alone took functions with 100+ parameters from 3,304 to
0, but 7,617 still take 40+ (hot ones ~85). Why, with tools in the scratchpad
of session 8c82ecb6 (why_live.py LOC ADDR follows a location's liveness down
the call graph):
- upper halves h2..h13 of temporaries: vu0 statements (qmtc2 of a register
  whose top half was never set, e.g. RayTriIntersect 0x13d98c) read all 128
  bits, so the caller's stale upper half is an input, transitively.
- r12-r15, r24, r25, hi/lo: no reader in the function itself; they pass
  through via OUT (a callee may define them, a caller NEEDs them after a
  call), so on paths that do not define them the entry value flows out.
  Find the seed: the first caller whose live_out at a call holds r12.

Next, in order: (1) narrow signatures (the two seeds above: ABI assumption
that temporaries are dead after a call, and vu0 reads of upper halves only
where the code set them); (2) inline checkpoint counter (+ asm memory clobber
in NC_BACKEDGE); (3) direct memory mapping; then ThinLTO.

## Signatures narrowed; the right metric (2026-11-08, later)

tools/rgcpu/analysis.py now treats the calling convention as the contract
between translated functions (RGCPU_EXACT=1 restores the exact sets, for
oracle diffs that compare every register):
- MUSTDEF: a call kills what the callee writes on every path (exact).
- NEED/OUT: a caller gets back only results (v0/v1 and upper halves, f0/f1)
  and callee-saved registers; other temporaries a callee leaves are garbage.
- Boundaries (syscall, jalr, untranslated callees) take arguments, gp, sp,
  ra, fcr31; callee-saved s0-s7/fp/upper halves/f20-f31 are neither flushed
  nor reloaded (BOUNDARY_SAVED): the callee gives them back.
- A prologue save (sd/sq/swc1 to sp) of a register the function provably
  preserves (PRES) is not a use. Not for unproven ones: OperatorNew
  (0x100990) restores s0/fp through fp, they are its outputs, and taking
  them as unused handed s0 = 0 to every caller (black screen; found with
  gbisect.py).
Inputs: median 13 -> 7, 40+ inputs 7,606 -> 1,907; outputs median 3.

cyc.sh's game-thread Mcycles/frame is the wrong number now: every run lands
on ~120-133 fps whatever the EE does, so the game thread spins in the
scheduler (publishSnapshotNow, clock_gettime, IOP, syscall lookup) for
whatever native saves. Use the EE wait log instead (RG_EE_WAITLOG=1 on the
walk; average the [eewait] lines after the unpaced point): main thread
guest share of wall / fps = guest ms/frame.

    native  main thread 45.9% of wall at 133 fps = 3.45 ms/frame
    oracle  main thread 55.5% of wall at 133 fps = 4.17 ms/frame   (-17%)

(one run each; emulated vsyncs also run faster natively, 52k/s vs 38k/s,
so other threads' shares are not per-frame comparable.) The frame rate cap
is elsewhere (render side). Still on the game thread: nativeCheckpoint 9%.

Next: inline checkpoint counter, then direct memory mapping; measure each
with the wait-log metric, several runs a side.

## What caps the frame rate (2026-11-08, end)

The field walk lands at ~120-133 fps whatever the EE does: the VIF1 thread is
86-95% busy and the game thread stalls 7-10 ms/frame on it (native pipeline
native CPU on/off, VU1 skip mode ~90 fps, and the old GS path with the
oracle, which also has the GS backend busy). VIF1 profile (native CPU on):
vu1native buildMesh/buildChunk ~12%, nativeproducer mesh/piece ~9%,
native2d::build ~8%, vif1 decode ~16%, GIF decode ~4%, malloc/free ~4%.

Unpaced (RG_SPEED=0) the emulated vsync free-runs at 30k-100k/s, so
per-vsync guest threads (t6 0x1bc520, ~10-23% of wall) do hundreds to
thousands of vsyncs of work per drawn frame: EE numbers taken unpaced are
inflated and not like real play.

Plan agreed with the user, in order:
1. Unpaced mode: one vsync per flip (no free-running vsync clock), so EE
   numbers are real.
2. VIF1 on several threads: keep the list decode in order on one thread
   (VU1 memory/register state machine), fan out mesh production
   (buildMesh, nativeproducer mesh, native2d build) to workers, reassemble
   in submission order; drop the per-frame allocations. Read the earlier
   VIF1-split notes first.
3. Game thread: guest code stays single-threaded (the game assumes one EE,
   no locks); move host work off it instead (nativeproducer hashing ~4%,
   IOP LLE ~2-3%, publishSnapshotNow ~5%).
4. Native CPU: inline checkpoint counter (nativeCheckpoint ~9%), then direct
   memory mapping; measure with the EE wait log, several runs a side.

## Unpaced vsyncs per frame (patch 0041, 2026-11-08)

Plan step 1 done. The idle path's unpaced jump to the next VBlank now gives
the guest 2 VBlanks per flip, then holds while VIF1 has lists in flight
(event, next flip, or 2 ms). Field walk, RG_SPEED=0@3700, native pipeline:

| | fps | main thread | vsyncs/s |
|---|---|---|---|
| native CPU | 150.7 | 3.67 ms/frame | 511 |
| oracle | 150.8 | 4.69 ms/frame | 365 |

Native is -22% on the main thread (one run a side). t6 (0x1bc520) fell from
10-23% of wall to ~1.5%. Both sides are still held at ~150 fps by VIF1 (92%
busy, main stall ~8 ms/frame): step 2 (VIF1 workers) is next. About one hold
per frame times out: VIF1's frame (~6.5 ms) is longer than the 2 ms hold, so
a frame gets ~3.4 VBlanks; close enough to play, raise kUnpacedVsyncHold if
it matters. (A blocking SIF RPC creates a semaphore per call, ids 0x2025xx+;
IOP waits are not held while VIF1 is idle.)

## VIF1, the producer lock and the presenter (2026-11-08, later)

Plan step 2 began as a split of the VIF1 thread and turned up the real
limits instead. Measured with the walk unpaced (`RG_WALK_SHOTS=0`, see below):

1. VIF1's "busy 92%" was mostly blocked time: on the CPU 35% of the wall,
   ~4,200 context switches a second, 94% of them on the producer's
   `s.mutex` in `nativeproducer::mesh/piece`. The game thread held it
   through `texture()`/`palette()` hashing; now hashed and converted
   unlocked (src/native_producer.cpp).
2. The presenter blocked ~4,400 times a second in `operator delete` in
   `Renderer::apply`: freeing mesh buffers VIF1 allocated takes VIF1's
   glibc arena lock. Mesh vertex/index buffers are now recycled
   (`native::reuseMeshBuffers`, dq8-gfx native_pipeline).
3. Patch 0041's hold held the whole idle jump, so guest DelayThread alarms
   waited out each 2 ms hold; it now holds only the VBlankStart.
4. The walk's screenshots made the backend read back every frame from the
   GPU (not only the saved ones): ~10% of the frame rate. walk.sh takes
   RG_WALK_SHOTS=0 for performance runs.
5. The draw stage (src/vif1_thread.cpp, RG_DRAW_STAGE, on under the native
   pipeline): what a list makes for the GS is consumed on a thread of its
   own (2D converter, effects, upload withholding, producer meshes); the
   list completes when the stage has run its end. +4% (180.6 -> 187.6 fps);
   lists finish later, so the game thread's drains grow.

Field walk, native CPU, no screenshots: 180.6 fps stage off, 187.6 on
(150.7 with screenshots before these). Now the presenter is the limit:
backend 93-100% of wall, in `Renderer::render` (drawMeshes ~40% of it,
apply ~19%, uploadMeshes ~7%). Next: the presenter's per-draw cost
(binding, descriptor sets), then VIF1 again.

Profiling recipe (perf_event_paranoid 1 after boot): on-CPU
`perf record -F 499 -e cpu-clock --call-graph dwarf,8192 -t TID` stopped
with `timeout -s INT`; off-CPU `perf record -e context-switches -c 1
--call-graph dwarf,16384 -t TID` (no tracefs needed). Thread names: VIF1,
Draw, GameThread, the process's main thread is the presenter.

## Commands

    # translate (~95 s), into the game build's input
    python3 tools/rgcpu/rgcpu.py disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv \
        build/generated/main build/generated/native
    cd build/game && ninja            # ~3 min after a header change
    # walk (tools/rgcpu/bench/walk.sh, cyc.sh)
    RG_NATIVE_CPU=1 RG_DIR=$PWD RG_NATIVE_PIPELINE=1 walk.sh ncon gpu 110 3000
    # harness (diff vs oracle)
    NC_ONLY=1 NO_RUN=1 tools/rgcpu/check_trees.sh OUT
    RG_ELF=disc/SCUS_974.90 OUT/rg_harness OUT/lib.so NAMES.txt 4   # ulimit -s unlimited
    # RG_VERBOSE=1, RG_TRACE=1|2, RG_DUMP=prefix, RG_ORACLE=file (bisect)

walk.sh, cpuframe.sh, cyc.sh (walks; cyc.sh = perf stat on GameThread),
tree.py (a function's native call tree) and bisect.sh (harness bisect) are
in tools/rgcpu/bench/. walk.sh and cpuframe.sh find the repo from their own
path; bisect.sh still assumes the scratchpad layout it was written in.

## After the hang

1. A/B on the unpaced walk (cyc.sh): game-thread Mcycles/frame, flag on vs off.
2. Then the structural steps in NATIVE-CPU.md: direct memory mapping (no
   bounds checks), cross-function inlining, VU0 registers as locals.

## The presenter: draw table, pool leak, and the snapshot fence (2026-11-08, evening)

Field, unpaced, screenshots off, draw stage on: 187.7 -> **267.5 fps**.

- The presenter reported "backend 100%" but was on the CPU only 33% of
  the time (perf stat on the main thread), with the GPU 67% busy. The
  off-CPU stacks (context-switches event) showed two waits:
  1. `DisplayPreparedPresentation` waited on the frame's own fence
     (`SDL_WaitForGPUFences(snapshot)`, vendored from DQ8) before the
     window blit: CPU and GPU took turns every frame. Removed: the blit
     is a later submission on the same queue and SDL GPU orders it. This
     alone was 192 -> 267.
  2. `Renderer::apply` blocked in malloc's arena lock: the mesh buffer
     pool sat at its 16384 cap because `rectDraw` and the effect draws
     (~140 a frame in the field) allocated fresh buffers that went into
     the pool and never came out; past the cap the presenter freed them,
     on the producer's arena. Both now take pooled buffers.
- Per-draw cost (187.7 -> 192 on its own): the mesh draws read their
  uniforms from a per-frame storage buffer (the draw table, one 320-byte
  row per draw, indexed by a draw id in each vertex) instead of two
  uniform pushes per draw. `drawFrame` now plans every pass and draw
  first (textures made, retired textures released after recording),
  uploads vertices + rebased indices + table once, then records.
  Neighbouring draws with the same pipeline, textures, sampler, scissor
  and blend constant are one GPU call: ~1260 draws a frame -> ~440 calls
  (the report line's "in N GPU calls").
- On the presenter's remaining CPU: recordPass ~41% (pipeline binds 12%),
  uploadMeshes' vertex repack ~21% (writes to mapped memory), apply 8%.
- Next: with backend at 38% of wall, VIF1 (80%) and the draw stage (83%)
  are the busiest; re-measure what caps the field now.

## Inline checkpoint (patch 0042) and what caps the field now (2026-11-08, night)

Plan step 4a. NC_BACKEDGE / NC_CALLCHECK are now inline: subtract the charge
from `g_eeNativeBudget`, read `g_eeCheckpointPending`, call out (cold) only
when the budget is spent or work is pending. The budget is the cycles to the
next deadline / slice end / timer batch, so the stop lands where it did.
Game thread guest time in the field 83.5% -> 77.9% of wall; fps unchanged
(260.5 vs 267.5, noise), because the time saved went into the held vsync
(sched 16.5% -> 22.1%).

What caps the field (~265 fps): no single thread. Per-thread CPU over 2 s:
game thread 95%, Draw 84%, VIF1 77%, presenter 39%. Per frame VIF1 idles
~25% waiting for the first list, and at the end the EE idles ~22% in the
vsync hold waiting for VIF1 + Draw to finish the frame's lists. The data is
copied at sceDmaSend, so the guest never needs the lists converted to go on;
what it waits for is 0041's hold (keeps ~2 vsyncs per frame) and the
libgraph sync drain (L2684, 1-9%). Overlapping frame N's VIF1/Draw tail with
frame N+1's guest logic is worth up to ~20%; making Draw (84%) and VIF1
cheaper raises the ceiling for everything (and is what handhelds need).

### VIF1 and Draw per-unit costs (same evening)

Field 263.7 -> 272.0 fps; VIF1 busy 79% -> 70%.
- clang lowers std::fmin/fmax to libm calls at the baseline x86-64 target
  (x86 MINSS differs on NaN and signed zeros). `src/fminmax.h` has rg::fmin /
  rg::fmax, bit-exact with glibc (tested over 10M random pairs: IEEE
  754-2019's -0 < +0, a NaN gives the other operand, a signalling NaN gives
  x + y). Used in vu1_native.cpp (widenBounds was 13% of VIF1, half of it
  the calls) and native_producer.cpp. gs_types.h (runtime) still calls libm.
- Patch 0043: V4-32 UNPACK copies runs, not a memcpy call per qword (~10% of
  VIF1).
- The 2D converter's register writes (a PRIM per mesh, the vertex-state
  restore after each packet) went out as A+D packets made with adPacket and
  decoded again by sendRaw (mirror, absorb, withhold, watch). They are now
  `Piece::writes`, applied by `sendWrites` straight to the mirror
  (`drawstate::observeWrites`, same carried GIF state) and absorbed; any
  transfer register, vertex kick or pending image data still goes the
  packet's way. Small on its own (+1%).
- Tried and reverted: holding the vsync only for lists a frame old (one
  frame of overlap). 232 fps: the guest got ~13 vsyncs a frame instead of 6
  and the libgraph sync (L2701) waited out the same tail. The game syncs
  every frame; the stages have to get cheaper.
- Draw (85%) is the busiest stage: 2D packets ~58% of it (native2d::build,
  sendRaw, piece), meshes ~23% (nativeproducer::mesh: per-vertex projection
  for the GS cull, the index loop, the MeshDraw's init).
- Tried and reverted: nativeproducer::mesh split in two, the stateless half
  (cull, indices, vertices, matrices) on the VIF1 thread ahead of the draw
  stage. Total VIF1 + Draw CPU fell 8 points (no 1 KiB VU1 copy per mesh)
  but fps fell 272 -> 256.7 (twice): VIF1 went to 81%, Draw to 65%. Work
  moved earlier in the EE -> VIF1 -> Draw chain lengthens the frame's tail
  more than it shortens Draw. Cut work, or move it later, not earlier.

## Next session (2026-11-08, end of night)

State: b93f833, field 272 fps (walk `gpu 110 3000`, mean of last 60 fps
lines, noise about +-5). Per-thread: game thread guest 78% of wall, Draw 85%
busy (82% on CPU), VIF1 70%, presenter about 38%. The frame is an
EE -> VIF1 -> Draw chain the game syncs every frame, so the gains come from
making a stage cheaper or moving work later in the chain.

Not yet tested in game by the user since the fence removal (be534b7) and
the inline checkpoint (cd931a1): field, battle, menus, movies, and any timing
change (cutscene and animation speed) from the new checkpoint budget.

Candidates, in order:
1. Draw's 2D path (about 58% of Draw). Each 2D packet is still walked
   three or four times: native2d::build, the draw-state mirror, upload
   withholding and the signal watch in sendRaw. One pass that feeds all of
   them, plus a cheaper per-piece MeshDraw in piece(). Expected 10-15% of
   Draw.
2. Draw's 3D meshes (about 23% of Draw): the per-vertex projection for
   the GS cull and the MeshDraw init. Back-face culling on the GPU
   rasterizer only if the GS's exact rules can be kept.
3. Plan step 3: host work off the game thread, e.g. imageHash (about 7% of
   the game thread), moved onto Draw (later in the chain).
4. Plan step 4 remainder: direct memory mapping, then ThinLTO.
Also: gs_types.h in the runtime still calls libm fmin/fmax (use
src/fminmax.h's approach in a patch).

Profiling notes: "busy" counters include blocked time; use
`perf stat -t TID` task-clock for real CPU, and the context-switches event
with dwarf call graphs for off-CPU stacks. Record one perf session at a
time; wait for 2xx fps in the log (past the paced phase) before profiling.

## 2D path, first pass (2026-11-09, 92aba24)

Field 268-272 -> 282.6 fps. Draw profile before (self): native2d::build 8.9%
+ closeMesh 4.7% + flushWrites 1.8%, decodeGifPacket 5.8% + writeRegister
4.2% (build and the mirror's observe), piece 7%, sendRaw 4.9%, mutex 2.9%,
uploadHash 3.6%, nativeproducer::mesh (3D) 18.5%.
- closeMesh ran at every register write and reset a ~300-byte Piece; when
  the mesh was empty it also freed the spare vertex vector it held. Now it
  only forgets the PRIM when the mesh is empty.
- withholdUploads copied every packet into `kept` even when nothing was
  dropped; it now copies only from the first drop.
- watchTransfers and absorb are one walk under absorb.
- A converted packet with no meshes went on whole through sendRaw, so the
  mirror decoded it a second time; under absorb it now goes as its pieces
  (writes straight to the mirror, uploads through sendRaw).
Next on this path: piece() (producer mutex, target/source lookups, a MeshDraw
per piece; neighbouring pieces with the same draw state could be one draw),
and the decode in build itself.

## 2D path, second pass: 300 fps (2026-11-09)

Field ~282 -> ~301 fps (A/B in one build, two pairs: 300.6 / 301.4 against
279.4 / 294.9). Draw CPU 2.81-2.91 -> 2.60-2.67 ms/frame.
- Of ~1,700 GIF packets a frame only ~250 draw anything; the rest are 3D
  meshes' register writes (PRIM + A+D, A+D + RGBAQ, A+D runs, cut A+D tags),
  transfer setups and lone IMAGE tags. `native2d::quick` decodes them once
  against a copy of the carried GIF state; if they made no vertex, their
  writes go straight to the mirror (`applyWrites`), and transfers and a
  lone upload tag go to sendRaw as they are, as build's pieces would.
  Anything with a vertex falls back to build. 43M of 52M packets take it.
- 2D quads (sprites, points, lines) are four indexed vertices, not six
  (`Piece::indices`; piece() copies them; the GS fallback expands). No fps
  change by itself: per-vertex work was not the cost.
- Noise: single walks swung 282-294 for the same build today. Compare A/B
  with a temporary env switch in one build, alternating order, and use
  tools/rgcpu/bench/cpuframe.sh NAME (per-thread CPU ms/frame over 10 s
  once unpaced) rather than fps alone.
Next: plan item 2 (3D meshes as packed VU1 output, unpacked and culled on
the GPU; nativeproducer::mesh is now the largest Draw cost).

## 3D meshes: the cull and the vertex pack on the presenter (2026-11-09)

nativeproducer::mesh no longer culls or converts vertices. The MeshDraw
carries the chunk as VIF1 made it (`MeshDraw::raw`, GSNativeMesh::Vertex
field for field, one copy), the program's header as opaque bytes
(`rawHeader`) and its colour cap. The renderer, at the start of a frame it
draws, calls the producer's cull (`native::setMeshCull` -> `cullMesh`, the
same code and arithmetic as before) for the indices, and uploadMeshes packs
GpuVertex straight from the raw vertices with an inline glibc-exact fmin.
With the depth log or a frame dump on, the Vertex form is made too, so
those tools read what they did. Frames the renderer drops are never culled.
- Why not the GPU: a vertex shader cannot drop a triangle, and a compute
  cull would put GPU division (not correctly rounded) into the game's exact
  skip rule. The presenter is later in the chain and exact.
- Before profile: mesh() was ~20% of Draw: resize zero-filling 52-byte
  Vertex ~18% of it, the copy with four fmin ~17%, the cull ~20%.
- A/B (old/new binaries via RG_BIN, cpuframe.sh): Draw 2.72/2.65 ->
  2.42/2.47 ms/frame, presenter 1.43 -> 1.70, VIF1 unchanged ~2.7;
  fps 297.9/297.7 -> 302.5/299.7.
- Screenshot runs are slower now (the per-frame readback serialises the
  presenter): 47 shots in 110 s where 57-63 before. Play is not affected.
VIF1 (~2.7 ms) is now the longest stage of the chain, Draw ~2.45. Next:
VIF1's runVu1, unpackPlain, readChunk, deliverPacket, widenBounds.

## VIF1: what could simply go (2026-11-09)

VIF1 2.76 -> 2.39-2.43 ms/frame (A/B, cpuframe.sh). fps unchanged within
this session's noise (the old binary measured 281-283 here, ~298 earlier):
- runVu1's own time was ~95% imageHash: the whole 16 KiB of VU1 code
  rehashed, a serial multiply chain, at every code upload (the game uploads
  program pieces per object). Now a hash per 256-byte block against a copy,
  rehashed only where memcmp finds a change, folded into one key.
- A mesh's DrawStage record carried 64 VU1 quads (1 KiB); the producer reads
  16 of them (4-11, 24-27, 60-63). Only those go; the Draw side puts them
  back at their offsets in a 1 KiB buffer of its own.
- DrawStage's staging vector zero-filled every record before the memcpy
  over it: now a vector whose growth leaves bytes uninitialised.
- Kept: widenBounds (VU1 memory quads 98/99 the game reads back).
Per frame now: game thread ~3.3 ms CPU of a ~3.5 ms frame, Draw ~2.6, VIF1
~2.4, presenter ~1.8. The game thread is the stage to cut next (plan step
4b: direct memory mapping).
Note: in one screenshot run the backend stopped saving shots after frame
13215 while the game ran on normally; not looked into.

## Texture hashing off the game thread's back: guest RAM write watch (2026-11-09)

The game thread hashed ~4.6 MB of texture uploads a frame (the game sends
the same textures from the same memory every frame): ~9% of it.
src/guest_write_watch.{h,cpp} keeps the pages a texture came from read-only
after hashing it; the first write to such a page (any thread) faults, the
handler stamps the page written and makes it writable, and the next upload
of an unchanged range reuses its hash (nativeproducer `sourceHash`). Exact:
no sampling, no collisions beyond the hash's own. A texture whose memory
changed twice is hashed and no longer watched, or hot pages cost a fault
and an mprotect (with a TLB shootdown) every frame: with that rule kernel
time stays near where it was and ~0.85 MB a frame is still hashed.
- Kernel writes into guest RAM cannot fault and retry (fread straight into
  a protected page fails with EFAULT): patch 0044 calls
  `beforeHostWrite` before each (CD reads, module and ELF loaders, fioRead,
  LibC fread, memory card, IOP host files), which unwatches the range. A
  new host read into guest memory must call it too.
- Only whole pages inside guest RAM are watched (it is a `new[]`, not page
  aligned). RG_WRITE_WATCH=0 turns it off.
- A/B in one build, two pairs: game thread 3.30/3.28 -> 3.17/3.14 ms/frame;
  fps 292.9/287.9 -> 291.5/299.4.
- Same mechanism is the base for direct memory mapping (a SIGSEGV handler
  on guest RAM).

## Next session: ThinLTO trial, then mapping (agreed 2026-11-09)

State at 2b330d8: field ~290-300 fps (this machine swings ~10 between runs).
Per frame CPU: game thread ~3.15 ms of a ~3.4 ms frame (the limit now),
Draw ~2.6, VIF1 ~2.4, presenter ~1.8. Game thread: guest code ~50% (nc_
38.9%, older recompiled 11.6%), scheduler/boundary ~10% (grantNativeBudget
+ checkpointDue 4.4%), memory slow path ~5%, kernel ~5-6%, DMA list copy
(vif1 send) ~4%, VU0 ~4%.
Plan, in order:
1. ThinLTO trial (-flto=thin over the game, generated native code included)
   and an A/B with cpuframe.sh: old/new binaries in scratch dirs named
   roguegalaxy, RG_BIN=..., alternate, two pairs.
2. Memory mapping, safe half: RAM, its mirrors (0x2000_0000, 0x3000_0000,
   0x8000_0000, 0xA000_0000) and the scratchpad in one host region, so only
   hardware registers take the slow path; count hardware accesses per frame.
3. Full mapping (no inline check, fault emulation for I/O) only if 2 shows
   I/O accesses are rare: an AOT build cannot backpatch, so every I/O
   access would be a ~µs fault.
4. Scheduler/boundary overhead in small cuts.

## ThinLTO and the memory slow path (2026-11-09)

- ThinLTO on by default (89c8ab6, RG_THIN_LTO; clang, not Windows; VU AOT
  shards excluded). Game thread 3.04 -> 2.94 ms/frame, ~307 -> ~320 fps.
- Memory slow path, counted (temporary counters in nc_contract.h's cold
  path, field, ~320 fps), per frame: ~50,000 stores to the uncached mirror
  0x30xxxxxx, all from GsBuildImageUploadRefPkt (0x158390) building texture
  upload packets; ~550 mirror reads; scratchpad 0; hardware ~3. 534785b
  takes the 0x20000000-0x3FFFFFFF mirror inline: ncSlowWrite ~2.8% of the
  game thread -> gone (~0.08 ms), below the walk's noise.
- So the "safe half" (one host mapping for RAM, mirrors, scratchpad) has
  nothing left to win: what reaches the cold path now is a handful a frame.
  Full mapping (no inline compare, I/O by fault) is viable on these counts
  (~3 faults a frame) but removes only the well-predicted compare per
  access: small. Not next.
- Game thread profile after (perf, 8 s): nc_ ~37%; scheduler/boundary/clock
  ~14.5% (__vdso_clock_gettime 3.85, nativeBoundary 2.63,
  grantNativeBudget 2.62, checkpointDue 2.18, EeScheduler::run 1.95);
  vif1 send 3.75; nativeproducer::texture 2.06; translateAddress 1.57
  (caller not yet known: not the native memory path). Next lever: the
  scheduler/boundary/clock share.

## Scheduler and boundary counts (2026-11-09)

Counted in the field (temporary counters, ~300 fps), per frame:
- ~12,000 native->runtime boundary calls (ncBoundaryCall), ~1,700
  checkpoints, ~1,600 clock reads, ~23,000 budget grants (each boundary and
  checkpoint; 97% of grants sized by the EE timer batch's room).
- The EE wait log (RG_EE_WAITLOG=1) at ~318 fps: guest main thread 95% of
  wall, scheduler 2-3%: the game thread is busy with guest work, boundaries
  included, so cutting boundaries is cutting the frame.
- Boundary targets (share of samples): AabbOverlap ~1/3 (hooked) -> now
  native (73e40f9, hooks skipped under RG_NATIVE_CPU, regenerate rgcpu).
  Left, roughly per frame: jalr into native functions (DataTableGetKey
  0x428070 ~1,100, CharaStatusTest 0x2121c0, ModelMeshLinksIsKindOf
  0x15b120, AnimKeyEvalVec3Linear 0x155180, DataTableFindByKey 0x3680a0,
  sceGifPkCloseGifTag 0x10a630: ~300-450 each), hooks GsPktSetXYZ 0x13ef10
  / GsPktSetXYZf 0x13f110 (HUD/widescreen, must stay; ~600 + ~450), stubs
  memcpy 0x1319a8 and MtxMulVec4 0x10a8c0 (~280 each), 0x41d960 (overlay?).
- The clock reads are the run loop's (pacingNow, unpacedVBlankHeld), ~490
  a frame from a held VBlank yielding at every safe point. That is waiting
  on the renderer, not frame cost: polling it instead made it slower.
- Next: devirtualize the hot jalr sites (record targets per site, emit a
  compare + direct native call before the boundary), then a cheaper call
  form for host leaf hooks/stubs.

## Deck mode results (2026-11-09, at 73e40f9)

User's deck mode on (`deckmode on|off`, lives in ~/Projects/deckmode, machine-local; 8 CPUs at 2.87 GHz,
~1.5x a real Deck's CPU). Field walk, uncapped part, per-thread CPU
ms/frame (a copy of cpuframe.sh waiting for >70 fps, walk 170 s; GPU busy
from /sys/class/drm/card1/device/gpu_busy_percent):

    scale  settings                    fps (mean, range)  game  Draw  VIF1  presenter  GPU busy
    1x     defaults                    189-203            4.4-4.7 3.8-4.0 3.6-3.8 2.5  -
    2x     draw 301%, camera 125       165 (155-176)      5.4   4.8   4.4   3.0        94%
    4x     defaults                    68.6 (65-77)       13.5  11.4  10.9  2.9        93%
    4x     draw 301%, camera 125       68.4 (62-85)       13.4  11.7  10.9  3.2        91%

- 1x: game thread bound, same proportions as unthrottled (~1.5x the ms).
- 2x and 4x: GPU bound (an RX 6700 XT, as throttled). At 4x the threads'
  CPU time is mostly waiting on the GPU. 2x keeps >2.5x headroom over 60.
  A real Deck's GPU (~1.6 TFLOPs) is far weaker: the GPU side carries over
  only as far as deck mode throttles the GPU to match.
- The HD texture pack was not in these runs: this worktree has no
  textures/ folder (the setting does nothing here, play.sh included).
- Walk settings: walk.sh hard-sets RG_GS_SCALE=1 and an empty settings
  file. A full play ini (native-on.ini) breaks the scripted inputs
  (skip_opening, instant text change the boot), so give it only graphics
  keys ([game] draw_distance, camera_distance, texture_pack_*).

## For the in-game QA pass (changes since 2b330d8)

- ThinLTO (89c8ab6): whole game; anything odd anywhere.
- Uncached mirror inline (534785b): texture upload packets
  (GsBuildImageUploadRefPkt); watch for texture corruption.
- VU0 box tests translated under RG_NATIVE_CPU=1 (73e40f9): collision;
  watch for falling through floors (Salgin chest house was the old case).
  RG_NATIVE_CPU=0 or play.sh cpu0 puts the host versions back to compare.
- Budget grant without the scale division (patch 0045): exact, but it is
  the scheduler; watch for timing oddities (movies, sound sync).
- Still open from before: write watch (2b330d8: area transitions, battles,
  cutscenes, save/load; RG_WRITE_WATCH=0 rules it out), 2D quick path and
  indexed quads (menus, HUD), raw meshes culled on the presenter (3D).
- Build note: after 73e40f9 the native code must be regenerated:
  python3 tools/rgcpu/rgcpu.py disc/SCUS_974.90
  config/SCUS_974.90/functions_main.csv build/generated/main
  build/generated/native (done in this worktree; build/game is current).

Unthrottled 4x (same walk, 2026-11-09, 73e40f9): 276.5 / 282.0 fps
(defaults), 278.7 (draw 301%, camera 125); GPU busy 94-96%; game thread
3.17-3.32 ms of a ~3.6 ms frame, Draw ~2.7-2.8, VIF1 ~2.5-2.7, presenter
~1.75. At 1x the same build does ~320. So at 4x the GPU is the limit now,
narrowly: the game thread is ~90% busy right behind it (deck mode
throttles the GPU too: 4x there was 68 fps).
