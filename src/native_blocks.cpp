#include "native_blocks.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "draw_state_mirror.h"
#include "hud_size.h"
#include "native_2d.h"
#include "native_effects.h"
#include "native_gspkt.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"
#include "widescreen.h"

namespace rg::nativeblocks
{
namespace
{
constexpr uint32_t kDrawListBeginPrim = 0x00141B00u;
constexpr uint32_t kDrawListEndPrim = 0x00141A20u;
// DrawEnvWritePacket_157460(env, at): the draw environment's packet written
// at `at`, its size in quadwords returned.
constexpr uint32_t kDrawEnvWritePacket = 0x00157460u;

uint8_t *g_rdram = nullptr;
bool g_on = false;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

// The game thread's draw state (the draw environments and the blocks, in
// the order the game writes them), and the blocks being written (they
// nest: DrawListAddTexLoad can run TexAnime's block inside another).
rg::drawstate::Tracker *g_state = nullptr;
int g_depth = 0;
uint32_t g_blockOwner = 0u; // DrawListBeginPrim's return address
// DrawListEndPrim's restore packet (DrawEnvWritePacket inside it), for
// after the block's own bytes; and whether a block ended inside another.
uint32_t g_envAt = 0u, g_envQwords = 0u;
bool g_nested = false;
// The draw context and its builder (NATIVE-ONLY-NOTES.md, "The GsPkt
// builder"): +0xF0 the first GsPktOpen's CNT tag since GsPktBegin.
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;
constexpr uint32_t kBuilder = 0x130u;
constexpr uint32_t kBuilderFirstOpen = 0xF0u;

// A block's pieces, kept by unit until the VIF1 thread reaches the block.
struct KeptPiece
{
    GSNativeMesh mesh;
    std::vector<uint32_t> indices;
    bool texels = false;
    GSDrawState state{};
    uint8_t layer = 0u;
};
struct Block
{
    uint32_t unit = 0u;
    std::vector<KeptPiece> pieces;
    GSDrawState end{}; // the game thread's state at its end
    // A block the game did not write (its packets never reach the draw
    // stage): its register writes, in order, for the stage's state.
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    bool unwritten = false;
};
std::mutex g_mutex;
std::deque<Block> g_blocks;
uint32_t g_units = 0u;
std::atomic<uint64_t> g_kept{0u}, g_emitted{0u}, g_unknown{0u}, g_unread{0u}, g_mismatch{0u};
// The builder's shadow against the game's bytes (src/native_gspkt.h): the
// same, differing, and blocks it does not follow.
std::atomic<uint64_t> g_shadowSame{0u}, g_shadowDiffer{0u}, g_shadowNone{0u};
// Blocks the game did not write (their 2D from the shadow), and those lost
// (a skipped block written through something the shadow does not follow).
std::atomic<uint64_t> g_unwritten{0u}, g_lost{0u};

// By owner (DrawListBeginPrim's return address, game thread): its shadows
// that agreed with its bytes, and whether one ever did not. An owner's
// blocks are skipped (written by nobody, drawn from the shadow) once
// kAgreed of its shadows agreed and none differed or went unfollowed.
struct OwnerRecord
{
    uint32_t agreed = 0u;
    bool bad = false;
};
std::unordered_map<uint32_t, OwnerRecord> g_owners;
constexpr uint32_t kAgreed = 3u;
// The blocks open (they nest): their owners, whether skipped, and whether
// DrawListEndPrim's hook already made the block.
struct OpenBlock
{
    uint32_t owner = 0u;
    bool skip = false;
    bool made = false;
};
std::vector<OpenBlock> g_open;

// The GIF packets in a stretch of a list the game wrote: DMA cnt tags, each
// with its two VIFcodes; a DIRECT's data is GIF. False: something else (a
// reference, a call), not read.
template <typename Visit> bool eachDirect(uint32_t start, uint32_t end, Visit &&visit)
{
    uint32_t at = start;
    while (at + 16u <= end)
    {
        uint64_t tag;
        uint32_t codes[2];
        std::memcpy(&tag, g_rdram + physical(at), sizeof(tag));
        std::memcpy(codes, g_rdram + physical(at + 8u), sizeof(codes));
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
        const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u);
        // GsPktEnd's RET (no data) ends the block.
        if (id == 6u && qwc == 0u && at + 16u == end)
            return true;
        if (id != 1u)
            return false;
        const uint32_t payload = at + 16u, bytes = qwc * 16u;
        if (payload + bytes > end)
            return false;
        for (const uint32_t code : codes)
        {
            const uint32_t op = (code >> 24) & 0x7Fu;
            if (op == 0x50u || op == 0x51u)
            {
                const uint32_t direct = std::min<uint32_t>((code & 0xFFFFu) * 16u, bytes);
                if (direct != 0u)
                    visit(g_rdram + physical(payload), direct);
            }
            else if (op != 0x00u && op != 0x10u && op != 0x11u && op != 0x13u)
                return false; // (NOP, FLUSHE, FLUSH, FLUSHA only)
        }
        at = payload + bytes;
    }
    return at == end;
}

// The bytes of a draw environment's packet: its writes into the state.
void observeEnvironment(uint32_t at, uint32_t qwords)
{
    if (!g_state || qwords == 0u || qwords > 256u)
        return;
    eachDirect(at, at + qwords * 16u, [](const uint8_t *data, uint32_t size) { g_state->observe(data, size); });
}

PS2Runtime::RecompiledFunction g_envOriginal = nullptr;

void afterEnv(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t at)
{
    const uint32_t qwords = static_cast<uint32_t>(getRegU32(ctx, 2));
    if (g_depth > 0)
    {
        // DrawListEndPrim's restore: after the block's bytes are read.
        g_envAt = at;
        g_envQwords = qwords;
        return;
    }
    observeEnvironment(at, qwords);
}

void drawEnvWritePacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_envOriginal, rdram, ctx, runtime, &afterEnv, getRegU32(ctx, 5));
}

