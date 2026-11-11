#include "native_emit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
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
// ModelEmitShadowCasterPacketsMaybe: the same loop, one material for every
// group (*(dctx+0x3B90), set by the shadow map's render).
constexpr uint32_t kModelEmitShadowCaster = 0x0014E780u;
// The sorted path: ModelQueueSortedPackets_14f0d0 queues a node per group
// (DrawListAddNode), DrawListEmitBucket_1405a0 writes them at the buckets'
// replay (NATIVE-ONLY-NOTES.md, "ModelQueueSortedPackets").
constexpr uint32_t kQueueSorted = 0x0014F0D0u;
constexpr uint32_t kEmitBucket = 0x001405A0u;
// The markers' kinds (the unit's high byte; 1 passes, 2 blocks).
constexpr uint32_t kUnwritten = 5u;
// A captured stream's marker (M6): what an emitter wrote into the list,
// taken out and replayed where it was, under the game thread's state.
constexpr uint32_t kStream = 8u;
// EffModelDrawNodes and EffQuadDrawNodes (the effect models and particles:
// NATIVE-ONLY-NOTES.md, "Entry points, and emitters that bypass
// Prepare/Emit") write their packets into the list themselves.
constexpr uint32_t kEffModelDrawNodes = 0x001A7B80u;
constexpr uint32_t kEffQuadDrawNodes = 0x001A7890u;
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;
// ModelMeshLinks: its chains are built once at load (static spans), so a
// call of only these can be drawn from its record with nothing written.
constexpr uint32_t kMeshLinks = 0x00470010u;
// ModelMesh: its chunks are rebuilt into the frame's buffer at every emit
// (its GetGroupPkts, vtable +0x18, runs BuildUnpackPkt); the hook runs that
// itself before reading the record.
constexpr uint32_t kMesh = 0x00470040u;
// ModelMeshMode1 (the PrimBatch class): inside a model it emits an empty
// chain, so its item draws nothing (NATIVE-ONLY-NOTES.md, 4.4).
constexpr uint32_t kMeshMode1 = 0x004701C0u;

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
void write32(uint32_t address, uint32_t value)
{
    std::memcpy(g_rdram + physical(address), &value, sizeof(value));
}
Quad readQuad(uint32_t address)
{
    Quad q;
    std::memcpy(q.data(), g_rdram + physical(address & ~15u), sizeof(q));
    return q;
}

std::mutex g_mutex;
std::deque<Call> g_calls; // by unit, until the list reaches them
uint32_t g_units = 0u;
std::atomic<uint64_t> g_unknown{0u};

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
// `resident`: a slot the table leaves empty runs what is resident, the
// piece the microcode cache (dctx+0x3C48, six slots) says the list loaded
// last; for a call drawn where it is emitted (the sorted path's records are
// made at the queue, not where they are drawn).
bool programOf(uint32_t dctx, uint32_t t, uint32_t itemFlags, const rg::vu1native::Program *&out,
               bool resident = false)
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
    uint32_t slots[6] = {read32(row), read32(row + 4u), read32(row + 8u),
                               read32(read32(row + 0x300u) + 4u * (itemFlags & 3u)),
                               read32(read32(row + 0x304u) + 4u * uint32_t(c)),
                               read32(read32(row + 0x308u) + 4u * uint32_t(d))};
    uint64_t key = 0x9E3779B97F4A7C15ull;
    for (uint32_t k = 0; k < 6u; ++k)
    {
        uint32_t &slot = slots[k];
        if ((slot == 0u || slot == kEmptyPacket) && resident)
            slot = read32(dctx + 0x3C48u + 4u * k);
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
void predict(uint32_t dctx, uint32_t req, Call &call, bool caster = false, bool resident = false)
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
        if (mesh == 0u || read32(mesh + 0x50u) == kMeshMode1)
            continue; // (Mode1: nothing drawn)
        Item &item = call.items.emplace_back();
        item.mesh = mesh;
        item.p14 = p14;
        item.base = read32(mesh) & 0x3FFu;
        item.p1C = read32(it + 0x1Cu);
        item.paletteAt = read32(mesh + 0x124u) & 0x3FFu;
        item.bones = read32(mesh + 0x128u);
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
            const uint32_t t = caster ? read32(dctx + 0x3B90u) : read32(materials + 4u * read32(group + 0x0Cu));
            Group &out = item.groups.emplace_back();
            out.chain = read32(group + 0x10u);
            out.chunksKnown = chunksOf(out.chain, out.chunks);
            if (t == 0u)
                continue;
            const uint32_t pkt = read32(t);
            const uint8_t animated = g_rdram[physical(t + 6u)];
            out.q82 = animated ? readQuad(t + 0x20u) : readQuad(pkt + 0x10u);
            out.q84 = readQuad(pkt + 0x30u);
            out.q86 = readQuad(pkt + 0x50u);
            out.q87 = readQuad(pkt + 0x60u);
            out.programKnown = programOf(dctx, t, itemFlags, out.program, resident);
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
            // The texenv's writes (DrawListWriteTexEnvPacket): the GsEnv
            // with ZBUF's base and format and FRAME from the draw
            // environment (FRAME's mask the material's when it has a mask
            // mode), then the material packet's TEX1 and CLAMP, then TEX0.
            const uint32_t drawEnv = read32(dctx + 0x270u);
            const uint64_t zbufBase = (0x1FFull | (0xFull << 24));
            const uint64_t zbuf = (read64(env + 0x30u) & ~zbufBase) | (read64(drawEnv + 0x90u) & zbufBase);
            uint64_t frame = read64(drawEnv + 0x80u);
            if (static_cast<int32_t>(read32(env + 0x5Cu)) != -1)
                frame = (frame & 0xFFFFFFFFull) | (read64(env + 0x50u) & 0xFFFFFFFF00000000ull);
            out.writes = {{0x42u, out.alpha}, {0x47u, out.test}, {0x4Eu, zbuf}, {0x4Au, read64(env + 0x40u)},
                          {0x4Cu, frame}};
            if (out.textured)
            {
                out.writes.emplace_back(0x14u, out.tex1);
                out.writes.emplace_back(0x08u, out.clamp);
                out.writes.emplace_back(0x06u, out.tex0);
            }
        }
    }
}

