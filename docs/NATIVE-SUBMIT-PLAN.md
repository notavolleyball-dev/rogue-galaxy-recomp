# Native submit path: plan

Status: proposed (2026-11-02), not started. Phase 1 first.

## Why

With the native renderer, the three draw conversions (`RG_NATIVE_2D`,
`RG_NATIVE_ALL_PACKETS`, `RG_NATIVE_VU1_MESHES`) and
`DQ8_GFX_NATIVE_BYPASS=1`, nothing on the walk writes GS local memory and the
GS no longer assembles a vertex. Yet the renderer's thread (the "GS backend",
`GSWorker`) is still the one that limits the frame rate: about 200 fps at 4x
on the field, the thread saturated. It is the renderer, not GS emulation, but
it still works in GS terms: every draw and upload reaches it as GS registers,
GS addresses and GS pages, and it does the bookkeeping that model needs.

A native renderer for a PS2-sized scene should need well under 1 ms a frame
on that thread. It takes about 5.

## The numbers (field, all conversions on, bypass on, 4x, ~200 fps ceiling)

Per frame, from the `[gfx]` stats lines:

| | |
|---|---|
| Uploads | ~352, ~4.1 MB, almost all the same textures sent again |
| Native meshes | ~1,296, every one through submit's full path (0 same-state) |
| Vulkan draws / passes / flushes | ~233 / ~30 / ~17 |
| GIF packets through the GS frontend | ~4,750 (register state) |

Per thread:

| Thread | Per frame | What it is |
|---|---|---|
| Backend (`GSWorker`, saturated) | ~5 ms | Vulkan driver ~25% (per-draw descriptor binds, passes); libc copies ~13% (upload bytes into named images, vertex uploads); named-image scans and block marking ~10% (`dropNamed`, `gsMarkBlocks`, covered marking); target/palette page bookkeeping ~4% (`markReadPages`, `invalidate`, `markClutReads`); mesh submit ~9% and primitive submit/state ~5% (`submitNative` builds a stand-in primitive and takes the full path per mesh); texture identity ~6% (hashing, linearize); swizzled write-backs into the CPU copy of GS memory ~5% |
| VIF1 | ~4 ms | ~35% real work (native VU1 programs, unpacking, mesh building); ~25% converting packets (`build2D`, GIF decoding done twice with the mirror, `deliverPacket`); ~6% vector copies and frees |
| GIF | ~1.2 ms | Register packets into the GS frontend for draws already converted |

`clock_gettime` (3% backend, 17% GIF) only runs with `DQ8_GFX_STATS_EVERY`
set, which the measurement script sets; normal play doesn't pay it.

Profiles: `perf record -F 499 -g` launched through
`tools/vu1/oracle/native-perf.sh` with `EXE=...`; per-thread with
`perf report --comms GSWorker --percentage relative`.

## Goal

With the native renderer and the bypass on, a converted draw never touches
GS addresses, GS registers or GS pages. The GS path stays, unchanged, as the
fallback for anything not converted (upstream keeps it).

## Phases

Each behind its own env switch (documented in `docs/DEVELOPMENT.md`), and
checked against the screenshot walk before the next.

### 1. Textures by name, not by address

- `TexBuildUploadPkt_158610` (RGDecomp) builds each upload from the image
  node: its mip addresses in main RAM (`Tex::mip[]`) and palette
  (`Tex::clut`), sent by reference (`GsBuildImageUploadRefPkt_158390`) or by
  copy (`GsBuildImageUploadPkt_1581b0`). The game sets a node's image through
  `NameTableNodeSetImage_15a640`.
- Each texture is resident on the GPU, keyed by its node, with a generation
  bumped when `NameTableNodeSetImage` changes the node (or when a hash of the
  referenced RAM changes; `DQ8_GFX_VERIFY_NAMES=1` keeps hashing as a check).
- An unchanged re-upload is skipped before its bytes are copied anywhere: no
  transfer to the backend, no named-image copy, no hash, no block marking.
- Expected: most of the ~4 MB a frame of copying, hashing and marking gone,
  roughly a quarter to a third of the backend thread.
- Settle first in the decomp: does anything write texture memory in RAM
  without `NameTableNodeSetImage` (generated textures; the movie is already a
  canvas)?

### 2. Native draw records

- The converter's mirror (`build2D`'s `Carried`, `observe2D`) already tracks
  PRIM and the vertex registers. It also tracks the draw state (TEX0, CLAMP,
  TEX1, ALPHA, TEST, FRAME, ZBUF, SCISSOR, XYOFFSET, FBA, PABE, COLCLAMP,
  TEXA), and resolves the target (descriptor hooks, `gs_named_targets.h`) and
  the texture (phase 1) by name on the VIF1 thread.
- It emits {target, texture, pipeline key, scissor, mesh} records straight
  to the backend. Converted draws send no register packets to the GS
  frontend, so the GIF thread is mostly idle.

### 3. A lean backend path for those records

- Pipeline, descriptors and texture resolved once per state change, not per
  mesh.
- No page sets; hazards checked by name ("is this texture the current
  target?"), with a by-name snapshot where an effect samples its own target.
- Consecutive same-state meshes merge into one Vulkan draw (the per-mesh
  transform records already exist: `GsNativeMeshRecord`).
- Expected: the full-path submit cost goes away, and the driver share
  shrinks with fewer draws and binds.
- Settle first: which effects sample their own target.

### 4. VIF1 thread cleanup

- Decode each packet once, not in both `build2D` and the mirror.
- Drop the per-packet vector copies and allocations.
- Skip the GIF stage for converted output.

## Expected outcome

- Backend thread from ~5 ms to maybe 1.5–2 ms a frame.
- The VIF1 thread (~4 ms, mostly real VU1 work) then sets the ceiling, at
  roughly 250 fps, until its own work is cut or split across threads.
- At normal speeds this is mainly power, latency and headroom.

## Measuring

Single ceiling runs vary by about ±10 fps: at unlimited speed the last-25 s
window of `native-perf.sh` covers different game content each run (draw rates
differed 14,300 vs 22,700 a second on identical code). So:

- each build: the median of 4 runs of
  `DQ8_GFX_NATIVE_BYPASS=1 RG_SPEED=0@3700 tools/vu1/oracle/native-perf.sh NAME gpu 4`;
- plus per-thread time per frame from the stats lines and profiles;
- never two game instances at once (they share the memory-card directory and
  the walk diverges).

A pad-frame range doesn't fix the content at unlimited speed: the game runs
many frames of logic per drawn frame.

## Parked

`wip/named-graves` (393e20c): lazy write-backs for dropped named images
("graves", removed the ~5% of swizzled write-backs; the walk's pictures and
memory reads unchanged), a same-shape re-upload fast path (not checked on
screen), and `RG_BENCH` (doesn't work as intended, see above). Unmeasured;
phase 1 replaces most of it.
