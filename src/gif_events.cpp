#include "gif_events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rg::gif
{
namespace
{

constexpr uint8_t kPrim = 0x00;
constexpr uint8_t kRgbaq = 0x01;
constexpr uint8_t kSt = 0x02;
constexpr uint8_t kUv = 0x03;
constexpr uint8_t kXyzf2 = 0x04;
constexpr uint8_t kXyz2 = 0x05;
constexpr uint8_t kFog = 0x0A;
constexpr uint8_t kXyzf3 = 0x0C;
constexpr uint8_t kXyz3 = 0x0D;
// PACKED descriptors that are not register addresses.
constexpr uint8_t kAd = 0x0E;
constexpr uint8_t kNop = 0x0F;

float asFloat(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void vertex(GifState &state, std::vector<GifEvent> &out, bool drawing, bool fog, uint16_t x, uint16_t y, uint32_t z,
            uint8_t f)
{
    if (fog)
    {
        state.f = f;
    }
    GifEvent event;
    event.kind = GifEvent::Kind::Vertex;
    event.drawing = drawing;
    event.fog = fog;
    event.prim = state.prim;
    event.x = x;
    event.y = y;
    event.z = z;
    event.f = state.f;
    std::memcpy(event.rgba, state.rgba, sizeof(event.rgba));
    event.q = state.q;
    event.s = state.s;
    event.t = state.t;
    event.u = state.u;
    event.v = state.v;
    out.push_back(event);
}

// A register write in its 64-bit form (REGLIST, A+D, and PACKED descriptors
// without a packed format of their own).
void writeRegister(uint8_t reg, uint64_t value, GifState &state, std::vector<GifEvent> &out)
{
    switch (reg)
    {
    case kPrim:
        state.prim = static_cast<uint16_t>(value & 0x7FFu);
        break;
    case kRgbaq:
        for (int i = 0; i < 4; ++i)
        {
            state.rgba[i] = static_cast<uint8_t>(value >> (8 * i));
        }
        state.q = asFloat(static_cast<uint32_t>(value >> 32));
        return;
    case kSt:
        state.s = asFloat(static_cast<uint32_t>(value));
        state.t = asFloat(static_cast<uint32_t>(value >> 32));
        return;
    case kUv:
        state.u = static_cast<uint16_t>(value & 0x3FFFu);
        state.v = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        return;
    case kXyzf2:
    case kXyzf3:
        vertex(state, out, reg == kXyzf2, true, static_cast<uint16_t>(value), static_cast<uint16_t>(value >> 16),
               static_cast<uint32_t>((value >> 32) & 0xFFFFFFu), static_cast<uint8_t>(value >> 56));
        return;
    case kXyz2:
    case kXyz3:
        vertex(state, out, reg == kXyz2, false, static_cast<uint16_t>(value), static_cast<uint16_t>(value >> 16),
               static_cast<uint32_t>(value >> 32), 0u);
        return;
    case kFog:
        state.f = static_cast<uint8_t>(value >> 56);
        return;
    default:
        break;
    }
    GifEvent event;
    event.reg = reg;
    event.value = value;
    out.push_back(event);
}

void writePacked(uint8_t descriptor, const uint8_t *qword, GifState &state, std::vector<GifEvent> &out)
{
    uint32_t w[4];
    uint64_t lo;
    std::memcpy(w, qword, sizeof(w));
    std::memcpy(&lo, qword, sizeof(lo));
    switch (descriptor)
    {
    case kRgbaq:
        for (int i = 0; i < 4; ++i)
        {
            state.rgba[i] = static_cast<uint8_t>(w[i]);
        }
        return;
    case kSt:
        state.s = asFloat(w[0]);
        state.t = asFloat(w[1]);
        state.q = asFloat(w[2]);
        return;
    case kUv:
        state.u = static_cast<uint16_t>(w[0] & 0x3FFFu);
        state.v = static_cast<uint16_t>(w[1] & 0x3FFFu);
        return;
    case kXyzf2:
    {
        const bool adc = (w[3] >> 15) & 1u;
        vertex(state, out, !adc, true, static_cast<uint16_t>(w[0]), static_cast<uint16_t>(w[1]),
               (w[2] >> 4) & 0xFFFFFFu, static_cast<uint8_t>(w[3] >> 4));
        return;
    }
    case kXyz2:
    {
        const bool adc = (w[3] >> 15) & 1u;
        vertex(state, out, !adc, false, static_cast<uint16_t>(w[0]), static_cast<uint16_t>(w[1]), w[2], 0u);
        return;
    }
    case kFog:
        state.f = static_cast<uint8_t>(w[3] >> 4);
        return;
    case kAd:
        writeRegister(qword[8], lo, state, out);
        return;
    case kNop:
        return;
    default:
        // PRIM and the rest: the lower 64 bits to the register itself.
        writeRegister(descriptor, lo, state, out);
        return;
    }
}

} // namespace

GifDecodeStats decodeGifPacket(const uint8_t *data, uint32_t size, GifState &state, std::vector<GifEvent> &out)
{
    GifDecodeStats stats;
    uint32_t at = 0u;
    while (at + 16u <= size)
    {
        uint64_t tag, regs;
        std::memcpy(&tag, data + at, sizeof(tag));
        std::memcpy(&regs, data + at + 8u, sizeof(regs));
        at += 16u;
        ++stats.tags;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const bool eop = (tag >> 15) & 1u;
        const bool pre = (tag >> 46) & 1u;
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        if (nreg == 0u)
        {
            nreg = 16u;
        }
        if (flg == 0u)
        {
            if (pre)
            {
                writeRegister(kPrim, (tag >> 47) & 0x7FFu, state, out);
            }
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (at + 16u > size)
                    {
                        stats.truncated = true;
                        return stats;
                    }
                    writePacked(static_cast<uint8_t>((regs >> (4u * r)) & 0xFu), data + at, state, out);
                    at += 16u;
                }
            }
        }
        else if (flg == 1u)
        {
            const uint64_t words = static_cast<uint64_t>(nloop) * nreg;
            for (uint64_t i = 0; i < words; ++i)
            {
                if (at + 8u > size)
                {
                    stats.truncated = true;
                    return stats;
                }
                uint64_t value;
                std::memcpy(&value, data + at, sizeof(value));
                const uint8_t reg = static_cast<uint8_t>((regs >> (4u * (i % nreg))) & 0xFu);
                if (reg != kNop && reg != kAd)
                {
                    writeRegister(reg, value, state, out);
                }
                at += 8u;
            }
            at = (at + 15u) & ~15u;
        }
        else
        {
            stats.imageQwords += nloop;
            at += nloop * 16u;
        }
        if (eop)
        {
            break;
        }
    }
    if (at > size)
    {
        stats.truncated = true;
    }
    return stats;
}


} // namespace rg::gif
