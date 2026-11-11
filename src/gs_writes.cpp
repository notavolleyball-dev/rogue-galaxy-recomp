#include "gs_writes.h"

#include <cstring>

namespace rg::gswrites
{
namespace
{
uint64_t bits(const uint8_t *q, uint32_t from, uint32_t count)
{
    uint64_t lo, hi;
    std::memcpy(&lo, q, 8);
    std::memcpy(&hi, q + 8, 8);
    uint64_t v;
    if (from >= 64u)
        v = hi >> (from - 64u);
    else if (from + count <= 64u)
        v = lo >> from;
    else
        v = (lo >> from) | (hi << (64u - from));
    return count >= 64u ? v : (v & ((1ull << count) - 1ull));
}

void note(Read &r, uint8_t reg)
{
    if (reg == kXyzf2 || reg == kXyz2 || reg == kXyzf3 || reg == kXyz3)
    {
        r.vertices = true;
        r.kicks |= reg == kXyzf2 || reg == kXyz2;
    }
}

// One PACKED qword for register descriptor `desc`.
void packed(const uint8_t *q, uint32_t desc, Read &r, std::vector<Write> &out)
{
    uint8_t reg = 0u;
    uint64_t value = 0u;
    switch (desc)
    {
    case 0x0u: reg = kPrim; value = bits(q, 0, 11); break;
    case 0x1u: // RGBAQ: R, G, B, A from bits 0, 32, 64, 96 (Q the GS's own)
        reg = kRgbaq;
        value = bits(q, 0, 8) | (bits(q, 32, 8) << 8) | (bits(q, 64, 8) << 16) | (bits(q, 96, 8) << 24) |
                (0x3F800000ull << 32);
        break;
    case 0x2u: reg = kSt; value = bits(q, 0, 64); break;
    case 0x3u: reg = kUv; value = bits(q, 0, 14) | (bits(q, 32, 14) << 16); break;
    case 0x4u: // XYZF2 (ADC: XYZF3)
        reg = bits(q, 111, 1) ? kXyzf3 : kXyzf2;
        value = bits(q, 0, 16) | (bits(q, 32, 16) << 16) | (bits(q, 68, 24) << 32) | (bits(q, 100, 8) << 56);
        break;
    case 0x5u: // XYZ2 (ADC: XYZ3)
        reg = bits(q, 111, 1) ? kXyz3 : kXyz2;
        value = bits(q, 0, 16) | (bits(q, 32, 16) << 16) | (bits(q, 64, 32) << 32);
        break;
    case 0xAu: reg = kFog; value = bits(q, 100, 8) << 56; break;
    case 0xCu:
        reg = kXyzf3;
        value = bits(q, 0, 16) | (bits(q, 32, 16) << 16) | (bits(q, 68, 24) << 32) | (bits(q, 100, 8) << 56);
        break;
    case 0xDu:
        reg = kXyz3;
        value = bits(q, 0, 16) | (bits(q, 32, 16) << 16) | (bits(q, 64, 32) << 32);
        break;
    case 0xEu: // A+D
        reg = static_cast<uint8_t>(bits(q, 64, 8));
        value = bits(q, 0, 64);
        break;
    case 0xFu: return; // NOP
    default: // TEX0_1/2, CLAMP_1/2 (6-9): the low 64 bits as the register
        reg = static_cast<uint8_t>(desc);
        value = bits(q, 0, 64);
        break;
    }
    note(r, reg);
    out.emplace_back(reg, value);
}
} // namespace

Read read(const uint8_t *data, uint32_t size, std::vector<Write> &out)
{
    Read r;
    uint32_t at = 0u;
    while (data && at + 16u <= size)
    {
        uint64_t tag, regs;
        std::memcpy(&tag, data + at, 8);
        std::memcpy(&regs, data + at + 8, 8);
        at += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const bool pre = ((tag >> 46) & 1u) != 0u;
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        nreg = nreg == 0u ? 16u : nreg;
        if (pre && flg != 2u)
            out.emplace_back(kPrim, (tag >> 47) & 0x7FFu);
        if (flg == 0u)
        {
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t k = 0; k < nreg; ++k, at += 16u)
                {
                    if (at + 16u > size)
                    {
                        r.whole = false;
                        return r;
                    }
                    packed(data + at, static_cast<uint32_t>((regs >> (4u * k)) & 0xFu), r, out);
                }
        }
        else if (flg == 1u)
        {
            const uint32_t words = nloop * nreg;
            for (uint32_t i = 0; i < words; ++i)
            {
                if (at + 8u * (i + 1u) > size)
                {
                    r.whole = false;
                    return r;
                }
                const uint32_t desc = static_cast<uint32_t>((regs >> (4u * (i % nreg))) & 0xFu);
                if (desc == 0xEu || desc == 0xFu)
                    continue; // (no A+D in REGLIST; NOP)
                uint64_t value;
                std::memcpy(&value, data + at + 8u * i, 8);
                const uint8_t reg = desc == 0xAu ? kFog : static_cast<uint8_t>(desc);
                note(r, reg);
                out.emplace_back(reg, value);
            }
            at += (words * 8u + 15u) & ~15u;
        }
        else
        {
            r.image = true;
            at += nloop * 16u;
            if (at > size)
            {
                r.whole = false;
                return r;
            }
        }
    }
    return r;
}

std::vector<uint8_t> packet(const std::vector<Write> &writes)
{
    std::vector<uint8_t> bytes(16u + writes.size() * 16u);
    const uint64_t tag[2] = {static_cast<uint64_t>(writes.size()) | (1ull << 15) | (1ull << 60), 0xEull};
    std::memcpy(bytes.data(), tag, 16u);
    for (size_t i = 0; i < writes.size(); ++i)
    {
        const uint64_t q[2] = {writes[i].second, writes[i].first};
        std::memcpy(bytes.data() + 16u + i * 16u, q, 16u);
    }
    return bytes;
}

} // namespace rg::gswrites
