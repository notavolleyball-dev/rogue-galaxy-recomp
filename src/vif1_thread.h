#pragma once

class PS2Runtime;

// Runs the game's VIF1 DMA lists -- the VU1 microprograms and the GS packets
// they kick -- on a thread of their own, beside the game, as the PS2 runs VU1
// beside the EE.
//
// Rogue Galaxy builds a frame's display list, starts it on VIF1 at the end of
// GfxEndFrame and only waits for it at the start of the next GfxEndFrame, so
// on the console VU1 and the GS draw one frame while the EE computes the
// next. The runtime instead runs the whole list inside sceDmaSend, on the
// game's thread; in heavy scenes that is two thirds of the thread's time.
//
// Here sceDmaSend to VIF1 copies the DMA chain and hands it over. The list's
// closing VIFcode interrupt still reaches the game's own handler, which
// timestamps it (its frame-time measure) and signals the semaphore the next
// GfxEndFrame waits on. Everything else that reaches the GS or VIF1 from the
// game's thread first waits for the lists in flight: the GS functions bound
// to runtime stubs, DMA started through sceDmaSend, and DMA started through
// the registers (via a framework patch, see patches/README.md).
//
// RG_VIF1_THREAD=0 runs the lists on the game's thread, as before.
namespace rg::vif1
{

// After loadELF(): hooks the game's libdma and libgraph entry points.
void install(PS2Runtime &runtime);

// After PS2Runtime::run() returns, before the runtime goes: runs the lists
// still queued and ends the thread.
void shutdown();

// Waits for the lists in flight, from the game's thread. For tools that
// cut the GS stream at a frame boundary (RG_GS_FRAMELOG): the list GfxEndFrame
// started is otherwise still running at the flip, and where it is cut would
// depend on thread timing. A no-op on the VIF1 thread or without it.
void drainForSnapshot();

} // namespace rg::vif1
