#include "draw_origins.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "flip_pacing.h"
#include "gif_events.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::debug
{
namespace
{

// The draw context (GetDrawCtx_15de80 reads it at gp-0x40cc).
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;
// In the draw context: the frame's buffer index (+0) and the per-buffer
// memory stacks (+8), each kept 8 bytes into the pointer there. Every packet
// of the frame is written in the stack of the buffer being built.
constexpr uint32_t kCtxBuffer = 0x000u;
constexpr uint32_t kCtxStacks = 0x008u;
// In a memory stack: its base and the quadwords used.
constexpr uint32_t kStackBase = 0x28u;
constexpr uint32_t kStackUsed = 0x2Cu;

constexpr uint32_t kBeginPrim = 0x00141B00u; // DrawListBeginPrim
constexpr uint32_t kEndPrim = 0x00141A20u;   // DrawListEndPrim
// Builders that write their packets in one call.
constexpr uint32_t kBuilders[] = {
    0x001421D0u, // DrawListAddEnv
    0x00142280u, // DrawListEmitFrameEnvMaybe
    0x001423C0u, // DrawListClearScreen
    0x001414A0u, // DrawListAddVramCopy
    0x001417B0u, // DrawListAddVramCopyFixed
    0x001418C0u, // DrawListAddTexLoad
    0x00140560u, // DrawListAddTexLoadChecked
    // With GsPkts of their own, in the same memory.
    0x00141000u, // GfxBlitTexRect
    0x0013FFD0u, // RefractionCaptureFrame
    0x00141C20u, // GfxEndFrame
};
constexpr size_t kBuilderCount = std::size(kBuilders);
// The memory stack's allocators: what code outside a builder writes, it
// allocates first (or writes, then allocates over it, as DrawListEndPrim does).
constexpr uint32_t kAllocators[] = {
    0x0014D950u, // MemStackAllocQw
    0x0014DA10u, // MemStackAllocAligned64
};
constexpr size_t kExtraHooks = 16u;

const char *g_path = nullptr;
uint64_t g_from = 0u;
uint64_t g_seqFlip = 0u;
uint8_t *g_rdram = nullptr;

// RDRAM through any of its windows (cached, uncached, uncached accelerated).
uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

uint32_t read32(uint32_t address)
{
    const uint32_t at = physical(address);
    if (!g_rdram || at + 4u > PS2_RAM_SIZE)
    {
        return 0u;
    }
    uint32_t value;
    std::memcpy(&value, g_rdram + at, sizeof(value));
    return value;
}

uint32_t drawCtx()
{
    return read32(kDrawCtxPointer);
}

// Where a memory stack is written next.
uint32_t allocTop(uint32_t stack)
{
    return read32(stack + kStackBase) + read32(stack + kStackUsed) * 16u;
}

// ... and the stack of the frame a draw context is building.
uint32_t stackTop(uint32_t ctx)
{
    if (ctx == 0u)
    {
        return 0u;
    }
    const uint32_t stack = read32(ctx + kCtxStacks + read32(ctx + kCtxBuffer) * 4u);
    return stack == 0u ? 0u : allocTop(stack + 8u);
}

// Ranges of EE memory (physical) and the call site that wrote each. Written
// on the game's thread, read on the VIF1 thread's.
struct Range
{
    uint32_t end;
    uint32_t caller;
};
std::mutex g_rangesMutex;
std::map<uint32_t, Range> g_ranges;

void addRange(uint32_t start, uint32_t end, uint32_t caller)
{
    start = physical(start);
    end = physical(end);
    // A stack starting over, or a pointer not read right: nothing to keep.
    if (end <= start || end - start > (512u << 10))
    {
        return;
    }
    std::lock_guard lock(g_rangesMutex);
    // The newest writer wins: an outer builder's range replaces the ranges of
    // the builders it called, and a buffer's next frame its last.
    auto it = g_ranges.lower_bound(start);
    if (it != g_ranges.begin())
    {
        auto before = std::prev(it);
        if (before->second.end > start)
        {
            it = before;
        }
    }
    while (it != g_ranges.end() && it->first < end)
    {
        it = g_ranges.erase(it);
    }
    g_ranges.emplace(start, Range{end, caller});
}

uint32_t callerOf(uint32_t address)
{
    address = physical(address);
    std::lock_guard lock(g_rangesMutex);
    auto it = g_ranges.upper_bound(address);
    if (it == g_ranges.begin())
    {
        return 0u;
    }
    --it;
    return address < it->second.end ? it->second.caller : 0u;
}

// The game's thread: the calls in progress, outermost first.
struct Call
{
    uint32_t caller;
    uint32_t top;
};
std::vector<Call> g_calls;
// DrawListBeginPrim's caller, until its DrawListEndPrim.
uint32_t g_primCaller = 0u;
uint32_t g_primStart = 0u;

uint32_t callSite(const R5900Context *ctx)
{
    return getRegU32(ctx, 31) - 8u;
}

void afterBuilder(uint8_t *, R5900Context *, PS2Runtime *, uint32_t depth)
{
    if (depth >= g_calls.size())
    {
        return;
    }
    const Call call = g_calls[depth];
    g_calls.resize(depth);
    // Only the outermost: an inner builder's caller is the builder.
    if (depth == 0u)
    {
        addRange(call.top, stackTop(drawCtx()), call.caller);
    }
}

PS2Runtime::RecompiledFunction g_builderOriginals[kBuilderCount + kExtraHooks] = {};

template <size_t Index>
void builder(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t depth = static_cast<uint32_t>(g_calls.size());
    g_calls.push_back(Call{callSite(ctx), stackTop(drawCtx())});
    rg::hooks::callThen(g_builderOriginals[Index], rdram, ctx, runtime, &afterBuilder, depth);
}

template <size_t... Index>
constexpr auto builderHooks(std::index_sequence<Index...>)
{
    return std::array<PS2Runtime::RecompiledFunction, sizeof...(Index)>{&builder<Index>...};
}
const auto g_builderHooks = builderHooks(std::make_index_sequence<kBuilderCount + kExtraHooks>{});

PS2Runtime::RecompiledFunction g_allocatorOriginals[std::size(kAllocators)] = {};
uint32_t g_allocStack = 0u;
uint32_t g_allocTop = 0u;

void afterAllocator(uint8_t *, R5900Context *, PS2Runtime *, uint32_t caller)
{
    addRange(g_allocTop, allocTop(g_allocStack), caller);
}

template <size_t Index>
void allocator(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Inside a builder, the builder's range covers it.
    if (!g_calls.empty() || g_primCaller != 0u)
    {
        g_allocatorOriginals[Index](rdram, ctx, runtime);
        return;
    }
    g_allocStack = getRegU32(ctx, 4);
    g_allocTop = allocTop(g_allocStack);
    rg::hooks::callThen(g_allocatorOriginals[Index], rdram, ctx, runtime, &afterAllocator, callSite(ctx));
}

PS2Runtime::RecompiledFunction g_beginPrim = nullptr;
PS2Runtime::RecompiledFunction g_endPrim = nullptr;

uint32_t g_primCallerPending = 0u;

void afterBeginPrim(uint8_t *, R5900Context *, PS2Runtime *, uint32_t primCtx)
{
    g_primCaller = g_primCallerPending;
    // The GsPkt's write pointer is stale until its first OpenPrim takes the
    // stack's top: what the prim writes starts there.
    g_primStart = stackTop(primCtx);
}

void beginPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Inside a builder, the builder's caller is the origin.
    g_primCallerPending = g_calls.empty() ? callSite(ctx) : g_calls.front().caller;
    rg::hooks::callThen(g_beginPrim, rdram, ctx, runtime, &afterBeginPrim, getRegU32(ctx, 4));
}

void afterEndPrim(uint8_t *, R5900Context *, PS2Runtime *, uint32_t primCtx)
{
    // The prim's bytes and the environment DrawListEndPrim puts back after
    // them, both in the stack, which it has just advanced past them.
    if (g_primCaller != 0u)
    {
        addRange(g_primStart, stackTop(primCtx), g_primCaller);
    }
    g_primCaller = 0u;
}

void endPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_endPrim, rdram, ctx, runtime, &afterEndPrim, getRegU32(ctx, 4));
}

