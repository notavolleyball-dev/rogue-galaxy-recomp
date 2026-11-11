#include "native_gspkt.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "native_effects.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::gspkt
{
namespace
{
uint8_t *g_rdram = nullptr;

constexpr uint32_t kDrawCtxPointer = 0x004810A4u;
constexpr uint32_t kBuilder = 0x130u;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}
uint32_t read32(uint32_t address)
{
    uint32_t v;
    std::memcpy(&v, g_rdram + physical(address), sizeof(v));
    return v;
}
uint64_t read64(uint32_t address)
{
    uint64_t v;
    std::memcpy(&v, g_rdram + physical(address), sizeof(v));
    return v;
}
float readFloat(uint32_t address)
{
    float v;
    std::memcpy(&v, g_rdram + physical(address), sizeof(v));
    return v;
}
uint64_t reg64(const R5900Context *ctx, int n)
{
    uint64_t v;
    std::memcpy(&v, &ctx->r[n], sizeof(v));
    return v;
}
int64_t sext32(uint32_t v)
{
    return static_cast<int64_t>(static_cast<int32_t>(v));
}
uint32_t floatBits(float f)
{
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    return v;
}
// The VU's float to int conversions (VFTOI0/VFTOI4): truncated, saturated.
int32_t ftoi(float f, float scale)
{
    const double v = std::trunc(double(f) * scale);
    if (!(v == v))
        return 0;
    if (v >= 2147483647.0)
        return std::numeric_limits<int32_t>::max();
    if (v <= -2147483648.0)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(v);
}

// The block's shadow (game thread). Blocks nest (TexAnime's inside
// another's): each its own, the outer one spoiled.
struct Shadow
{
    std::vector<uint8_t> bytes;
    bool broken = false;
    size_t tagAt = SIZE_MAX;    // the open GIF tag (BeginPrimAD's or OpenGifTag's)
    size_t headerAt = SIZE_MAX; // the last GsPktOpen's header tag
    bool packed = false;
    uint32_t nreg = 1u;
    bool skip = false;
};
std::vector<Shadow> g_shadows;

uint32_t builder()
{
    const uint32_t ctx = read32(kDrawCtxPointer);
    return ctx != 0u ? ctx + kBuilder : 0u;
}

// The shadow a builder call writes into: inside a block, on its builder.
Shadow *shadowFor(const R5900Context *ctx)
{
    if (g_shadows.empty() || g_shadows.back().broken)
        return nullptr;
    const uint32_t b = builder();
    if (b == 0u || getRegU32(ctx, 4) != b)
        return nullptr;
    return &g_shadows.back();
}

void write32(uint32_t address, uint32_t value)
{
    std::memcpy(g_rdram + physical(address), &value, sizeof(value));
}

void putQword(Shadow &s, uint64_t lo, uint64_t hi)
{
    const size_t at = s.bytes.size();
    s.bytes.resize(at + 16u);
    std::memcpy(s.bytes.data() + at, &lo, 8u);
    std::memcpy(s.bytes.data() + at + 8u, &hi, 8u);
}
void putAD(Shadow &s, uint64_t data, uint64_t reg)
{
    putQword(s, data, reg);
}
void setTagLoop(Shadow &s, size_t tagAt, uint32_t nloop)
{
    uint32_t w0;
    std::memcpy(&w0, s.bytes.data() + tagAt, 4u);
    w0 = nloop | 0x8000u;
    std::memcpy(s.bytes.data() + tagAt, &w0, 4u);
}

// XYZ2 as the A+D setters make it: x, y with the builder's offsets, no
// field masked (sign-extended and ORed, as the game's code does).
uint64_t xyz(uint32_t b, int32_t x, int32_t y, int32_t z)
{
    const int64_t px = sext32(static_cast<uint32_t>(x) + read32(b + 0x120u));
    const int64_t py = sext32(static_cast<uint32_t>(y) + read32(b + 0x124u));
    return static_cast<uint64_t>(px | (py << 16) | (int64_t(z) << 32));
}
uint64_t rgbaq(uint32_t b, int32_t r, int32_t g, int32_t bl, int32_t a)
{
    const uint64_t q = read32(b + 0x110u);
    return static_cast<uint64_t>(int64_t(r) | (int64_t(g) << 8) | (int64_t(bl) << 16) | (int64_t(a) << 24)) |
           (q << 32);
}

// --- the hooks ------------------------------------------------------------

#define RG_GSPKT_HOOKS(X)                                                                                          \
    X(Open, 0x0013F4A0u)                                                                                           \
    X(BeginPrimAD, 0x0013F6B0u)                                                                                    \
    X(OpenGifTag, 0x0013F3B0u)                                                                                     \
    X(FinalizeTags, 0x0013F260u)                                                                                   \
    X(Close, 0x0013F5B0u)                                                                                          \
    X(PushQword, 0x0013F170u)                                                                                      \
    X(WriteAD, 0x0013EC80u)                                                                                        \
    X(SetTexa, 0x0013ECA0u)                                                                                        \
    X(SetTexture, 0x0013EC00u)                                                                                     \
    X(SetST, 0x0013ED10u)                                                                                          \
    X(SetSTVec, 0x0013ECC0u)                                                                                       \
    X(SetUV, 0x0013ED60u)                                                                                          \
    X(SetUVFixed, 0x0013ED90u)                                                                                     \
    X(SetRGBAQ, 0x0013EE40u)                                                                                       \
    X(SetRGBAQVec, 0x0013EDD0u)                                                                                    \
    X(SetXYZ, 0x0013EF10u)                                                                                         \
    X(SetXYZVecI, 0x0013EEB0u)                                                                                     \
    X(SetXYZPixel, 0x0013F110u)                                                                                    \
    X(SetXYZPixelVec, 0x0013EF70u)                                                                                 \
    X(SetXYZf, 0x0013F060u)                                                                                        \
    X(SetXYZfVec, 0x0013EFE0u)                                                                                     \
    X(DrawSprite, 0x0013EA30u)                                                                                     \
    X(AddTexSprite, 0x0013E520u)                                                                                   \
    X(ReserveQwords, 0x0013F230u)                                                                                  \
    X(AddDmaCallTag, 0x0013F1F0u)

#define RG_GSPKT_ORIGINAL(name, address) PS2Runtime::RecompiledFunction g_##name = nullptr;
RG_GSPKT_HOOKS(RG_GSPKT_ORIGINAL)
#undef RG_GSPKT_ORIGINAL

// GsPktOpen: the segment's header (TEXFLUSH, PRMODECONT 1) and the env's
// GIF tag and five A+D qwords, as the builder holds them now.
void hookOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t b = getRegU32(ctx, 4);
        if (read32(b) == 0u)
            s->broken = true; // no memstack: the setters write into an old packet
        else if (s->skip && rg::nativefx::inEffectCall())
            s->broken = true; // an effect's segments inside a skipped block: written (its range, its pass)
        else
        {
            s->headerAt = s->bytes.size();
            s->tagAt = SIZE_MAX;
            putQword(*s, 0x1000000000008002ull, 0xEull);
            putAD(*s, 0u, 0x3Fu);
            putAD(*s, 1u, 0x1Au);
            const size_t at = s->bytes.size();
            s->bytes.resize(at + 6u * 16u);
            std::memcpy(s->bytes.data() + at, g_rdram + physical(b + 0x10u), 6u * 16u);
            if (s->skip)
            {
                // A segment over scratch: the setters write above the
                // memstack's top, Close and Commit allocate nothing (+0xE8
                // 1), the block's start (+0xF0) stays 0.
                const uint32_t stack = read32(b);
                const uint32_t top = read32(stack + 0x28u) + read32(stack + 0x2Cu) * 16u;
                write32(b + 0xE8u, 1u);
                write32(b + 0xECu, top);
                write32(b + 0xF4u, top);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
    }
    g_Open(rdram, ctx, runtime);
}

// GsPktBeginPrimAD: an A+D tag (its NLOOP patched later) and PRIM.
void hookBeginPrimAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t b = getRegU32(ctx, 4);
        s->tagAt = s->bytes.size();
        s->packed = false;
        putQword(*s, 0x1000000000008000ull, 0xEull);
        putAD(*s, read64(b + 0x70u), 0x00u);
    }
    g_BeginPrimAD(rdram, ctx, runtime);
}

