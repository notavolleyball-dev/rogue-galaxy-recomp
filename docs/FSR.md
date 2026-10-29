# FSR: temporal upscaling, and maybe frame generation

Where AMD FidelityFX Super Resolution 3 stands in this port: what it needs
from the renderer, what is built, what is next, and how each step is checked.
Kept up to date as the work goes; the newest state is at the top of each
section. The research behind it is `docs/VU1-RECON.md`; the renderer it plugs
into is `docs/VU1-NATIVE.md` and `docs/GS-AUDIT.md`.

## Goal

The Steam Deck first: 1280x800, RDNA2 (8 CUs, about 1.6 TFLOPS), RADV, either
the native Linux build or the Windows build through Proton (both Vulkan). The
game is 4:3 at 448 lines, so plain 2x (896 lines) is already above the
Deck's screen. FSR's job there is to render the scene at 1x and reconstruct it
at 2x (FSR's "Performance" ratio), downscaled to 800 lines at present: anti-
aliasing and detail for about the cost of 1x. On a desktop the same path
renders at 2x or 3x and reconstructs 4x to 8x.

Frame generation (FSR 3's other half) comes after upscaling works, if at all:
it needs FSR's swapchain proxy where SDL owns the swapchain, the HUD kept out
of the generated frames, and a base frame rate high enough that the added
latency is acceptable.

## Testing without a Deck

The development machine's RX 6700 XT is RDNA2 too, on the same driver
(RADV), so the shaders, wave sizes, formats and driver paths FSR takes are
the Deck's: correctness and image quality can be judged here. The budget is
not: the 6700 XT has about 8 times the Deck's compute and 4 times its memory
bandwidth. Until a Deck is measured:

- `DQ8_GFX_GPU_TIMES=1` (patched SDL, `thirdparty/dq8-gfx/README.md`) gives
  the GPU's milliseconds by label; FSR's passes get labels of their own. The
  Deck's frame is roughly the 6700 XT's GPU time times 8 for compute-bound
  passes and times 4 for bandwidth-bound ones, an estimate to be replaced.
- The 6700 XT can be held near the Deck's clocks (`pp_dpm_sclk` and
  `power1_cap` in `/sys/class/drm/card1/device`, root) for a closer feel;
  its 40 CUs still make it faster.
- The same timers run on a Deck (RADV), natively or through Proton, when one
  is available: 1x and 2x of the field walk first, to know whether FSR is
  needed for 60 fps or only for the picture.

## What FSR's upscaler needs

| Input or step | State | Notes |
|---|---|---|
| Sub-pixel jitter of the scene | built, to retune | `RG_JITTER` (Halton(2,3), up to half a host pixel, scene draws only). FSR wants its own sequence (`ffxFsr3GetJitterOffset`) in render pixels, and told to it each frame |
| Scene depth | built | The depth surface (0x1C00 in the field) is a GPU texture; GS Z grows toward the viewer, so FSR is told the depth is inverted |
| Scene draws told from the rest | built | Each batch carries its VU1 program tag and camera (patches 0019, 0020) |
| The scene's camera, this frame and the last | built | `CameraRegistry` (patch 0020); the busiest camera per present is the scene's |
| Per-object transforms | half | Native meshes reach the GPU as object-space positions and a per-mesh transform (`GsNativeMeshRecord`); nothing keeps last frame's transform for the same mesh yet |
| Motion vectors | not built | See step 1 |
| The scene before the post effects and HUD | not built | See step 3 |
| Reactive and transparency masks | not built | From the draw tags: particles, additive and alpha-blended draws, the per-object composites |
| A way to run FSR | not built | SDL 3.2 exposes no Vulkan handles, which the FidelityFX SDK takes. Either a third SDL patch (device, command buffer, images), or FSR 3.1's upscaler shaders (MIT) compiled to SPIR-V and run as SDL GPU compute passes. See step 4 |
| Texture LOD bias for a jittered, lower-resolution scene | not built | The draw shader picks no mip level today (`texelFetch` at level 0); revisit if textures shimmer |

## Plan

Each step ends with something to look at or a number, before the next.

1. **Motion vectors, with a debug view.**
   - Camera-only vectors for every scene pixel: depth to a world position
     through this frame's camera, back through the last frame's. Exact for
     static geometry (about 75% of draws). First the debug view
     `VU1-RECON.md` asks for: the reconstructed world position as a
     repeating pattern, which stays pinned to surfaces while the camera
     moves if the mapping (XYOFFSET, the 2048 center, quads 6-7's Z scale,
     the backend's depth normalization) is right.
   - Per-object vectors for native meshes: the previous frame's transform
     (and, for skinned meshes, previous positions) found by mesh identity,
     written by `gs_native.vert` into a motion target beside the colour.
     Identity: the mesh-shape hash from the section pointers (quads 103-104)
     that `capture_report.py` pairs across lists. Unmatched draws fall back
     to camera vectors.
   - Check: a view of the vectors as colour, and the reprojected last frame
     laid over this one; static geometry should line up to a fraction of a
     pixel.
2. **A temporal pass of our own (TAA).** Jittered scene, the vectors, a
   history buffer, neighbourhood clamping. Small, no SDK, and the proof that
   jitter, depth and vectors agree before FSR is blamed for anything.
3. **The frame at two scales.** The scene renders at the render scale; at
   the first draw that reads the scene back (the bloom's copy, the
   per-object composites, the HUD), the upscaler runs and the frame target
   carries on at the output scale, so the effects and HUD draw sharp on the
   upscaled frame. Surfaces at different scales already share pages on the
   GPU (`importColorPages` and the snapshot padding blit between scales,
   2026-10-29); the frame changing scale within a present is new.
4. **FSR 3.1 upscaling.** The SDK through a Vulkan-handle SDL patch is the
   shorter road and covers the Deck (Linux and Proton are both Vulkan); the
   shaders ported into SDL GPU compute keep D3D12 and Metal open. Decide
   after step 2, with its numbers.
5. **Frame generation**, if the Deck's base frame rate and latency allow.

## Checks

- **Frame bytes off:** with FSR off, the frame log of the 30 fps walk stays
  the same bytes (`tools/vu1/oracle/native-run.sh`,
  `tools/bench/cmpvram.py`), as for every change to the renderer.
- **Image:** FSR on, side by side with the scene's scale at the output
  resolution, in motion (the walk) and still; ghosting on the party and
  particles, shimmer on the town's textures.
- **Speed:** `native-perf.sh` and `DQ8_GFX_GPU_TIMES` at the render and
  output scales, FSR's passes by label.

## Log

- 2026-10-29: this file. Built before it: jitter and the camera registry
  (`VU1-RECON.md`), every recorded VU1 program native with the scene's
  strips drawn on the GPU from object space (`VU1-NATIVE.md`), the GS
  backend's costs cut to 60 fps at 8x with headroom, GPU timers, and pages
  moved between surfaces of different scales on the GPU (`GS-AUDIT.md`).