PS2Runtime::RecompiledFunction g_beginOriginal = nullptr;
PS2Runtime::RecompiledFunction g_endOriginal = nullptr;

void beginPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t owner = getRegU32(ctx, 31);
    if (g_depth > 0)
    {
        g_nested = true;
        // The outer block is reset by this one: never skipped again.
        if (!g_open.empty())
            g_owners[g_open.back().owner].bad = true;
    }
    ++g_depth;
    const auto known = g_owners.find(owner);
    const bool skip = g_depth == 1 && known != g_owners.end() && !known->second.bad &&
                      known->second.agreed >= kAgreed;
    g_open.push_back({owner, skip, false});
    rg::gspkt::beginBlock(skip);
    g_envAt = g_envQwords = 0u;
    g_blockOwner = owner;
    g_beginOriginal(rdram, ctx, runtime);
}

// The block's 2D from its GIF data (the DIRECTs' data in order, or the
// shadow): its pieces with their states, its register writes. False: not
// expressible (left as it is).
template <typename EachSpan> bool buildBlock(EachSpan &&eachSpan, Block &block, bool keepWrites)
{
    thread_local std::vector<rg::native2d::Piece> pieces;
    bool ok = true;
    const uint8_t layer = rg::nativefx::currentLayer();
    const bool read = eachSpan([&](const uint8_t *data, uint32_t size) {
        if (!ok)
            return;
        const rg::native2d::Span span{data, size};
        std::string why;
        if (!rg::native2d::build(&span, 1u, pieces, why))
        {
            ok = false;
            g_state->observe(data, size);
            return;
        }
        for (rg::native2d::Piece &piece : pieces)
        {
            if (piece.isWrites())
            {
                g_state->observeWrites(piece.writes.data(), piece.writes.size());
                if (keepWrites)
                    block.writes.insert(block.writes.end(), piece.writes.begin(), piece.writes.end());
            }
            else if (piece.isPacket())
            {
                g_state->observe(piece.bytes(), piece.size());
                if (keepWrites)
                    ok = false; // (an upload: a GsPkt block has none)
            }
            else
            {
                KeptPiece &kept = block.pieces.emplace_back();
                kept.mesh = std::move(piece.mesh);
                kept.indices = std::move(piece.indices);
                kept.texels = piece.texelUvs;
                kept.state = g_state->current();
                kept.layer = layer;
            }
        }
    });
    return read && ok;
}