// GsPktOpenGifTag(b, type, regsLo, regsHi, nreg): a PACKED tag, PRE with
// the PRIM image (its type the argument's).
void hookOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t b = getRegU32(ctx, 4);
        const uint32_t type = getRegU32(ctx, 5) & 7u;
        const uint32_t prim = static_cast<uint32_t>(((read64(b + 0x70u) & ~7ull) | type) & 0x7FFu);
        const uint32_t nreg = getRegU32(ctx, 8);
        s->tagAt = s->bytes.size();
        s->packed = true;
        s->nreg = nreg;
        const uint32_t w[4] = {0x8000u, (nreg << 28) | (prim << 15) | 0x4000u, getRegU32(ctx, 6), getRegU32(ctx, 7)};
        const size_t at = s->bytes.size();
        s->bytes.resize(at + 16u);
        std::memcpy(s->bytes.data() + at, w, 16u);
    }
    g_OpenGifTag(rdram, ctx, runtime);
}

// GsPktFinalizeTags: the open tag's NLOOP (PACKED: qwords / NREG).
void hookFinalizeTags(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        if (s->tagAt == SIZE_MAX || s->tagAt >= s->bytes.size())
            s->broken = true; // it would patch the header's tag, or a stale one
        else
        {
            const uint32_t gif = static_cast<uint32_t>((s->bytes.size() - s->tagAt) / 16u) - 1u;
            setTagLoop(*s, s->tagAt, s->packed && s->nreg != 0u ? gif / s->nreg : gif);
        }
    }
    g_FinalizeTags(rdram, ctx, runtime);
}

