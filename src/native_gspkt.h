#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

// M4 of docs/NATIVE-ONLY.md: the 2D blocks from the GsPkt builder's calls.
// Every block (DrawListBeginPrim to DrawListEndPrim) is built through one
// object, the draw context's builder at ctx+0x130 (NATIVE-ONLY-NOTES.md,
// "The GsPkt builder: an exact map"). The builder's framing functions and
// its emitters are hooked, and from their arguments and the builder's
// fields (never from the bytes they write) a shadow of the block's GIF data
// is made on the game thread: the stream the block's DIRECTs carry, in
// order. A block written through anything the shadow does not follow (the
// PACKED sprite composites that write their data themselves, raw writes,
// a DMA call) has no shadow.
//
// Step one (now): the shadow is compared, byte for byte, with what the
// game wrote, and counted in the [native-blocks] line; the block's 2D is
// still read from the game's bytes. Step two, once the count says they
// agree: the 2D from the shadow and the builder's emitters skipped, so the
// game writes no 2D bytes at all.
namespace rg::gspkt
{

// After loadELF(), with the native blocks.
void install(PS2Runtime &runtime);

// A block begins (DrawListBeginPrim) and ends (after DrawListEndPrim): its
// shadow, false when the block was written through something it does not
// follow.
void beginBlock();
bool endBlock(std::vector<uint8_t> &out);

} // namespace rg::gspkt
