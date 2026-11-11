#include "native_emit.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "draw_state_mirror.h"
#include "native_effects.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"
#include "vu1_native.h"

#include <unordered_map>

namespace rg::nativeemit
{
namespace
{
uint8_t *g_rdram = nullptr;

constexpr uint32_t kModelEmitDrawPackets = 0x0014EC20u;
// The markers' kinds (the unit's high byte; 1 passes, 2 blocks).
constexpr uint32_t kBegin = 3u, kEnd = 4u;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}
uint64_t read64(uint32_t address)
{
    uint64_t v;
    std::memcpy(&v, g_rdram + physical(address), sizeof(v));
    return v;
}
uint32_t read32(uint32_t address)
{
    uint32_t v;
    std::memcpy(&v, g_rdram + physical(address), sizeof(v));
    return v;
}
using Quad = std::array<uint32_t, 4>;
Quad readQuad(uint32_t address)
{
    Quad q;
    std::memcpy(q.data(), g_rdram + physical(address & ~15u), sizeof(q));
    return q;
}

// What a group's chunks should find in VU1 memory: the material packet's
// quads 82, 84, 86, 87.
struct Group
{
    Quad q82{}, q84{}, q86{}, q87{};
    // The program the microcode table names (null: a slot left as it was,
    // or pieces identify() does not know; not compared).
    const rg::vu1native::Program *program = nullptr;
    bool programKnown = false;
    // The GS state its texenv sets: the material's GsEnv (t+0x14: ALPHA,
    // TEST, ZBUF's mask, FBA), TEX0 from its texture node, TEX1 and CLAMP
    // from its packet (frozen at load).
    uint64_t alpha = 0u, test = 0u, fba = 0u, tex0 = 0u, tex1 = 0u, clamp = 0u;
    bool zmask = false, textured = false, stateKnown = false;
    // Its chunks' two header quads (TOP, TOP+1), from its chain's spans in
    // EE memory, in the order they are drawn; and how many the list drew.
    std::vector<std::array<Quad, 2>> chunks;
    bool chunksKnown = false;
    uint32_t drawn = 0u;
};
// An item's: the model matrix (60-63) and the mesh header (76-79, 81, 42),
// from its packets p14 and p18; its groups, in the order they are drawn.
struct Item
{
    std::array<Quad, 4> model{};
    Quad q76{}, q77{}, q78{}, q79{}, q81{}, q42{};
    std::vector<Group> groups;
};
struct Call
{
    uint32_t unit = 0u;
    std::vector<Item> items;
};
std::mutex g_mutex;
std::deque<Call> g_calls; // by unit, until the list reaches them
uint32_t g_units = 0u;
std::atomic<uint64_t> g_same{0u}, g_differ{0u}, g_unknown{0u}, g_outside{0u};

// p18's quads (WriteVuHeader): 76-79 at +0x20.., 81 right after the UNPACK
// word at +0x68 (unaligned), 42 at +0x80.
Quad unaligned(uint32_t address)
{
    Quad q;
    std::memcpy(q.data(), g_rdram + physical(address), sizeof(q));
    return q;
}

// The program a group draws with (NATIVE-ONLY-NOTES.md, "The program a
// group draws with"): the six slots' packets (A, F, E, B, C, D) the table at
// 0x491280 names for the material's type and the item's flags, their MPG
// code put in a micro memory image, identified as the list's run identifies
// what the VU1 holds. False: a slot is the empty packet (it keeps whatever
// code is resident) or not an MPG.
constexpr uint32_t kProgramTable = 0x00491280u;
constexpr uint32_t kEmptyPacket = 0x00436CA0u;
bool programOf(uint32_t dctx, uint32_t t, uint32_t itemFlags, const rg::vu1native::Program *&out)
{
    const uint32_t type = g_rdram[physical(t + 0x0Au)] | (uint32_t(g_rdram[physical(t + 0x0Bu)]) << 8);
    if (type >= 12u)
        return false;
    const uint32_t row = kProgramTable + type * 0x10u;
    const auto s16 = [](uint32_t address) {
        int16_t v;
        std::memcpy(&v, g_rdram + physical(address), sizeof(v));
        return int32_t(v);
    };
    const int32_t c = s16(t + 0x18u) + ((itemFlags & 4u) ? 2 : 0);
    const int32_t d = (s16(t + 0x1Au) + ((itemFlags & 8u) ? 2 : 0)) | (read32(dctx + 0x3CCCu) ? 4 : 0);
    if (c < 0 || c >= 8 || d < 0 || d >= 8)
        return false;
    const uint32_t slots[6] = {read32(row), read32(row + 4u), read32(row + 8u),
                               read32(read32(row + 0x300u) + 4u * (itemFlags & 3u)),
                               read32(read32(row + 0x304u) + 4u * uint32_t(c)),
                               read32(read32(row + 0x308u) + 4u * uint32_t(d))};
    uint64_t key = 0x9E3779B97F4A7C15ull;
    for (const uint32_t slot : slots)
    {
        if (slot == 0u || slot == kEmptyPacket)
            return false;
        key = (key ^ slot) * 0x100000001B3ull;
    }
    static std::unordered_map<uint64_t, const rg::vu1native::Program *> known;
    if (const auto it = known.find(key); it != known.end())
    {
        out = it->second;
        return true;
    }
    thread_local std::vector<uint8_t> image;
    image.assign(PS2_VU1_CODE_SIZE, 0u);
    for (const uint32_t slot : slots)
    {
        // A ret tag carrying NOP and MPG load,num; the code after it.
        uint32_t words[4];
        std::memcpy(words, g_rdram + physical(slot & ~15u), sizeof(words));
        const uint32_t mpg = words[3];
        if (((mpg >> 24) & 0x7Fu) != 0x4Au)
            return false;
        const uint32_t pairs = ((mpg >> 16) & 0xFFu) == 0u ? 256u : ((mpg >> 16) & 0xFFu);
        const uint32_t load = (mpg & 0xFFFFu) * 8u;
        if (load + pairs * 8u > PS2_VU1_CODE_SIZE)
            return false;
        std::memcpy(image.data() + load, g_rdram + physical((slot & ~15u) + 16u), pairs * 8u);
    }
    out = rg::vu1native::identify(image.data(), 0x320u);
    if (known.size() > 4096u)
        known.clear();
    known.emplace(key, out);
    return true;
}

// The words a VIF code takes after it (UNPACK, MPG, DIRECT, STMASK, STROW,
// STCOL), as the VIF reads them.
uint32_t vifDataWords(uint32_t cmd)
{
    const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if (op == 0x20u)
        return 1u;
    if (op == 0x30u || op == 0x31u)
        return 4u;
    if (op == 0x4Au)
        return (num == 0u ? 256u : num) * 2u;
    if (op == 0x50u || op == 0x51u)
        return (imm == 0u ? 65536u : imm) * 4u;
    if ((op & 0x60u) == 0x60u)
    {
        const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, n = num == 0u ? 256u : num;
        const uint32_t bits = vl == 3u ? 16u : (32u >> vl) * (vn + 1u);
        return (n * bits + 31u) / 32u;
    }
    return 0u;
}

// A group's chunks (NATIVE-ONLY-NOTES.md, "Chunk byte layout"): its chain is
// REF tags to spans of chunks, then a RET; each chunk an STCYCL and an
// UNPACK of its two header quads to TOP, its sections, its MSCNT. Each
// chunk's header, read from the span as the VIF would. False: not of that
// shape.
bool chunksOf(uint32_t chain, std::vector<std::array<Quad, 2>> &out)
{
    for (int tags = 0; tags < 4096; ++tags, chain += 16u)
    {
        const uint32_t tag = read32(chain);
        const uint32_t id = (tag >> 28) & 7u, qwc = tag & 0xFFFFu;
        if (id == 6u)
            return true; // RET
        if (id != 3u || qwc == 0u)
            return false;
        const uint32_t start = read32(chain + 4u) & 0x0FFFFFFFu, end = start + qwc * 16u;
        uint32_t at = start;
        bool header = false;
        while (at + 4u <= end)
        {
            const uint32_t cmd = read32(at);
            at += 4u;
            const uint32_t op = (cmd >> 24) & 0x7Fu;
            if (cmd == 0x6C028000u && !header)
            {
                // UNPACK V4-32, two quads to TOP: the chunk's header.
                std::array<Quad, 2> h;
                h[0] = {read32(at), read32(at + 4u), read32(at + 8u), read32(at + 12u)};
                h[1] = {read32(at + 16u), read32(at + 20u), read32(at + 24u), read32(at + 28u)};
                out.push_back(h);
                header = true;
            }
            if (op == 0x17u)
                header = false; // MSCNT: the chunk's end
            at += vifDataWords(cmd) * 4u;
        }
    }
    return false;
}

// The call's items and groups, from inst a0, req a2 (read after the call:
// ModelMesh's groups are rebuilt by it).
void predict(uint32_t dctx, uint32_t req, Call &call)
{
    const uint32_t key = read32(req + 0x40u);
    const uint32_t meshes = read32(read32(req + 0x0Cu) + 4u);
    const uint32_t materials = read32(read32(req + 0x08u) + 4u);
    uint32_t node = read32(req + 0x44u);
    for (int guard = 0; node != 0u && guard < 4096; ++guard, node = read32(node))
    {
        const uint32_t it = node + 0x10u;
        if (key != 0u && read32(it) != key)
            continue;
        const uint32_t p14 = read32(it + 0x14u), p18 = read32(it + 0x18u);
        if (p14 == 0u || p18 == 0u)
            continue;
        const uint32_t mesh = read32(meshes + 4u * read32(it + 0x08u));
        const uint32_t itemFlags = read32(it + 0x0Cu);
        if (mesh == 0u)
            continue;
        Item &item = call.items.emplace_back();
        for (uint32_t r = 0; r < 4u; ++r)
            item.model[r] = readQuad(p14 + 0x10u + 16u * r);
        item.q76 = readQuad(p18 + 0x20u);
        item.q77 = readQuad(p18 + 0x30u);
        item.q78 = readQuad(p18 + 0x40u);
        item.q79 = readQuad(p18 + 0x50u);
        item.q81 = unaligned(p18 + 0x6Cu);
        item.q42 = readQuad(p18 + 0x80u);
        // The groups: ModelMeshLinks at +0x130, ModelMesh at +0xF0 ({next,
        // , , material, chain}); one without a chain is not drawn.
        const uint32_t vtable = read32(mesh + 0x50u);
        uint32_t group = vtable == 0x00470010u ? read32(mesh + 0x130u) : vtable == 0x00470040u ? read32(mesh + 0xF0u) : 0u;
        for (int g = 0; group != 0u && g < 256; ++g, group = read32(group))
        {
            if (read32(group + 0x10u) == 0u)
                continue;
            const uint32_t t = read32(materials + 4u * read32(group + 0x0Cu));
            Group &out = item.groups.emplace_back();
            out.chunksKnown = chunksOf(read32(group + 0x10u), out.chunks);
            if (t == 0u)
                continue;
            const uint32_t pkt = read32(t);
            const uint8_t animated = g_rdram[physical(t + 6u)];
            out.q82 = animated ? readQuad(t + 0x20u) : readQuad(pkt + 0x10u);
            out.q84 = readQuad(pkt + 0x30u);
            out.q86 = readQuad(pkt + 0x50u);
            out.q87 = readQuad(pkt + 0x60u);
            out.programKnown = programOf(dctx, t, itemFlags, out.program);
            const uint32_t env = t + 0x14u;
            out.alpha = read64(env + 0x10u);
            out.test = read64(env + 0x20u);
            out.zmask = ((read64(env + 0x30u) >> 32) & 1u) != 0u;
            out.fba = read64(env + 0x40u) & 1u;
            const uint32_t texnode = read32(t + 0x0Cu);
            out.textured = texnode != 0u && read32(t + 4u) >= 12u;
            if (out.textured)
            {
                out.tex0 = read64(texnode + 0x38u);
                out.tex1 = read64(pkt + 0xA0u);
                out.clamp = read64(pkt + 0xB0u);
            }
            out.stateKnown = true;
        }
    }
}

PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;
// The call's request (a2), for after it (game thread; the emitter does not
// nest outside effects, which are not followed).
uint32_t t_pendingReq = 0u, t_pendingDctx = 0u;

void afterEmit(uint8_t *, R5900Context *, PS2Runtime *, uint32_t data)
{
    // data: the unit.
    Call call;
    call.unit = data;
    predict(t_pendingDctx, t_pendingReq, call);
    rg::nativefx::writeMarker(kEnd << 24);
    std::lock_guard lock(g_mutex);
    if (g_calls.size() >= 4096u)
        g_calls.pop_front();
    g_calls.push_back(std::move(call));
}

void emit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Inside an effect's call (the composite's parts), the effect's pass
    // stands for the draws: not followed here.
    if (rg::nativefx::inEffectCall())
    {
        g_emitOriginal(rdram, ctx, runtime);
        return;
    }
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    const uint32_t unit = g_units | (kBegin << 24);
    if (!rg::nativefx::writeMarker(unit))
    {
        g_emitOriginal(rdram, ctx, runtime);
        return;
    }
    t_pendingReq = getRegU32(ctx, 6);
    t_pendingDctx = getRegU32(ctx, 5);
    rg::hooks::callThen(g_emitOriginal, rdram, ctx, runtime, &afterEmit, unit);
}