// GsPktClose: TEXFLUSH, then the A+D NLOOP (whatever the tag is).
void hookClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t b = getRegU32(ctx, 4);
        // (A skipped block's segment has +0xE8 1, but closes as written.)
        if (s->skip || read32(b + 0xE8u) == 0u)
        {
            if (s->tagAt == SIZE_MAX || s->tagAt >= s->bytes.size() || s->packed)
                s->broken = true;
            else
            {
                putAD(*s, 0u, 0x3Fu);
                const uint32_t gif = static_cast<uint32_t>((s->bytes.size() - s->tagAt) / 16u) - 1u;
                setTagLoop(*s, s->tagAt, gif);
            }
        }
    }
    g_Close(rdram, ctx, runtime);
}

void hookPushQword(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const size_t at = s->bytes.size();
        s->bytes.resize(at + 16u);
        std::memcpy(s->bytes.data() + at, g_rdram + physical(getRegU32(ctx, 5) & ~15u), 16u);
    }
    g_PushQword(rdram, ctx, runtime);
}

void hookWriteAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s, reg64(ctx, 6), reg64(ctx, 5));
    g_WriteAD(rdram, ctx, runtime);
}

void hookSetTexa(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s, reg64(ctx, 5), 0x3Bu);
    g_SetTexa(rdram, ctx, runtime);
}

// GsPktSetTexture(b, node): TEXFLUSH, TEX1, TEX0, CLAMP from the builder's
// copy of the node once the call made it (its filter applied): filled in
// after the call.
void afterSetTexture(uint8_t *, R5900Context *, PS2Runtime *, uint32_t at)
{
    if (g_shadows.empty() || g_shadows.back().broken)
        return;
    Shadow &s = g_shadows.back();
    const uint32_t b = builder();
    if (b == 0u || at + 64u > s.bytes.size())
        return;
    const uint64_t qwords[8] = {0u, 0x3Fu, read64(b + 0xB8u), 0x14u, read64(b + 0xB0u), 0x06u, read64(b + 0xC0u), 0x08u};
    std::memcpy(s.bytes.data() + at, qwords, sizeof(qwords));
}
void hookSetTexture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    Shadow *s = shadowFor(ctx);
    if (!s || getRegU32(ctx, 5) == 0u)
    {
        g_SetTexture(rdram, ctx, runtime);
        return;
    }
    const uint32_t at = static_cast<uint32_t>(s->bytes.size());
    s->bytes.resize(at + 64u, 0u);
    rg::hooks::callThen(g_SetTexture, rdram, ctx, runtime, &afterSetTexture, at);
}

void hookSetST(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s, uint64_t(floatBits(ctx->f[12])) | (uint64_t(floatBits(ctx->f[13])) << 32), 0x02u);
    g_SetST(rdram, ctx, runtime);
}

void hookSetSTVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t v = getRegU32(ctx, 5);
        putAD(*s, uint64_t(read32(v)) | (uint64_t(read32(v + 4u)) << 32), 0x02u);
    }
    g_SetSTVec(rdram, ctx, runtime);
}

void hookSetUV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const int64_t u = sext32(getRegU32(ctx, 5) << 4), v = sext32(getRegU32(ctx, 6) << 4);
        putAD(*s, static_cast<uint64_t>(u | (v << 16)), 0x03u);
    }
    g_SetUV(rdram, ctx, runtime);
}

void hookSetUVFixed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const int64_t u = sext32(getRegU32(ctx, 5)), v = sext32(getRegU32(ctx, 6));
        putAD(*s, static_cast<uint64_t>(u | (v << 16)), 0x03u);
    }
    g_SetUVFixed(rdram, ctx, runtime);
}

