# Native submit: honest status and where to resume (2026-11-02 night)

**Superseded as the direction (2026-11-04):** the goal and plan are now
[NATIVE-PIPELINE.md](NATIVE-PIPELINE.md): no GS at all with the native flag
on, with boundary counters as the definition of done. This file stays as
the record of the 2026-11-02 work.

Read this before `docs/NATIVE-SUBMIT-PLAN.md`. The plan's status line
overstates what was done; this file is the correction.

## What went wrong

- **Wrong bottleneck.** The plan assumed the backend thread (GSWorker)
  limits the frame rate, from the `[gfx]` line's "backend N% of wall". That
  stat counts time *inside* backend calls, waiting included. Measured on-CPU
  time (`/proc/<pid>/task/*/stat`, 10 s at 1x, everything on):
  GameThread 98%, GSWorker 77–82%, VIF1 72–75%, GIF 24–26%. **The game thread
  limits the ceiling.** GSWorker blocks ~3,300 times a second, half of them
  idle in the queue waiting for work. 1x and 4x give the same fps (~200), so
  it is not the GPU either.
- **Phases reported done that were not.** The plan's goal: *a converted
  draw never touches GS addresses, GS registers or GS pages.* Phase 2 was
  built as meshes carrying a `GSDrawState` (GS registers), not native
  records by name. Of phase 3, only the same-state path was built; hazards
  by name (no page sets) were not. Both were reported as "code in".
- **Gains claimed from single noisy runs** (±10 fps at `RG_SPEED=0`).

## What is in (commits on `native-renderer`)

| Commit | What | Verified? |
|---|---|---|
| 6998c14 | Held uploads (`src/held_uploads.cpp`), kept images by content, lazy write-backs (graves) | Counters only: 2.6M kept, ~380 adopted |
| e37f99a | Native meshes take the same-state path | Did nothing until 0604445 |
| 842b4bb | Converter owns the draw registers; meshes carry `GSDrawState` (patch 0032) | No |
| 3e9c51b | build2D on packets in place, uploads as views, vertex reuse | No |
| 6c424e6 | A re-upload of the image already at its base does nothing | Counters only |
| 0604445 | Same-state test compares `GSDrawState` field by field (padding broke `memcmp`; the GS path's same-state never worked either) | Counter: 62k of 78k meshes same-state |
| ab59c86 | VIF1 chain copy leaves by-reference textures in RAM (patch 0033) | One run: 0 fallbacks; 215.9 fps at 1x vs 191–202 before (single run) |

None of these are checked on screen. The last walk with all of them was not
compared. Memory uses went 46 → 49 after ab59c86; not looked into.

## Game thread profile (1x, everything on, before ab59c86)

68% recompiled game code; 17% PS2Recomp runtime (guest dispatch 6.7%,
checkpoints 3.4%, fiber switch 1.5%); 8.8% our VIF1 chain copy (fixed in
ab59c86); ~3% our other hooks. The ceiling is now the game and the runtime.
The renderer threads have 20–25% headroom.

## Resume here

1. **Verify what is in.** One screenshot walk (`scratchpad/shots.sh`, never
   two at once). Compare against a reference made with the new switches off:
   `RG_HELD_UPLOADS=0 DQ8_GFX_NO_KEPT_IMAGES=1 DQ8_GFX_EAGER_WRITE_BACKS=1
   DQ8_GFX_NO_NATIVE_SAME_STATE=1 RG_NATIVE_DRAW_STATE=0 RG_VIF1_REF_IMAGES=0`.
   Check the 46 → 49 memory uses. Revert what draws wrong.
2. **Define decoupled as three counters that must read 0 on the walk**, for
   converted draws: (a) the backend receives a GS frame/texture address,
   (b) the backend receives a `GSDrawState`, (c) the backend's page
   bookkeeping (page sets, invalidation, `m_shadow`) is entered. Add them,
   run the walk once, and the nonzero ones are the remaining work. Nothing
   else gets added to the list.

   **Built (2026-11-04), not yet run.** The `[decoupled]` line, printed with
   the `[gfx]` stats (`DQ8_GFX_STATS_EVERY`), counts per window. Converted
   draws are the calls to `SdlGpuBackend::SubmitNativeMesh` and
   `SubmitNativeEffect`, host reads on the effect queue excluded. (a) counts
   every mesh (its `GSDrawState` holds FRAME, ZBUF, TEX0) and every effect
   with buffers (`Buffer::base`). (b) counts every mesh. Both are true by
   the interface's types, so they read 0 only once the interface changes.
   (c) counts the draws whose submit entered a function that calls
   `GS_NOTE_PAGE_BOOKKEEPING()` (`gs_vram.h`, a thread-local count): page
   and block marking, both caches' `invalidate`, the target cache's page-set
   lookups and resolves, `markReadPages`, `markClutReads`, `sourcePagesFor`,
   texture reads served from `m_shadow`, and the backend's pending-page
   helpers, including the same-state path's inline test. Not counted: work
   deferred past the call (a later flush), and effects carry raw ALPHA/TEST/ZBUF
   values, which are GS registers but not a `GSDrawState`.
3. **Performance** beyond that is the game thread (the recompiler's output,
   the runtime's dispatch and checkpoints, or `docs/LOGIC-DRAW-SPLIT.md`).
   Separate project.

## Working rules (agreed after tonight)

- State a checkable acceptance criterion before implementing. Announce any
  simplification or deviation *before* doing it.
- Report: criterion, measured result, pass/fail. "Builds; unverified" is not
  done.
- Measure per-thread on-CPU time before any optimization or profiling.
- One check run after a change, then report. No benchmark series unless
  asked.
- One phase per request.
