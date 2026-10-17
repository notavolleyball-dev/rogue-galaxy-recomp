# Patches to pinned dependencies

`ps2recomp/` holds changes to the framework in `thirdparty/PS2Recomp`. The
submodule stays at its upstream commit; configuring the game applies each
patch here to its checkout, in name order, and skips one that is already
applied (`git apply --reverse --check`). To undo them, run
`git -C thirdparty/PS2Recomp checkout -- .`.

| Patch | Why |
|---|---|
| `0001-dma-start-observer.patch` | `PS2Memory::setDmaStartObserver`: a callback before any DMA transfer runs. The VIF1 thread (`src/vif1_thread.cpp`) uses it to order transfers the game starts through the registers behind the VIF1 lists it is still running. |
| `0002-gs-release-prepared-frames.patch` | The GS worker drops its reference to each prepared presentation as soon as it is ready. It kept them until the end of its batch, and each holds one of the SDL backend's three presentation slots: a batch with four presentations, which flip pacing queues when the worker falls behind on light frames (a loading screen), waited on itself forever. |
| `0003-ee-guest-rounding.patch` | Guest code runs with the EE's floating point mode: results rounded toward zero, denormals as zero (PCSX2's default, MXCSR 0xFFC0). Under round-to-nearest, code that rounds with the 1.5 * 2^23 trick gets the next integer: Rogue Galaxy's angle wrap adds a full turn to small angles, and characters in battle swing back and forth around their target every frame. `PS2X_EE_ROUNDING=nearest` keeps the host's mode. |

A new patch is the output of `git -C thirdparty/PS2Recomp diff` for the
files it changes, with the earlier patches applied.