// The list side.
Call t_current;
bool t_in = false;
int t_item = -1, t_group = -1;

void report()
{
    const uint64_t n = g_same.load() + g_differ.load();
    if (n == 1u || n % 60000u == 0u)
        std::fprintf(stderr,
                     "[model-emit] chunks against their emit call's structures: the same %llu, differing %llu; "
                     "calls not found %llu, chunks outside an item or group %llu\n",
                     (unsigned long long)g_same.load(), (unsigned long long)g_differ.load(),
                     (unsigned long long)g_unknown.load(), (unsigned long long)g_outside.load());
}
} // namespace

void install(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kModelEmitDrawPackets, &emit, g_emitOriginal))
        std::fprintf(stderr, "[model-emit] ModelEmitDrawPackets not hooked\n");
}

bool marker(uint32_t unit)
{
    const uint32_t kind = unit >> 24;
    if (kind == kEnd)
    {
        t_in = false;
        t_current = Call{};
        return true;
    }
    if (kind != kBegin)
        return false;
    t_in = false;
    t_item = t_group = -1;
    std::lock_guard lock(g_mutex);
    for (auto it = g_calls.begin(); it != g_calls.end(); ++it)
        if (it->unit == unit)
        {
            t_current = std::move(*it);
            g_calls.erase(it);
            t_in = true;
            return true;
        }
    ++g_unknown;
    return true;
}

