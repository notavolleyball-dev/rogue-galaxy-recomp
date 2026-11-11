#pragma once

#include <cstdint>

// The hashes PCSX2 names a texture's replacement by (TEX0HASH-CLUTHASH-BITS),
// made from what the native producer decodes: the texture's indices, a byte
// each in rows, and its palette's colours (docs/TEXTURE_PACKS.md). They were
// made from GS memory by the GS backend's texture cache until M1 of
// docs/NATIVE-ONLY.md; the same bytes are hashed here without one.
namespace rg::pcsx2
{

// Whether PCSX2 names a texture of this format (the producer's T8 0x13 or
// T4 0x14) and size: indexed, each side a power of two.
bool hashable(uint32_t psm, uint32_t width, uint32_t height);

// XXH3-64 of the texture's texels as PCSX2 hashes them. A T8 texture of at
// least 16x16, or a T4 one of at least 32x16, is hashed as the GS blocks it
// fills sit in memory: block after block, a row of them after another, each
// block's 256 bytes in the GS's order within it. A smaller one is hashed as
// its indices, a byte each, row after row. `indices` holds width * height
// bytes, row after row, an index each (a T4 texture's widened).
uint64_t texelHash(const uint8_t *indices, uint32_t width, uint32_t height, uint32_t psm);

// XXH3-64 of a palette as PCSX2 hashes it: its 16 or 256 entries in index
// order, each RGBA8 as one 32-bit word (alpha the game's, 0x80 opaque).
uint64_t clutHash(const uint8_t *rgba, uint32_t entries);

// PCSX2's BITS: the format in the low 6 bits, then log2 of the width and of
// the height in 4 bits each.
uint32_t nameBits(uint32_t psm, uint32_t width, uint32_t height);

} // namespace rg::pcsx2
