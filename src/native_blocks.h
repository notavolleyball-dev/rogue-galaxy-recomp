#pragma once

#include <cstdint>

class PS2Runtime;

// M4 of docs/NATIVE-ONLY.md: the game's 2D at its source. Every GsPkt block
// the game writes (DrawListBeginPrim to DrawListEndPrim, 126 call sites) is
// read on the game thread when it ends, from the bytes it wrote, under the
// draw state the game thread follows itself (rg::drawstate::Tracker: the
// draw environments' packets and the blocks, in the game's order): its
// primitives become the 2D converter's pieces there, each with its state
// and the running draw task's layer, kept by the unit of the block's packet
// range. The VIF1 thread, reaching the range, sends the pieces to the
// producer in the stream's order (as it does a pass) and the packets on
// with their kicks withheld: the VIF1 thread's converter no longer sees
// the blocks. RG_NATIVE_BLOCKS=0: the VIF1 thread converts them, as before.
namespace rg::nativeblocks
{

// After loadELF(), with the native effects (their ranges carry the units).
void install(PS2Runtime &runtime);
bool enabled();

// The VIF1 thread (or the draw stage), at a block's range: its pieces to the
// producer. False: no pieces kept for `unit` (the converter takes the
// packets).
bool emit(uint32_t unit);

// After the block's packets went on: the VIF1 thread's mirror compared
// with what the game thread's said at the block's end (counted).
void compare(uint32_t unit);

} // namespace rg::nativeblocks
