# Patches to pinned dependencies

`ps2recomp/` holds changes to the framework in `thirdparty/PS2Recomp`. The
submodule stays at its upstream commit; configuring the game applies each
patch here to its checkout, in name order, and skips one that is already
applied (`git apply --reverse --check`). To undo them, run
`git -C thirdparty/PS2Recomp checkout -- .`.

| Patch | Why |
|---|---|
| `0001-dma-start-observer.patch` | `PS2Memory::setDmaStartObserver`: a callback before any DMA transfer runs. The VIF1 thread (`src/vif1_thread.cpp`) uses it to order transfers the game starts through the registers behind the VIF1 lists it is still running. |

A new patch is the output of `git -C thirdparty/PS2Recomp diff` for the
files it changes, with the earlier patches applied.