// A call whose every item's mesh is a ModelMeshLinks (static chains) or a
// ModelMesh (rebuilt here), and every group a program the table names and
// the native programs draw, is drawn from its record: the game writes
// nothing for it. Others (a program the table leaves to what is resident,
// or one identify() does not know) are written as the game writes them.
std::atomic<uint64_t> g_unwrittenCalls{0u}, g_writtenCalls{0u};

// Where a call's marker goes: each group's draw registers, the game
// thread's with its writes applied, in order (the game thread's then as
// the list's would be after them).
void syncGlobals();
Globals currentGlobals();
// The draw environment the game draws under now (DrawEnv at dctx+0x270:
// FRAME, ZBUF, XYOFFSET, SCISSOR, PRMODECONT, COLCLAMP, DTHE, TEST_1, each a
// value and its register from +0x80), on the game thread's draw state: what
// the env packets in the list set (PRMODECONT.AC 0, so a model's PRMODE
// gives its texture, fog and blend bits).
void applyDrawEnv()
{
    const uint32_t dctx = read32(kDrawCtxPointer);
    const uint32_t env = dctx != 0u ? read32(dctx + 0x270u) : 0u;
    if (env == 0u)
        return;
    std::pair<uint8_t, uint64_t> writes[8];
    for (uint32_t i = 0; i < 8u; ++i)
        writes[i] = {static_cast<uint8_t>(read64(env + 0x88u + 16u * i) & 0xFFu), read64(env + 0x80u + 16u * i)};
    rg::drawstate::game().observeWrites(writes, 8u);
}

void captureState(Call &call)
{
    syncGlobals();
    applyDrawEnv();
    call.globals = currentGlobals();
    rg::drawstate::Tracker &game = rg::drawstate::game();
    for (Item &item : call.items)
        for (Group &group : item.groups)
        {
            game.observeWrites(group.writes.data(), group.writes.size());
            group.regs = game.registers();
        }
}

// Why a call is written by the game (counted in [model-emit]).
enum Why : uint8_t
{
    kDrawable,
    kNoItems,
    kMeshClass,
    kProgramSlot,
    kProgramUnknown,
    kNoMaterial,
    kChunks,
    kProgramKind,
    kRebuild,
    kWhyCount,
};
const char *const kWhyNames[kWhyCount] = {"drawn",          "no items",        "mesh class", "program slot empty",
                                          "program unknown", "no material",     "chain",      "program kind",
                                          "ModelMesh rebuild"};
std::atomic<uint64_t> g_why[kWhyCount];

Why whyWritten(const Call &call)
{
    if (call.items.empty())
        return kNoItems;
    for (const Item &item : call.items)
    {
        const uint32_t vtable = read32(item.mesh + 0x50u);
        if (vtable != kMeshLinks && vtable != kMesh)
            return kMeshClass;
        for (const Group &group : item.groups)
        {
            if (!group.stateKnown)
                return kNoMaterial;
            if (!group.programKnown)
                return kProgramSlot;
            if (!group.program)
                return kProgramUnknown;
            if (!group.chunksKnown)
                return kChunks;
            // What the list's run draws natively: strips and projectors (the
            // model vertex stage's), outlines, particles, shadow volumes.
            const rg::vu1native::Kind kind = group.program->kind;
            if (!rg::vu1native::modelOnGpu(*group.program) && kind != rg::vu1native::Kind::Outline &&
                kind != rg::vu1native::Kind::Particles && kind != rg::vu1native::Kind::Shadow)
                return kProgramKind;
        }
    }
    return kDrawable;
}

