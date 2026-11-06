# Splitting native-renderer into stacked PRs

Written 2026-11-05. `native-renderer` (185 commits, ~26k lines over 125
files) was too large to merge as one PR, and much of it is GS-side work
that the native pipeline makes unnecessary. It is being split into PRs
against `victor-takai/rogue-galaxy-recomp`, each one built fresh from
`main`. This branch stays as the archive the pieces are carved from.

## Rules

- **Build from the final code, don't replay history.** Each PR is cut from
  the previous one and brings over files or commits as they stand at the
  end of `native-renderer` (`18fc450`), minus what is dropped. Reverts and
  rewrites in the history don't come along.
- **Stacked.** `split/N` branches from `split/N-1`. There's no push access
  upstream, so every PR targets `main` and its description starts with
  "Stacked on #X" plus a link to the fork compare
  (`notavolleyball-dev/rogue-galaxy-recomp/compare/split/N-1...split/N`).
  Each PR's diff shrinks to its own changes as the one below merges.
- **Every branch passes a full build before the next one starts.** Do it in
  the split worktree (`~/Projects/rogue-galaxy-recomp-split`, with its own
  submodules and a `disc` symlink to `../rogue-galaxy-recomp/disc`):
  `python setup.py tools && python setup.py recompile && python setup.py build`.
  Also check that the patch stack applies in order to a clean PS2Recomp
  checkout at the pinned commit (a detached `git worktree` of the
  submodule, `git apply` each patch in name order).
- **Patches are renumbered** to follow main's (0022 stays reserved for the
  Linux branch). A patch whose context named a dropped patch is regenerated
  over the new stack, not fuzzed.
- **Drop all GS-thread efficiency work.** With the native flag on the GS is
  never entered, and the GS path stays only as a fallback until the native
  path's graphical issues are fixed.

## Status

| PR | Branch | What | State |
|---|---|---|---|
| #38 | `split/1-fixes` | mini_json on libstdc++ 16, `RG_SPEED`, `cdrom1:` paths (patch 0024) | open, built |
| #39 | `split/2-cpu` | direct call table (0026), `lean_pc` (0027), the VU0 matrix program native (0025) | open, built; `rg_vu0_check` 100,000 cases, 0 differ |
| #40 | `split/3-vu1` | the remaining VU1 program pieces, clipping chunks on the GPU (0028), program identification by code-image hash (0029), backface lists (0030), the menu switch turns the native VU1 programs on, `tools/vu1`, `VU1-NATIVE.md` | open, built; `rg_vu1_oracle --candidate native` over 268 recordings: 4,164 runs, 0 differ |
| #41 | `split/4-pipeline` | pipeline foundation: `dq8_gfx_native`, backend hosting, producer (textures, palettes, targets, VU1 mesh draws), `src/native_capture.*`, `src/draw_state_mirror.*`, draw registers (0031), `NATIVE-PIPELINE.md` | open, built, **not run in game** |
| 5 | `split/5-pipeline-frame` | effects as sprite draws (`d8d98cc`), 2D through the converter (`47d47f4`); host uploads and local copies next (below) | in progress, built, **not run in game** |
| 6 | `split/6-fsr` | FSR 1 display filter | not started |

