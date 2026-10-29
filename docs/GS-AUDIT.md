# GS backend audit, 2026-10-29

Where the field walk's frame goes on the GPU and the CPU, with every VU1
program native (`RG_VU1_NATIVE=gpu`), what was cut, and what is left.
RX 6700 XT; the walk and measure of `tools/vu1/oracle/native-perf.sh`
(File 01's street, 60 fps mode).

## Method

At 8x the walk is GPU-bound (99% busy), so frame rate is a direct measure
of GPU work. A temporary probe in `sdlgpu_backend.cpp` (not committed)
waited until the party stood still, then skipped one category of draws
for 5 s at a time and logged the frame rate of each step, with a baseline
first and last; it also counted passes, snapshot copies and the draws of
one frame by target and state. The uncommitted `DQ8_GFX_PROFILE_CYCLE` in
the main checkout does the same with power; this one reads frame rate.

## Done

| Change | Commit | 8x fps | 3x draws/s | 3x GS backend |
|---|---|---|---|---|
| Before (gpu mode, a draw per mesh) | 6d4e848 | 37.8 | 110,789 | 71% |
| Meshes of one state share a draw | e625ba1 | 38.9 | 51,248 | 56% |
| Meshes wholly off screen not drawn | cac9414 | 39.0 | 46,014 | 55% |
| Snapshots copy only what is read | 4f2f822 | standing: 41.7 -> 43.4 (the walk window not run) | | |
| Source-alpha blends in one pass (dual-source) | d1c6a8c | 44.2 | 38,397 | 56% |

The VU1's own mode (`0`) gets the last two as well: 44.2 fps at 8x,
38,212 draws/s at 3x. At 3x both modes hold 60 fps; `gpu` keeps the VIF1
thread at 18% of the wall against 45%, and the GS backend at 56%
against 66%.

Tried and dropped: a fragment shader without the alpha test's `discard`
for draws whose test always passes, so the depth test could run first
(GS-PERFORMANCE.md candidate 6). No change in frame rate (43.5 against
43.4 fps); not committed.

## Where the frame goes now (8x, standing, 20.0 ms)

Frame time saved by skipping each category (they overlap):

| Skipped | fps | ms saved |
|---|---|---|
| nothing | 50.1 | |
| flat draws (no depth buffer: blur, bloom, sprites, UI) | 69.4 | 5.6 |
| draws into the per-object buffers (02a00 and the others with depth) | 62.8 | 4.1 |
| blur and bloom draws (feedback: sampling their own target) | 58.9 | 3.0 |
| snapshot copies | 53.0 | 1.1 |
| shadow volumes | 52.8 | 1.1 |
| native meshes | 51.7 | 0.6 |
| every 3D draw | 75.6 | 6.8 |
| every draw | 75.6 | 6.8 (the CPU limit at 8x, about 75 fps) |

One frame: 626 draws (91 of them native meshes), 195 render passes, 105
snapshot copies of the blur's buffers, 14 snapshot copies through local
memory's padding path.

## What is left, by size

1. **Render passes and the stalls between them (about 195 a frame).**
   Each character is drawn into a buffer of its own (02a00, cleared over
   its bounds), then composited into the frame: about 30 round trips
   between two 8x targets. The blur draws 34-pixel strips into a 192x192
   buffer, each reading what the one before wrote, so each takes a
   snapshot and a pass of its own (about 100). The draws themselves are
   small; skipping them saves far more than their pixels would cost, so
   the stalls at each pass and copy look like most of it. Cutting them
   means drawing the per-object work without leaving the pass (several
   objects into one buffer, or the composite folded into the object's
   own draws), or the blur in one pass with a shader that reads the strip
   it needs from the snapshot before. Neither is a small change.
2. **The effect buffers at the scene's scale.** The 192x192 blur and bloom
   buffers are 1536x1536 at 8x, larger than the PS2's frame. At a scale of
   2 they would cost a sixteenth in fill and copies. The backend keeps
   every target at one scale, and these buffers alias others' pages, so
   it is a change to the target cache (GS-PERFORMANCE.md candidates 1d
   and 2).
3. **Shadow volumes, 1.1 ms.** 287 draws a frame, untextured, counted by
   alternate add and subtract blends; they cannot share draws without
   reordering a clamped count. Fill bound.
4. **The CPU limit at 8x, about 75 fps**, with every draw skipped. Not the
   limit today.

The alpha replays (dual-source now) were the one large cost that was a
pure duplicate; the rest is work the frame does once.