bool drawable(const Call &call)
{
    return whyWritten(call) == kDrawable;
}

// --- Captured streams (M6) ------------------------------------------------
// What an emitter the records do not cover writes into the draw list (a
// call the game still writes, the effect models and particles, the sorted
// path's written nodes): the stretch it added is read, on the game thread
// where it ends, into the VIF stream the list would have carried (each
// tag's VIF codes and data, CALLs followed), the list cursor put back and a
// marker written in its place. The list's run replays the stream at the
// marker as the VIF would have run it there (the same uploads, programs,
// chunks), its draws under the draw state the game thread had where the
// emitter wrote (rg::drawstate::game(), then the stream's own writes), not
// under the list's.
struct Stream
{
    uint32_t unit = 0u;
    std::vector<uint8_t> bytes;
    rg::drawstate::Registers regs{};
    Globals globals;
};
std::deque<Stream> g_streams; // under g_mutex
std::atomic<uint64_t> g_captured{0u}, g_captureFailed{0u};

void appendFrom(std::vector<uint8_t> &out, uint32_t address, uint32_t bytes)
{
    const size_t at = out.size();
    out.resize(at + bytes);
    for (uint32_t i = 0; i < bytes; i += 16u)
        std::memcpy(out.data() + at + i, g_rdram + physical(address + i), std::min<uint32_t>(16u, bytes - i));
}