// A block made: reshaped, given a unit, kept for the draw stage.
uint32_t keepBlock(Block &block)
{
    // What reshapes the block's 2D (the HUD's sizes, the movie's box on a
    // wide screen), on its records.
    {
        thread_local std::vector<BlockMesh> meshes;
        meshes.clear();
        for (KeptPiece &piece : block.pieces)
            meshes.push_back({&piece.mesh, piece.texels || piece.state.prim.tme != 0u});
        rg::hudsize::reshape(g_blockOwner, meshes);
        rg::widescreen::reshape(g_blockOwner, meshes);
    }
    block.end = g_state->current();
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    block.unit = g_units | (2u << 24);
    const uint32_t unit = block.unit;
    {
        std::lock_guard lock(g_mutex);
        if (g_blocks.size() >= 8192u) // a few frames' worth; an older one was never reached
            g_blocks.pop_front();
        g_blocks.push_back(std::move(block));
    }
    ++g_kept;
    return unit;
}

enum class Shadowed : uint8_t
{
    Same,
    Differ,
    None,
};

// The shadow against the block's own bytes (its DIRECTs' data in order).
Shadowed compareShadow(uint32_t start, uint32_t end, const std::vector<uint8_t> *shadow)
{
    if (!shadow)
    {
        ++g_shadowNone;
        return Shadowed::None;
    }
    thread_local std::vector<uint8_t> game;
    game.clear();
    if (!eachDirect(start, end, [&](const uint8_t *data, uint32_t size) { game.insert(game.end(), data, data + size); }))
        return Shadowed::None;
    if (game == *shadow)
    {
        ++g_shadowSame;
        return Shadowed::Same;
    }
    const uint64_t n = ++g_shadowDiffer;
    if (n > 8u)
        return Shadowed::Differ;
    size_t at = 0u;
    while (at < game.size() && at < shadow->size() && game[at] == (*shadow)[at])
        ++at;
    at &= ~size_t(15u);
    uint64_t g[2] = {}, s[2] = {};
    if (at + 16u <= game.size())
        std::memcpy(g, game.data() + at, 16u);
    if (at + 16u <= shadow->size())
        std::memcpy(s, shadow->data() + at, 16u);
    std::fprintf(stderr,
                 "[native-blocks] the builder's shadow differs from the block (owner %08x) at qword %zu of %zu/%zu: "
                 "game %016llx %016llx, shadow %016llx %016llx\n",
                 g_blockOwner, at / 16u, game.size() / 16u, shadow->size() / 16u, (unsigned long long)g[0],
                 (unsigned long long)g[1], (unsigned long long)s[0], (unsigned long long)s[1]);
    return Shadowed::Differ;
}

// A block the game wrote: its bytes read from its first GsPktOpen to its
// GsPktEnd's RET (the restore packet DrawListEndPrim writes after them is
// observed after them, and stays out of the range), the range marked; its
// owner's record from its shadow. A block not of the shape expected is
// left to the draw stage's converter.
void readBlock(uint32_t start, uint32_t end, const std::vector<uint8_t> *shadow)
{
    const Shadowed shadowed = compareShadow(start, end, shadow);
    OwnerRecord &owner = g_owners[g_blockOwner];
    if (shadowed == Shadowed::Same)
        ++owner.agreed;
    else
        owner.bad = true;
    Block block;
    const bool read = buildBlock(
        [&](auto &&visit) { return eachDirect(start, end, visit); }, block, false);
    if (!read)
    {
        ++g_unread;
        return;
    }
    if (block.pieces.empty())
        return; // nothing drawn: the packets go on as they are
    rg::nativefx::markBlock(start, end, keepBlock(block));
}

