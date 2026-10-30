#include "native_effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "gif_events.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::nativefx
{
namespace
{

constexpr uint32_t kGlareFilterDraw = 0x0017A0D0u; // GlareFilterDrawMaybe
constexpr uint32_t kPlaneShadowEnd = 0x00146CC0u;   // DrawListEndPlaneShadowPass
constexpr uint32_t kPlaneShadowBegin = 0x00147B80u; // DrawListBeginPlaneShadowPass
// ModelDrawOffscreenCompositeAtMaybe: the composite's only way in (it jumps
// into ModelDrawOffscreenCompositeMaybe_16aac0 past the function table).
constexpr uint32_t kCompositeAt = 0x0016B8A0u;
constexpr uint32_t kModelEmitDrawPackets = 0x0014EC20u; // ModelEmitDrawPacketsMaybe
constexpr uint32_t kDrawListFrameBegin = 0x001425D0u;

// The draw context (GetDrawCtx_15de80 reads it at gp-0x40cc): the frame's
// buffer index at +0 and its memory stacks at +8, each 8 bytes into the
// pointer there, with its base at +0x28 and the quadwords used at +0x2C.
// Every packet of the frame is written in the stack of the buffer being
// built (src/debug/draw_origins.cpp).
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;

uint8_t *g_rdram = nullptr;
bool g_log = false;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

uint32_t read32(uint32_t address)
{
    uint32_t value = 0u;
    if (g_rdram)
    {
        std::memcpy(&value, g_rdram + (physical(address) & ~3u), sizeof(value));
    }
    return value;
}

// The memory stack of the buffer being built, 0 when there is none.
uint32_t frameStack()
{
    const uint32_t ctx = read32(kDrawCtxPointer);
    if (ctx == 0u)
    {
        return 0u;
    }
    const uint32_t stack = read32(ctx + 8u + read32(ctx) * 4u);
    return stack == 0u ? 0u : stack + 8u;
}

uint32_t stackTop()
{
    const uint32_t stack = frameStack();
    return stack == 0u ? 0u : read32(stack + 0x28u) + read32(stack + 0x2Cu) * 16u;
}

// The packet memory each effect's calls wrote, by start: a few frames'
// worth, the VIF1 thread reading a frame or two behind the game.
struct Range
{
    uint32_t start = 0u;
    uint32_t end = 0u;
    Place place;
};
std::mutex g_mutex;
std::vector<Range> g_ranges;

// The ranges `start`-`end` overlaps go: the memory is something else's now.
void dropRangesLocked(uint32_t start, uint32_t end)
{
    g_ranges.erase(std::remove_if(g_ranges.begin(), g_ranges.end(),
                                  [&](const Range &range) { return range.start < end && start < range.end; }),
                   g_ranges.end());
}

void addRange(uint32_t start, uint32_t end, Effect effect, uint32_t unit = 0u)
{
    start = physical(start);
    end = physical(end);
    if (end <= start || end - start > (256u << 10))
    {
        return;
    }
    std::lock_guard lock(g_mutex);
    dropRangesLocked(start, end);
    const Range range{start, end, Place{effect, unit}};
    g_ranges.insert(std::upper_bound(g_ranges.begin(), g_ranges.end(), range,
                                     [](const Range &a, const Range &b) { return a.start < b.start; }),
                    range);
}

// A frame begins in a buffer: what the effects wrote there before is some
// other packet's now. (The game waits for the lists of a buffer to finish
// before it builds in it again.)
void dropRanges(uint32_t start, uint32_t end)
{
    start = physical(start);
    end = physical(end);
    std::lock_guard lock(g_mutex);
    dropRangesLocked(start, end);
}

PS2Runtime::RecompiledFunction g_frameBeginOriginal = nullptr;

void afterFrameBegin(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    // The stack's base and its capacity in quadwords.
    const uint32_t stack = frameStack();
    if (stack != 0u)
    {
        const uint32_t base = read32(stack + 0x28u);
        dropRanges(base, base + read32(stack + 0x34u) * 16u);
    }
}

void drawListFrameBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_frameBeginOriginal, rdram, ctx, runtime, &afterFrameBegin, 0u);
}

PS2Runtime::RecompiledFunction g_glareOriginal = nullptr;

void afterGlare(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Glare);
}

void glareFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_glareOriginal, rdram, ctx, runtime, &afterGlare, stackTop());
}

PS2Runtime::RecompiledFunction g_shadowOriginal = nullptr;

void afterShadow(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Shadow);
}

void planeShadowEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_shadowOriginal, rdram, ctx, runtime, &afterShadow, stackTop());
}

PS2Runtime::RecompiledFunction g_shadowBeginOriginal = nullptr;

void afterShadowBegin(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::ShadowSetup);
}

void planeShadowBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_shadowBeginOriginal, rdram, ctx, runtime, &afterShadowBegin, stackTop());
}

// Stage E, an object at a time. The composite draws offscreen when its
// settings (a0) have +0x50 and +0x5C, a buffer at +0x20 and a strength at
// +0x24 not negative (docs/NATIVE-RENDERER.md, "Stage E, why offscreen"):
// the packets up to the end of its last ModelEmitDrawPackets are the head,
// the rest the tail. (Game thread only.)
struct Composite
{
    uint32_t unit = 0u;
    uint32_t start = 0u;
    uint32_t headEnd = 0u;
};
Composite g_composite;
uint32_t g_units = 0u;
PS2Runtime::RecompiledFunction g_compositeOriginal = nullptr;
PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;