// A CALLed chain, as the DMA runs it (TTE: each tag's two VIF codes before
// its data), until its RET. False: not of that shape.
bool gatherCallee(uint32_t at, std::vector<uint8_t> &out, int depth)
{
    for (int steps = 0; steps < 8192 && out.size() < (8u << 20); ++steps)
    {
        const uint64_t tag = read64(at);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>((tag >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        if (addr & 0x70000000u)
            return false; // (scratchpad: not read here)
        appendFrom(out, at + 8u, 8u);
        switch (id)
        {
        case 1u: // cnt
            appendFrom(out, at + 16u, qwc * 16u);
            at += 16u + qwc * 16u;
            break;
        case 2u: // next
            appendFrom(out, at + 16u, qwc * 16u);
            at = addr;
            break;
        case 3u: // ref
        case 4u: // refs
            appendFrom(out, addr, qwc * 16u);
            at += 16u;
            break;
        case 5u: // call
            appendFrom(out, at + 16u, qwc * 16u);
            if (depth >= 2 || !gatherCallee(addr, out, depth + 1))
                return false;
            at += 16u + qwc * 16u;
            break;
        case 6u: // ret
            appendFrom(out, at + 16u, qwc * 16u);
            return true;
        default: // refe, end: the list would end here
            return false;
        }
    }
    return false;
}

// The list's stretch [from, to): cnt and call tags (and refs), as the
// emitters write them.
bool gatherStretch(uint32_t from, uint32_t to, std::vector<uint8_t> &out)
{
    out.clear();
    uint32_t at = from;
    while (at < to && out.size() < (8u << 20))
    {
        const uint64_t tag = read64(at);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>((tag >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        appendFrom(out, at + 8u, 8u);
        if (id == 1u)
        {
            appendFrom(out, at + 16u, qwc * 16u);
            at += 16u + qwc * 16u;
        }
        else if (id == 5u)
        {
            appendFrom(out, at + 16u, qwc * 16u);
            if ((addr & 0x70000000u) || !gatherCallee(addr, out, 1))
                return false;
            at += 16u + qwc * 16u;
        }
        else if (id == 3u || id == 4u)
        {
            if (addr & 0x70000000u)
                return false;
            appendFrom(out, addr, qwc * 16u);
            at += 16u;
        }
        else
            return false;
    }
    return at == to;
}

// The stream's DIRECT payloads (the GS state it sets), as the VIF reads its
// codes (UNPACK's size by STCYCL, as the runtime's VIF1 has it).
template <typename Visit> void eachDirect(const std::vector<uint8_t> &bytes, Visit &&visit)
{
    uint32_t cl = 1u, wl = 1u;
    size_t pos = 0u;
    while (pos + 4u <= bytes.size())
    {
        uint32_t cmd;
        std::memcpy(&cmd, bytes.data() + pos, 4u);
        pos += 4u;
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
        if (op == 0x01u)
        {
            cl = std::max(imm & 0xFFu, 1u);
            wl = std::max((imm >> 8) & 0xFFu, 1u);
        }
        else if (op == 0x20u)
            pos += 4u;
        else if (op == 0x30u || op == 0x31u)
            pos += 16u;
        else if (op == 0x4Au)
            pos += (num == 0u ? 256u : num) * 8u;
        else if (op == 0x50u || op == 0x51u)
        {
            const size_t n = (imm == 0u ? 65536u : imm) * 16u;
            if (pos + n <= bytes.size())
                visit(bytes.data() + pos, static_cast<uint32_t>(n));
            pos += n;
        }
        else if ((op & 0x60u) == 0x60u)
        {
            const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, write = num == 0u ? 256u : num;
            const uint32_t bits = (vl == 3u && vn == 3u) ? 16u : (vn + 1u) * (vl == 3u ? 16u : (32u >> vl));
            uint32_t source = write;
            if (cl < wl)
                source = (write / wl) * cl + std::min(write % wl, cl);
            pos += ((source * ((bits + 7u) / 8u)) + 3u) & ~3u;
        }
    }
}

// The VU1 state the list puts in quads 0-59 (the view packet's 0-19, the
// camera's 24-28, the lights' 30-51), followed on the game thread as the
// list is written: before a marker, the list from where it was last read
// to its cursor (CALLs followed, as the DMA would) and each absolute V4-32
// UNPACK into those quads applied. DrawListFrameBegin starts the reading
// over at the frame's list; VU1 memory keeps the last frame's meanwhile.
Globals g_globals;
uint32_t g_listRead = 0u, g_listDctx = 0u;
std::atomic<uint64_t> g_globalsTaken{0u}; // view, camera and light CALLs taken out

void applyGlobals(const std::vector<uint8_t> &bytes)
{
    size_t pos = 0u;
    uint32_t cl = 1u, wl = 1u;
    while (pos + 4u <= bytes.size())
    {
        uint32_t cmd;
        std::memcpy(&cmd, bytes.data() + pos, 4u);
        pos += 4u;
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
        if (op == 0x01u)
        {
            cl = std::max(imm & 0xFFu, 1u);
            wl = std::max((imm >> 8) & 0xFFu, 1u);
        }
        else if (op == 0x20u)
            pos += 4u;
        else if (op == 0x30u || op == 0x31u)
            pos += 16u;
        else if (op == 0x4Au)
            pos += (num == 0u ? 256u : num) * 8u;
        else if (op == 0x50u || op == 0x51u)
            pos += (imm == 0u ? 65536u : imm) * 16u;
        else if ((op & 0x60u) == 0x60u)
        {
            const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, write = num == 0u ? 256u : num;
            const uint32_t bits = (vl == 3u && vn == 3u) ? 16u : (vn + 1u) * (vl == 3u ? 16u : (32u >> vl));
            uint32_t source = write;
            if (cl < wl)
                source = (write / wl) * cl + std::min(write % wl, cl);
            const size_t size = ((source * ((bits + 7u) / 8u)) + 3u) & ~size_t(3u);
            // V4-32 to an absolute address, no mask, one quad a vector.
            if ((op & 0x7Fu & ~0x10u) == 0x6Cu && !(op & 0x10u) && !(imm & 0x8000u) && cl == wl &&
                pos + size <= bytes.size())
                for (uint32_t i = 0; i < write; ++i)
                {
                    const uint32_t q = (imm & 0x3FFu) + i;
                    if (q >= 60u)
                        break;
                    std::memcpy(g_globals.quads[q].data(), bytes.data() + pos + i * 16u, 16u);
                    g_globals.known |= 1ull << q;
                }
            pos += size;
        }
    }
}

// Whether a CALLed packet only sets that VU1 state (the view, camera and
// light packets): absolute V4-32 UNPACKs into quads 0-59, and NOP, STCYCL,
// FLUSH.
bool globalsOnly(const std::vector<uint8_t> &bytes)
{
    size_t pos = 0u;
    bool any = false;
    while (pos + 4u <= bytes.size())
    {
        uint32_t cmd;
        std::memcpy(&cmd, bytes.data() + pos, 4u);
        pos += 4u;
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
        if (op == 0x00u || op == 0x01u || op == 0x10u || op == 0x11u || op == 0x13u)
            continue;
        const uint32_t n = num == 0u ? 256u : num;
        if (op != 0x6Cu || (imm & 0x8000u) || (imm & 0x3FFu) + n > 60u || pos + n * 16u > bytes.size())
            return false;
        pos += n * 16u;
        any = true;
    }
    return any;
}

// The list written since it was last read, up to the cursor. The view,
// camera and light packets' CALLs are taken out of it (an empty cnt in
// their place): records and streams carry that state.
void syncGlobals()
{
    const uint32_t dctx = read32(kDrawCtxPointer);
    if (dctx == 0u)
        return;
    const uint32_t cursor = read32(dctx + 4u);
    if (dctx != g_listDctx || g_listRead == 0u || cursor < g_listRead || cursor - g_listRead > (4u << 20))
    {
        g_listDctx = dctx;
        g_listRead = cursor; // (nothing known to read from: from here)
        return;
    }
    thread_local std::vector<uint8_t> bytes;
    bytes.clear();
    uint32_t at = g_listRead;
    while (at < cursor)
    {
        const uint64_t tag = read64(at);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>((tag >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        appendFrom(bytes, at + 8u, 8u);
        if (id == 1u)
        {
            appendFrom(bytes, at + 16u, qwc * 16u);
            at += 16u + qwc * 16u;
        }
        else if (id == 5u)
        {
            appendFrom(bytes, at + 16u, qwc * 16u);
            thread_local std::vector<uint8_t> callee;
            callee.clear();
            if (!(addr & 0x70000000u) && gatherCallee(addr, callee, 1))
            {
                if (qwc == 0u && globalsOnly(callee))
                {
                    applyGlobals(bytes);
                    bytes.clear();
                    applyGlobals(callee);
                    write32(at, 0x10000000u); // cnt, no data, NOP NOP
                    write32(at + 4u, 0u);
                    write32(at + 8u, 0u);
                    write32(at + 12u, 0u);
                    ++g_globalsTaken;
                }
                else
                    bytes.insert(bytes.end(), callee.begin(), callee.end());
            }
            at += 16u + qwc * 16u;
        }
        else if ((id == 3u || id == 4u) && !(addr & 0x70000000u))
        {
            appendFrom(bytes, addr, qwc * 16u);
            at += 16u;
        }
        else
            break; // (the list goes on elsewhere: read from the cursor on)
    }
    applyGlobals(bytes);
    g_listRead = cursor;
}

Globals currentGlobals()
{
    return g_globals;
}

// Where an emitter's call began: the list's cursor, and the snapshot of the
// game thread's state there.
struct CaptureStart
{
    uint32_t dctx = 0u, cursor = 0u;
};
std::vector<CaptureStart> g_captureStarts;

CaptureStart beginCapture()
{
    syncGlobals(); // (what the list holds before the emitter's own stretch)
    CaptureStart start;
    start.dctx = read32(kDrawCtxPointer);
    start.cursor = start.dctx != 0u ? read32(start.dctx + 4u) : 0u;
    return start;
}

// The emitter's stretch taken out of the list and kept under a marker.
void endCapture(const CaptureStart &start)
{
    if (start.dctx == 0u || start.dctx != read32(kDrawCtxPointer))
        return;
    const uint32_t end = read32(start.dctx + 4u);
    if (end <= start.cursor)
        return; // (nothing written)
    Stream stream;
    if (end - start.cursor > (1u << 20) || !gatherStretch(start.cursor, end, stream.bytes))
    {
        ++g_captureFailed;
        return; // (left in the list as written)
    }
    applyDrawEnv();
    stream.regs = rg::drawstate::game().registers();
    stream.globals = g_globals;
    // The stretch's own view, camera and light packets (it replays them).
    applyGlobals(stream.bytes);
    // The GS state it sets, on the game thread's as where it was written.
    eachDirect(stream.bytes, [](const uint8_t *data, uint32_t size) { rg::drawstate::game().observe(data, size); });
    write32(start.dctx + 4u, start.cursor);
    g_listRead = start.cursor;
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    stream.unit = g_units | (kStream << 24);
    if (!rg::nativefx::writeMarker(stream.unit))
    {
        write32(start.dctx + 4u, end); // (no room: as it was)
        ++g_captureFailed;
        return;
    }
    ++g_captured;
    std::lock_guard lock(g_mutex);
    if (g_streams.size() >= 4096u)
        g_streams.pop_front();
    g_streams.push_back(std::move(stream));
}

void afterCaptured(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_captureStarts.empty())
        return;
    const CaptureStart start = g_captureStarts.back();
    g_captureStarts.pop_back();
    endCapture(start);
}

// The emitter's original, its output captured.
void callCaptured(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_captureStarts.push_back(beginCapture());
    rg::hooks::callThen(original, rdram, ctx, runtime, &afterCaptured, 0u);
}

PS2Runtime::RecompiledFunction g_effModelOriginal = nullptr;
PS2Runtime::RecompiledFunction g_effQuadOriginal = nullptr;
void effModel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    callCaptured(g_effModelOriginal, rdram, ctx, runtime);
}
void effQuad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    callCaptured(g_effQuadOriginal, rdram, ctx, runtime);
}

PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;
PS2Runtime::RecompiledFunction g_casterOriginal = nullptr;
// The call's request (a2), for after it (game thread; the emitters do not
// nest outside effects, which are not followed).
uint32_t t_pendingReq = 0u, t_pendingDctx = 0u;
bool t_pendingCaster = false;

// The call's ModelMesh items' chunks rebuilt as the emitter would (their
// GetGroupPkts into the frame's current buffer, the quadwords it wrote
// allocated there), so their groups' chains hold this frame's chunks.
// False: one could not be (no buffer, no room, the method did not finish).
bool rebuildMeshes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dctx, uint32_t req)
{
    const uint32_t key = read32(req + 0x40u);
    const uint32_t meshes = read32(read32(req + 0x0Cu) + 4u);
    const uint32_t count = read32(read32(req + 0x08u));
    if (count > 2048u)
        return false;
    uint32_t node = read32(req + 0x44u);
    for (int guard = 0; node != 0u && guard < 4096; ++guard, node = read32(node))
    {
        const uint32_t it = node + 0x10u;
        if (key != 0u && read32(it) != key)
            continue;
        if (read32(it + 0x14u) == 0u || read32(it + 0x18u) == 0u)
            continue;
        const uint32_t mesh = read32(meshes + 4u * read32(it + 0x08u));
        if (mesh == 0u || read32(mesh + 0x50u) != kMesh)
            continue;
        // DrawListGetCurBuf: the current buffer's memstack, its top.
        const uint32_t t = read32(dctx + 8u + 4u * read32(dctx));
        if (t == 0u)
            return false;
        const uint32_t pk = t + 8u;
        if (read32(pk + 0x20u) != 0u)
            return false;
        const uint32_t cur = read32(pk + 0x28u) + read32(pk + 0x2Cu) * 16u;
        // The method's arrays (materials and chains, -1 ended) below the
        // guest's stack, and its own frame below them.
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t arrays = (sp - 0x4000u) & ~15u;
        R5900Context local = *ctx;
        const auto set = [&](int r, uint32_t v) { local.r[r] = _mm_set_epi64x(0, int64_t(int32_t(v))); };
        set(4, mesh);
        set(5, cur);
        set(6, arrays);
        set(7, arrays + 0x2000u);
        set(8, count);
        set(29, (sp - 0x8000u) & ~15u);
        const uint32_t entry = read32(read32(mesh + 0x50u) + 0x18u);
        PS2Runtime::RecompiledFunction method = runtime->lookupFunction(entry);
        if (!method || !rg::hooks::runToEnd(method, entry, rdram, local, runtime))
            return false;
        const uint32_t qwc = getRegU32(&local, 2);
        // MemStackAllocQw(buffer, qwc).
        if (read32(pk + 0x2Cu) + qwc > read32(pk + 0x34u))
            return false;
        write32(pk + 0x2Cu, read32(pk + 0x2Cu) + qwc);
    }
    return true;
}

void emitWith(PS2Runtime::RecompiledFunction original, bool caster, uint8_t *rdram, R5900Context *ctx,
              PS2Runtime *runtime)
{
    // Inside an effect's call (the composite's parts, drawn into its layer)
    // the call is written as the game writes it, and captured.
    if (rg::nativefx::inEffectCall() || (caster && read32(getRegU32(ctx, 5) + 0x3B90u) == 0u))
    {
        callCaptured(original, rdram, ctx, runtime);
        return;
    }
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    // A call of trusted meshes: drawn from its record, nothing written (the
    // emitter returns 0 quadwords; its caches are left as they are).
    {
        Call call;
        call.unit = g_units | (kUnwritten << 24);
        predict(getRegU32(ctx, 5), getRegU32(ctx, 6), call, caster, true);
        // ModelMesh items: their chunks rebuilt first, the record read again.
        bool rebuilt = true;
        for (const Item &item : call.items)
            if (read32(item.mesh + 0x50u) == kMesh)
            {
                rebuilt = rebuildMeshes(rdram, ctx, runtime, getRegU32(ctx, 5), getRegU32(ctx, 6));
                call.items.clear();
                predict(getRegU32(ctx, 5), getRegU32(ctx, 6), call, caster, true);
                break;
            }
        const Why why = rebuilt ? whyWritten(call) : kRebuild;
        ++g_why[why];
        if (why == kDrawable && rg::nativefx::writeMarker(call.unit))
        {
            ++g_unwrittenCalls;
            captureState(call);
            {
                std::lock_guard lock(g_mutex);
                if (g_calls.size() >= 4096u)
                    g_calls.pop_front();
                g_calls.push_back(std::move(call));
            }
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    ++g_writtenCalls;
    callCaptured(original, rdram, ctx, runtime);
}

void emit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    emitWith(g_emitOriginal, false, rdram, ctx, runtime);
}

// The sorted path. At the queue: each item's groups as single-node
// records, by (the item's p14, the group's chain), which the node carries
// to its replay (its header's third entry, its chain).
PS2Runtime::RecompiledFunction g_queueOriginal = nullptr;
PS2Runtime::RecompiledFunction g_bucketOriginal = nullptr;
std::unordered_map<uint64_t, Call> g_sorted;

void afterQueue(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    Call call;
    predict(t_pendingDctx, t_pendingReq, call);
    if (g_sorted.size() > 65536u)
        g_sorted.clear();
    for (Item &item : call.items)
        for (Group &group : item.groups)
        {
            Call node;
            node.single = true;
            Item one = item;
            one.groups = {group};
            node.items.push_back(std::move(one));
            g_sorted[(uint64_t(item.p14 & 0x0FFFFFFFu) << 32) | (group.chain & 0x0FFFFFFFu)] = std::move(node);
        }
}

void queueSorted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (rg::nativefx::inEffectCall())
    {
        g_queueOriginal(rdram, ctx, runtime);
        return;
    }
    t_pendingReq = getRegU32(ctx, 6);
    t_pendingDctx = getRegU32(ctx, 5);
    rg::hooks::callThen(g_queueOriginal, rdram, ctx, runtime, &afterQueue, 0u);
}

uint32_t nextUnit(uint32_t kind)
{
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    return g_units | (kind << 24);
}

void keepCall(Call &&call)
{
    std::lock_guard lock(g_mutex);
    if (g_calls.size() >= 4096u)
        g_calls.pop_front();
    g_calls.push_back(std::move(call));
}

// DrawListEmitBucket(dctx, bucket), as the game writes it (RGDecomp's C
// and the disassembly), with each node it knows a record of: compared (its
// output between markers) or, its mesh trusted, drawn from the record (a
// marker in place of its program, texenv, MSCAL and chain CALLs; the
// header's changed entries, view, lights and the item's packets, still
// CALLed as the game would).
void emitBucket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t o = getRegU32(ctx, 4);
    const int32_t idx = static_cast<int32_t>(getRegU32(ctx, 5));
    const uint32_t t = read32(o + 8u + 4u * read32(o));
    if (rg::nativefx::inEffectCall() || t == 0u || g_sorted.empty())
    {
        callCaptured(g_bucketOriginal, rdram, ctx, runtime);
        return;
    }
    const auto done = [&](int32_t value) {
        setReturnS32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    };
    if (idx < 0 || idx >= static_cast<int32_t>(read32(o + 0x3BBCu)))
        return done(0);
    const uint32_t bucket = read32(read32(o + 0x3BC0u) + 4u * uint32_t(idx));
    if (bucket == 0u)
        return done(1);
    const uint32_t pk = t + 8u;
    const uint32_t hdr = read32(pk + 0x28u) + read32(pk + 0x2Cu) * 16u;
    write32(hdr, 0x60000000u);
    write32(hdr + 4u, 0u);
    write32(hdr + 8u, 0x14000064u); // MSCAL 0x64
    write32(hdr + 12u, 0u);
    const uint32_t ha = hdr & 0x0FFFFFFFu;
    const auto call = [&](uint32_t to) {
        const uint32_t cur = read32(o + 4u);
        write32(cur, 0x50000000u);
        write32(cur + 4u, to & 0x0FFFFFFFu);
        write32(cur + 8u, 0u);
        write32(cur + 12u, 0u);
        write32(o + 4u, cur + 16u);
    };
    uint32_t last = 0u, prev = 0u;
    const int32_t count = static_cast<int32_t>(read32(read32(o + 0x3BC8u) + 4u * uint32_t(idx)));
    uint32_t pp = bucket + 4u * uint32_t(count - 1);
    for (int32_t i = 0; i < count; ++i, pp -= 4u)
    {
        const uint32_t e = read32(pp);
        const uint32_t n = read32(e + 0x0Cu);
        uint32_t src = read32(e + 0x10u);
        if (prev == 0u || n != read32(prev + 0x0Cu))
        {
            for (uint32_t j = 0; j < n; ++j)
                call(read32(src + 4u * j));
        }
        else
        {
            const uint32_t psrc = read32(prev + 0x10u);
            if (psrc != src)
                for (uint32_t j = 0; j < n; ++j)
                    if (read32(src + 4u * j) != read32(psrc + 4u * j))
                        call(read32(src + 4u * j));
        }
        prev = e;
        // Its record, by its item's p14 and its chain.
        const uint32_t chain = read32(e + 0x20u);
        const auto found = n >= 3u ? g_sorted.find((uint64_t(read32(src + 8u) & 0x0FFFFFFFu) << 32) |
                                                   (chain & 0x0FFFFFFFu))
                                   : g_sorted.end();
        if (found != g_sorted.end() && drawable(found->second))
        {
            Call node = found->second;
            node.unit = nextUnit(kUnwritten);
            if (rg::nativefx::writeMarker(node.unit))
            {
                ++g_unwrittenCalls;
                captureState(node);
                keepCall(std::move(node));
                // Its record's state writes stand between: the next written
                // node CALLs its texenv again.
                last = 0u;
                continue;
            }
        }
        // A node written as the game writes it, captured (its item's header
        // CALLs above stay in the list).
        const CaptureStart written = beginCapture();
        // DrawCtxAddChangedCallTags(dctx, e+0x18, e+0x14): six slots, a CALL
        // for each that differs from the cache at dctx+0x3C48.
        const uint32_t programs = read32(e + 0x18u), slots = read32(e + 0x14u);
        if (slots == 6u)
        {
            for (uint32_t k = 0; k < 6u; ++k)
            {
                const uint32_t ptr = read32(programs + 4u * k);
                if (read32(o + 0x3C48u + 4u * k) != ptr)
                {
                    call(ptr);
                    write32(o + 0x3C48u + 4u * k, ptr);
                }
            }
        }
        const uint32_t texenv = read32(e + 0x1Cu);
        if (last != texenv)
        {
            last = texenv;
            call(last);
        }
        call(ha);
        call(chain);
        endCapture(written);
    }
    // MemStackAllocQw(pk, 1): the RET+MSCAL quadword.
    if (read32(pk + 0x20u) == 0u && read32(pk + 0x2Cu) + 1u <= read32(pk + 0x34u))
        write32(pk + 0x2Cu, read32(pk + 0x2Cu) + 1u);
    done(1);
}

void emitCaster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    emitWith(g_casterOriginal, true, rdram, ctx, runtime);
}