// The VIF1 thread's list.
thread_local const uint8_t *t_list = nullptr;
thread_local size_t t_listSize = 0u;
thread_local const std::vector<std::pair<uint32_t, uint32_t>> *t_listOrigins = nullptr;

// The EE address a packet came from, or ~0u.
uint32_t addressOf(const uint8_t *data)
{
    if (t_list && data >= t_list && data < t_list + t_listSize && t_listOrigins && !t_listOrigins->empty())
    {
        const uint32_t offset = static_cast<uint32_t>(data - t_list);
        auto it = std::upper_bound(t_listOrigins->begin(), t_listOrigins->end(), offset,
                                   [](uint32_t value, const auto &piece) { return value < piece.first; });
        if (it == t_listOrigins->begin())
        {
            return ~0u;
        }
        --it;
        return it->second + (offset - it->first);
    }
    if (g_rdram && data >= g_rdram && data < g_rdram + PS2_RAM_SIZE)
    {
        return static_cast<uint32_t>(data - g_rdram);
    }
    return ~0u;
}

// What an origin drew.
struct Stats
{
    uint64_t packets = 0u;
    uint64_t prims = 0u; // primitives drawn
    uint64_t sprites = 0u;
    double spritePixels = 0.0;
    uint64_t triangles = 0u; // of triangles, strips and fans
    uint64_t textured = 0u;
    uint64_t targetReads = 0u;   // primitives whose texture is a render target
    uint64_t feedback = 0u;      // ... the target being drawn
    uint64_t reinterpreted = 0u; // ... read in another format family
    uint64_t uploads = 0u;
    uint64_t uploadBytes = 0u;
    uint64_t readbacks = 0u;
    uint64_t copies = 0u;
    uint64_t meshes = 0u;
    uint64_t meshVertices = 0u;
    std::map<uint32_t, uint64_t> targets;  // block address << 8 | PSM, primitives
    std::map<uint32_t, uint64_t> textures; // likewise
};