float readFloat(uint32_t address)
{
    const uint32_t bits = read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The fixed DMA chain each composite's tail calls (GsPktAddDmaCallTag in
// ModelDrawOffscreenCompositeMaybe at 0x16b180): the memory it sends from,
// any tail's (unit 0). Walked once, when the first composite is marked.
constexpr uint32_t kCompositeTailCall = 0x00445C60u;

void markTailCall()
{
    static bool marked = false;
    if (marked)
        return;
    marked = true;
    uint32_t address = kCompositeTailCall;
    std::vector<uint32_t> returns;
    for (int step = 0; step < 256; ++step)
    {
        const uint32_t tag = read32(address);
        const uint32_t qwc = tag & 0xFFFFu;
        const uint32_t id = (tag >> 28) & 7u;
        const uint32_t target = read32(address + 4u) & 0x7FFFFFF0u;
        // The data after the tag (cnt, next, call, ret, end) or at its
        // address (ref, refe, refs).
        if (g_log)
            std::fprintf(stderr, "[nativefx] tail call: tag at %08x id %u qwc %u address %08x\n", address, id, qwc, target);
        if (id == 0u || id == 3u || id == 4u)
            addRange(target, target + qwc * 16u, Effect::CompositeTail, 0u);
        else
            addRange(address, address + 16u + qwc * 16u, Effect::CompositeTail, 0u);
        if (id == 0u || id == 7u)
            break;
        if (id == 1u)
            address += 16u + qwc * 16u;
        else if (id == 2u)
            address = target;
        else if (id == 3u || id == 4u)
            address += 16u;
        else if (id == 5u)
        {
            returns.push_back(address + 16u + qwc * 16u);
            address = target;
        }
        else
        {
            if (returns.empty())
                break;
            address = returns.back();
            returns.pop_back();
        }
    }
}

void afterComposite(uint8_t *, R5900Context *, PS2Runtime *, uint32_t unit)
{
    if (g_composite.unit != unit)
        return;
    markTailCall();
    const uint32_t end = stackTop();
    if (g_composite.headEnd > g_composite.start && g_composite.headEnd < end)
    {
        addRange(g_composite.start, g_composite.headEnd, Effect::CompositeHead, unit);
        addRange(g_composite.headEnd, end, Effect::CompositeTail, unit);
    }
    g_composite = Composite{};
}

void compositeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t settings = getRegU32(ctx, 4);
    const bool offscreen = read32(settings + 0x5Cu) != 0u && read32(settings + 0x50u) != 0u &&
                           read32(settings + 0x20u) != 0u && readFloat(settings + 0x24u) >= 0.0f;
    if (!offscreen || g_composite.unit != 0u)
    {
        g_compositeOriginal(rdram, ctx, runtime);
        return;
    }
    g_units = g_units + 1u == 0u ? 1u : g_units + 1u;
    g_composite = Composite{g_units, stackTop(), 0u};
    rg::hooks::callThen(g_compositeOriginal, rdram, ctx, runtime, &afterComposite, g_units);
}

void afterEmit(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_composite.unit != 0u)
        g_composite.headEnd = stackTop();
}

void modelEmitDrawPackets(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_composite.unit == 0u)
    {
        g_emitOriginal(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_emitOriginal, rdram, ctx, runtime, &afterEmit, 0u);
}

bool g_glare = false;
bool g_shadow = false;
bool g_composites = false;

// --- Packets -----------------------------------------------------------

constexpr uint8_t kXyzf2 = 0x04u, kXyz2 = 0x05u, kXyzf3 = 0x0Cu, kXyz3 = 0x0Du, kAd = 0x0Eu;

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

// The packet with every drawing kick made a plain vertex write: XYZ2 and
// XYZF2 as XYZ3 and XYZF3, PACKED ones with ADC set.
void withholdKicks(std::vector<uint8_t> &bytes)
{
    uint8_t *data = bytes.data();
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    uint32_t at = 0u;
    while (at + 16u <= size)
    {
        uint64_t tag, regs;
        std::memcpy(&tag, data + at, sizeof(tag));
        std::memcpy(&regs, data + at + 8u, sizeof(regs));
        const uint32_t tagAt = at;
        at += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        nreg = nreg == 0u ? 16u : nreg;
        if (flg == 0u)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg && at + 16u <= size; ++r, at += 16u)
                {
                    const uint8_t descriptor = static_cast<uint8_t>((regs >> (4u * r)) & 0xFu);
                    if (descriptor == kXyz2 || descriptor == kXyzf2)
                    {
                        data[at + 13u] |= 0x80u; // ADC, bit 111
                    }
                    else if (descriptor == kAd)
                    {
                        uint8_t &reg = data[at + 8u];
                        reg = reg == kXyz2 ? kXyz3 : (reg == kXyzf2 ? kXyzf3 : reg);
                    }
                }
            }
        }
        else if (flg == 1u)
        {
            for (uint32_t r = 0; r < nreg; ++r)
            {
                const uint64_t shift = 4u * r;
                const uint64_t descriptor = (regs >> shift) & 0xFu;
                const uint64_t replaced = descriptor == kXyz2 ? kXyz3 : (descriptor == kXyzf2 ? kXyzf3 : descriptor);
                regs = (regs & ~(0xFull << shift)) | (replaced << shift);
            }
            std::memcpy(data + tagAt + 8u, &regs, sizeof(regs));
            at += ((nloop * nreg * 8u) + 15u) & ~15u;
        }
        else
        {
            at += nloop * 16u;
        }
    }
}

// --- Sprites -------------------------------------------------------------

// The GS registers the sprites depend on, and whether the effect's packets
// set them (anything before the effect is not seen here).
struct Registers
{
    std::array<uint64_t, 0x64> value{};
    std::array<bool, 0x64> set{};
    void write(uint8_t reg, uint64_t v)
    {
        if (reg < value.size())
        {
            value[reg] = v;
            set[reg] = true;
        }
    }
};

// The VIF1 thread's: what the last shadow setup's packets wrote.
thread_local Registers t_shadowSetup;