void mscal(uint32_t startPC)
{
    if (!t_in)
        return;
    if (startPC == 0u)
    {
        ++t_item;
        t_group = -1;
    }
    else if (startPC == 0x320u)
        ++t_group;
}

void chunk(const uint8_t *vu1Data, uint32_t top, const void *program)
{
    if (!t_in)
        return;
    if (t_item < 0 || t_group < 0 || size_t(t_item) >= t_current.items.size() ||
        size_t(t_group) >= t_current.items[size_t(t_item)].groups.size())
    {
        ++g_outside;
        return;
    }
    const Item &item = t_current.items[size_t(t_item)];
    Group &group = t_current.items[size_t(t_item)].groups[size_t(t_group)];
    const uint32_t index = group.drawn++;
    const auto quad = [&](uint32_t q) {
        Quad v;
        std::memcpy(v.data(), vu1Data + q * 16u, sizeof(v));
        return v;
    };
    const char *what = nullptr;
    Quad seen{}, want{};
    const auto check = [&](const char *name, uint32_t q, const Quad &expected, uint32_t lanes = 0xFu) {
        if (what)
            return;
        const Quad v = quad(q);
        for (int l = 0; l < 4; ++l)
            if (((lanes >> l) & 1u) && v[size_t(l)] != expected[size_t(l)])
            {
                what = name;
                seen = v;
                want = expected;
                return;
            }
    };
    for (uint32_t r = 0; r < 4u; ++r)
        check("model (60-63)", 60u + r, item.model[r]);
    check("76", 76u, item.q76, 0x7u);
    check("77", 77u, item.q77);
    check("78", 78u, item.q78);
    check("79", 79u, item.q79);
    check("81", 81u, item.q81);
    check("42", 42u, item.q42);
    check("82", 82u, group.q82);
    check("84", 84u, group.q84);
    check("86", 86u, group.q86, 0xEu); // (its NLOOP is the chunk's)
    check("87", 87u, group.q87);
    if (!what && group.chunksKnown)
    {
        if (index >= group.chunks.size())
            what = "a chunk past the chain's";
        else
        {
            check("the chunk's header (TOP)", top & 1023u, group.chunks[index][0]);
            check("the chunk's header (TOP+1)", (top + 1u) & 1023u, group.chunks[index][1]);
        }
    }
    if (!what && group.programKnown && group.program != program)
    {
        what = "the program (the table's against what the VU1 holds)";
        seen = Quad{uint32_t(reinterpret_cast<uintptr_t>(program)), 0u, 0u, 0u};
        want = Quad{uint32_t(reinterpret_cast<uintptr_t>(group.program)), 0u, 0u, 0u};
    }
    if (!what && group.stateKnown)
    {
        const rg::DrawState state = rg::drawstate::current();
        const rg::DrawContext &c = state.context;
        const auto lo = [](uint64_t v) { return uint32_t(v); };
        const auto hi = [](uint64_t v) { return uint32_t(v >> 32); };
        const auto gs = [&](const char *name, uint64_t have, uint64_t expected) {
            if (!what && have != expected)
            {
                what = name;
                seen = Quad{lo(have), hi(have), 0u, 0u};
                want = Quad{lo(expected), hi(expected), 0u, 0u};
            }
        };
        gs("ALPHA", c.alpha, group.alpha);
        gs("TEST", c.test, group.test);
        gs("FBA", c.fba & 1u, group.fba);
        gs("ZBUF's mask", c.zbuf.zmask ? 1u : 0u, group.zmask ? 1u : 0u);
        if (group.textured)
        {
            gs("TEX0's TBP0", c.tex0.tbp0, group.tex0 & 0x3FFFu);
            gs("TEX0's PSM", c.tex0.psm, (group.tex0 >> 20) & 0x3Fu);
            gs("TEX0's CBP", c.tex0.cbp, (group.tex0 >> 37) & 0x3FFFu);
            gs("TEX1", c.tex1, group.tex1);
            gs("CLAMP", c.clamp, group.clamp);
        }
    }
    if (!what)
    {
        ++g_same;
        report();
        return;
    }
    const uint64_t n = ++g_differ;
    if (n <= 12u)
        std::fprintf(stderr,
                     "[model-emit] a chunk differs from its emit call's structures (item %d group %d): quad %s "
                     "VU1 %08x %08x %08x %08x, expected %08x %08x %08x %08x\n",
                     t_item, t_group, what, seen[0], seen[1], seen[2], seen[3], want[0], want[1], want[2], want[3]);
    report();
}

} // namespace rg::nativeemit