// The GS state the census follows, per context.
struct Context
{
    uint32_t fbp = 0u, fbw = 0u, fpsm = 0u;
    uint32_t tbp = 0u, tpsm = 0u;
    uint32_t scissorBottom = 0u;
};

std::mutex g_mutex;
std::map<std::string, Stats> g_stats;
rg::gif::GifState g_gif;
Context g_contexts[2];
uint32_t g_bitbltDbp = 0u;
uint32_t g_bitbltDpsm = 0u;
uint32_t g_trxWidth = 0u, g_trxHeight = 0u;
// PRMODECONT.AC and PRMODE: the attributes when AC is 0.
bool g_primAttributes = true;
uint16_t g_prmode = 0u;
// Targets drawn into: base page -> (PSM, pages).
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_targets;
uint64_t g_startFlip = ~0ull;
uint16_t g_lastX = 0u, g_lastY = 0u;
// Vertices since PRIM was written, as the GS queues them.
uint32_t g_queued = 0u;
std::chrono::steady_clock::time_point g_nextReport;
// RG_DRAW_ORIGINS_SEQ: the runs of the frame being written.
std::FILE *g_seq = nullptr;
std::string g_seqKey;
uint32_t g_seqTarget = ~0u, g_seqTexture = ~0u;
uint64_t g_seqPrims = 0u;
bool g_seqDone = false;

uint32_t family(uint32_t psm)
{
    switch (psm)
    {
    case 0x00: case 0x01: case 0x30: case 0x31: return 32u;
    case 0x02: case 0x0A: case 0x32: case 0x3A: return 16u;
    default: return 8u; // the indexed formats
    }
}

// Bits a texel of the indexed formats.
uint32_t bitsOf(uint32_t psm)
{
    return psm == 0x13u || psm == 0x1Bu ? 8u : 4u;
}

uint32_t pageHeight(uint32_t psm)
{
    return family(psm) == 16u ? 64u : 32u;
}

void flushSeq()
{
    if (g_seq && g_seqPrims != 0u)
    {
        char texture[16] = "-";
        if (g_seqTexture != ~0u)
        {
            std::snprintf(texture, sizeof(texture), "%05x:%02x", (g_seqTexture >> 8) & 0xFFFFFu, g_seqTexture & 0xFFu);
        }
        std::fprintf(g_seq, "%-28s target %05x:%02x  texture %-8s  prims %llu\n", g_seqKey.c_str(),
                     (g_seqTarget >> 8) & 0xFFFFFu, g_seqTarget & 0xFFu, texture,
                     static_cast<unsigned long long>(g_seqPrims));
    }
    g_seqPrims = 0u;
}

void seqPrim(const std::string &key, uint32_t target, uint32_t texture)
{
    if (!g_seq)
    {
        return;
    }
    if (key != g_seqKey || target != g_seqTarget || texture != g_seqTexture)
    {
        flushSeq();
        g_seqKey = key;
        g_seqTarget = target;
        g_seqTexture = texture;
    }
    ++g_seqPrims;
}