constexpr uint8_t kTex0 = 0x06u, kXyoffset = 0x18u, kPrmodecont = 0x1Au, kPrmode = 0x1Bu,
                  kTex1 = 0x14u, kScissor = 0x40u, kAlpha = 0x42u, kDthe = 0x45u, kColclamp = 0x46u,
                  kTest = 0x47u, kPabe = 0x49u, kTexa = 0x3Bu, kFba = 0x4Au, kFrame = 0x4Cu, kZbuf = 0x4Eu;

struct BufferKey
{
    uint32_t base, fbw, psm;
    bool operator==(const BufferKey &other) const
    {
        return base == other.base && fbw == other.fbw && psm == other.psm;
    }
};

struct Builder
{
    GSNativeEffect &out;
    std::string &why;
    std::array<BufferKey, GSNativeEffect::kMaxBuffers> keys{};
    // Per buffer: drawn or read yet, in op order.
    std::array<int, GSNativeEffect::kMaxBuffers> firstWrite{};
    std::array<int, GSNativeEffect::kMaxBuffers> firstRead{};
    std::array<float, GSNativeEffect::kMaxBuffers> readBottom{};
    std::string missing;
    bool failed = false;
    // Per op: how many CT16 strips a kGreenToAlpha op stands for.
    std::array<uint32_t, GSNativeEffect::kMaxOps> strips{};

    bool fail(const char *reason)
    {
        if (!failed)
            why = reason;
        failed = true;
        if (trace)
            std::fprintf(stderr, "[nativefx]   (refused: %s)\n", reason);
        return trace; // tracing goes on past a refusal, to show every sprite
    }

    int buffer(const BufferKey &key)
    {
        for (uint32_t i = 0; i < out.bufferCount; ++i)
        {
            if (keys[i] == key)
            {
                return static_cast<int>(i);
            }
        }
        if (out.bufferCount == GSNativeEffect::kMaxBuffers)
        {
            return -1;
        }
        const uint32_t i = out.bufferCount++;
        keys[i] = key;
        out.buffers[i].base = key.base;
        out.buffers[i].fbw = key.fbw;
        out.buffers[i].psm = key.psm;
        firstWrite[i] = -1;
        firstRead[i] = -1;
        readBottom[i] = 0.0f;
        return static_cast<int>(i);
    }

