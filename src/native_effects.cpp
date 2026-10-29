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

// The packet memory each effect's last calls wrote: a few frames' worth,
// the VIF1 thread reading a frame or two behind the game.
struct Range
{
    uint32_t start = 0u;
    uint32_t end = 0u;
    Effect effect = Effect::None;
};
std::mutex g_mutex;
std::array<Range, 8> g_ranges{};
size_t g_next = 0u;

void addRange(uint32_t start, uint32_t end, Effect effect)
{
    start = physical(start);
    end = physical(end);
    if (end <= start || end - start > (256u << 10))
    {
        return;
    }
    std::lock_guard lock(g_mutex);
    // A range the new one overlaps is the same memory reused: gone.
    for (Range &range : g_ranges)
    {
        if (range.start < end && start < range.end)
        {
            range = Range{};
        }
    }
    g_ranges[g_next] = Range{start, end, effect};
    g_next = (g_next + 1u) % g_ranges.size();
}

// A frame begins in a buffer: what the effects wrote there before is some
// other packet's now. (The game waits for the lists of a buffer to finish
// before it builds in it again.)
void dropRanges(uint32_t start, uint32_t end)
{
    start = physical(start);
    end = physical(end);
    std::lock_guard lock(g_mutex);
    for (Range &range : g_ranges)
    {
        if (range.start < end && start < range.end)
        {
            range = Range{};
        }
    }
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

constexpr uint8_t kTex0 = 0x06u, kXyoffset = 0x18u, kPrmodecont = 0x1Au, kPrmode = 0x1Bu,
                  kTex1 = 0x14u, kScissor = 0x40u, kAlpha = 0x42u, kDthe = 0x45u, kColclamp = 0x46u,
                  kTest = 0x47u, kPabe = 0x49u, kFba = 0x4Au, kFrame = 0x4Cu, kZbuf = 0x4Eu;

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

    bool fail(const char *reason)
    {
        why = reason;
        return false;
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
    bool sprite(const Registers &r, uint16_t prim, const rg::gif::GifEvent &v0, const rg::gif::GifEvent &v1)
    {
        const uint32_t ctxt = (prim >> 9) & 1u;
        const bool tme = (prim >> 4) & 1u;
        const bool abe = (prim >> 6) & 1u;
        if ((prim >> 5) & 1u || (prim >> 7) & 1u)
            return fail("a sprite with fog or antialiasing");
        if (tme && !((prim >> 8) & 1u))
            return fail("a sprite with STQ");
        const auto need = [&](uint8_t reg) { return r.set[reg]; };
        if (!need(kFrame + ctxt) || !need(kScissor + ctxt) || !need(kXyoffset + ctxt) || !need(kTest + ctxt) ||
            !need(kZbuf + ctxt))
            return fail("a sprite under a draw environment set before the effect");
        if (tme && (!need(kTex0 + ctxt) || !need(kTex1 + ctxt)))
            return fail("a sprite under a texture set before the effect");
        if (abe && !need(kAlpha + ctxt))
            return fail("a sprite under a blend set before the effect");
        const uint64_t frame = r.value[kFrame + ctxt];
        const uint64_t test = r.value[kTest + ctxt];
        const uint64_t zbuf = r.value[kZbuf + ctxt];
        if (((frame >> 24) & 0x3Fu) != 0u || (frame >> 32) != 0u)
            return fail("a sprite into a target not CT32, or masked");
        if ((test >> 14) & 1u)
            return fail("a sprite with the destination alpha test");
        if (((test >> 16) & 1u) && ((test >> 17) & 3u) != 1u)
            return fail("a sprite with a depth test");
        if (((zbuf >> 32) & 1u) == 0u)
            return fail("a sprite that writes depth");
        if ((r.value[kDthe] & 1u) || (r.value[kFba + ctxt] & 1u) || (abe && (r.value[kPabe] & 1u)))
            return fail("a sprite with dithering, FBA or PABE");

        GSNativeEffect::Op op;
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
            if (((tex0 >> 20) & 0x3Fu) != 0u || tfx > 1u)
                return fail("a texture not CT32, or not modulated or decal");
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
            op.flags |= tfx == 1u ? GSNativeEffect::kDecal : 0u;
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
        if (out.opCount != 0u && join(out.ops[out.opCount - 1u], op))
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
            const bool external = firstWrite[i] < 0 || (firstRead[i] >= 0 && firstRead[i] <= firstWrite[i]);
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
            if (!out.buffers[op.target].external)
                continue;
            if (op.source < 0 || out.buffers[op.source].external)
                return fail("a sprite into the frame not from an effect buffer");
            // Cs + Cd: A = Cs, B = 0, C = FIX (1.0), D = Cd.
            if ((op.flags & GSNativeEffect::kBlend) && ((op.alpha & 0xFFu) != 0x68u || ((op.alpha >> 32) & 0xFFu) != 0x80u))
                return fail("a sprite into the frame with a blend other than an add");
            if (op.test & 1u)
                return fail("a sprite into the frame with the alpha test");
        }
        return true;
    }
};

} // namespace

bool enabled()
{
    static const bool on = [] {
        const char *glare = std::getenv("RG_NATIVE_GLARE");
        g_log = std::getenv("RG_NATIVE_EFFECTS_LOG") != nullptr;
        return glare && std::strcmp(glare, "0") != 0;
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
    if (!rg::timing::hookFunction(runtime, kDrawListFrameBegin, &drawListFrameBegin, g_frameBeginOriginal) ||
        !rg::timing::hookFunction(runtime, kGlareFilterDraw, &glareFilterDraw, g_glareOriginal))
    {
        std::fprintf(stderr, "[nativefx] the glare stays GS packets\n");
    }
}

Effect effectAt(uint32_t address)
{
    address = physical(address);
    std::lock_guard lock(g_mutex);
    for (const Range &range : g_ranges)
    {
        if (address >= range.start && address < range.end)
        {
            return range.effect;
        }
    }
    return Effect::None;
}

bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why)
{
    out = GSNativeEffect{};
    withheld.clear();
    if (effect != Effect::Glare)
    {
        why = "no such effect";
        return false;
    }
    Builder builder{out, why};
    Registers registers;
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
    if (!builder.finish())
        return false;
    if (g_log)
    {
        static bool shown = false;
        if (!shown)
        {
            shown = true;
            std::fprintf(stderr, "[nativefx] glare: %u buffers, %u sprites, %u DIRECTs ending inside a GIF tag\n",
                         out.bufferCount, out.opCount, cut);
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
