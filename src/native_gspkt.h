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
// The shadow is compared, byte for byte, with what the game wrote, and
// counted in the [native-blocks] line. A block's owner (DrawListBeginPrim's
// return address) whose shadows agreed with its bytes some times, and never
// differed or went unfollowed, has its blocks skipped (beginBlock's
// `skip`): their 2D comes from the shadow, and the game writes no 2D bytes
// for them (src/native_blocks.cpp).
namespace rg::gspkt
{

// After loadELF(), with the native blocks.
void install(PS2Runtime &runtime);

// A block begins (DrawListBeginPrim) and ends (after DrawListEndPrim): its
// shadow, false when the block was written through something it does not
// follow.
// With `skip`, the builder writes no block: each GsPktOpen is replaced by
// a segment opened over scratch above the memstack's top (the setters
// write there, nothing is allocated, GsPktEnd answers 0 so no CALL is
// written), and the shadow is the block's only copy.
void beginBlock(bool skip);
bool endBlock(std::vector<uint8_t> &out);
// The open block has had a GsPktOpen (what is written from now on comes
// after its first segment).
bool opened();

} // namespace rg::gspkt