    // A sprite from v0 to v1 under `r`.
    bool trace = false;
    bool sprite(const Registers &r, uint16_t prim, const rg::gif::GifEvent &v0, const rg::gif::GifEvent &v1)
    {
        const uint32_t ctxt = (prim >> 9) & 1u;
        if (trace)
            std::fprintf(stderr,
                         "[nativefx]   sprite prim %03x frame %llx tex0 %llx tex1 %llx alpha %llx test %llx texa %llx "
                         "xyoff %llx scissor %llx xy %u,%u-%u,%u uv %u,%u-%u,%u rgba %02x%02x%02x%02x\n",
                         prim, (unsigned long long)r.value[kFrame + ctxt], (unsigned long long)r.value[kTex0 + ctxt],
                         (unsigned long long)r.value[kTex1 + ctxt], (unsigned long long)r.value[kAlpha + ctxt],
                         (unsigned long long)r.value[kTest + ctxt], (unsigned long long)r.value[kTexa],
                         (unsigned long long)r.value[kXyoffset + ctxt], (unsigned long long)r.value[kScissor + ctxt],
                         v0.x, v0.y, v1.x, v1.y, v0.u, v0.v, v1.u, v1.v, v1.rgba[0], v1.rgba[1], v1.rgba[2],
                         v1.rgba[3]);
        const bool tme = (prim >> 4) & 1u;
        const bool abe = (prim >> 6) & 1u;
        if ((prim >> 5) & 1u || (prim >> 7) & 1u)
            return fail("a sprite with fog or antialiasing");
        if (tme && !((prim >> 8) & 1u))
            return fail("a sprite with STQ");
        const auto need = [&](uint8_t reg) { return r.set[reg]; };
        if (!need(kFrame + ctxt) || !need(kScissor + ctxt) || !need(kXyoffset + ctxt) || !need(kTest + ctxt) ||
            !need(kZbuf + ctxt))
        {
            missing = "a sprite under a draw environment set before the effect (context " + std::to_string(ctxt) + ":";
            for (uint8_t reg : {kFrame, kScissor, kXyoffset, kTest, kZbuf})
                if (!need(reg + ctxt))
                    missing += " " + std::to_string(reg + ctxt);
            missing += ")";
            return fail(missing.c_str());
        }
        if (tme && (!need(kTex0 + ctxt) || !need(kTex1 + ctxt)))
            return fail("a sprite under a texture set before the effect");
        if (abe && !need(kAlpha + ctxt))
            return fail("a sprite under a blend set before the effect");
        const uint64_t frame = r.value[kFrame + ctxt];
        const uint64_t test = r.value[kTest + ctxt];
        const uint64_t zbuf = r.value[kZbuf + ctxt];
        if (((frame >> 24) & 0x3Fu) == 2u && (frame >> 32) == 0xFFFFu)
            return greenToAlpha(r, prim, ctxt, v0, v1);
        if (((frame >> 24) & 0x3Fu) != 0u || ((frame >> 32) != 0u && (frame >> 32) != 0xFF000000u))
        {
            missing = "a sprite into a target not CT32, or masked (FRAME " + std::to_string(frame) + ")";
            return fail(missing.c_str());
        }
        if ((test >> 14) & 1u)
            return fail("a sprite with the destination alpha test");
        if (((test >> 16) & 1u) && ((test >> 17) & 3u) != 1u)
            return fail("a sprite with a depth test");
        if (((zbuf >> 32) & 1u) == 0u)
            return fail("a sprite that writes depth");
        if ((r.value[kDthe] & 1u) || (r.value[kFba + ctxt] & 1u) || (abe && (r.value[kPabe] & 1u)))
            return fail("a sprite with dithering, FBA or PABE");

        GSNativeEffect::Op op;
        op.flags |= (frame >> 32) != 0u ? GSNativeEffect::kKeepAlpha : 0u;
        const BufferKey target{static_cast<uint32_t>(frame & 0x1FFu) << 5u, static_cast<uint32_t>((frame >> 16) & 0x3Fu),
                               0u};
        const int targetIndex = buffer(target);
        if (targetIndex < 0)
            return fail("more buffers than an effect has");
        op.target = static_cast<uint32_t>(targetIndex);
        if (tme)
        {
            const uint64_t tex0 = r.value[kTex0 + ctxt];
            const uint32_t tfx = static_cast<uint32_t>((tex0 >> 35) & 3u);
            const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
            if (psm == 1u)
            {
                if (!r.set[kTexa])
                    return fail("a CT24 texture under a TEXA set before the effect");
                const uint64_t texa = r.value[kTexa];
                op.flags |= GSNativeEffect::kCt24;
                op.texa = static_cast<uint32_t>(texa & 0xFFu) | static_cast<uint32_t>(texa & 0x8000u);
            }
            if (psm > 1u)
            {
                missing = "a texture not CT32 or CT24 (TEX0 " + std::to_string(tex0) + ", psm " +
                          std::to_string((tex0 >> 20) & 0x3Fu) + ", tfx " + std::to_string(tfx) + ")";
                return fail(missing.c_str());
            }
            const BufferKey source{static_cast<uint32_t>(tex0 & 0x3FFFu), static_cast<uint32_t>((tex0 >> 14) & 0x3Fu),
                                   0u};
            const int sourceIndex = buffer(source);
            if (sourceIndex < 0)
                return fail("more buffers than an effect has");
            op.source = sourceIndex;
            const uint64_t tex1 = r.value[kTex1 + ctxt];
            const uint32_t mmag = static_cast<uint32_t>((tex1 >> 5) & 1u);
            const uint32_t mmin = static_cast<uint32_t>((tex1 >> 6) & 7u);
            op.flags |= (mmag != 0u || mmin == 1u || (mmin & 4u) != 0u) ? GSNativeEffect::kLinear : 0u;
            op.flags |= tfx == 1u ? GSNativeEffect::kDecal
                        : tfx == 2u ? GSNativeEffect::kHighlight
                        : tfx == 3u ? GSNativeEffect::kHighlight2
                                    : 0u;
            op.flags |= ((tex0 >> 34) & 1u) ? GSNativeEffect::kTextureAlpha : 0u;
        }
        if (abe)
        {
            op.flags |= GSNativeEffect::kBlend;
            op.alpha = r.value[kAlpha + ctxt];
        }
        // COLCLAMP is 1 unless the game says otherwise; it never does here.
        op.flags |= (!r.set[kColclamp] || (r.value[kColclamp] & 1u)) ? GSNativeEffect::kColClamp : 0u;
        op.test = test;
        const uint64_t offset = r.value[kXyoffset + ctxt];
        const float ox = static_cast<float>(offset & 0xFFFFu) / 16.0f;
        const float oy = static_cast<float>((offset >> 32) & 0xFFFFu) / 16.0f;
        op.rect[0] = v0.x / 16.0f - ox;
        op.rect[1] = v0.y / 16.0f - oy;
        op.rect[2] = v1.x / 16.0f - ox;
        op.rect[3] = v1.y / 16.0f - oy;
        op.uv[0] = v0.u / 16.0f;
        op.uv[1] = v0.v / 16.0f;
        op.uv[2] = v1.u / 16.0f;
        op.uv[3] = v1.v / 16.0f;
        if (op.rect[2] < op.rect[0])
        {
            std::swap(op.rect[0], op.rect[2]);
            std::swap(op.uv[0], op.uv[2]);
        }
        if (op.rect[3] < op.rect[1])
        {
            std::swap(op.rect[1], op.rect[3]);
            std::swap(op.uv[1], op.uv[3]);
        }
        const uint64_t scissor = r.value[kScissor + ctxt];
        op.scissor[0] = static_cast<uint32_t>(scissor & 0x7FFu);
        op.scissor[1] = static_cast<uint32_t>((scissor >> 32) & 0x7FFu);
        op.scissor[2] = static_cast<uint32_t>((scissor >> 16) & 0x7FFu);
        op.scissor[3] = static_cast<uint32_t>((scissor >> 48) & 0x7FFu);
        std::memcpy(op.rgba, v1.rgba, sizeof(op.rgba));
        return add(op);
    }