void report()
{
    // Every ten seconds (the list's run, at a record's or a stream's marker).
    static auto next = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t n = g_unwrittenCalls.load();
    if (now >= next)
    {
        next = now + std::chrono::seconds(10);
        std::string why;
        for (uint32_t k = kDrawable + 1u; k < kWhyCount; ++k)
            if (const uint64_t c = g_why[k].load())
                why += std::string(" ") + kWhyNames[k] + "=" + std::to_string(c);
        std::fprintf(stderr,
                     "[model-emit] model draw calls drawn from the game's structures %llu, written by the game %llu "
                     "(emit calls by why:%s), records not found %llu; emitters' output captured %llu (not %llu); "
                     "view, camera and light packets taken out of the list %llu\n",
                     (unsigned long long)n, (unsigned long long)g_writtenCalls.load(),
                     why.empty() ? " none" : why.c_str(), (unsigned long long)g_unknown.load(),
                     (unsigned long long)g_captured.load(), (unsigned long long)g_captureFailed.load(),
                     (unsigned long long)g_globalsTaken.load());
    }
}
} // namespace

void install(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kModelEmitDrawPackets, &emit, g_emitOriginal) ||
        !rg::timing::hookFunction(runtime, kModelEmitShadowCaster, &emitCaster, g_casterOriginal) ||
        !rg::timing::hookFunction(runtime, kQueueSorted, &queueSorted, g_queueOriginal) ||
        !rg::timing::hookFunction(runtime, kEmitBucket, &emitBucket, g_bucketOriginal) ||
        !rg::timing::hookFunction(runtime, kEffModelDrawNodes, &effModel, g_effModelOriginal) ||
        !rg::timing::hookFunction(runtime, kEffQuadDrawNodes, &effQuad, g_effQuadOriginal))
        std::fprintf(stderr, "[model-emit] the model emitters not all hooked\n");
}

bool takeUnwritten(uint32_t unit, Call &out)
{
    std::lock_guard lock(g_mutex);
    for (auto it = g_calls.begin(); it != g_calls.end(); ++it)
        if (it->unit == unit)
        {
            out = std::move(*it);
            g_calls.erase(it);
            return true;
        }
    ++g_unknown;
    return false;
}

void listBegun(uint32_t start)
{
    g_listDctx = read32(kDrawCtxPointer);
    g_listRead = start;
}

bool takeStream(uint32_t unit, std::vector<uint8_t> &bytes, rg::drawstate::Registers &regs, Globals &globals)
{
    std::lock_guard lock(g_mutex);
    for (auto it = g_streams.begin(); it != g_streams.end(); ++it)
        if (it->unit == unit)
        {
            bytes = std::move(it->bytes);
            regs = it->regs;
            globals = it->globals;
            report();
            g_streams.erase(it);
            return true;
        }
    ++g_unknown;
    return false;
}

bool marker(uint32_t unit, bool &unwritten)
{
    unwritten = (unit >> 24) == kUnwritten;
    if (unwritten)
        report();
    return unwritten;
}

} // namespace rg::nativeemit
