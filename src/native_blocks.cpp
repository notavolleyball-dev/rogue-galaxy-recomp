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
// GsPktBegin(b) and GsPktEnd(b) -> the block's first CNT tag (0: none): a
// block's own bounds, whoever frames it (DrawListBeginPrim/EndPrim, or the
// functions that inline them: GfxBlitTexRect, GfxEndFrame, ...).
constexpr uint32_t kGsPktBegin = 0x0013F7E0u;
constexpr uint32_t kGsPktEnd = 0x0013F790u;
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
// The draw environments written after the block's first segment
// (DrawListEndPrim's restore packet, and what a function that frames its
// block itself writes after it), for after the block's own bytes: the
// first ends the block; and whether a block ended inside another.
std::vector<std::pair<uint32_t, uint32_t>> g_envs; // address, quadwords
bool g_nested = false;

// A block's pieces, kept by unit until the VIF1 thread reaches the block.
struct KeptPiece
{
    rg::NativeMesh mesh;
    std::vector<uint32_t> indices;
    bool texels = false;
    rg::DrawState state{};
    uint8_t layer = 0u;
};
struct Block
{
    uint32_t unit = 0u;
    std::vector<KeptPiece> pieces;
    // A block the game did not write (its packets never reach the draw
    // stage): its register writes, in order, for the stage's state.
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    bool unwritten = false;
};
std::mutex g_mutex;
std::deque<Block> g_blocks;
uint32_t g_units = 0u;
std::atomic<uint64_t> g_kept{0u}, g_emitted{0u}, g_unknown{0u};
// Blocks drawn from their shadow (the game wrote none of them), and those
// lost (a block written through something the shadow does not follow).
std::atomic<uint64_t> g_unwritten{0u}, g_lost{0u}, g_discarded{0u};

// The blocks open (they nest): their owners, whether skipped, and whether
// DrawListEndPrim's hook already made the block.
struct OpenBlock
{
    uint32_t owner = 0u;
    bool skip = false;
    bool made = false;
    bool effect = false; // inside a pass's call (its pass draws it)
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
    if (g_depth > 0 && rg::gspkt::opened())
    {
        // After the block's segments: once its bytes are read.
        g_envs.emplace_back(at, qwords);
        return;
    }
    observeEnvironment(at, qwords);
}

void drawEnvWritePacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_envOriginal, rdram, ctx, runtime, &afterEnv, getRegU32(ctx, 5));
}

PS2Runtime::RecompiledFunction g_beginOriginal = nullptr;
PS2Runtime::RecompiledFunction g_pktBeginOriginal = nullptr;
PS2Runtime::RecompiledFunction g_pktEndOriginal = nullptr;
// The owner DrawListBeginPrim's caller gives the block GsPktBegin opens next.
uint32_t g_pendingOwner = 0u;

// DrawListBeginPrim: its caller is what owns the block (the hud's, the
// movie's, ...), not DrawListBeginPrim itself.
void beginPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_pendingOwner = getRegU32(ctx, 31);
    g_beginOriginal(rdram, ctx, runtime);
}

// GsPktBegin: a block opens, its owner DrawListBeginPrim's caller or, for a
// function that frames its block itself, the call site.
void pktBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t owner = g_pendingOwner != 0u ? g_pendingOwner : getRegU32(ctx, 31);
    g_pendingOwner = 0u;
    if (g_depth > 0)
        g_nested = true;
    ++g_depth;
    // Every block is drawn from its shadow and the game writes none of it.
    // A block inside a pass effect's call is not drawn: the effect's pass
    // draws it. A composite's head's blocks are drawn (into its layer), its
    // tail's are its passes'.
    const bool discard =
        rg::nativefx::inCompositeCall() ? rg::nativefx::compositeTail() : rg::nativefx::inEffectCall();
    g_open.push_back({owner, true, false, discard});
    rg::gspkt::beginBlock(discard);
    g_envs.clear();
    g_blockOwner = owner;
    g_pktBeginOriginal(rdram, ctx, runtime);
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

// A composite head's clear: the box its untextured sprites cover in the
// buffer they draw into (all one, CT32), in pixels.
bool clearBox(const Block &block, rg::nativefx::Box &box)
{
    bool found = false;
    for (const KeptPiece &piece : block.pieces)
    {
        const rg::DrawState &st = piece.state;
        if (piece.texels || st.prim.tme || st.prim.type != rg::kPrimSprite || piece.mesh.vertices.empty())
            continue;
        const uint32_t base = st.context.frame.fbp << 5, fbw = st.context.frame.fbw;
        if (st.context.frame.psm != 0u || fbw == 0u || (found && (base != box.base || fbw != box.fbw)))
            return false;
        const int32_t ox = st.context.xyoffset.ofx, oy = st.context.xyoffset.ofy;
        for (const rg::NativeMesh::Vertex &v : piece.mesh.vertices)
        {
            const int32_t x = int32_t(vertexX(piece.mesh, v) * 16.0f) - ox;
            const int32_t y = int32_t(vertexY(piece.mesh, v) * 16.0f) - oy;
            const int32_t x0 = x >> 4, y0 = y >> 4, x1 = (x + 15) >> 4, y1 = (y + 15) >> 4;
            if (!found)
                box = rg::nativefx::Box{base, fbw, x0, y0, x1, y1};
            box.x0 = std::min(box.x0, x0);
            box.y0 = std::min(box.y0, y0);
            box.x1 = std::max(box.x1, x1);
            box.y1 = std::max(box.y1, y1);
            found = true;
        }
    }
    return found;
}

