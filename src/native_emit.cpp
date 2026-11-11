#include "native_emit.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "native_effects.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

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

// The call's items and groups, from inst a0, req a2 (read after the call:
// ModelMesh's groups are rebuilt by it).
void predict(uint32_t req, Call &call)
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
            if (t == 0u)
                continue;
            const uint32_t pkt = read32(t);
            const uint8_t animated = g_rdram[physical(t + 6u)];
            out.q82 = animated ? readQuad(t + 0x20u) : readQuad(pkt + 0x10u);
            out.q84 = readQuad(pkt + 0x30u);
            out.q86 = readQuad(pkt + 0x50u);
            out.q87 = readQuad(pkt + 0x60u);
        }
    }
}

PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;
// The call's request (a2), for after it (game thread; the emitter does not
// nest outside effects, which are not followed).
uint32_t t_pendingReq = 0u;

void afterEmit(uint8_t *, R5900Context *, PS2Runtime *, uint32_t data)
{
    // data: the unit.
    Call call;
    call.unit = data;
    predict(t_pendingReq, call);
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

void chunk(const uint8_t *vu1Data)
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
    const Group &group = item.groups[size_t(t_group)];
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
