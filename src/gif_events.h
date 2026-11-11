#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// What a GS packet does, as a list of events: each register it writes, and
// each vertex it kicks with the vertex state at that moment. M6: the 2D
// builder's decoder for a block's shadow (src/native_2d.h) and nothing
// else; the game's own packets are read as register state
// (src/gs_writes.h).
namespace rg::gif
{

struct GifEvent
{
    enum class Kind : uint8_t
    {
        Register, // a write to a register that is not vertex state
        Vertex,   // XYZ2, XYZF2 (drawing kick) or XYZ3, XYZF3 (no kick)
    };
    Kind kind = Kind::Register;
    // Register: its address and value.
    uint8_t reg = 0;
    uint64_t value = 0;
    // Vertex: whether it kicks a primitive, and the vertex state.
    bool drawing = false;
    bool fog = false; // from XYZF*, with its F
    uint16_t prim = 0;
    uint16_t x = 0, y = 0; // 12.4 fixed point
    uint32_t z = 0;
    uint8_t f = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0; // 10.4 fixed point
};

struct GifDecodeStats
{
    uint32_t tags = 0;
    uint32_t imageQwords = 0;
    bool truncated = false;
};

// The vertex state the GS keeps from one packet to the next.
struct GifState
{
    uint16_t prim = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0;
    uint8_t f = 0;
};

// Appends the events of one packet: a chain of GIF tags up to EOP or the end
// of the bytes. IMAGE data is counted, not decoded.
GifDecodeStats decodeGifPacket(const uint8_t *data, uint32_t size, GifState &state, std::vector<GifEvent> &out);

// The GIF packets in a DIRECT's bytes, each up to its EOP: `visit` gets
// the offset and size of each. The GS reads on past an EOP, and a tag whose
// data runs past the DIRECT's end is cut there (GS::processGIFPacket drops
// the rest; the next DIRECT starts at a tag): the last piece is visited as
// it is, and false says one was cut.
template <typename Visit> bool eachPacket(const uint8_t *data, uint32_t size, Visit visit)
{
    uint32_t start = 0u;
    uint32_t at = 0u;
    while (at + 16u <= size)
    {
        uint64_t tag;
        std::memcpy(&tag, data + at, sizeof(tag));
        at += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        nreg = nreg == 0u ? 16u : nreg;
        if (flg == 0u)
        {
            at += nloop * nreg * 16u;
        }
        else if (flg == 1u)
        {
            at += ((nloop * nreg * 8u) + 15u) & ~15u;
        }
        else
        {
            at += nloop * 16u;
        }
        if (at > size)
        {
            visit(start, size - start);
            return false;
        }
        if ((tag >> 15) & 1u)
        {
            visit(start, at - start);
            start = at;
        }
    }
    if (start < size && at >= size)
    {
        visit(start, size - start);
    }
    return true;
}

} // namespace rg::gif