void writeReport(uint64_t frames)
{
    std::FILE *out = std::fopen(g_path, "w");
    if (!out)
    {
        return;
    }
    std::vector<std::pair<const std::string *, const Stats *>> rows;
    uint64_t prims = 0u, attributed = 0u;
    for (const auto &[key, stats] : g_stats)
    {
        rows.emplace_back(&key, &stats);
        prims += stats.prims;
        if (key.rfind("unattributed", 0) != 0)
        {
            attributed += stats.prims;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.second->prims + a.second->meshVertices / 3u > b.second->prims + b.second->meshVertices / 3u;
    });
    const double perFrame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
    std::fprintf(out, "# draw origins over %llu frames; %.1f%% of primitives attributed. Per frame:\n",
                 static_cast<unsigned long long>(frames), prims ? 100.0 * attributed / prims : 0.0);
    std::fprintf(out, "# origin | packets prims sprites spriteKpx tris textured | targetReads feedback reinterp | "
                      "uploads uploadKB readbacks copies | meshes | targets (block:psm prims) | textures\n");
    for (const auto &[key, stats] : rows)
    {
        const Stats &s = *stats;
        std::fprintf(out, "%-28s | %7.1f %8.1f %7.1f %8.1f %8.1f %8.1f | %7.1f %7.1f %7.1f | %6.2f %8.1f %5.2f %5.2f | %6.1f |",
                     key->c_str(), s.packets * perFrame, s.prims * perFrame, s.sprites * perFrame,
                     s.spritePixels * perFrame / 1000.0, s.triangles * perFrame, s.textured * perFrame,
                     s.targetReads * perFrame, s.feedback * perFrame, s.reinterpreted * perFrame, s.uploads * perFrame,
                     s.uploadBytes * perFrame / 1024.0, s.readbacks * perFrame, s.copies * perFrame,
                     s.meshes * perFrame);
        auto top = [&](const std::map<uint32_t, uint64_t> &counts) {
            std::vector<std::pair<uint64_t, uint32_t>> sorted;
            for (const auto &[what, count] : counts)
            {
                sorted.emplace_back(count, what);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < 6u; ++i)
            {
                std::fprintf(out, " %05x:%02x %.1f", (sorted[i].second >> 8) & 0xFFFFFu, sorted[i].second & 0xFFu,
                             sorted[i].first * perFrame);
            }
            if (sorted.size() > 6u)
            {
                std::fprintf(out, " +%zu", sorted.size() - 6u);
            }
        };
        top(s.targets);
        std::fprintf(out, " |");
        top(s.textures);
        std::fprintf(out, "\n");
    }
    std::fclose(out);
}