void hookSetRGBAQ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s,
              rgbaq(getRegU32(ctx, 4), int32_t(getRegU32(ctx, 5)), int32_t(getRegU32(ctx, 6)),
                    int32_t(getRegU32(ctx, 7)), int32_t(getRegU32(ctx, 8))),
              0x01u);
    g_SetRGBAQ(rdram, ctx, runtime);
}

void hookSetRGBAQVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t v = getRegU32(ctx, 5);
        putAD(*s,
              rgbaq(getRegU32(ctx, 4), ftoi(readFloat(v), 1.0f), ftoi(readFloat(v + 4u), 1.0f),
                    ftoi(readFloat(v + 8u), 1.0f), ftoi(readFloat(v + 12u), 1.0f)),
              0x01u);
    }
    g_SetRGBAQVec(rdram, ctx, runtime);
}

void hookSetXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s, xyz(getRegU32(ctx, 4), int32_t(getRegU32(ctx, 5)), int32_t(getRegU32(ctx, 6)),
                      int32_t(getRegU32(ctx, 7))),
              0x05u);
    g_SetXYZ(rdram, ctx, runtime);
}

void hookSetXYZVecI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t v = getRegU32(ctx, 5);
        putAD(*s, xyz(getRegU32(ctx, 4), int32_t(read32(v)), int32_t(read32(v + 4u)), int32_t(read32(v + 8u))),
              0x05u);
    }
    g_SetXYZVecI(rdram, ctx, runtime);
}

void hookSetXYZPixel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s,
              xyz(getRegU32(ctx, 4), int32_t(getRegU32(ctx, 5) << 4), int32_t(getRegU32(ctx, 6) << 4),
                  int32_t(getRegU32(ctx, 7))),
              0x05u);
    g_SetXYZPixel(rdram, ctx, runtime);
}

void hookSetXYZPixelVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t v = getRegU32(ctx, 5);
        putAD(*s,
              xyz(getRegU32(ctx, 4), int32_t(read32(v) << 4), int32_t(read32(v + 4u) << 4), int32_t(read32(v + 8u))),
              0x05u);
    }
    g_SetXYZPixelVec(rdram, ctx, runtime);
}

void hookSetXYZf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
        putAD(*s,
              xyz(getRegU32(ctx, 4), ftoi(ctx->f[12], 16.0f), ftoi(ctx->f[13], 16.0f), ftoi(ctx->f[14], 1.0f)),
              0x05u);
    g_SetXYZf(rdram, ctx, runtime);
}

void hookSetXYZfVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (Shadow *s = shadowFor(ctx))
    {
        const uint32_t v = getRegU32(ctx, 5);
        putAD(*s,
              xyz(getRegU32(ctx, 4), ftoi(readFloat(v), 16.0f), ftoi(readFloat(v + 4u), 16.0f),
                  ftoi(readFloat(v + 8u), 1.0f)),
              0x05u);
    }
    g_SetXYZfVec(rdram, ctx, runtime);
}

// What the shadow does not follow: the sprite composites write their PACKED
// data themselves; raw writes and a DMA call put non-GIF data in the block.
void spoil(const R5900Context *ctx)
{
    if (Shadow *s = shadowFor(ctx))
        s->broken = true;
}
void hookDrawSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    spoil(ctx);
    g_DrawSprite(rdram, ctx, runtime);
}
void hookAddTexSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    spoil(ctx);
    g_AddTexSprite(rdram, ctx, runtime);
}
void hookReserveQwords(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    spoil(ctx);
    g_ReserveQwords(rdram, ctx, runtime);
}
void hookAddDmaCallTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    spoil(ctx);
    g_AddDmaCallTag(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    size_t hooked = 0u, total = 0u;
#define RG_GSPKT_HOOK(name, address)                                                                               \
    ++total;                                                                                                       \
    hooked += rg::timing::hookFunction(runtime, address, &hook##name, g_##name) ? 1u : 0u;
    RG_GSPKT_HOOKS(RG_GSPKT_HOOK)
#undef RG_GSPKT_HOOK
    if (hooked != total)
        std::fprintf(stderr, "[native-gspkt] %zu of %zu builder functions hooked\n", hooked, total);
}

void beginBlock(bool skip)
{
    // An outer block's builder is reset by the inner one's: it is spoiled.
    if (!g_shadows.empty())
        g_shadows.back().broken = true;
    g_shadows.emplace_back();
    g_shadows.back().bytes.reserve(4096u);
    g_shadows.back().skip = skip;
}

bool endBlock(std::vector<uint8_t> &out)
{
    if (g_shadows.empty())
        return false;
    Shadow s = std::move(g_shadows.back());
    g_shadows.pop_back();
    out = std::move(s.bytes);
    return !s.broken;
}

} // namespace rg::gspkt