// A block the game did not write (skipped): made from the shadow before
// DrawListEndPrim runs, and a marker put in the list where its CALL would
// have been (before the restore packet's).
void makeSkipped()
{
    thread_local std::vector<uint8_t> shadow;
    const bool whole = rg::gspkt::endBlock(shadow);
    if (!whole)
    {
        // Written through something the shadow does not follow (a raw
        // write, an outer block reset by an inner one): what was skipped of
        // it is lost.
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
        ++g_lost;
        return;
    }
    if (block.pieces.empty() && block.writes.empty())
        return;
    // A composite's head: its layer begins at its clear.
    rg::nativefx::Box box;
    if (rg::nativefx::inCompositeCall() && !rg::nativefx::compositeTail() && clearBox(block, box))
        rg::nativefx::compositeBegin(box);
    const uint32_t unit = keepBlock(block);
    if (rg::nativefx::writeMarker(unit))
        ++g_unwritten;
    else
        ++g_lost;
}

void afterEnd(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    const OpenBlock open = g_open.empty() ? OpenBlock{} : g_open.back();
    if (!g_open.empty())
        g_open.pop_back();
    if (!open.made)
    {
        thread_local std::vector<uint8_t> unused;
        rg::gspkt::endBlock(unused); // (an effect's block: written, not followed)
    }
    --g_depth;
    thread_local std::vector<std::pair<uint32_t, uint32_t>> envs;
    envs.swap(g_envs);
    g_envs.clear();
    if (g_depth <= 0)
    {
        g_depth = 0;
        g_nested = false;
    }
    (void)ctx;
    for (const auto &[at, qwords] : envs)
        observeEnvironment(at, qwords);
}

// GsPktEnd: a skipped block's 2D made from the shadow and its marker put
// where its CALL would have gone (the caller writes the CALLs after this);
// then, after it, a written block read.
void pktEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_open.empty() && g_open.back().skip && g_open.back().effect)
    {
        // A pass effect's block: not written, not drawn (its pass is).
        thread_local std::vector<uint8_t> unused;
        rg::gspkt::endBlock(unused);
        ++g_discarded;
        g_open.back().made = true;
    }
    else if (!g_open.empty() && g_open.back().skip && g_state)
    {
        g_blockOwner = g_open.back().owner;
        makeSkipped();
        g_open.back().made = true;
    }
    rg::hooks::callThen(g_pktEndOriginal, rdram, ctx, runtime, &afterEnd, 0u);
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

thread_local Block t_last; // the block emit() sent (its storage kept)
} // namespace

// A fogged piece's clip is (x w, y w, 0, w), w its z (native_2d's closeMesh).
bool fogged(const rg::NativeMesh &mesh)
{
    return mesh.header.matrix[3][3] == 0.0f && mesh.header.matrix[2][3] == 1.0f;
}

void transform(rg::NativeMesh &mesh, float sx, float sy, float tx, float ty)
{
    const bool fog = fogged(mesh);
    for (rg::NativeMesh::Vertex &v : mesh.vertices)
    {
        const float w = fog ? v.z : 1.0f;
        v.x = sx * v.x + tx * w;
        v.y = sy * v.y + ty * w;
    }
}

float vertexX(const rg::NativeMesh &mesh, const rg::NativeMesh::Vertex &v)
{
    return fogged(mesh) && v.z != 0.0f ? v.x / v.z : v.x;
}

float vertexY(const rg::NativeMesh &mesh, const rg::NativeMesh::Vertex &v)
{
    return fogged(mesh) && v.z != 0.0f ? v.y / v.z : v.y;
}

bool enabled()
{
    return true; // (RG_NATIVE_BLOCKS went in M6: no converter takes them on the list)
}

void install(PS2Runtime &runtime)
{
    if (!enabled())
        return;
    g_rdram = runtime.memory().getRDRAM();
    g_state = &rg::drawstate::game();
    if (!rg::timing::hookFunction(runtime, kDrawEnvWritePacket, &drawEnvWritePacket, g_envOriginal) ||
        !rg::timing::hookFunction(runtime, kDrawListBeginPrim, &beginPrim, g_beginOriginal) ||
        !rg::timing::hookFunction(runtime, kGsPktBegin, &pktBegin, g_pktBeginOriginal) ||
        !rg::timing::hookFunction(runtime, kGsPktEnd, &pktEnd, g_pktEndOriginal))
    {
        std::fprintf(stderr, "[native-blocks] not hooked: the game's 2D is written and not drawn\n");
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
    const uint64_t emitted = ++g_emitted;
    if (emitted == 1u || emitted % 60000u == 0u)
        std::fprintf(stderr,
                     "[native-blocks] blocks drawn from the builder's calls %llu (made %llu, not found %llu, lost "
                     "%llu), effects' blocks left to their passes %llu\n",
                     (unsigned long long)emitted, (unsigned long long)g_kept.load(),
                     (unsigned long long)g_unknown.load(), (unsigned long long)g_lost.load(),
                     (unsigned long long)g_discarded.load());
    return true;
}


} // namespace rg::nativeblocks
