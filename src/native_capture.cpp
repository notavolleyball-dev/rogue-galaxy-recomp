#include "native_capture.h"
#include "guest_write_watch.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "gfx/native/native_pipeline.h"

#include "native_effects.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"
#include "vif1_thread.h"

namespace rg::nativecapture
{
namespace
{
constexpr uint32_t kTexBuildUploadPkt = 0x00158610u;
// GsBuildImageUploadRefPkt_158390(p, dbp, dpsm, dbw, addr, qwc, dsax, dsay,
// rrw, rrh): an upload of qwc quadwords from addr, by reference (REF tags
// of at most 0x7FF0 each, the first cut to a 128-byte boundary).
constexpr uint32_t kGsBuildImageUploadRefPkt = 0x00158390u;
// GsUploadImage16Blocks_1d27a0(pkt, src, _, w, h): the movie's frame (the
// MPEG decoder's CT32 macroblocks at src, 16x16 each, column by column) as
// a GIF-channel packet of 16x16 uploads by reference to the texture
// *(0x4817A8)'s TBP and TBW.
constexpr uint32_t kGsUploadImage16Blocks = 0x001D27A0u;
constexpr uint32_t kMovieTexture = 0x004817A8u;
// DrawEnvSetFrameFromDesc_144fe0(env, desc, flags).
constexpr uint32_t kSetFrameFromDesc = 0x00144FE0u;
// The getters that fill the frame's and the display buffer's descriptors.
constexpr uint32_t kGetFrameTexDesc = 0x00144CC0u;
constexpr uint32_t kGetDispFbTexDesc = 0x00144A00u;

// The kinds rg::nativeproducer::target takes.
constexpr int kAuto = 0;
constexpr int kFrame = 1;
constexpr int kDisplay = 2;

uint8_t *g_rdram = nullptr;
PS2Runtime::RecompiledFunction g_texBuildUploadOriginal = nullptr;
PS2Runtime::RecompiledFunction g_setFrameFromDescOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getFrameTexDescOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getDispFbTexDescOriginal = nullptr;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

template <typename T> T readAt(uint32_t address)
{
    T value{};
    std::memcpy(&value, g_rdram + physical(address), sizeof(T));
    return value;
}

// A descriptor's TEX0 (+0x38: TBP0 in 14 bits, TBW in 6, PSM in 6), with the
// width and height given, as a target of `kind`.
void declare(uint64_t tex0, int32_t width, int32_t height, int kind)
{
    const uint32_t base = static_cast<uint32_t>(tex0 & 0x3FFFu) & ~31u;
    const uint32_t bufferWidth = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
    rg::nativeproducer::target(base, bufferWidth, width > 0 ? static_cast<uint32_t>(width) : bufferWidth * 64u,
                               height > 0 ? static_cast<uint32_t>(height) : 0u, psm, kind);
}

// DrawEnvSetFrameFromDesc_144fe0(env, desc, flags): when desc is set, the
// draw environment's FRAME becomes the descriptor's TEX0 with its height
// (+4, s16). Every offscreen buffer the effects draw into (CBlur's, the
// bloom's, the glare's, the shadow map, the per-object composites, the
// minimap, the mob preview) comes through here.
void setFrameFromDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t desc = getRegU32(ctx, 5);
    if (desc != 0u)
    {
        declare(readAt<uint64_t>(desc + 0x38u), 0, readAt<int16_t>(desc + 4u), kAuto);
        rg::nativefx::noteFrameDesc(desc);
    }
    g_setFrameFromDescOriginal(rdram, ctx, runtime);
}

// The descriptor a getter filled (its node: w at +2, h at +4, TEX0 at
// +0x38), declared as the frame or the display buffer.
void declareScreen(uint32_t node, int kind)
{
    if (node != 0u)
        declare(readAt<uint64_t>(node + 0x38u), readAt<int16_t>(node + 2u), readAt<int16_t>(node + 4u), kind);
}

void afterGetFrameTexDesc(uint8_t *, R5900Context *, PS2Runtime *, uint32_t node)
{
    declareScreen(node, kFrame);
}

void afterGetDispFbTexDesc(uint8_t *, R5900Context *, PS2Runtime *, uint32_t node)
{
    declareScreen(node, kDisplay);
}

void getFrameTexDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_getFrameTexDescOriginal, rdram, ctx, runtime, &afterGetFrameTexDesc, getRegU32(ctx, 5));
}

void getDispFbTexDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_getDispFbTexDescOriginal, rdram, ctx, runtime, &afterGetDispFbTexDesc, getRegU32(ctx, 5));
}

// TexBuildUploadPkt_158610(obj, dst, tex, mode) writes, at dst, the uploads
// of one image node: its palettes (count of them, `stride` apart, to
// `base + i*n` blocks; 16x16 for 8-bit, 8x2 for 4-bit), then each mip
// level. 8-bit art with the node's flag (+0x36) set goes as CT32 at half
// the width and height, pre-swizzled. Each palette and level is handed to
// the producer from where the node keeps it. The arithmetic is the
// function's own.
// The upload by reference (M6): the packet the builder wrote, read for its
// transfer (BITBLTBUF, TRXPOS, TRXREG at its fixed places), replaced by a
// host marker (rg::nativeproducer::kHostRegister) of it and its source.
PS2Runtime::RecompiledFunction g_uploadRefOriginal = nullptr;
struct UploadRef
{
    uint32_t p = 0u, addr = 0u, qwc = 0u;
};
std::vector<UploadRef> g_uploadRefs;
std::atomic<uint64_t> g_uploadRefsMarked{0u};

uint32_t transferBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u;
    case 0x01u: return 24u;
    case 0x02u: case 0x0Au: return 16u;
    case 0x13u: case 0x1Bu: return 8u;
    case 0x14u: case 0x24u: case 0x2Cu: return 4u;
    default: return 0u;
    }
}

void afterUploadRef(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    namespace producer = rg::nativeproducer;
    if (g_uploadRefs.empty())
        return;
    const UploadRef r = g_uploadRefs.back();
    g_uploadRefs.pop_back();
    const uint32_t words = getRegU32(ctx, 2);
    const uint32_t at = physical(r.p);
    if (words < 16u || at + 0x70u > PS2_RAM_SIZE || (r.addr & 15u) != 0u)
        return;
    uint64_t bitblt, trxpos, trxreg;
    std::memcpy(&bitblt, g_rdram + at + 48u, 8u);
    std::memcpy(&trxpos, g_rdram + at + 64u, 8u);
    std::memcpy(&trxreg, g_rdram + at + 80u, 8u);
    const uint32_t dbp = uint32_t((bitblt >> 32) & 0x3FFFu), dbw = uint32_t((bitblt >> 48) & 0x3Fu),
                   dpsm = uint32_t((bitblt >> 56) & 0x3Fu);
    const uint32_t x = uint32_t((trxpos >> 32) & 0x7FFu), y = uint32_t((trxpos >> 48) & 0x7FFu);
    const uint32_t w = uint32_t(trxreg & 0xFFFu), h = uint32_t((trxreg >> 32) & 0xFFFu);
    const uint32_t bits = transferBits(dpsm);
    if (bits == 0u || w == 0u || h == 0u || uint64_t(w) * h * bits / 8u > uint64_t(r.qwc) * 16u)
        return;
    const uint32_t head[8] = {3u | (1u << 28), 0u, 0u, 0x50000003u, 2u | 0x8000u, 0x10000000u, 0xEu, 0u};
    const uint64_t writes[4] = {producer::takeValue(dbp, dbw, dpsm, w, h), producer::kHostRegister,
                                uint64_t(physical(r.addr)) | (uint64_t(x) << 32) | (uint64_t(y) << 43),
                                producer::kHostSourceRegister};
    std::memcpy(g_rdram + at, head, sizeof(head));
    std::memcpy(g_rdram + at + sizeof(head), writes, sizeof(writes));
    setReturnS32(ctx, 4 * 4);
    ++g_uploadRefsMarked;
}

void uploadRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!rg::vif1::bindingTextures())
    {
        g_uploadRefOriginal(rdram, ctx, runtime);
        return;
    }
    g_uploadRefs.push_back({getRegU32(ctx, 4), getRegU32(ctx, 8), getRegU32(ctx, 9)});
    rg::hooks::callThen(g_uploadRefOriginal, rdram, ctx, runtime, &afterUploadRef, 0u);
}

// The movie's frame upload (M6): its packet replaced, once built, by a
// GIF-channel host marker (an END tag over an A+D packet) of the frame,
// its source flagged as macroblocks.
PS2Runtime::RecompiledFunction g_upload16Original = nullptr;
struct Upload16
{
    uint32_t pkt = 0u, src = 0u, w = 0u, h = 0u;
    uint64_t tex0 = 0u;
};
std::vector<Upload16> g_upload16s;

void afterUpload16(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    namespace producer = rg::nativeproducer;
    if (g_upload16s.empty())
        return;
    const Upload16 u = g_upload16s.back();
    g_upload16s.pop_back();
    const uint32_t at = physical(u.pkt), src = physical(u.src);
    if (u.w == 0u || u.h == 0u || (u.w & 15u) || (u.h & 15u) || at + 64u > PS2_RAM_SIZE ||
        uint64_t(src) + uint64_t(u.w) * u.h * 4u > PS2_RAM_SIZE)
        return;
    const uint32_t tbp = uint32_t(u.tex0 & 0x3FFFu), tbw = uint32_t((u.tex0 >> 14) & 0x3Fu);
    const uint64_t q[8] = {3ull | (7ull << 28), 0ull, // END, three quadwords
                           2ull | (1ull << 15) | (1ull << 60), 0xEull, // GIF: NLOOP 2, EOP, A+D
                           producer::takeValue(tbp, tbw, 0u, u.w, u.h), producer::kHostRegister,
                           uint64_t(src) | producer::kHostSourceBlocks, producer::kHostSourceRegister};
    std::memcpy(g_rdram + at, q, sizeof(q));
}

void upload16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t texture = readAt<uint32_t>(kMovieTexture);
    g_upload16s.push_back({getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 7), getRegU32(ctx, 8),
                           texture != 0u ? readAt<uint64_t>(texture + 0x38u) : 0u});
    rg::hooks::callThen(g_upload16Original, rdram, ctx, runtime, &afterUpload16, 0u);
}

// A take marker (rg::nativeproducer::kTakeRegister) of `takes` at `at`:
// the words it takes, as TexBuildUploadPkt returns them.
uint32_t writeTakeMarker(uint32_t at, const std::vector<uint64_t> &takes)
{
    namespace producer = rg::nativeproducer;
    const uint32_t k = static_cast<uint32_t>(takes.size());
    const uint32_t head[8] = {(1u + k) | (1u << 28), 0u, 0u, 0x50000000u | (1u + k), k | 0x8000u, 0x10000000u, 0xEu, 0u};
    std::memcpy(g_rdram + at, head, sizeof(head));
    for (uint32_t q = 0; q < k; ++q)
    {
        const uint64_t pair[2] = {takes[q], producer::kTakeRegister};
        std::memcpy(g_rdram + at + sizeof(head) + q * 16u, pair, sizeof(pair));
    }
    return (2u + k) * 4u;
}

// A node whose palettes the manager's own callback writes
// (TexBuildClutUploadPkt lays out each palette's packet, a NEXT over the
// palette's 0x400 or 0x40 bytes at +0x10, which the callback fills, then
// its upload by reference; every palette of a node the same size): made
// records from where the callback put them once the function returns, and,
// every palette and level a record, its output replaced by a take marker.
struct CallbackNode
{
    uint32_t dst = 0u, count = 0u, n = 0u, base = 0u, cpsm = 0u, entries = 0u;
    uint64_t producedBefore = 0u;
    std::vector<uint64_t> levelTakes;
};
std::vector<CallbackNode> g_callbackNodes;
std::atomic<uint64_t> g_callbackTaken{0u}, g_callbackLeft{0u};