    // A strip of the CT16 view of a CT32 buffer (192 x 384 for 192 x 192):
    // in each 16 pixels of a row, the 8 that hold the BA halves of 8 CT32
    // pixels drawn from the 8 that hold their RG halves, the same row, with
    // R and G masked, so CT16's B5 and A1 take G's bits 2-7 into A's. The
    // strips together, every BA half of the buffer, are one kGreenToAlpha.
    bool greenToAlpha(const Registers &r, uint16_t prim, uint32_t ctxt, const rg::gif::GifEvent &v0,
                      const rg::gif::GifEvent &v1)
    {
        const uint64_t frame = r.value[kFrame + ctxt];
        const uint64_t tex0 = r.value[kTex0 + ctxt];
        const uint32_t base = static_cast<uint32_t>(frame & 0x1FFu) << 5u;
        const uint32_t fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
        const bool tme = (prim >> 4) & 1u, abe = (prim >> 6) & 1u;
        if (!tme || abe || static_cast<uint32_t>(tex0 & 0x3FFFu) != base ||
            static_cast<uint32_t>((tex0 >> 14) & 0x3Fu) != fbw || ((tex0 >> 20) & 0x3Fu) != 2u ||
            ((tex0 >> 35) & 3u) > 1u)
            return fail("a CT16 sprite other than a channel copy");
        if (((tex0 >> 35) & 3u) == 0u && (v1.rgba[0] != 0x80u || v1.rgba[1] != 0x80u || v1.rgba[2] != 0x80u))
            return fail("a CT16 channel copy with a colour");
        if (r.value[kTest + ctxt] & 1u)
            return fail("a CT16 channel copy with the alpha test");
        const uint64_t offset = r.value[kXyoffset + ctxt];
        const int32_t ox = static_cast<int32_t>(offset & 0xFFFFu), oy = static_cast<int32_t>((offset >> 32) & 0xFFFFu);
        const int32_t x0 = static_cast<int32_t>(v0.x) - ox, x1 = static_cast<int32_t>(v1.x) - ox;
        const int32_t y0 = static_cast<int32_t>(v0.y) - oy, y1 = static_cast<int32_t>(v1.y) - oy;
        // In 1/16 pixel: 8 wide at the second 8 of 16; the texels the first
        // 8 of the same rows (at their centres).
        if (x1 - x0 != 128 || (x0 & 255) != 128 || y0 != 0 || static_cast<int32_t>(v0.u) != x0 - 128 + 8 ||
            static_cast<int32_t>(v0.v) != y0 + 8 || static_cast<int32_t>(v1.u) != x1 - 128 + 8)
            return fail("a CT16 channel copy of another shape");
        const uint64_t scissor = r.value[kScissor + ctxt];
        const uint32_t rows = static_cast<uint32_t>(y1 / 16);
        if ((scissor & 0x7FFu) != 0u || ((scissor >> 32) & 0x7FFu) != 0u ||
            ((scissor >> 16) & 0x7FFu) + 1u < fbw * 64u || ((scissor >> 48) & 0x7FFu) + 1u < rows ||
            y1 % 16 != 0 || rows % 2u != 0u)
            return fail("a CT16 channel copy under a scissor");
        const int index = buffer(BufferKey{base, fbw, 0u});
        if (index < 0)
            return fail("more buffers than an effect has");
        if (out.opCount != 0u)
        {
            GSNativeEffect::Op &last = out.ops[out.opCount - 1u];
            if ((last.flags & GSNativeEffect::kGreenToAlpha) && last.target == static_cast<uint32_t>(index) &&
                last.rect[3] == static_cast<float>(rows / 2u))
            {
                ++strips[out.opCount - 1u];
                return true;
            }
        }
        GSNativeEffect::Op op;
        op.target = static_cast<uint32_t>(index);
        op.source = index;
        op.flags = GSNativeEffect::kGreenToAlpha;
        op.rect[2] = static_cast<float>(fbw * 64u);
        op.rect[3] = static_cast<float>(rows / 2u);
        op.uv[2] = op.rect[2];
        op.uv[3] = op.rect[3];
        op.scissor[2] = fbw * 64u - 1u;
        op.scissor[3] = rows / 2u - 1u;
        op.test = r.value[kTest + ctxt];
        if (!add(op))
            return false;
        strips[out.opCount - 1u] = 1u;
        return true;
    }

    static bool sameDrawing(const GSNativeEffect::Op &a, const GSNativeEffect::Op &b)
    {
        return a.target == b.target && a.source == b.source && a.flags == b.flags && a.alpha == b.alpha &&
               a.test == b.test && std::memcmp(a.rgba, b.rgba, sizeof(a.rgba)) == 0 &&
               std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0;
    }

    // The next strip of the last sprite joins it: same drawing, the same rows
    // and texture rows, starting where it ended at nearly the same texels a
    // pixel (the strips' UVs are rounded to 1/16 texel each).
    bool join(GSNativeEffect::Op &last, const GSNativeEffect::Op &op)
    {
        if (!sameDrawing(last, op) || last.rect[2] != op.rect[0] || last.rect[1] != op.rect[1] ||
            last.rect[3] != op.rect[3])
            return false;
        if (op.source < 0)
        {
            last.rect[2] = op.rect[2];
            return true;
        }
        if (last.uv[2] != op.uv[0] || last.uv[1] != op.uv[1] || last.uv[3] != op.uv[3])
            return false;
        const float rateLast = (last.uv[2] - last.uv[0]) / (last.rect[2] - last.rect[0]);
        const float rate = (op.uv[2] - op.uv[0]) / (op.rect[2] - op.rect[0]);
        if (std::fabs(rate - rateLast) > 0.02f * std::max(std::fabs(rateLast), 1e-3f))
            return false;
        last.rect[2] = op.rect[2];
        last.uv[2] = op.uv[2];
        return true;
    }

    bool add(const GSNativeEffect::Op &op)
    {
        if (out.opCount != 0u && !(op.flags & GSNativeEffect::kGreenToAlpha) &&
            !(out.ops[out.opCount - 1u].flags & GSNativeEffect::kGreenToAlpha) && join(out.ops[out.opCount - 1u], op))
            return true;
        if (out.opCount == GSNativeEffect::kMaxOps)
            return fail("more sprites than an effect has");
        const int index = static_cast<int>(out.opCount);
        out.ops[out.opCount++] = op;
        if (op.source >= 0)
        {
            int &read = firstRead[op.source];
            read = read < 0 ? index : read;
            readBottom[op.source] = std::max(readBottom[op.source], std::max(op.uv[1], op.uv[3]));
        }
        int &write = firstWrite[op.target];
        write = write < 0 ? index : write;
        return true;
    }

    static bool readsDestination(const GSNativeEffect::Op &op)
    {
        if (!(op.flags & GSNativeEffect::kBlend))
            return false;
        const uint32_t a = op.alpha & 3u, b = (op.alpha >> 2) & 3u, c = (op.alpha >> 4) & 3u,
                       d = (op.alpha >> 6) & 3u;
        return a == 1u || b == 1u || c == 1u || d == 1u;
    }