// Under g_mutex: the flip count, starting the count and the sequence.
bool counting()
{
    const uint64_t flips = rg::graphics::guestFlips();
    if (flips < g_from)
    {
        return false;
    }
    if (g_startFlip == ~0ull)
    {
        g_startFlip = flips;
    }
    if (g_seqFlip != 0u && !g_seqDone)
    {
        if (!g_seq && flips == g_seqFlip)
        {
            g_seq = std::fopen((std::string(g_path) + ".seq").c_str(), "w");
        }
        else if (g_seq && flips != g_seqFlip)
        {
            flushSeq();
            std::fclose(g_seq);
            g_seq = nullptr;
            g_seqDone = true;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_nextReport)
    {
        g_nextReport = now + std::chrono::seconds(2);
        writeReport(flips - g_startFlip);
    }
    return true;
}

std::string keyOf(const uint8_t *data, uint32_t vu1Tag, const char *vu1Name)
{
    char text[40];
    if (vu1Name)
    {
        std::snprintf(text, sizeof(text), "vu1 %s", vu1Name);
    }
    else if (vu1Tag != 0u)
    {
        std::snprintf(text, sizeof(text), "vu1 %08x", vu1Tag);
    }
    else
    {
        const uint32_t address = addressOf(data);
        const uint32_t caller = address == ~0u ? 0u : callerOf(address);
        if (caller == 0u)
        {
            if (address == ~0u)
            {
                return t_list ? "unattributed (copy)" : "unattributed (path 3)";
            }
            std::snprintf(text, sizeof(text), "unattributed %06x", physical(address) & ~0xFFFFu);
            return text;
        }
        std::snprintf(text, sizeof(text), "fn %06x", caller);
    }
    return text;
}

void account(Stats &stats, const std::string &key, const rg::gif::GifEvent &event)
{
    if (event.kind == rg::gif::GifEvent::Kind::Register)
    {
        const uint64_t v = event.value;
        switch (event.reg)
        {
        case 0x00: // PRIM
            g_queued = 0u;
            break;
        case 0x4C: case 0x4D: // FRAME_1, FRAME_2
        {
            Context &c = g_contexts[event.reg - 0x4C];
            c.fbp = static_cast<uint32_t>(v & 0x1FFu);
            c.fbw = static_cast<uint32_t>((v >> 16) & 0x3Fu);
            c.fpsm = static_cast<uint32_t>((v >> 24) & 0x3Fu);
            break;
        }
        case 0x06: case 0x07: // TEX0_1, TEX0_2
        {
            Context &c = g_contexts[event.reg - 0x06];
            c.tbp = static_cast<uint32_t>(v & 0x3FFFu);
            c.tpsm = static_cast<uint32_t>((v >> 20) & 0x3Fu);
            break;
        }
        case 0x16: case 0x17: // TEX2_1, TEX2_2
            g_contexts[event.reg - 0x16].tpsm = static_cast<uint32_t>((v >> 20) & 0x3Fu);
            break;
        case 0x40: case 0x41: // SCISSOR_1, SCISSOR_2
            g_contexts[event.reg - 0x40].scissorBottom = static_cast<uint32_t>((v >> 48) & 0x7FFu);
            break;
        case 0x1A: // PRMODECONT
            g_primAttributes = (v & 1u) != 0u;
            break;
        case 0x1B: // PRMODE
            g_prmode = static_cast<uint16_t>(v & 0x7F8u);
            break;
        case 0x50: // BITBLTBUF
            g_bitbltDbp = static_cast<uint32_t>((v >> 32) & 0x3FFFu);
            g_bitbltDpsm = static_cast<uint32_t>((v >> 56) & 0x3Fu);
            break;
        case 0x52: // TRXREG
            g_trxWidth = static_cast<uint32_t>(v & 0xFFFu);
            g_trxHeight = static_cast<uint32_t>((v >> 32) & 0xFFFu);
            break;
        case 0x53: // TRXDIR
            switch (v & 3u)
            {
            case 0:
            {
                ++stats.uploads;
                // The pages the upload covers are no target's any more.
                const uint32_t bits = family(g_bitbltDpsm) == 8u ? bitsOf(g_bitbltDpsm) : family(g_bitbltDpsm);
                const uint32_t bytes = g_trxWidth * g_trxHeight * bits / 8u;
                const uint32_t first = g_bitbltDbp / 32u;
                const uint32_t last = first + std::max(1u, (bytes + 8191u) / 8192u);
                for (auto it = g_targets.begin(); it != g_targets.end();)
                {
                    const bool overlaps = it->first < last && it->first + it->second.second > first;
                    it = overlaps ? g_targets.erase(it) : std::next(it);
                }
                break;
            }
            case 1: ++stats.readbacks; break;
            case 2: ++stats.copies; break;
            default: break;
            }
            break;
        default:
            break;
        }
        return;
    }
    const uint16_t prim = g_primAttributes ? event.prim : static_cast<uint16_t>((event.prim & 7u) | g_prmode);
    const uint32_t type = prim & 7u;
    // The vertices a primitive takes, and whether each further one draws
    // another (strips and fans) or the queue starts over (lists).
    static constexpr uint32_t kVertices[8] = {1u, 2u, 2u, 3u, 3u, 3u, 2u, 1u};
    const bool strip = type == 2u || type == 4u || type == 5u;
    const bool complete = ++g_queued >= kVertices[type];
    if (complete && !strip)
    {
        g_queued = 0u;
    }
    if (!event.drawing || !complete)
    {
        g_lastX = event.x;
        g_lastY = event.y;
        return;
    }
    const Context &c = g_contexts[(prim >> 9) & 1u];
    ++stats.prims;
    if (type == 6u)
    {
        ++stats.sprites;
        const double w = std::abs(static_cast<int>(event.x) - static_cast<int>(g_lastX)) / 16.0;
        const double h = std::abs(static_cast<int>(event.y) - static_cast<int>(g_lastY)) / 16.0;
        stats.spritePixels += w * h;
    }
    else if (type >= 3u && type <= 5u)
    {
        ++stats.triangles;
    }
    g_lastX = event.x;
    g_lastY = event.y;
    const uint32_t pages = std::max(1u, c.fbw) *
                           std::max(1u, (c.scissorBottom + pageHeight(c.fpsm)) / pageHeight(c.fpsm));
    g_targets[c.fbp] = {c.fpsm, pages};
    const uint32_t target = (c.fbp * 32u) << 8 | c.fpsm;
    ++stats.targets[target];
    uint32_t texture = ~0u;
    if ((prim >> 4) & 1u)
    {
        ++stats.textured;
        texture = c.tbp << 8 | c.tpsm;
        ++stats.textures[texture];
        const uint32_t page = c.tbp / 32u;
        auto it = g_targets.upper_bound(page);
        if (it != g_targets.begin())
        {
            --it;
            if (page < it->first + it->second.second)
            {
                ++stats.targetReads;
                if (it->first == c.fbp || (page >= c.fbp && page < c.fbp + pages))
                {
                    ++stats.feedback;
                }
                if (family(it->second.first) != family(c.tpsm))
                {
                    ++stats.reinterpreted;
                }
            }
        }
    }
    seqPrim(key, target, texture);
}

} // namespace