void afterCallbackNode(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    namespace producer = rg::nativeproducer;
    if (g_callbackNodes.empty())
        return;
    const CallbackNode node = std::move(g_callbackNodes.back());
    g_callbackNodes.pop_back();
    const uint32_t words = getRegU32(ctx, 2);
    const uint32_t at = physical(node.dst);
    const uint32_t bytes = node.entries * ((node.cpsm & 0x3Fu) == 0u ? 4u : 2u);
    if (node.count == 0u || words == 0u || (words * 4u) % node.count != 0u || at + words * 4u > PS2_RAM_SIZE)
    {
        ++g_callbackLeft;
        return;
    }
    const uint32_t each = words * 4u / node.count;
    std::vector<uint64_t> takes;
    for (uint32_t i = 0; i < node.count; ++i)
    {
        const uint32_t data = at + i * each + 16u;
        if (each < 16u + bytes)
        {
            ++g_callbackLeft;
            return;
        }
        const uint32_t cbp = (node.base + i * node.n) & 0x3FFFu;
        producer::palette(g_rdram + data, PS2_RAM_SIZE - data, cbp, node.cpsm, node.entries);
        takes.push_back(producer::takeValue(cbp, 1u, node.cpsm, node.entries == 256u ? 16u : 8u,
                                            node.entries == 256u ? 16u : 2u));
    }
    takes.insert(takes.end(), node.levelTakes.begin(), node.levelTakes.end());
    if (producer::producedCount() - node.producedBefore != takes.size() || (2u + takes.size()) * 16u > words * 4u)
    {
        ++g_callbackLeft; // (uploads after all: the records made are withheld there)
        return;
    }
    setReturnS32(ctx, static_cast<int32_t>(writeTakeMarker(at, takes)));
    ++g_callbackTaken;
}