    // Which buffers are the GS's and the extent of each; and whether the
    // effect's own buffers are wholly drawn before anything reads them, and
    // the GS's drawn only in ways a fixed-function blend can.
    bool finish()
    {
        if (out.opCount == 0u)
            return fail("no sprites");
        for (uint32_t i = 0; i < out.bufferCount; ++i)
        {
            GSNativeEffect::Buffer &buffer = out.buffers[i];
            // The GS's: never drawn here, read first, or first drawn over
            // what it holds.
            const bool external = firstWrite[i] < 0 || (firstRead[i] >= 0 && firstRead[i] <= firstWrite[i]) ||
                                  readsDestination(out.ops[firstWrite[i]]);
            buffer.external = external ? 1u : 0u;
            uint32_t width = 0u, height = 0u;
            for (uint32_t n = 0; n < out.opCount; ++n)
            {
                const GSNativeEffect::Op &op = out.ops[n];
                if (op.target != i)
                    continue;
                width = std::max(width, std::min(op.scissor[2] + 1u, static_cast<uint32_t>(std::ceil(op.rect[2]))));
                height = std::max(height, std::min(op.scissor[3] + 1u, static_cast<uint32_t>(std::ceil(op.rect[3]))));
            }
            if (external)
            {
                buffer.width = std::max(width, buffer.fbw * 64u);
                buffer.height = std::max(height, static_cast<uint32_t>(std::ceil(readBottom[i])));
                buffer.height = std::min(buffer.height, 2048u);
                continue;
            }
            buffer.width = width;
            buffer.height = height;
            const GSNativeEffect::Op &first = out.ops[firstWrite[i]];
            if (readsDestination(first) || first.rect[0] > 0.0f || first.rect[1] > 0.0f || first.scissor[0] != 0u ||
                first.scissor[1] != 0u || std::ceil(first.rect[2]) < static_cast<float>(width) ||
                std::ceil(first.rect[3]) < static_cast<float>(height) || (first.test & 1u))
                return fail("an effect buffer read before it is wholly drawn");
        }
        for (uint32_t n = 0; n < out.opCount; ++n)
        {
            const GSNativeEffect::Op &op = out.ops[n];
            if (op.flags & GSNativeEffect::kGreenToAlpha)
            {
                const GSNativeEffect::Buffer &buffer = out.buffers[op.target];
                if (buffer.external || strips[n] != buffer.fbw * 64u / 16u ||
                    op.rect[3] < static_cast<float>(buffer.height))
                    return fail("a CT16 channel copy of part of a buffer");
                continue;
            }
            if (!out.buffers[op.target].external)
            {
                if (op.flags & GSNativeEffect::kKeepAlpha)
                    return fail("an effect buffer drawn with its alpha masked");
                continue;
            }
            if (op.source >= 0 && out.buffers[op.source].external)
                return fail("a sprite into a GS target from another");
            if (op.test & 1u)
                return fail("a sprite into a GS target with the alpha test");
            if (!(op.flags & GSNativeEffect::kBlend))
                continue;
            // (A - B) C + D with D = Cd and C not Ad: A = Cs and B = 0 (an
            // add), A = 0 and B = Cs (a subtract), A = Cs and B = Cd (laid
            // over, its alpha masked: the pipeline blends by the shader's).
            const uint32_t ab = static_cast<uint32_t>(op.alpha & 0xFu);
            const uint32_t c = static_cast<uint32_t>((op.alpha >> 4) & 3u);
            if (((op.alpha >> 6) & 3u) != 1u || c == 1u || (ab != 0x8u && ab != 0x2u && ab != 0x4u))
                return fail("a sprite into a GS target with a blend it cannot draw");
            if (ab == 0x4u && !(op.flags & GSNativeEffect::kKeepAlpha))
                return fail("a sprite laid over a GS target that writes its alpha");
        }
        return !failed;
    }
};

} // namespace

const char *name(Effect effect)
{
    switch (effect)
    {
    case Effect::Glare: return "glare";
    case Effect::Shadow: return "shadow";
    case Effect::ShadowSetup: return "shadow setup";
    case Effect::CompositeHead: return "composite head";
    case Effect::CompositeTail: return "composite tail";
    default: return "none";
    }
}

bool compositesBatched()
{
    return enabled() && g_composites;
}

bool enabled()
{
    static const bool on = [] {
        const auto flag = [](const char *name) {
            const char *value = std::getenv(name);
            return value && std::strcmp(value, "0") != 0;
        };
        g_log = std::getenv("RG_NATIVE_EFFECTS_LOG") != nullptr;
        g_glare = flag("RG_NATIVE_GLARE");
        g_shadow = flag("RG_NATIVE_SHADOW");
        g_composites = flag("RG_NATIVE_COMPOSITE");
        return g_glare || g_shadow || g_composites;
    }();
    return on;
}

void install(PS2Runtime &runtime)
{
    if (!enabled())
    {
        return;
    }
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kDrawListFrameBegin, &drawListFrameBegin, g_frameBeginOriginal))
    {
        std::fprintf(stderr, "[nativefx] the effects stay GS packets\n");
        return;
    }
    if (g_glare && !rg::timing::hookFunction(runtime, kGlareFilterDraw, &glareFilterDraw, g_glareOriginal))
    {
        std::fprintf(stderr, "[nativefx] the glare stays GS packets\n");
    }
    if (g_shadow && (!rg::timing::hookFunction(runtime, kPlaneShadowBegin, &planeShadowBegin, g_shadowBeginOriginal) ||
                     !rg::timing::hookFunction(runtime, kPlaneShadowEnd, &planeShadowEnd, g_shadowOriginal)))
    {
        std::fprintf(stderr, "[nativefx] the plane shadow stays GS packets\n");
    }
    if (g_composites &&
        (!rg::timing::hookFunction(runtime, kCompositeAt, &compositeAt, g_compositeOriginal) ||
         !rg::timing::hookFunction(runtime, kModelEmitDrawPackets, &modelEmitDrawPackets, g_emitOriginal)))
    {
        g_composites = false;
        std::fprintf(stderr, "[nativefx] the composites stay unbatched\n");
    }
}