**Before PR 5: run #41 in game.** `RG_NATIVE_RENDERER=1
RG_NATIVE_PIPELINE=1 build/game/src/roguegalaxy disc/SCUS_974.90` in the
split worktree. Expected: the 3D scene drawn natively, with no HUD, text or
effects. With the flag off it should behave exactly as #40. The mirror and
the capture file are new arrangements of the branch's code and have never
run.

Patch numbers taken: 0024 cdrom1, 0025 vu0 hook, 0026 direct call table,
0027 lean_pc, 0028 clip planes, 0029 MPG of the same code, 0030 backface
lists, 0031 draw registers. Next free: **0032**.

## What was dropped

From PR 3: `d084f79` (the GPU-timer label per VU1 program, and patch 0028's
`GSNativeMesh::Header::source`), `0c3e247` (shadow-volume fans grouped by
blend; it only cuts draw calls in the GS backend), `ab59c86` (textures sent
by reference).

Everywhere: the upload audit, upload store and host-writes memory, GS VRAM
reserve (0025), the GPU-side texture expansion and palettes in
`sdlgpu_textures`, named textures and targets in the backend, the bypass,
the native-submit path, upload skips and their reverts, the display-frame
deferral, the same-state tests on the GS path, draw origins, the
composite census, `gs_memory_uses`, and the in-backend effects drawing
(`sdlgpu_effect.cpp`, `gs_effect*.frag`, the backend's object layers,
`SubmitNativeEffect`).

**Correction to the first plan.** `native_effects.cpp` is not only
GS-backend work. Most of it is the pipeline's capture layer: the game hooks,
the effect builder (its descriptions are what `nativeproducer::effect`
draws), the 2D converter, and the draw-register mirror. Only the sdlgpu
side of the effects is dropped. Main has none of `native_effects.cpp`.

## PR 5: the rest of the frame

**Progress (2026-11-06).** On `split/5-pipeline-frame`:
- `d8d98cc`: `GSNativeEffect` moved game-side (`src/native_effect.h`, no
  patch). `src/native_effects.*` keeps the effect hooks, the builder, the
  colour filter and the flip's copy. Probes, composites, 2D, store-image
  and the target/texture hooks (#41's `native_capture`) are out. The
  producer's `noteEffect`, `effect` and `layerCommand` are back. In the
  VIF1 thread: the held effect, `listAddress`, list origins gathered when
  effects are on. A refused effect goes on as its packets, since there's
  no `SubmitNativeEffect` fallback.
- `47d47f4`: the converter is `src/native_2d.*` (`rg::native2d::build`
  and `observe`). There's no owed-register mode: it writes each mesh's PRIM
  before it, and #41's `draw_state_mirror` gives the state, so there's one
  mirror. The texel flag is `Piece::texelUvs` plus a `piece()` argument, so
  patch 0028's `source` isn't needed. Every packet is converted under the
  pipeline (`RG_NATIVE_2D=0` turns it off); the Prim2D block hooks were
  dropped as redundant. A refused piece goes to `gs.submitNativeMesh`.
  `eachPacket` moved to `gif_events.h`.

**Next in PR 5:** host uploads and local copies (items 4 and 5 below). The
archive only calls `hostUpload` and `noteLocalCopy` from the absorber;
here, capture them in `sendRaw` while still sending to the GS, so a 2D
piece reading a non-named upload isn't "source unknown". Then the
stale-record check (`replaceUpload`, from `withholdUploads`). Composites
(object layers) need `vif1Cursor` (from the dropped patch 0022): either a
small new patch or another way to tell a tail's VU1 kicks. Step 7 and the
tripwire could be PR 6.

Steps 5 to 7 of the branch's `docs/NATIVE-PIPELINE.md`, carved from the
final code. Read that doc's step records before starting. What has to come
over, and the problems already known:

1. **The effect description type.** The effect builder makes
   `GSNativeEffect`s, a type from the dropped framework patch 0022 (which
   also adds `GS::submitNativeEffect` and the backend's effect path).
   Recommended: move the struct game-side (e.g. `src/native_effect.h`) and
   have the builder hand it straight to `nativeproducer::effect`, with no
   framework patch. Then bring back `noteEffect`, `effect` and
   `layerCommand` into the producer (cut from #41), and the stats-line
   segments and counters trimmed with them.
2. **The effect builder and its hooks**, out of `native_effects.cpp`: glare,
   shadow, CBlur, bloom, DOF, refraction capture, focus filter, shadow-map
   blur, colour filter, the flip's 32 strips, the composites (object
   layers, step 5c). Only the parts that feed the producer. Everything that
   called `SubmitNativeEffect` or drew in the sdlgpu backend stays dropped.
3. **The 2D converter** (`build2D`, `observe2D`, GsPkt blocks, points and
   lines as thin quads, fogged vertices) and the producer's `piece()`
   (cut from #41). `piece()` reads a texel-UV flag from
   `GSNativeMesh::Header::source` (bit 31), a field from the dropped patch
   0028. Either carry the flag another way (a field of the game's own, or
   pass it as an argument) or bring back a minimal header field in a new
   patch. The converter owns draw state on the branch (owed registers,
   `takeOwedRegisters`, `payOwed`): `src/draw_state_mirror.*` from #41
   should grow into that, not get a second mirror beside it.
4. **Upload withholding (step 3b)**: `withholdUploads` and
   `withheldContinuation` in `vif1_thread.cpp`, with the producer's
   `takeProduced` and the stale-record check (`replaceUpload`, uploads up
   to 16 KiB).
5. **Leftover packets absorbed (step 6)**: host uploads nobody took kept as
   images (`hostUpload`), local copies mirrored (`noteLocalCopy`).
6. **Step 7**: the stubs' register writes through the converter (7a, patch
   0035 the register sink), depth reads from the renderer (7b,
   `readDepth`; `src/depth_reads.*`), colour captures (7c,
   `captureTarget`, `native::beginColorRead`), the pipeline's own present
   from the flip (7d, patch 0036 the host present hook; `graphics.cpp`'s
   `presentNative`, `serveNativeReads`, `presentNativeFrame`, flip pacing
   skipped under the pipeline).
7. **The structural check**: `src/gs_tripwire.*` and `graphics.cpp`'s
   install path that builds no GS around the backend under the pipeline.
   Patch 0034 (the boundary counters) touches many GS-internal sites. Leave
   it out unless the tripwire isn't enough.
8. **The fixes after step 7**: `9636e01`, `e31a744`, `c5d7c6b`, `db6bfbe`,
   `66835f2`, `9a7a708`, `6bd4631`, `4aad920`, `8338690`, `7817d14`,
   `a2a9c87`. Most are in the final files already. Check each one is
   covered.
9. **Docs**: extend `NATIVE-PIPELINE.md` with the steps' records and the
   known limits (targets clamp alpha above 0x80; 7b needs D32_FLOAT; 7c is
   served from the last frame drawn; the title screen's unknown-source 2D
   pieces; the Flash Sword trail, parked). `tools/native-check.sh` comes
   along.

If PR 5 is too large to review, split it at item 6: effects and 2D first
(5a–5c, 6), then step 7 and the tripwire.

## PR 6: FSR 1

`a630976`, `81992ad`, `86a563f`: FSR 1 (EASU + RCAS) as a display scaling
filter, its sharpness slider in the F1 menu (default 70%),
`present_fsr.frag`, `docs/FSR.md`. Independent of the pipeline; it works on
either path. It could go before PR 5 if PR 5 waits.
