# Patches to pinned dependencies

`ps2recomp/` holds changes to the framework in `thirdparty/PS2Recomp`. The
submodule stays at its upstream commit; configuring the game applies each
patch here to its checkout, in name order, and skips one that is already
applied (`git apply --reverse --check`). To undo them, run
`git -C thirdparty/PS2Recomp checkout -- .`.

To change the newest patch, edit the checkout and regenerate it from a
base that holds the earlier ones: a detached `git worktree` of the
submodule at its commit with patches 0001 to N-1 applied and committed,
the modified files copied over it, `git diff` into the patch file. Then
check that every patch still reverse-applies to the checkout: a later
patch that inserts lines inside an earlier patch's context breaks that
earlier patch's check, and the next configure tries to apply it again
and fails (which happened once, 2026-10-19). A build does not
reconfigure by itself, so run `cmake -S . -B build/game` once after.

| Patch | Why |
|---|---|
| `0001-dma-start-observer.patch` | `PS2Memory::setDmaStartObserver`: a callback before any DMA transfer runs. The VIF1 thread (`src/vif1_thread.cpp`) uses it to order transfers the game starts through the registers behind the VIF1 lists it is still running. |
| `0002-gs-release-prepared-frames.patch` | The GS worker drops its reference to each prepared presentation as soon as it is ready. It kept them until the end of its batch, and each holds one of the SDL backend's three presentation slots: a batch with four presentations, which flip pacing queues when the worker falls behind on light frames (a loading screen), waited on itself forever. |
| `0003-ee-guest-rounding.patch` | Guest code runs with the EE's floating point mode: results rounded toward zero, denormals as zero (PCSX2's default, MXCSR 0xFFC0). Under round-to-nearest, code that rounds with the 1.5 * 2^23 trick gets the next integer: Rogue Galaxy's angle wrap adds a full turn to small angles, and characters in battle swing back and forth around their target every frame. `PS2X_EE_ROUNDING=nearest` keeps the host's mode. |
| `0004-mxcsr-rounding-reads.patch` | The GS worker queue read the rounding mode with `std::fegetround` for every command (one per primitive from the VIF1 thread), and the VU1 core switched it with `fesetround` for every MSCAL; MinGW's go through the CRT and touch the x87 control word too, 7% of the VIF1 thread. Both read and set MXCSR directly on x86. |
| `0005-vif1-bulk-xgkick-plain-unpack.patch` | Two cuts to the VIF1 thread. An XGKICK's packet went out a quadword per two simulated cycles even after the program had ended or while it stalled on its next XGKICK, where nothing can write VU memory any more: those two cases now copy the rest at once, at the clock the copy would have reached (`VU1Interpreter::gatherXgkick`, `flushPipelines`, `Run::stall`). And UNPACK's common shapes (no mask, no row add, CL >= WL, 32/16/8-bit) go straight into VU memory instead of through the generic loop that reads the destination back, decodes through lambdas and masks every lane (`unpackPlain`). |
| `0006-vu1-fmac-flags-ee-timer-batch.patch` | Three cuts on the emulation threads. The VU1 flags: `compile_vu_programs.py` decides pair by pair whether a MAC flag reader (FMEQ, FMAND, FMOR) can still see an FMAC's result (`flag_liveness`: a later FMAC that lands before any reader kills it; a call, a register jump or a pair left to the interpreter keeps it; the E bit ends it), and the compiled pair queues the result only then (`Block::flags`, `Frame::issue<..., Needed>`). The status flags are sticky, so an image that reads them (FSEQ, FSAND, FSOR; `VU1Interpreter::fmacFlagsWanted`, from a bit per pair that `PS2Memory::markVU1CodeWritten` keeps as the VIF's MPG writes the code, so no scan of the image after each of the game's code uploads, which was 2% of the VIF1 thread; a write from the EE side rescans once) queues every result, as does `PS2X_VU_FMAC_FLAGS=1`. Rogue Galaxy reads status flags nowhere, and 138 of its 1775 FMAC pairs keep their flags; the map's routine went from 14% to 9% of the VIF1 thread and `commitFlags` from 7% to 3%, byte-identical frames at 1x and 3x. And `PS2Memory::advanceEeTimers`, called at every guest safe point, batches its cycles to 2048 before walking the timers (a compare or overflow interrupt arrives up to 7 us late); the timer register reads and writes settle the batch first, so they see the exact count. And a texture split over DIRECT packets (a GIF IMAGE continuing in the next DIRECT) is handed to the GS in place through `PS2Memory::setGifImageCallback` / `GS::processImageContinuation` instead of being copied whole behind a synthesized IMAGE tag, which was 5% of the VIF1 thread in the streets. |
| `0007-vu-region-counters-extern.patch` | `ps2_vu_detail::regionCounters`, a thread_local with a destructor, is declared `extern` in `ps2_vu1_region.h` and defined once in `ps2_vu1_core.cpp`: as an inline variable every translation unit including the header emitted its TLS initialization wrapper, and lld on COFF refuses the duplicates as soon as the compiled VU blocks' shards (`PS2X_VU_AOT_IMAGES`, 32 of them) include it. Needed to compile the game's VU0 micro programs as blocks. |
| `0008-dispatch-single-lookup.patch` | `PS2Runtime::dispatchGuestBranch` looks the target up in the function table once, for the presence test and the call, instead of `hasFunction` and then `lookupFunction` (which looked again). It runs for every inter-function transfer of the generated code; with File 03's three characters the dispatcher was 7% of the game thread and the second lookup 3%. The dispatch history still records the call. |

A new patch is the output of `git -C thirdparty/PS2Recomp diff` for the
files it changes, with the earlier patches applied.