// A block the game did not write (skipped): made from the shadow before
// DrawListEndPrim runs, and a marker put in the list where its CALL would
// have been (before the restore packet's).
void makeSkipped()
{
    thread_local std::vector<uint8_t> shadow;
    const bool whole = rg::gspkt::endBlock(shadow);
    OwnerRecord &owner = g_owners[g_blockOwner];
    if (!whole)
    {
        // Written through something the shadow does not follow: lost this
        // once, and the owner's blocks written by the game from now on.
        owner.bad = true;
        ++g_lost;
        return;
    }
    Block block;
    block.unwritten = true;
    const bool built = buildBlock(
        [&](auto &&visit) {
            if (!shadow.empty())
                visit(shadow.data(), static_cast<uint32_t>(shadow.size()));
            return true;
        },
        block, true);
    if (!built)
    {
        owner.bad = true;
        ++g_lost;
        return;
    }
    if (block.pieces.empty() && block.writes.empty())
        return;
    const uint32_t unit = keepBlock(block);
    if (rg::nativefx::writeMarker(unit))
        ++g_unwritten;
    else
        ++g_lost;
}

void afterEnd(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    const OpenBlock open = g_open.empty() ? OpenBlock{} : g_open.back();
    if (!g_open.empty())
        g_open.pop_back();
    thread_local std::vector<uint8_t> shadowBytes;
    const bool shadowed = open.made ? false : rg::gspkt::endBlock(shadowBytes);
    --g_depth;
    const uint32_t envAt = g_envAt, envQwords = g_envQwords;
    g_envAt = g_envQwords = 0u;
    // An outer block that had one end inside it: its first Open was the
    // inner one's (GsPktBegin zeroes it), so its bytes are not read here.
    const bool nested = g_nested && g_depth == 0;
    if (g_depth <= 0)
    {
        g_depth = 0;
        g_nested = false;
    }
    if (!open.made)
    {
        g_blockOwner = open.owner;
        const uint32_t ctx =
            g_rdram ? *reinterpret_cast<const uint32_t *>(g_rdram + physical(kDrawCtxPointer)) : 0u;
        const uint32_t start = ctx != 0u ? *reinterpret_cast<const uint32_t *>(
                                               g_rdram + physical(ctx + kBuilder + kBuilderFirstOpen)) : 0u;
        const uint32_t end = envAt != 0u ? physical(envAt) : 0u;
        if (!nested && g_state && start != 0u && end > physical(start) && end - physical(start) <= (1u << 20))
            readBlock(physical(start), end, shadowed ? &shadowBytes : nullptr);
        else if (start != 0u)
            ++g_unread;
    }
    if (envAt != 0u)
        observeEnvironment(envAt, envQwords);
}

void endPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_open.empty() && g_open.back().skip && g_state)
    {
        g_blockOwner = g_open.back().owner;
        makeSkipped();
        g_open.back().made = true;
    }
    rg::hooks::callThen(g_endOriginal, rdram, ctx, runtime, &afterEnd, 0u);
}

// The kept block of `unit`, taken out (VIF1 thread).
bool take(uint32_t unit, Block &out)
{
    std::lock_guard lock(g_mutex);
    const auto it = std::find_if(g_blocks.begin(), g_blocks.end(), [&](const Block &b) { return b.unit == unit; });
    if (it == g_blocks.end())
        return false;
    out = std::move(*it);
    g_blocks.erase(it);
    return true;
}

thread_local Block t_last; // the block emit() sent, for compare()
} // namespace

// A fogged piece's clip is (x w, y w, 0, w), w its z (native_2d's closeMesh).
bool fogged(const GSNativeMesh &mesh)
{
    return mesh.header.matrix[3][3] == 0.0f && mesh.header.matrix[2][3] == 1.0f;
}

void transform(GSNativeMesh &mesh, float sx, float sy, float tx, float ty)
{
    const bool fog = fogged(mesh);
    for (GSNativeMesh::Vertex &v : mesh.vertices)
    {
        const float w = fog ? v.z : 1.0f;
        v.x = sx * v.x + tx * w;
        v.y = sy * v.y + ty * w;
    }
}