bool drawOriginsEnabled()
{
    static const bool enabled = [] {
        g_path = std::getenv("RG_DRAW_ORIGINS");
        if (g_path && *g_path)
        {
            if (const char *from = std::getenv("RG_DRAW_ORIGINS_FROM"))
            {
                g_from = std::strtoull(from, nullptr, 10);
            }
            if (const char *seq = std::getenv("RG_DRAW_ORIGINS_SEQ"))
            {
                g_seqFlip = std::strtoull(seq, nullptr, 10);
            }
            return true;
        }
        return false;
    }();
    return enabled;
}

void installDrawOrigins(PS2Runtime &runtime)
{
    if (!drawOriginsEnabled())
    {
        return;
    }
    g_rdram = runtime.memory().getRDRAM();
    bool complete = rg::timing::hookFunction(runtime, kBeginPrim, &beginPrim, g_beginPrim) &&
                    rg::timing::hookFunction(runtime, kEndPrim, &endPrim, g_endPrim);
    complete &= rg::timing::hookFunction(runtime, kAllocators[0], &allocator<0>, g_allocatorOriginals[0]) &&
                rg::timing::hookFunction(runtime, kAllocators[1], &allocator<1>, g_allocatorOriginals[1]);
    size_t index = 0u;
    for (const uint32_t address : kBuilders)
    {
        complete &= rg::timing::hookFunction(runtime, address, g_builderHooks[index], g_builderOriginals[index]);
        ++index;
    }
    if (const char *extra = std::getenv("RG_DRAW_ORIGINS_HOOK"))
    {
        for (const char *at = extra; *at && index < kBuilderCount + kExtraHooks;)
        {
            char *end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(at, &end, 16));
            if (end == at)
            {
                break;
            }
            complete &= rg::timing::hookFunction(runtime, address, g_builderHooks[index], g_builderOriginals[index]);
            ++index;
            at = *end == ',' ? end + 1 : end;
        }
    }
    std::fprintf(stderr, "[origins] %s, to %s\n", complete ? "builders hooked" : "some builders not hooked", g_path);
}

void drawOriginsList(const uint8_t *data, size_t size, const std::vector<std::pair<uint32_t, uint32_t>> *origins)
{
    t_list = data;
    t_listSize = size;
    t_listOrigins = origins;
}

void drawOriginsPacket(const uint8_t *data, uint32_t size, uint32_t vu1Tag, const char *vu1Name)
{
    thread_local std::vector<rg::gif::GifEvent> events;
    std::lock_guard lock(g_mutex);
    if (!counting())
    {
        return;
    }
    const std::string key = keyOf(data, vu1Tag, vu1Name);
    Stats &stats = g_stats[key];
    ++stats.packets;
    events.clear();
    const rg::gif::GifDecodeStats decoded = rg::gif::decodeGifPacket(data, size, g_gif, events);
    stats.uploadBytes += static_cast<uint64_t>(decoded.imageQwords) * 16u;
    for (const auto &event : events)
    {
        account(stats, key, event);
    }
}

void drawOriginsMesh(uint32_t vertices, uint32_t vu1Tag, const char *vu1Name)
{
    std::lock_guard lock(g_mutex);
    if (!counting())
    {
        return;
    }
    char text[40];
    if (vu1Name)
    {
        std::snprintf(text, sizeof(text), "vu1 %s", vu1Name);
    }
    else
    {
        std::snprintf(text, sizeof(text), "vu1 %08x", vu1Tag);
    }
    Stats &stats = g_stats[text];
    ++stats.meshes;
    stats.meshVertices += vertices;
}

} // namespace rg::debug