Place placeAt(uint32_t address)
{
    address = physical(address);
    std::lock_guard lock(g_mutex);
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), address,
                               [](uint32_t value, const Range &range) { return value < range.start; });
    if (it == g_ranges.begin())
        return Place{};
    --it;
    return address < it->end ? it->place : Place{};
}

Effect effectAt(uint32_t address)
{
    return placeAt(address).effect;
}

bool clearBox(const std::vector<std::vector<uint8_t>> &packets, Box &box)
{
    Registers registers;
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    bool found = false;
    uint32_t queued = 0u;
    rg::gif::GifEvent first;
    for (const std::vector<uint8_t> &packet : packets)
    {
        events.clear();
        eachPacket(packet.data(), static_cast<uint32_t>(packet.size()),
                   [&](uint32_t at, uint32_t size) { rg::gif::decodeGifPacket(packet.data() + at, size, state, events); });
        for (const rg::gif::GifEvent &event : events)
        {
            if (event.kind == rg::gif::GifEvent::Kind::Register)
            {
                if (event.reg == 0x00u)
                    queued = 0u;
                registers.write(event.reg, event.value);
                continue;
            }
            if ((event.prim & 7u) != 6u || ((event.prim >> 4) & 1u))
                continue;
            if (++queued < 2u)
            {
                first = event;
                continue;
            }
            queued = 0u;
            const uint32_t ctxt = (event.prim >> 9) & 1u;
            const uint64_t frame = registers.value[kFrame + ctxt];
            const uint64_t offset = registers.value[kXyoffset + ctxt];
            const uint32_t base = static_cast<uint32_t>(frame & 0x1FFu) << 5u;
            const uint32_t fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
            if (((frame >> 24) & 0x3Fu) != 0u || (found && (base != box.base || fbw != box.fbw)))
                return false;
            const int32_t ox = static_cast<int32_t>(offset & 0xFFFFu), oy = static_cast<int32_t>((offset >> 32) & 0xFFFFu);
            const int32_t x0 = (std::min<int32_t>(first.x, event.x) - ox) >> 4;
            const int32_t y0 = (std::min<int32_t>(first.y, event.y) - oy) >> 4;
            const int32_t x1 = (std::max<int32_t>(first.x, event.x) - ox + 15) >> 4;
            const int32_t y1 = (std::max<int32_t>(first.y, event.y) - oy + 15) >> 4;
            if (!found)
                box = Box{base, fbw, x0, y0, x1, y1};
            box.x0 = std::min(box.x0, x0);
            box.y0 = std::min(box.y0, y0);
            box.x1 = std::max(box.x1, x1);
            box.y1 = std::max(box.y1, y1);
            found = true;
        }
    }
    return found && box.fbw != 0u;
}

void boxPages(const Box &box, uint32_t &first, uint32_t &last)
{
    // CT32 pages are 64 x 32 pixels, fbw to a row.
    const uint32_t y0 = static_cast<uint32_t>(std::max(box.y0, 0)), y1 = static_cast<uint32_t>(std::max(box.y1, 1));
    first = box.base / 32u + (y0 / 32u) * box.fbw;
    last = box.base / 32u + ((y1 - 1u) / 32u) * box.fbw + box.fbw - 1u;
}

bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last)
{
    // An upload's registers come as one A+D packet: BITBLTBUF, TRXPOS,
    // TRXREG, TRXDIR.
    if (size < 32u)
        return false;
    uint64_t tag, regs;
    std::memcpy(&tag, data, sizeof(tag));
    std::memcpy(&regs, data + 8, sizeof(regs));
    if ((tag >> 58) & 3u || (tag >> 60) != 1u || (regs & 0xFu) != kAd)
        return false;
    const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
    uint64_t bitblt = 0u, pos = 0u, reg = 0u;
    bool host = false;
    for (uint32_t i = 0; i < nloop && 32u + i * 16u <= size; ++i)
    {
        uint64_t value, address;
        std::memcpy(&value, data + 16u + i * 16u, sizeof(value));
        std::memcpy(&address, data + 24u + i * 16u, sizeof(address));
        switch (address & 0xFFu)
        {
        case 0x50u: bitblt = value; break;
        case 0x51u: pos = value; break;
        case 0x52u: reg = value; break;
        case 0x53u: host = (value & 3u) == 0u; break;
        default: break;
        }
    }
    if (!host)
        return false;
    const uint32_t dbp = static_cast<uint32_t>((bitblt >> 32) & 0x3FFFu);
    const uint32_t dbw = std::max<uint32_t>(static_cast<uint32_t>((bitblt >> 48) & 0x3Fu), 1u);
    const uint32_t dpsm = static_cast<uint32_t>((bitblt >> 56) & 0x3Fu);
    const uint32_t dsay = static_cast<uint32_t>((pos >> 48) & 0x7FFu);
    const uint32_t rrh = std::max<uint32_t>(static_cast<uint32_t>((reg >> 32) & 0xFFFu), 1u);
    // A page's size in pixels by format: 64 x 32 (32 and 24 bits), 64 x 64
    // (16), 128 x 64 (8), 128 x 128 (4); a row of pages spans the width.
    const bool four = dpsm == 0x14u || dpsm == 0x24u || dpsm == 0x2Cu;
    const bool eight = dpsm == 0x13u || dpsm == 0x1Bu;
    const bool sixteen = dpsm == 0x02u || dpsm == 0x0Au || dpsm == 0x30u || dpsm == 0x31u || dpsm == 0x32u ||
                         dpsm == 0x3Au;
    const uint32_t pageW = (four || eight) ? 128u : 64u;
    const uint32_t pageH = four ? 128u : (eight || sixteen) ? 64u : 32u;
    const uint32_t perRow = std::max<uint32_t>(dbw * 64u / pageW, 1u);
    first = dbp / 32u + (dsay / pageH) * perRow;
    last = dbp / 32u + ((dsay + rrh - 1u) / pageH) * perRow + perRow - 1u;
    return true;
}