void texBuildUpload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    const uint32_t dst = getRegU32(ctx, 5);
    const uint32_t tex = getRegU32(ctx, 6);
    const uint32_t mode = getRegU32(ctx, 7);
    if (dst != 0u && tex != 0u)
    {
        const int32_t nodeBpp = readAt<int16_t>(tex + 6u);
        int32_t w = readAt<int16_t>(tex + 2u);
        int32_t h = readAt<int16_t>(tex + 4u);
        const int16_t flag = readAt<int16_t>(tex + 0x36u);
        const uint64_t tex0 = readAt<uint64_t>(tex + 0x38u);
        const uint32_t count = readAt<uint8_t>(tex + 0x32u);
        const uint32_t base = static_cast<uint16_t>(readAt<int16_t>(tex + 0x34u));
        const uint32_t clut = readAt<uint32_t>(tex + 0x60u);
        const uint32_t fn = readAt<uint32_t>(obj + 0x1F4u);
        // What the producer has queued before this node (the take marker
        // below, when every palette and level becomes a record).
        const uint64_t producedBefore = rg::nativeproducer::producedCount();
        uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
        uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
        uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
        const uint32_t cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xFu);
        const uint32_t texturePsm = psm;
        uint32_t textureTbw = tbw;
        int32_t bpp = nodeBpp;
        bool preSwizzled = false;
        uint32_t n = 0u;
        if (nodeBpp == 8)
        {
            if (flag != 0)
            {
                w >>= 1;
                h >>= 1;
                psm = 0u;
                bpp = 32;
                tbw >>= 1;
                preSwizzled = true;
            }
            n = 4u;
        }
        else if (nodeBpp == 4)
        {
            n = 1u;
        }
        // The node as a binding marker (docs/NATIVE-BOUNDARY.md): entries
        // gathered here, written at dst in place of the upload packets when
        // every one of them can be bound.
        namespace producer = rg::nativeproducer;
        thread_local std::vector<producer::BindEntry> entries;
        entries.clear();
        // Not with the manager's own palette callback (which the function
        // must call), nor a mode other than the draw lists', nor palettes
        // counted from no address.
        const bool callback = n > 0u && fn != 0u && mode == 0u;
        bool palettesMade = false;
        bool bind = rg::vif1::bindingTextures() && mode == 0u && !callback && !(n > 0u && clut == 0u && count > 0u);
        // The palettes, unless the node's manager uploads them through its
        // own callback (a transfer of a shape not known here): palette i
        // `stride` bytes after the first.
        if (n > 0u && clut != 0u && !(fn != 0u && mode == 0u))
        {
            const int32_t stride = readAt<int16_t>(tex + 0x30u);
            const uint32_t paletteEntries = nodeBpp == 8 ? 256u : 16u;
            const uint32_t paletteBytes = paletteEntries * ((cpsm & 0x3Fu) == 0u ? 4u : 2u);
            const bool usable = (cpsm & 0x3Fu) == 0x00u || (cpsm & 0x3Fu) == 0x02u || (cpsm & 0x3Fu) == 0x0Au;
            bind = bind && usable;
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t at = physical(clut + i * static_cast<uint32_t>(stride > 0 ? stride : int32_t(paletteBytes)));
                if (bind && at + paletteBytes <= PS2_RAM_SIZE)
                    entries.push_back({at, static_cast<uint16_t>(paletteEntries), 0u, producer::kBindRegister,
                                       producer::kBindPalette, static_cast<uint16_t>((base + i * n) & 0x3FFFu),
                                       static_cast<uint8_t>(cpsm), 1u, 0u});
                else if (bind)
                    bind = false;
                if (!bind)
                    break;
            }
            // Without a marker: the records, with the uploads to come.
            palettesMade = !bind;
            for (uint32_t i = 0u; !bind && i < count; ++i)
            {
                const uint32_t at = physical(clut + i * static_cast<uint32_t>(stride > 0 ? stride : int32_t(paletteBytes)));
                producer::palette(g_rdram + at, PS2_RAM_SIZE - at, (base + i * n) & 0x3FFFu, cpsm, paletteEntries);
            }
        }
        thread_local std::vector<producer::TextureLevel> levels;
        levels.clear();
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            const uint32_t mip = readAt<uint32_t>(tex + 0x50u + i * 4u);
            if (mip == 0u || w <= 0 || h <= 0)
                break;
            if (tbw == 0u)
                tbw = 1u;
            rg::nativeproducer::TextureLevel level;
            const uint32_t at = physical(mip);
            level.ram = g_rdram + at;
            level.ramBytes = PS2_RAM_SIZE - at;
            level.tbp = tbp & 0x3FFFu;
            level.psm = texturePsm;
            level.transferWidth = static_cast<uint32_t>(w);
            level.transferHeight = static_cast<uint32_t>(h);
            level.transferBufferWidth = tbw;
            level.transferPsm = psm;
            level.width = preSwizzled ? level.transferWidth * 2u : level.transferWidth;
            level.height = preSwizzled ? level.transferHeight * 2u : level.transferHeight;
            level.bufferWidth = preSwizzled ? (textureTbw > 1u ? textureTbw : 1u) : tbw;
            level.preSwizzled = preSwizzled;
            level.cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFu);
            level.address = at;
            levels.push_back(level);
            // A captured frame uploaded back goes through texture(), which
            // makes it from the GPU's copy (colorReadKeep).
            bind = bind && level.bufferWidth <= 0xFFu && producer::bindable(level) &&
                   !producer::keptColorRead(at, level.ram, level.ramBytes);
            if (bind)
                entries.push_back({at, static_cast<uint16_t>(level.transferWidth),
                                   static_cast<uint16_t>(level.transferHeight), producer::kBindRegister,
                                   static_cast<uint8_t>(preSwizzled ? producer::kBindPreSwizzled : 0u),
                                   static_cast<uint16_t>(level.tbp), static_cast<uint8_t>(texturePsm),
                                   static_cast<uint8_t>(level.bufferWidth), static_cast<uint16_t>(level.cbp)});
            const int32_t k = bpp * (w * h) / 256 / 8;
            w >>= 1;
            h >>= 1;
            tbp += static_cast<uint16_t>(k);
            tbw >>= 1;
            textureTbw >>= 1;
        }
        if (bind)
        {
            // The marker, and the function not run: a DMA cnt tag (the magic
            // in its unused bits) with the VIFcodes NOP and DIRECT, a GIF tag
            // of A+D writes, and the entries as those writes. The function's
            // return is the words it wrote.
            const uint32_t k = static_cast<uint32_t>(entries.size());
            const uint32_t at = physical(dst);
            if (k == 0u || at + (2u + k) * 16u > PS2_RAM_SIZE)
            {
                // (Nothing to upload: the function writes nothing either.)
                setReturnS32(ctx, 0);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            const uint32_t head[8] = {(1u + k) | (producer::kBindTagMagic << 16) | (1u << 28), 0u, 0u,
                                      0x50000000u | (1u + k), k | 0x8000u, 0x10000000u, 0xEu, 0u};
            std::memcpy(g_rdram + at, head, sizeof(head));
            std::memcpy(g_rdram + at + sizeof(head), entries.data(), size_t(k) * sizeof(producer::BindEntry));
            setReturnS32(ctx, static_cast<int32_t>((2u + k) * 4u));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        // Uploads after all: the palettes not yet made records (the marker
        // was given up among the levels), then the levels.
        if (!palettesMade && n > 0u && clut != 0u && !(fn != 0u && mode == 0u))
        {
            const int32_t stride = readAt<int16_t>(tex + 0x30u);
            const uint32_t paletteEntries = nodeBpp == 8 ? 256u : 16u;
            const uint32_t paletteBytes = paletteEntries * ((cpsm & 0x3Fu) == 0u ? 4u : 2u);
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t at = physical(clut + i * static_cast<uint32_t>(stride > 0 ? stride : int32_t(paletteBytes)));
                producer::palette(g_rdram + at, PS2_RAM_SIZE - at, (base + i * n) & 0x3FFFu, cpsm, paletteEntries);
            }
        }
        for (const producer::TextureLevel &level : levels)
            producer::texture(level);
        // M6: every palette and level a record (the draw lists' mode, not the
        // manager's own palette callback): a take marker in place of the
        // upload packets, and the function not run, as the bind marker.
        const uint32_t palettes = (n > 0u && clut != 0u) ? count : 0u;
        const bool takeable = mode == 0u && !callback && !(n > 0u && clut == 0u && count > 0u) &&
                              rg::vif1::bindingTextures();
        const uint32_t k = palettes + static_cast<uint32_t>(levels.size());
        const uint32_t at = physical(dst);
        thread_local std::vector<uint64_t> takes;
        takes.clear();
        const uint32_t paletteEntries = nodeBpp == 8 ? 256u : 16u;
        for (uint32_t i = 0u; i < palettes; ++i)
            takes.push_back(producer::takeValue((base + i * n) & 0x3FFFu, 1u, cpsm,
                                                paletteEntries == 256u ? 16u : 8u, paletteEntries == 256u ? 16u : 2u));
        for (const producer::TextureLevel &level : levels)
            takes.push_back(producer::takeValue(level.tbp, level.transferBufferWidth, level.transferPsm,
                                                level.transferWidth, level.transferHeight));
        if (takeable && k != 0u && producer::producedCount() - producedBefore == k &&
            at + (2u + k) * 16u <= PS2_RAM_SIZE)
        {
            setReturnS32(ctx, static_cast<int32_t>(writeTakeMarker(at, takes)));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        if (callback && count > 0u && rg::vif1::bindingTextures() &&
            producer::producedCount() - producedBefore == levels.size())
        {
            // The callback's palettes: after the function, from where it
            // put them.
            CallbackNode node;
            node.dst = dst;
            node.count = count;
            node.n = n;
            node.base = base;
            node.cpsm = cpsm;
            node.entries = paletteEntries;
            node.producedBefore = producedBefore;
            node.levelTakes.assign(takes.begin() + palettes, takes.end());
            g_callbackNodes.push_back(std::move(node));
            rg::hooks::callThen(g_texBuildUploadOriginal, rdram, ctx, runtime, &afterCallbackNode, 0u);
            return;
        }
    }
    g_texBuildUploadOriginal(rdram, ctx, runtime);
}
// Steps 7b and 7c of docs/NATIVE-PIPELINE.md: the game's reads of GS memory
// (libgraph's sceGsSetDefStoreImage(si, sbp, sbw, spsm, x, y, w, h), then
// sceGsExecStoreImage(si, buf) and sceGsSyncPath(0, 0)) answered from the
// renderer instead: a depth read (the lens flare's 8x8 block of Z at the
// sun) from its depth a frame or two late, a colour read (the fades' and
// the menus' frame captures) from its targets as the last frame drawn left
// them. Straight into buf, with no GS transfer. The sceGsSyncPath after it
// is left alone: the game drains VIF1 before these reads, so it has nothing
// to wait for. A colour read the renderer does not answer in time gets the
// last answer of its size, or black (under the pipeline the GS has nothing
// to give); a depth read it cannot place goes to the GS as before.
constexpr uint32_t kSceGsSetDefStoreImage = 0x00109348u;
constexpr uint32_t kSceGsExecStoreImage = 0x00109488u;
PS2Runtime::RecompiledFunction g_setDefStoreImageOriginal = nullptr;
PS2Runtime::RecompiledFunction g_execStoreImageOriginal = nullptr;

