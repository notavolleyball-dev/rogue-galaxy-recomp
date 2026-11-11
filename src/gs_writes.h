#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// M6 of docs/NATIVE-ONLY.md: what is left of the GS packets the game still
// sends (draw environments, a mesh's state packets, uploads' registers) is
// register state. This reads a packet's GIF tags as the register writes
// they make, in order, without the GS's vertex queue: PACKED (A+D, and the
// packed forms of PRIM, RGBAQ, ST, UV, XYZF2, XYZ2, FOG, the context
// registers), REGLIST, and PRE's PRIM. A vertex's position is a write of
// XYZ2/XYZF2 (a kick) or XYZ3/XYZF3 like any other; IMAGE data is skipped
// and noted. Nothing is drawn from it.
namespace rg::gswrites
{

using Write = std::pair<uint8_t, uint64_t>;

struct Read
{
    bool whole = true;     // every tag's data inside the bytes
    bool image = false;    // IMAGE data among them
    bool vertices = false; // a position written (XYZ2, XYZF2, XYZ3, XYZF3)
    bool kicks = false;    // one that draws (XYZ2, XYZF2)
};

constexpr uint8_t kPrim = 0x00u, kRgbaq = 0x01u, kSt = 0x02u, kUv = 0x03u, kXyzf2 = 0x04u, kXyz2 = 0x05u,
                  kFog = 0x0Au, kXyzf3 = 0x0Cu, kXyz3 = 0x0Du;

// Appends the writes of every GIF tag in `data` (past an EOP: the GS reads
// on, as a GsPkt block's draw environment follows its primitives' EOP).
Read read(const uint8_t *data, uint32_t size, std::vector<Write> &out);

// Writes as one A+D packet (PACKED, NLOOP of them, EOP), for what still
// takes packets (the upload watcher).
std::vector<uint8_t> packet(const std::vector<Write> &writes);

} // namespace rg::gswrites