void observe(Effect effect, const uint8_t *data, uint32_t size)
{
    if (effect != Effect::ShadowSetup)
        return;
    thread_local std::vector<rg::gif::GifEvent> events;
    rg::gif::GifState state;
    events.clear();
    eachPacket(data, size, [&](uint32_t at, uint32_t piece) { rg::gif::decodeGifPacket(data + at, piece, state, events); });
    for (const rg::gif::GifEvent &event : events)
        if (event.kind == rg::gif::GifEvent::Kind::Register)
            t_shadowSetup.write(event.reg, event.value);
    if (g_log)
    {
        static int shown = 0;
        if (shown++ < 3)
        {
            std::fprintf(stderr, "[nativefx] shadow setup packet, %u bytes:", size);
            for (const rg::gif::GifEvent &event : events)
                if (event.kind == rg::gif::GifEvent::Kind::Register)
                    std::fprintf(stderr, " %02x=%llx", event.reg, static_cast<unsigned long long>(event.value));
            std::fprintf(stderr, "\n");
        }
    }
}

bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why)
{
    out = GSNativeEffect{};
    withheld.clear();
    if (effect != Effect::Glare && effect != Effect::Shadow)
    {
        why = "no such effect";
        return false;
    }
    Builder builder{out, why};
    static const bool traceShadow = std::getenv("RG_NATIVE_EFFECTS_TRACE") != nullptr;
    static int traced = 0;
    builder.trace = traceShadow && effect == Effect::Shadow && traced++ == 0;
    Registers registers = effect == Effect::Shadow ? t_shadowSetup : Registers{};
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    uint32_t queued = 0u;
    rg::gif::GifEvent first;
    bool ok = true;
    // Each DIRECT on its own, as GS::processGIFPacket takes them: a tag cut
    // at a DIRECT's end loses the rest of its data (the glare's first A+D
    // tag counts 14 and its DIRECT holds 8), and the next starts at a tag.
    uint32_t cut = 0u;
    for (const std::vector<uint8_t> &packet : packets)
    {
        if (!eachPacket(packet.data(), static_cast<uint32_t>(packet.size()), [&](uint32_t at, uint32_t size) {
                events.clear();
                rg::gif::decodeGifPacket(packet.data() + at, size, state, events);
                for (const rg::gif::GifEvent &event : events)
                {
                    if (!ok)
                        return;
                    if (event.kind == rg::gif::GifEvent::Kind::Register)
                    {
                        if (event.reg == 0x00u)
                            queued = 0u;
                        else if (event.reg == 0x53u || event.reg == 0x50u)
                            ok = builder.fail("a transfer among the sprites");
                        registers.write(event.reg, event.value);
                        continue;
                    }
                    const bool attributes = !registers.set[kPrmodecont] || (registers.value[kPrmodecont] & 1u);
                    const uint16_t prim =
                        attributes ? event.prim
                                   : static_cast<uint16_t>((event.prim & 7u) | (registers.value[kPrmode] & 0x7F8u));
                    if ((prim & 7u) != 6u)
                    {
                        ok = builder.fail("a primitive other than a sprite");
                        return;
                    }
                    if (++queued < 2u)
                    {
                        first = event;
                        continue;
                    }
                    queued = 0u;
                    if (event.drawing)
                        ok = builder.sprite(registers, prim, first, event);
                }
            }))
        {
            ++cut;
        }
        if (!ok)
            return false;
    }
    for (const std::vector<uint8_t> &packet : packets)
    {
        withheld.push_back(packet);
        withholdKicks(withheld.back());
    }
    if (!builder.finish() || builder.failed)
        return false;
    if (g_log)
    {
        static bool shown[3] = {};
        if (!shown[static_cast<int>(effect)])
        {
            shown[static_cast<int>(effect)] = true;
            std::fprintf(stderr, "[nativefx] %s: %u buffers, %u sprites, %u DIRECTs ending inside a GIF tag\n",
                         name(effect), out.bufferCount, out.opCount, cut);
            for (uint32_t i = 0; i < out.bufferCount; ++i)
            {
                const auto &b = out.buffers[i];
                std::fprintf(stderr, "[nativefx]   buffer %u: %05x fbw %u %ux%u%s\n", i, b.base, b.fbw, b.width,
                             b.height, b.external ? " (the GS's)" : "");
            }
            for (uint32_t n = 0; n < out.opCount; ++n)
            {
                const auto &op = out.ops[n];
                std::fprintf(stderr,
                             "[nativefx]   %2u: %u <- %d rect %g,%g-%g,%g uv %g,%g-%g,%g rgba %02x%02x%02x%02x "
                             "flags %x alpha %llx test %llx\n",
                             n, op.target, op.source, op.rect[0], op.rect[1], op.rect[2], op.rect[3], op.uv[0],
                             op.uv[1], op.uv[2], op.uv[3], op.rgba[0], op.rgba[1], op.rgba[2], op.rgba[3], op.flags,
                             static_cast<unsigned long long>(op.alpha), static_cast<unsigned long long>(op.test));
            }
        }
    }
    return true;
}

} // namespace rg::nativefx