struct StoreImageCall
{
    uint32_t si = 0u;
    uint32_t base = 0u, psm = 0u, x = 0u, y = 0u, width = 0u, height = 0u;
};
thread_local StoreImageCall t_storeImage;

void setDefStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    StoreImageCall &r = t_storeImage;
    r.si = getRegU32(ctx, 4);
    r.base = getRegU32(ctx, 5) & 0x3FFFu;
    r.psm = getRegU32(ctx, 7) & 0x3Fu;
    r.x = getRegU32(ctx, 8) & 0x7FFu;
    r.y = getRegU32(ctx, 9) & 0x7FFu;
    r.width = getRegU32(ctx, 10) & 0xFFFFu;
    r.height = getRegU32(ctx, 11) & 0xFFFFu;
    g_setDefStoreImageOriginal(rdram, ctx, runtime);
}

// A colour read's RGBA pixels in the read's format (CT32, CT24, CT16).
void toFormat(const std::vector<uint8_t> &rgba, uint32_t psm, size_t pixels, std::vector<uint8_t> &out)
{
    if (psm == 0x00u)
    {
        out = rgba;
        return;
    }
    if (psm == 0x01u)
    {
        out.resize(pixels * 3u);
        for (size_t i = 0; i < pixels; ++i)
            std::memcpy(out.data() + i * 3u, rgba.data() + i * 4u, 3u);
        return;
    }
    out.resize(pixels * 2u);
    for (size_t i = 0; i < pixels; ++i)
    {
        const uint8_t *c = rgba.data() + i * 4u;
        const uint16_t v = static_cast<uint16_t>((c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10) |
                                                 (c[3] >= 0x80u ? 0x8000u : 0u));
        out[i * 2u] = static_cast<uint8_t>(v);
        out[i * 2u + 1u] = static_cast<uint8_t>(v >> 8);
    }
}