float vertexX(const GSNativeMesh &mesh, const GSNativeMesh::Vertex &v)
{
    return fogged(mesh) && v.z != 0.0f ? v.x / v.z : v.x;
}

float vertexY(const GSNativeMesh &mesh, const GSNativeMesh::Vertex &v)
{
    return fogged(mesh) && v.z != 0.0f ? v.y / v.z : v.y;
}

bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_BLOCKS");
        return !(value && std::strcmp(value, "0") == 0) && rg::native2d::enabled();
    }();
    return on;
}

void install(PS2Runtime &runtime)
{
    if (!enabled())
        return;
    g_rdram = runtime.memory().getRDRAM();
    g_state = new rg::drawstate::Tracker();
    if (!rg::timing::hookFunction(runtime, kDrawEnvWritePacket, &drawEnvWritePacket, g_envOriginal) ||
        !rg::timing::hookFunction(runtime, kDrawListBeginPrim, &beginPrim, g_beginOriginal) ||
        !rg::timing::hookFunction(runtime, kDrawListEndPrim, &endPrim, g_endOriginal))
    {
        std::fprintf(stderr, "[native-blocks] not hooked: the VIF1 thread converts the 2D\n");
        return;
    }
    rg::gspkt::install(runtime);
    g_on = true;
}

bool emit(uint32_t unit)
{
    Block &block = t_last;
    if (!take(unit, block))
    {
        ++g_unknown;
        return false;
    }
    for (const KeptPiece &piece : block.pieces)
        rg::nativeproducer::piece(piece.mesh, &piece.state, piece.texels, piece.layer, piece.indices);
    // A block the game did not write: its register writes, which no packet
    // brings to the draw stage's state.
    if (block.unwritten && !block.writes.empty())
        rg::drawstate::observeWrites(block.writes.data(), block.writes.size());
    const uint64_t emitted = ++g_emitted;
    if (emitted == 1u || emitted % 60000u == 0u)
        std::fprintf(stderr,
                     "[native-blocks] blocks made on the game thread %llu, drawn %llu, not found %llu, not read %llu, "
                     "state differing at a block's end %llu; the builder's shadow: the same %llu, differing %llu, "
                     "not followed %llu; blocks the game did not write %llu, lost %llu\n",
                     (unsigned long long)g_kept.load(), (unsigned long long)emitted,
                     (unsigned long long)g_unknown.load(), (unsigned long long)g_unread.load(),
                     (unsigned long long)g_mismatch.load(), (unsigned long long)g_shadowSame.load(),
                     (unsigned long long)g_shadowDiffer.load(), (unsigned long long)g_shadowNone.load(),
                     (unsigned long long)g_unwritten.load(), (unsigned long long)g_lost.load());
    return true;
}

void compare(uint32_t unit)
{
    const Block &block = t_last;
    if (block.unit != unit)
        return;
    const GSDrawState now = rg::drawstate::current();
    const GSContext &a = now.context, &b = block.end.context;
    const bool same = a.frame.fbp == b.frame.fbp && a.tex0.tbp0 == b.tex0.tbp0 && a.tex0.psm == b.tex0.psm &&
                      a.alpha == b.alpha && a.test == b.test && a.scissor.x1 == b.scissor.x1 &&
                      a.scissor.y1 == b.scissor.y1 && a.xyoffset.ofx == b.xyoffset.ofx;
    if (!same)
    {
        const uint64_t n = ++g_mismatch;
        if (n <= 8u)
            std::fprintf(stderr,
                         "[native-blocks] the VIF1 thread's state differs from the game thread's at a block's end: "
                         "frame %x/%x tex0 %x/%x alpha %llx/%llx test %llx/%llx\n",
                         a.frame.fbp, b.frame.fbp, a.tex0.tbp0, b.tex0.tbp0, (unsigned long long)a.alpha,
                         (unsigned long long)b.alpha, (unsigned long long)a.test, (unsigned long long)b.test);
    }
}

} // namespace rg::nativeblocks
