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
// is made on the game thread: the stream the block's DIRECTs would carry,
// in order. The game writes no block: its segments are opened over scratch
// and the shadow is the block's only copy (src/native_blocks.cpp makes its
// 2D). A block written through anything the shadow does not follow (the
// PACKED sprite composites that write their data themselves, raw writes,
// a DMA call, no memstack) has no shadow, and is written as the game
// writes it.
namespace rg::gspkt
{

// After loadELF(), with the native blocks.
void install(PS2Runtime &runtime);

// A block begins (GsPktBegin) and ends (GsPktEnd): its shadow, false when
// the block was written through something it does not follow. Each
// GsPktOpen is replaced by a segment opened over scratch above the
// memstack's top (the setters write there, nothing is allocated, GsPktEnd
// answers 0 so no CALL is written).
void beginBlock();
bool endBlock(std::vector<uint8_t> &out);
// The open block has had a GsPktOpen (what is written from now on comes
// after its first segment).
bool opened();

} // namespace rg::gspkt
