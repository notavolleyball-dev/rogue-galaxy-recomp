#include "debug/selftest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if defined(RG_HAS_SDLGPU)
#include "gfx/gs/gs_transfer.h"
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
        const int mode = round % 4; // T8, T4, CT32, Z32
        const bool t4 = mode == 1;
        const bool wide = mode >= 2;
        const bool z = mode == 3;
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
        std::vector<uint8_t> src(t4 ? (count + 1u) / 2u : wide ? count * 4u : count);
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
            else if (wide)
            {
                uint32_t value = 0u;
                std::memcpy(&value, src.data() + i * 4u, sizeof(value));
                if (z)
                {
                    GSMem::WriteZ32(expected.data(), base, bw, x, y, value);
                }
                else
                {
                    GSMem::WriteCT32(expected.data(), base, bw, x, y, value);
                }
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
        else if (z)
        {
            vram.writeRunZ32(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else if (wide)
        {
            vram.writeRunCT32(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else
        {
            vram.writeRunT8(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        if (std::memcmp(expected.data(), actual.data(), kSize) != 0)
        {
            std::fprintf(stderr, "[selftest] %s run differs: base=0x%x bw=%u pos=(%u,%u) %ux%u start=%u\n",
                         t4 ? "T4" : z ? "Z32" : wide ? "CT32" : "T8", base, bw, dsax, dsay, rrw, rrh, start);
            return false;
        }
    }
    std::printf("[selftest] GsVram T4/T8/CT32/Z32 run writers match GSMem over 400 rectangles\n");

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

#if defined(RG_HAS_SDLGPU)
// The host writes' memory filled by copying each payload's landed pixels
// out of local memory (GsVram::copyLanded) against decoding the payload a
// second time, as GsTextureCache did: same bytes, for random transfers
// split into random payloads, local memory and the store starting apart.
bool storeCopiesUploads()
{
    GSMem::InitLookupTables();
    constexpr uint32_t kSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> local(kSize), decoded(kSize), copied(kSize);
    dq8::gfx::GsVram localVram, decodedVram, copiedVram;
    localVram.attach(local.data(), kSize);
    decodedVram.attach(decoded.data(), kSize);
    copiedVram.attach(copied.data(), kSize);
    dq8::gfx::GsTransferEngine localEngine(localVram), decodedEngine(decodedVram);
    std::mt19937 rng(4321);
    const uint8_t formats[3] = {GS_PSM_CT32, GS_PSM_T8, GS_PSM_T4};
    for (int round = 0; round < 300; ++round)
    {
        for (size_t i = 0; i < kSize; ++i)
        {
            local[i] = static_cast<uint8_t>(rng());
            decoded[i] = copied[i] = static_cast<uint8_t>(rng());
        }
        GSTransferCommand command{};
        command.direction = 0u;
        command.bitbltbuf.dpsm = formats[round % 3];
        command.bitbltbuf.dbw = static_cast<uint8_t>(1u + rng() % 16u);
        command.bitbltbuf.dbp = rng() % 0x3000u;
        // Half the rounds block-aligned, as most textures are.
        const bool aligned = (round & 4) != 0;
        command.trxpos.dsax = static_cast<uint16_t>(aligned ? (rng() % 8u) * 32u : rng() % 300u);
        command.trxpos.dsay = static_cast<uint16_t>(aligned ? (rng() % 8u) * 16u : rng() % 300u);
        command.trxreg.rrw = static_cast<uint16_t>(aligned ? 32u << (rng() % 3u) : 1u + rng() % 200u);
        command.trxreg.rrh = static_cast<uint16_t>(aligned ? 16u << (rng() % 3u) : 1u + rng() % 64u);
        const uint32_t pixels = uint32_t(command.trxreg.rrw) * command.trxreg.rrh;
        const uint32_t bytes = command.bitbltbuf.dpsm == GS_PSM_CT32 ? pixels * 4u
                               : command.bitbltbuf.dpsm == GS_PSM_T8 ? pixels
                                                                      : (pixels + 1u) / 2u;
        std::vector<uint8_t> payload(bytes);
        for (auto &b : payload)
        {
            b = static_cast<uint8_t>(rng());
        }
        localEngine.begin(command);
        decodedEngine.begin(command);
        for (uint32_t offset = 0u; offset < bytes;)
        {
            // Payloads of whole quadwords, as IMAGE carries them, or any size.
            uint32_t size = 1u + rng() % std::max<uint32_t>(bytes / 3u, 1u);
            if (rng() % 2u)
            {
                size = (size + 15u) & ~15u;
            }
            size = std::min(size, bytes - offset);
            const uint32_t before = localEngine.snapshot().copiedPixels;
            localEngine.upload(payload.data() + offset, size);
            decodedEngine.upload(payload.data() + offset, size);
            copiedVram.copyLanded(localVram, command.bitbltbuf.dpsm, command.bitbltbuf.dbp,
                                  std::max<uint32_t>(command.bitbltbuf.dbw, 1u), command.trxpos.dsax,
                                  command.trxpos.dsay, command.trxreg.rrw, before,
                                  localEngine.snapshot().copiedPixels);
            offset += size;
        }
        if (std::memcmp(decoded.data(), copied.data(), kSize) != 0)
        {
            std::fprintf(stderr, "[selftest] store copy differs: psm %02x dbp 0x%x dbw %u %ux%u at (%u,%u)\n",
                         command.bitbltbuf.dpsm, command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.trxreg.rrw,
                         command.trxreg.rrh, command.trxpos.dsax, command.trxpos.dsay);
            return false;
        }
    }
    std::printf("[selftest] host writes' memory copied from local memory matches decoding over 300 transfers\n");
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
        ok = storeCopiesUploads() && ok;
    }
#endif
    status = ok ? 0 : 1;
    return true;
}

} // namespace rg::debug
