#include "pcsx2_hash.h"

#include <array>
#include <vector>

#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

// xxHash's XXH3, the hash PCSX2 names its textures by.
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace rg::pcsx2
{
namespace
{

constexpr uint32_t kT8 = 0x13u, kT4 = 0x14u;

// Where each texel of a block sits within the block's 256 bytes: a byte for
// T8 (16x16 texels), a nibble for T4 (32x16), as the GS lays them out
// (the same in every block of a page).
struct BlockOrder
{
    std::array<uint16_t, 16u * 16u> t8{};
    std::array<uint16_t, 32u * 16u> t4{};
    BlockOrder()
    {
        for (uint32_t y = 0u; y < 16u; ++y)
            for (uint32_t x = 0u; x < 16u; ++x)
                t8[y * 16u + x] = static_cast<uint16_t>(GSPSMT8::addrPSMT8(0u, 2u, x, y) & 0xFFu);
        for (uint32_t y = 0u; y < 16u; ++y)
            for (uint32_t x = 0u; x < 32u; ++x)
                t4[y * 32u + x] = static_cast<uint16_t>(GSPSMT4::addrPSMT4(0u, 2u, x, y) & 0x1FFu);
    }
};

const BlockOrder &blockOrder()
{
    static const BlockOrder order;
    return order;
}

bool powerOfTwo(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

} // namespace

bool hashable(uint32_t psm, uint32_t width, uint32_t height)
{
    psm &= 0x3Fu;
    return (psm == kT8 || psm == kT4) && powerOfTwo(width) && powerOfTwo(height) && width <= 1024u &&
           height <= 1024u;
}

uint64_t texelHash(const uint8_t *indices, uint32_t width, uint32_t height, uint32_t psm)
{
    psm &= 0x3Fu;
    const bool four = psm == kT4;
    const uint32_t blockW = four ? 32u : 16u, blockH = 16u;
    if (width < blockW || height < blockH)
        return XXH3_64bits(indices, static_cast<size_t>(width) * height);
    // Blocks in rows over the texture, as GsVram::appendBlocks read them
    // from GS memory: each of the texture's blocks is whole (the sides are
    // powers of two at least a block's).
    const BlockOrder &order = blockOrder();
    std::vector<uint8_t> blocks(static_cast<size_t>(width / blockW) * (height / blockH) * 256u, 0u);
    size_t block = 0u;
    for (uint32_t by = 0u; by < height; by += blockH)
        for (uint32_t bx = 0u; bx < width; bx += blockW, ++block)
        {
            uint8_t *out = blocks.data() + block * 256u;
            for (uint32_t y = 0u; y < blockH; ++y)
            {
                const uint8_t *row = indices + static_cast<size_t>(by + y) * width + bx;
                for (uint32_t x = 0u; x < blockW; ++x)
                {
                    if (four)
                    {
                        const uint32_t nibble = order.t4[y * 32u + x];
                        out[nibble >> 1u] |= static_cast<uint8_t>((row[x] & 0x0Fu) << ((nibble & 1u) * 4u));
                    }
                    else
                    {
                        out[order.t8[y * 16u + x]] = row[x];
                    }
                }
            }
        }
    return XXH3_64bits(blocks.data(), blocks.size());
}

uint64_t clutHash(const uint8_t *rgba, uint32_t entries)
{
    return XXH3_64bits(rgba, static_cast<size_t>(entries) * 4u);
}

uint32_t nameBits(uint32_t psm, uint32_t width, uint32_t height)
{
    const auto log2 = [](uint32_t v) {
        uint32_t n = 0u;
        while ((1u << n) < v)
            ++n;
        return n;
    };
    return (psm & 0x3Fu) | (log2(width) << 6u) | (log2(height) << 10u);
}

} // namespace rg::pcsx2
