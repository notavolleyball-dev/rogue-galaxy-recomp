#include "debug/selftest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if defined(RG_HAS_SDLGPU)
#include "gfx/gs/gs_vram.h"
#endif
#include "runtime/gs/ps2_gs_memory.h"

namespace rg::debug
{
namespace
{

#if defined(RG_HAS_SDLGPU)
// The run writers added to the vendored GsVram against GSMem's own
// per-pixel writes: same bytes for random rectangles, bases and data.
bool vramRunWriters()
{
    GSMem::InitLookupTables();
    constexpr uint32_t kSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> expected(kSize), actual(kSize);
    dq8::gfx::GsVram vram;
    vram.attach(actual.data(), kSize);
    std::mt19937 rng(1234);
    for (int round = 0; round < 400; ++round)
    {
        const bool t4 = (round & 1) != 0;
        for (size_t i = 0; i < kSize; ++i)
        {
            expected[i] = actual[i] = static_cast<uint8_t>(rng());
        }
        const uint32_t bw = 1u + rng() % 16u;
        const uint32_t base = rng() % 0x3000u;
        const uint32_t dsax = rng() % 300u;
        const uint32_t dsay = rng() % 300u;
        const uint32_t rrw = 1u + rng() % 200u;
        const uint32_t rrh = 1u + rng() % 64u;
        // Start part-way into the rectangle, as a later IMAGE payload does.
        const uint32_t total = rrw * rrh;
        uint32_t start = rng() % total;
        if (t4)
        {
            start &= ~1u;
        }
        const uint32_t count = total - start;
        std::vector<uint8_t> src(t4 ? (count + 1u) / 2u : count);
        for (auto &b : src)
        {
            b = static_cast<uint8_t>(rng());
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t pixel = start + i;
            const uint32_t x = dsax + pixel % rrw;
            const uint32_t y = dsay + pixel / rrw;
            if (t4)
            {
                const uint8_t packed = src[i / 2u];
                GSMem::WriteP4(expected.data(), base, bw, x, y, (i & 1u) ? packed >> 4u : packed & 0x0fu);
            }
            else
            {
                GSMem::WriteP8(expected.data(), base, bw, x, y, src[i]);
            }
        }
        const uint32_t x0 = dsax + start % rrw;
        const uint32_t y0 = dsay + start / rrw;
        if (t4)
        {
            vram.writeRunT4(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else
        {
            vram.writeRunT8(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        if (std::memcmp(expected.data(), actual.data(), kSize) != 0)
        {
            std::fprintf(stderr, "[selftest] %s run differs: base=0x%x bw=%u pos=(%u,%u) %ux%u start=%u\n",
                         t4 ? "T4" : "T8", base, bw, dsax, dsay, rrw, rrh, start);
            return false;
        }
    }
    std::printf("[selftest] GsVram T4/T8 run writers match GSMem over 400 rectangles\n");

    // The T4 row reader against GSMem::ReadP4 over the random memory left above.
    std::vector<uint8_t> row(512);
    for (int round = 0; round < 2000; ++round)
    {
        const uint32_t bw = 1u + rng() % 16u;
        const uint32_t base = rng() % 0x3000u;
        const uint32_t x = rng() % 600u, y = rng() % 600u, count = 1u + rng() % 512u;
        vram.readRowT4(base, bw, x, y, count, row.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            if (row[i] != GSMem::ReadP4(actual.data(), base, bw, x + i, y))
            {
                std::fprintf(stderr, "[selftest] T4 row read differs: base=0x%x bw=%u (%u,%u)+%u\n", base, bw, x, y, i);
                return false;
            }
        }
    }
    std::printf("[selftest] GsVram T4 row reader matches GSMem over 2000 rows\n");
    return true;
}
#endif

} // namespace

bool runSelfTestIfRequested(int &status)
{
    const char *which = std::getenv("RG_SELFTEST");
    if (!which || !*which)
    {
        return false;
    }
    bool ok = true;
#if defined(RG_HAS_SDLGPU)
    if (std::string(which) == "gs" || std::string(which) == "all")
    {
        ok = vramRunWriters() && ok;
    }
#endif
    status = ok ? 0 : 1;
    return true;
}

} // namespace rg::debug