// Whether the last answer was the renderer's (not a stand-in).
thread_local bool t_served = false;

// The read's bytes, or false to leave it to the GS. `buffer`: where in the
// EE they go (physical).
bool answer(const StoreImageCall &q, uint32_t buffer, std::vector<uint8_t> &bytes)
{
    const uint32_t psm = q.psm;
    const bool depth = psm == 0x30u || psm == 0x31u || psm == 0x32u || psm == 0x3Au;
    if (depth)
        return rg::nativeproducer::readDepth(q.base, q.psm, q.x, q.y, q.width, q.height, bytes) && !bytes.empty();
    const bool colour = psm == 0x00u || psm == 0x01u || psm == 0x02u || psm == 0x0Au;
    if (!colour)
        return false;
    namespace native = dq8::gfx::native;
    native::ColorRead read;
    read.target = rg::nativeproducer::captureTarget(q.base);
    read.keep = rg::nativeproducer::colorReadKeep(buffer, q.width, q.height);
    read.rect = {static_cast<int32_t>(q.x), static_cast<int32_t>(q.y), static_cast<int32_t>(q.x + q.width),
                 static_cast<int32_t>(q.y + q.height)};
    // The presenting thread answers it (step 7d), nothing on the GS queue.
    const uint32_t id = native::beginColorRead(read);
    thread_local std::vector<float> units;
    thread_local std::vector<uint8_t> rgba;
    // The last answer, by the read's size and format, for a read not served.
    static std::mutex lastMutex;
    static std::vector<uint8_t> last;
    static uint32_t lastWidth = 0u, lastHeight = 0u, lastPsm = ~0u;
    if (native::waitColorRead(id, units, 2000u))
    {
        // The target's units (native_producer.cpp's: a byte over 0x80) back
        // to the game's bytes.
        rgba.resize(units.size());
        for (size_t i = 0; i < units.size(); ++i)
            rgba[i] = static_cast<uint8_t>(std::clamp(units[i] * 128.0f + 0.5f, 0.0f, 255.0f));
        toFormat(rgba, psm, static_cast<size_t>(q.width) * q.height, bytes);
        if (!bytes.empty())
        {
            t_served = true;
            std::lock_guard lock(lastMutex);
            last = bytes;
            lastWidth = q.width;
            lastHeight = q.height;
            lastPsm = psm;
            return true;
        }
    }
    {
        std::lock_guard lock(lastMutex);
        const bool same = !last.empty() && lastWidth == q.width && lastHeight == q.height && lastPsm == psm;
        static int told = 0;
        if (told < 8)
        {
            ++told;
            std::fprintf(stderr, "[native-capture] a colour read (%ux%u) was not served; %s\n", q.width, q.height,
                         same ? "the last one of its size is given" : "black is given");
        }
        if (same)
            bytes = last;
        else
            bytes.assign(static_cast<size_t>(q.width) * q.height * (psm == 0x00u ? 4u : psm == 0x01u ? 3u : 2u),
                         0u);
    }
    return !bytes.empty();
}

void execStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t si = getRegU32(ctx, 4);
    const uint32_t buffer = getRegU32(ctx, 5);
    thread_local std::vector<uint8_t> bytes;
    t_served = false;
    if (si == t_storeImage.si && answer(t_storeImage, physical(buffer), bytes))
    {
        const size_t size = std::min<size_t>(bytes.size(), PS2_RAM_SIZE - physical(buffer));
        std::memcpy(g_rdram + physical(buffer), bytes.data(), size);
        // (A stand-in answer is not the kept pixels: no texture from them.)
        rg::nativeproducer::colorReadAnswered(physical(buffer), g_rdram + physical(buffer), t_served ? size : 0u);
        t_storeImage.si = 0u;
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    t_storeImage.si = 0u;
    g_execStoreImageOriginal(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    rg::writewatch::init(g_rdram, PS2_RAM_SIZE);
    if (!rg::timing::hookFunction(runtime, kSetFrameFromDesc, &setFrameFromDesc, g_setFrameFromDescOriginal) ||
        !rg::timing::hookFunction(runtime, kGetFrameTexDesc, &getFrameTexDesc, g_getFrameTexDescOriginal) ||
        !rg::timing::hookFunction(runtime, kGetDispFbTexDesc, &getDispFbTexDesc, g_getDispFbTexDescOriginal))
        std::fprintf(stderr, "[native-capture] the target declarations could not be hooked\n");
    if (!rg::timing::hookFunction(runtime, kTexBuildUploadPkt, &texBuildUpload, g_texBuildUploadOriginal))
        std::fprintf(stderr, "[native-capture] TexBuildUploadPkt could not be hooked\n");
    if (!rg::timing::hookFunction(runtime, kGsBuildImageUploadRefPkt, &uploadRef, g_uploadRefOriginal))
        std::fprintf(stderr, "[native-capture] GsBuildImageUploadRefPkt could not be hooked\n");
    if (!rg::timing::hookFunction(runtime, kGsUploadImage16Blocks, &upload16, g_upload16Original))
        std::fprintf(stderr, "[native-capture] GsUploadImage16Blocks could not be hooked\n");
    if (!rg::timing::hookFunction(runtime, kSceGsSetDefStoreImage, &setDefStoreImage, g_setDefStoreImageOriginal) ||
        !rg::timing::hookFunction(runtime, kSceGsExecStoreImage, &execStoreImage, g_execStoreImageOriginal))
        std::fprintf(stderr, "[native-capture] the GS memory reads could not be hooked: they stay GS transfers\n");
}

} // namespace rg::nativecapture
