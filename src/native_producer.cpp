#include "native_producer.h"
#include "fminmax.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <array>
#include <deque>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gfx/native/native_pipeline.h"
#include "gs_swizzle.h"
#include "native_types.h"
#include "draw_state_mirror.h"
#include "guest_write_watch.h"
#include "flip_pacing.h"
#include "pcsx2_hash.h"
#include "texture_pack.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace rg::nativeproducer
{
namespace
{
namespace native = dq8::gfx::native;
// MeshDraw::source of the draws made here that no VU1 program drew (a VU1
// mesh has its program's, vif1_thread.cpp), named for the renderer's
// draw labels (a frame capture's; debug/bug_mark.h).
constexpr uint32_t kSource2d = 1u, kSourceLayer = 2u;
const bool g_sourcesNamed = [] {
    native::nameSource(kSource2d, "2D piece");
    native::nameSource(kSourceLayer, "object layer rectangle");
    return true;
}();

constexpr uint32_t kCT32 = 0x00u, kCT16 = 0x02u, kCT16S = 0x0Au, kT8 = 0x13u, kT4 = 0x14u;

// The units of every record made here (native_records.h): the game's color
// and alpha byte over 0x80, which its texture functions and blend factors
// take as 1.0. A texel's full 8 bits are worth 255/128 of them, the GS
// clamps a color to its 0xFF, and its 0xFF is white on the screen.
constexpr float kUnit = 1.0f / 128.0f;
constexpr float kTexelScale = 255.0f / 128.0f;
// CT16's 5-bit channels, sampled as UNORM (v / 31), are worth v * 8 / 128
// (the GS widens them by a shift).
constexpr float kCt16Scale = 31.0f * 8.0f / 128.0f;
// Bytes a texel; 0 for Index4's half byte (copied by the texel, not the byte).
uint32_t texelBytes(native::PixelFormat f)
{
    return f == native::PixelFormat::Index8 ? 1u
           : f == native::PixelFormat::Ct16 ? 2u
           : f == native::PixelFormat::Index4 ? 0u
                                              : 4u;
}

bool indexedFormat(native::PixelFormat f)
{
    return f == native::PixelFormat::Index8 || f == native::PixelFormat::Index4;
}

// An Index4 texture's indices a byte each (for the texture packs' hash and
// the tile cutter, which take them so).
void widenIndex4(const uint8_t *packed, size_t texels, uint8_t *out)
{
    for (size_t i = 0; i < texels; ++i)
        out[i] = (packed[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
}
float texelScale(native::PixelFormat f)
{
    return f == native::PixelFormat::Ct16 ? kCt16Scale : kTexelScale;
}
constexpr float kColorClamp = 255.0f / 128.0f;
constexpr float kFogUnit = 1.0f / 255.0f; // the GS's fog factor F, 0 to 255

struct Counts
{
    uint64_t levels = 0, texturesNew = 0, texturesKnown = 0, texturesResent = 0, texturesFromReads = 0;
    uint64_t refusedFormat = 0, refusedBytes = 0, refusedLayout = 0;
    uint64_t palettesNew = 0, palettesKnown = 0, palettesRefused = 0;
    uint64_t targetsNew = 0, targetsAuto = 0, targetsGrown = 0, texturesOverTargets = 0;
    uint64_t bytesHashed = 0;
    uint64_t withheldTextures = 0, withheldPalettes = 0, withheldBytes = 0;
    // Withheld uploads whose bytes never left guest memory (sourceUnchanged),
    // and those fetched from it after all.
    uint64_t elided = 0, elidedBytes = 0, elidedFetched = 0;
    uint64_t passedNotProduced = 0, split = 0, continuedInPacket = 0;
    // Bindings applied in the stream's order (bound), and entries no record
    // could be made of (bindResolve).
    uint64_t boundTextures = 0, boundPalettes = 0, bindRefused = 0;
    // Step 4a: VU1 meshes made mesh draws.
    uint64_t meshes = 0, meshesNoState = 0, meshesSplit = 0, meshesWhole = 0, camerasNew = 0;
    uint64_t materialKnown = 0, textureMiss = 0, textureMissPsm = 0, textureMissTarget = 0, paletteMiss = 0, targetMiss = 0;
    uint64_t unknownBlend = 0, unknownClamp = 0, unknownWriteMask = 0, unknownOther = 0;
    // Step 5b: 2D pieces made window-space mesh draws, and why not.
    uint64_t pieces = 0, piecesMade = 0, piecesNoState = 0, piecesNoTarget = 0, piecesNoSource = 0;
    uint64_t piecesFogged = 0, piecesFromTarget = 0, piecesSubTarget = 0, piecesCt24 = 0;
    uint64_t opsGreenToAlpha = 0;
    // Step 5c: object layers.
    uint64_t layersRouted = 0, drawsRouted = 0;
    // Step 6: host uploads nobody took, kept as images (hostUpload).
    uint64_t hostUploads = 0, hostRefused = 0, hostBytes = 0, hostTexturesNew = 0, hostTexturesKnown = 0;
    uint64_t hostPalettes = 0, hostReadsRefused = 0;
    // Narrow views: a buffer drawn or read narrower than its target.
    uint64_t viewsNew = 0, viewWrites = 0, viewReads = 0;
    // Stale records made again from what was sent (replaceUpload).
    uint64_t palettesReplaced = 0, texturesReplaced = 0;
    uint64_t copiesMirrored = 0, copiesOtherFormat = 0, copiesNoSource = 0, copiesNoDestination = 0;
    // Texture packs: pairs of a texture and a palette looked up, pictures
    // made records, pairs with none, and draws given a picture.
    uint64_t wholesAsked = 0, wholesMade = 0, wholesNone = 0, wholesDrawn = 0;
    uint64_t regionsAsked = 0, regionsMade = 0, regionsDrawn = 0, regionsUnhashable = 0;
    // Tiles of sheets (region clamp and repeat) cut into textures of their own.
    uint64_t tilesCut = 0;
    // M3: post effects sent as passes, and those whose buffers were unknown.
    uint64_t passesMade = 0, passesRefused = 0;
};

struct State
{
    std::mutex mutex;
    // Content (hash and shape) to id, and back, for what the renderer holds.
    std::unordered_map<uint64_t, native::Id> textureIds, paletteIds;
    std::unordered_map<native::Id, uint64_t> textureKeys;
    native::Id nextTexture = 1u, nextPalette = 1u, nextTarget = kSceneTarget + 1u;
    // The game's addresses to ids (step 4's lookups).
    std::unordered_map<uint32_t, std::pair<uint32_t, native::Id>> textureAt; // tbp -> (psm, id)
    std::unordered_map<uint32_t, native::Id> paletteAt;                       // cbp -> id
    std::unordered_map<uint32_t, native::Id> targetAt;                        // base -> id
    std::map<uint32_t, native::Target> targets;                               // base -> as declared
    std::unordered_map<uint32_t, native::Id> depthAt;                         // ZBUF base -> id
    std::unordered_map<uint64_t, native::Id> cameraIds;                       // matrices' hash -> id
    native::Id nextCamera = 1u;
    native::Id lastCamera = 0u; // the newest 3D draw's, for 2D draws that test depth
    // Step 5c: while an object is routed, the draws into `routeBase` go to
    // the layer of its own (`layer`, one target reused: each object's tail
    // is drawn from it, in order, before the next is routed).
    bool routing = false;
    uint32_t routeBase = 0u;
    native::Id layer = 0u;
    uint16_t layerWidth = 0u, layerHeight = 0u;
    int32_t layerBox[4] = {}; // the routed object's, x1 and y1 exclusive
    // Its viewport's depth row (z' = v22 z + v32 w), for depth read back
    // (step 7b).
    float lastDepthScale = 0.0f, lastDepthOffset = 0.0f;
    uint64_t depthReads = 0u, depthAnswered = 0u, depthNoTarget = 0u;
    uint32_t sceneBase = 0u;
    bool sceneBaseKnown = false;
    // Uploads made into records, in the game's order, for takeProduced.
    struct Produced
    {
        uint32_t dbp, dbw, dpsm, width, height;
        bool palette;
        uint32_t key, psm; // the texture's TBP and format, or the palette's CBP
        native::Id id;
        uint64_t hash; // imageHash of the guest memory it was made from
        uint32_t bytes;
    };
    // What GS memory holds as the VIF1 thread sends it on, in the stream's
    // order (takeProduced): the game thread's tables above run a frame of
    // uploads ahead, and the game reuses its texture addresses within one.
    std::unordered_map<uint32_t, std::pair<uint32_t, native::Id>> residentTexture; // tbp -> (psm, id)
    std::unordered_map<uint32_t, native::Id> residentPalette;                       // cbp -> id
    // When each address was last written, in the stream's order: uploaded
    // (a texture's TBP) or drawn into (a target's base).
    uint64_t sequence = 0u;
    std::unordered_map<uint32_t, uint64_t> uploadedSeq, drawnSeq;
    std::deque<Produced> produced;
    // When the VIF1 thread last took a produced texture (by TBP) or palette
    // (by CBP), against a host image's (step 6).
    std::unordered_map<uint32_t, uint64_t> takenSeq, takenPaletteSeq;
    // Step 6: what host uploads nobody took left in GS memory, one linear
    // image per base and format (the upload's pixels, rows of the buffer
    // width; a byte a texel for the 8- and 4-bit formats), read back by
    // draws of that format and buffer width: as the GS reads what it was
    // written with, no address swizzle is needed.
    struct HostImage
    {
        uint32_t dbw = 1u, psm = 0u, height = 0u;
        std::vector<uint8_t> pixels;
        uint64_t seq = 0u, version = 0u;
    };
    std::map<std::pair<uint32_t, uint32_t>, HostImage> hostImages; // (base, psm)
    // The record last made of an image, by its read (TBP, PSM, TW, TH; or
    // CBP, CPSM, entries), and the image's version then.
    struct HostRecord
    {
        uint64_t version = 0u;
        native::Id id = 0u;
    };
    std::unordered_map<uint64_t, HostRecord> hostTextures, hostPalettes;
    std::set<uint32_t> copiedTo; // local copies' destinations (logged reads)
    // A buffer the game also draws into at a narrower buffer width than its
    // target was declared at (an effect building a 64x64 texture in the
    // corner of a full-screen buffer, then sampling it at that width): a
    // target of its own, by base and width (viewKey), so the full-screen
    // draws do not cover it. Not modelled: a read at one width of what was
    // drawn at another, which the GS's pages fold.
    std::unordered_map<uint32_t, native::Target> views;          // viewKey -> as declared
    std::unordered_map<uint32_t, std::vector<uint32_t>> viewWidths; // base -> widths with a view
    struct Kept
    {
        std::shared_ptr<std::vector<uint8_t>> data;
        uint32_t width = 0u, height = 0u;
        native::PixelFormat format = native::PixelFormat::Rgba8;
        native::Id palette = 0u; // the record's own, for one sent again
    };
    // Every live texture record's texels (as its format has them), for
    // local copies (mirrorCopy) and for a record the renderer evicted that
    // GS memory still holds (forgetEvicted sends it again); forgotten with
    // the renderer's other evictions.
    std::unordered_map<native::Id, Kept> texels;
    // Colour reads kept on the GPU (colorReadKeep), by the EE address their
    // answer went to: the target, the answer's size and hash.
    struct KeptRead
    {
        native::Id id = 0u;
        uint32_t width = 0u, height = 0u;
        size_t size = 0u;
        uint64_t hash = 0u;
    };
    std::unordered_map<uint32_t, KeptRead> keptReads;
    // Every palette record's colours (palettes are never evicted), for the
    // texture packs' hash of a palette.
    std::unordered_map<native::Id, std::shared_ptr<const std::vector<uint8_t>>> paletteColors;
    // A palette's 16 entries from `first` (a CLUT window CSA selects), as a
    // palette of its own: by (palette, first).
    std::unordered_map<uint64_t, native::Id> paletteWindows;
    // Texture packs (texture_pack.h): by texture and palette ids, the pack's
    // picture made a texture record of its own (id), none, or still being
    // read; PCSX2's hashes of each, made once. Forgotten when the packs
    // switch (packGeneration) and when the renderer evicts a picture.
    struct Whole
    {
        native::Id id = 0u;
        bool none = false, nearest = false, hashed = false;
        uint64_t texels = 0u, clut = 0u;
    };
    std::unordered_map<uint64_t, Whole> wholes;          // texture << 32 | palette
    std::unordered_map<native::Id, uint64_t> wholeOf;    // a picture's record -> its key
    std::unordered_map<native::Id, uint64_t> clutHashes; // palette id -> PCSX2's hash
    // Region packs (M4): by the GS backend's region key, the pack's
    // picture made a record (id), none, or still being read.
    std::unordered_map<uint64_t, Whole> regions;
    std::unordered_map<native::Id, uint64_t> regionOf; // a region picture's record -> its key
    uint64_t regionGeneration = 0u;
    // Tiles cut from textures (cutRegion): by the texture's id and the GS's
    // CLAMP, the tile's own texture record; forgotten when either is evicted.
    std::map<std::pair<native::Id, uint64_t>, native::Id> cuts;
    std::unordered_map<native::Id, std::pair<native::Id, uint64_t>> cutOf;
    uint64_t packGeneration = 0u;
    // The depth target the scene's 3D draws last used (a pass's that tests
    // against the scene).
    native::Id sceneDepth = 0u;
    // The depth targets drawn with since the frame began (frameBoundary):
    // each is cleared at its first use in a frame, as the game's clear
    // sprites (not records until M4 of docs/NATIVE-ONLY.md) leave it.
    std::set<native::Id> depthsThisFrame;
    std::set<uint64_t> dumped;
    Counts counts, reported;
    bool statsSet = false;
};
// RG_NATIVE_TEXTURE_DUMP=tbp,tbp (hex): the textures 2D pieces read there,
// written to build/run/texdump as PPM (colour, and alpha beside it).
const std::vector<uint32_t> &dumpBases()
{
    static const std::vector<uint32_t> bases = [] {
        std::vector<uint32_t> out;
        const char *v = std::getenv("RG_NATIVE_TEXTURE_DUMP");
        while (v && *v)
        {
            char *end = nullptr;
            out.push_back(static_cast<uint32_t>(std::strtoul(v, &end, 16)));
            v = (end && *end == ',') ? end + 1 : nullptr;
        }
        return out;
    }();
    return bases;
}
bool dumpingTextures()
{
    return !dumpBases().empty();
}
// The flip after `touch build/run/dump-now` (0: none yet), whose 2D pieces
// and meshes are logged; it also asks the renderer for its side of that
// frame (build/run/dump-gpu). Checked once a flip.
State &state();
std::string holdingAt(State &s, uint32_t base);
uint64_t triggeredDumpFlip()
{
    static uint64_t checked = 0u, triggered = 0u;
    const uint64_t flip = rg::graphics::guestFlips();
    if (flip != checked)
    {
        checked = flip;
        if (std::remove("build/run/dump-now") == 0)
        {
            triggered = flip + 1u;
            if (FILE *f = std::fopen("build/run/dump-gpu", "w"))
                std::fclose(f);
            std::fprintf(stderr, "[piece-dump] dumping flip %llu\n", (unsigned long long)triggered);
            for (const auto &[base, t] : state().targets)
                std::fprintf(stderr, "[target-map] base %04x -> target %u %ux%u role %d; holds:%s\n", base, t.id,
                             t.width, t.height, int(t.role), holdingAt(state(), base).c_str());
        }
    }
    return triggered;
}
State &state()
{
    static State instance;
    return instance;
}

// The identity of an image's bytes, never 0 (as the GS path's named
// textures hashed them; one pass of four lanes, 32 bytes a step).
uint64_t imageHash(const uint8_t *at, size_t size)
{
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t lane[4] = {k, k ^ 1u, k ^ 2u, k ^ 3u};
    size_t left = size;
    while (left >= 32u)
    {
        for (int i = 0; i < 4; ++i)
        {
            uint64_t word;
            std::memcpy(&word, at + i * 8, 8);
            lane[i] = (lane[i] ^ word) * k;
            lane[i] ^= lane[i] >> 29;
        }
        at += 32;
        left -= 32u;
    }
    uint64_t tail = 0u;
    if (left)
        std::memcpy(&tail, at, left);
    uint64_t h = size;
    for (uint64_t v : {lane[0], lane[1], lane[2], lane[3], tail})
    {
        h = (h ^ v) * k;
        h ^= h >> 31;
    }
    return h | 1u;
}

// A texture upload's source hash, reused while its guest RAM pages are
// unwritten (rg::writewatch): the game sends the same textures from the
// same memory every frame (~4.6 MB a frame in the field, 2026-11-09), and
// hashing them was ~9% of the game thread. `hashed`: the bytes read.
struct SeenSource
{
    const uint8_t *at;
    size_t size;
    uint64_t hash, stamp;
    uint32_t changes; // times found written since its last hash
    // The record last made of it as a binding (bindTexture, bindPalette):
    // its key (the hash with the shape), its id, and the eviction epoch the
    // id was good in.
    uint64_t key = 0u, epoch = 0u;
    uint32_t id = 0u;
};
// The texture hooks' thread's (the game thread: sourceHash, sourceUnchanged).
thread_local std::unordered_map<uint64_t, SeenSource> t_seenSources;
uint64_t seenKey(const uint8_t *at, size_t size)
{
    return reinterpret_cast<uintptr_t>(at) ^ (uint64_t(size) * 0x9E3779B97F4A7C15ull);
}

// `found`: its entry (the game thread's, good until the next call), null
// without the write watch.
uint64_t sourceHash(const uint8_t *at, size_t size, size_t &hashed, SeenSource **found = nullptr)
{
    hashed = 0u;
    if (!rg::writewatch::enabled())
    {
        hashed = size;
        return imageHash(at, size);
    }
    auto &seen = t_seenSources;
    const uint64_t key = seenKey(at, size);
    const auto it = seen.find(key);
    const bool same = it != seen.end() && it->second.at == at && it->second.size == size;
    if (found && same)
        *found = &it->second;
    if (same && rg::writewatch::unchanged(at, size, it->second.stamp))
        return it->second.hash;
    hashed = size;
    // Memory written again and again (a texture the game rewrites, or one
    // sharing its pages with data that changes every frame) is hashed and
    // not watched: watching it cost a fault and a protection change (with
    // its TLB shootdown) every frame, more than the hash.
    const uint32_t changes = same ? it->second.changes + 1u : 0u;
    if (changes >= 2u)
    {
        it->second.changes = changes;
        it->second.stamp = 0u;
        return it->second.hash = imageHash(at, size);
    }
    // Watched first, then read: a write after the stamp is seen next time.
    const uint64_t stamp = rg::writewatch::watch(at, size);
    const uint64_t hash = imageHash(at, size);
    if (seen.size() >= 65536u)
        seen.clear();
    SeenSource &entry = seen[key];
    entry = {at, size, hash, stamp, changes};
    if (found)
        *found = &entry;
    return hash;
}

uint64_t keyOf(uint64_t hash, uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t k = hash ^ (uint64_t(a) * 0x9E3779B97F4A7C15ull);
    k ^= (uint64_t(b) << 21) * 0xC2B2AE3D27D4EB4Full;
    k ^= (uint64_t(c) << 42) * 0x165667B19E3779F9ull;
    return k ? k : 1u;
}

std::string statsLine()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const Counts &n = s.counts, &r = s.reported;
    const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a - b); };
    std::string line = "[native-producer] texture levels " + d(n.levels, r.levels) + ": new " +
                       d(n.texturesNew, r.texturesNew) + ", known " + d(n.texturesKnown, r.texturesKnown) +
                       ", resent " + d(n.texturesResent, r.texturesResent) + ", from kept reads " +
                       d(n.texturesFromReads, r.texturesFromReads) + ", refused: format " + d(n.refusedFormat, r.refusedFormat) + ", bytes " +
                       d(n.refusedBytes, r.refusedBytes) + ", layout " + d(n.refusedLayout, r.refusedLayout) +
                       " | palettes new " + d(n.palettesNew, r.palettesNew) + ", known " +
                       d(n.palettesKnown, r.palettesKnown) + ", refused " + d(n.palettesRefused, r.palettesRefused) +
                       " | targets new " + d(n.targetsNew, r.targetsNew) + " (undeclared, from their draws " +
                       d(n.targetsAuto, r.targetsAuto) + "; grown to their draws " +
                       d(n.targetsGrown, r.targetsGrown) + ") | hashed " +
                       std::to_string((n.bytesHashed - r.bytesHashed) >> 10) + " KiB" +
                       " | meshes " + d(n.meshes, r.meshes) + ": no draw state " +
                       d(n.meshesNoState, r.meshesNoState) + ", camera and model " + d(n.meshesSplit, r.meshesSplit) +
                       ", whole matrix (model check failed) " + d(n.meshesWhole, r.meshesWhole) + ", cameras new " +
                       d(n.camerasNew, r.camerasNew) + ", material known " + d(n.materialKnown, r.materialKnown) +
                       " (misses: texture " + d(n.textureMiss, r.textureMiss) + " [another format there " +
                       d(n.textureMissPsm, r.textureMissPsm) + ", a render target there " +
                       d(n.textureMissTarget, r.textureMissTarget) + "], palette " +
                       d(n.paletteMiss, r.paletteMiss) + ", target " + d(n.targetMiss, r.targetMiss) +
                       "; unknown: blend " + d(n.unknownBlend, r.unknownBlend) + ", clamp " +
                       d(n.unknownClamp, r.unknownClamp) + ", write mask " + d(n.unknownWriteMask, r.unknownWriteMask) +
                       ", other " + d(n.unknownOther, r.unknownOther) + ")" +
                       " | sampled a texture uploaded over a target " +
                       d(n.texturesOverTargets, r.texturesOverTargets) + " | 2D pieces " + d(n.pieces, r.pieces) + ": made " + d(n.piecesMade, r.piecesMade) +
                       " (from a target " + d(n.piecesFromTarget, r.piecesFromTarget) + ", as CT24 " +
                       d(n.piecesCt24, r.piecesCt24) + ", fogged " +
                       d(n.piecesFogged, r.piecesFogged) + "), to the GS: no state " +
                       d(n.piecesNoState, r.piecesNoState) + ", target not declared " +
                       d(n.piecesNoTarget, r.piecesNoTarget) + ", source unknown " +
                       d(n.piecesNoSource, r.piecesNoSource) + " (inside a target " +
                       d(n.piecesSubTarget, r.piecesSubTarget) + "); made green to alpha " +
                       d(n.opsGreenToAlpha, r.opsGreenToAlpha) +
                       " | bound in the stream: textures " + d(n.boundTextures, r.boundTextures) + ", palettes " +
                       d(n.boundPalettes, r.boundPalettes) + ", no record " + d(n.bindRefused, r.bindRefused) +
                       " | withheld from the GS: textures " + d(n.withheldTextures, r.withheldTextures) +
                       ", palettes " + d(n.withheldPalettes, r.withheldPalettes) + " (" +
                       std::to_string((n.withheldBytes - r.withheldBytes) >> 10) + " KiB; " +
                       d(n.elided, r.elided) + " never copied, " +
                       std::to_string((n.elidedBytes - r.elidedBytes) >> 10) + " KiB, " +
                       d(n.elidedFetched, r.elidedFetched) + " read after all); uploads sent on (no record) " +
                       d(n.passedNotProduced, r.passedNotProduced) + "; A+D blocks split (other registers kept) " +
                       d(n.split, r.split) + "; image continued in a packet " +
                       d(n.continuedInPacket, r.continuedInPacket) + " | object layers: routed " +
                       d(n.layersRouted, r.layersRouted) + " (" + d(n.drawsRouted, r.drawsRouted) +
                       " draws)" +
                       " | host uploads nobody took " + d(n.hostUploads, r.hostUploads) + " (" +
                       std::to_string((n.hostBytes - r.hostBytes) >> 10) + " KiB, refused " +
                       d(n.hostRefused, r.hostRefused) + "): textures made " +
                       d(n.hostTexturesNew, r.hostTexturesNew) + ", known " +
                       d(n.hostTexturesKnown, r.hostTexturesKnown) + ", palettes " +
                       d(n.hostPalettes, r.hostPalettes) + ", reads refused " +
                       d(n.hostReadsRefused, r.hostReadsRefused) +
                       " | stale records made again: palettes " + d(n.palettesReplaced, r.palettesReplaced) +
                       ", textures " + d(n.texturesReplaced, r.texturesReplaced) + " | local copies mirrored " +
                       d(n.copiesMirrored, r.copiesMirrored) + ", other format " + d(n.copiesOtherFormat, r.copiesOtherFormat) +
                       ", no source " + d(n.copiesNoSource, r.copiesNoSource) + ", no destination " +
                       d(n.copiesNoDestination, r.copiesNoDestination) + " | narrow views new " + d(n.viewsNew, r.viewsNew) + ", draws into them " +
                       d(n.viewWrites, r.viewWrites) + ", reads of them " + d(n.viewReads, r.viewReads) +
                       " | depth reads (all so far) " +
                       std::to_string(s.depthReads) + ": answered " + std::to_string(s.depthAnswered) +
                       ", no depth target there " + std::to_string(s.depthNoTarget) +
                       " | tiles cut " + d(n.tilesCut, r.tilesCut) + " | passes " + d(n.passesMade, r.passesMade) +
                       ", refused (buffer unknown) " + d(n.passesRefused, r.passesRefused) +
                       " | texture packs: pairs looked up " + d(n.wholesAsked, r.wholesAsked) + ", pictures made " +
                       d(n.wholesMade, r.wholesMade) + ", none " + d(n.wholesNone, r.wholesNone) +
                       ", draws from a picture " + d(n.wholesDrawn, r.wholesDrawn) + "; regions looked up " +
                       d(n.regionsAsked, r.regionsAsked) + ", pictures made " + d(n.regionsMade, r.regionsMade) +
                       ", draws from one " + d(n.regionsDrawn, r.regionsDrawn) + ", not hashable here " +
                       d(n.regionsUnhashable, r.regionsUnhashable);
    s.reported = s.counts;
    return line;
}

// Locked. An upload made into a record (or known as one), for takeProduced.
void noteProduced(State &s, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool palette,
                  uint32_t key, uint32_t psm, native::Id id, uint64_t hash, uint32_t bytes)
{
    // Never sent ones age out; a bound in case the VIF1 thread takes none.
    if (s.produced.size() >= 8192u)
        s.produced.pop_front();
    s.produced.push_back(
        {dbp & 0x3FFFu, std::max(dbw, 1u), dpsm & 0x3Fu, width, height, palette, key, psm, id, hash, bytes});
}

// Bumped when the renderer evicts: an id a source remembers from an earlier
// epoch is looked up again (bindTexture).
std::atomic<uint64_t> g_evictEpoch{1u};

// Locked. Whether a texture record is what GS memory holds at some address,
// for the game thread (textureAt) or the VIF1 thread (residentTexture).
bool textureNamed(const State &s, native::Id id)
{
    for (const auto &[tbp, at] : s.textureAt)
        if (at.second == id)
            return true;
    for (const auto &[tbp, at] : s.residentTexture)
        if (at.second == id)
            return true;
    return false;
}

// Locked. The renderer's evictions (the oldest records past its budget). One
// that GS memory still holds is sent again under the same id, since draws
// keep naming it and its upload may never come again (text, the HUD, an
// area's textures uploaded once); what GS memory holds is bounded by its
// 4 MiB. The rest are forgotten here, so they are made again when uploaded.
void forgetEvicted(State &s)
{
    const std::vector<native::Id> evicted = native::takeEvicted();
    if (!evicted.empty())
        g_evictEpoch.fetch_add(1u, std::memory_order_relaxed);
    for (const native::Id id : evicted)
    {
        // A texture pack's picture: looked up again at its next draw.
        if (const auto region = s.regionOf.find(id); region != s.regionOf.end())
        {
            s.regions.erase(region->second);
            s.regionOf.erase(region);
            continue;
        }
        if (const auto whole = s.wholeOf.find(id); whole != s.wholeOf.end())
        {
            s.wholes.erase(whole->second);
            s.wholeOf.erase(whole);
            continue;
        }
        // A tile: cut again at its next draw.
        if (const auto cut = s.cutOf.find(id); cut != s.cutOf.end())
        {
            s.cuts.erase(cut->second);
            s.cutOf.erase(cut);
            s.texels.erase(id);
            continue;
        }
        const auto key = s.textureKeys.find(id);
        if (key == s.textureKeys.end())
            continue;
        const auto kept = s.texels.find(id);
        if (kept != s.texels.end() && kept->second.data && textureNamed(s, id))
        {
            native::Texture record;
            record.scale = kTexelScale;
            record.id = id;
            record.width = static_cast<uint16_t>(kept->second.width);
            record.height = static_cast<uint16_t>(kept->second.height);
            record.format = kept->second.format;
            record.scale = texelScale(record.format);
            record.palette = kept->second.palette;
            record.texels = kept->second.data;
            native::push(std::move(record));
            ++s.counts.texturesResent;
            continue;
        }
        s.textureIds.erase(key->second);
        s.textureKeys.erase(key);
        if (kept != s.texels.end())
            s.texels.erase(kept);
    }
    if (!s.statsSet)
    {
        native::setProducerStats(&statsLine);
        s.statsSet = true;
    }
}

uint8_t expand5(uint32_t v)
{
    return static_cast<uint8_t>((v & 31u) << 3);
}

// A CT16 texel as RGBA8 with the game's alpha, A set as 0x80: only for the
// texture packs' palette hash, which PCSX2 takes of the CLUT expanded so.
// (Drawn, CT16 stays as the game has it: the renderer's shader expands it
// with the draw's TEXA.)
void ct16(uint16_t v, uint8_t *out)
{
    out[0] = expand5(v);
    out[1] = expand5(v >> 5);
    out[2] = expand5(v >> 10);
    out[3] = (v & 0x8000u) ? 0x80u : 0u;
}

// The byte of a pre-swizzled upload each T8 texel is, as the GS's memory
// lays a CT32 rectangle and reads it back as T8 (relative to the base; a
// page is a page in both). Empty: not that layout.
const std::vector<uint32_t> &unswizzle(const TextureLevel &l)
{
    const std::array<uint32_t, 6> shape = {l.transferWidth, l.transferHeight, std::max(l.transferBufferWidth, 1u),
                                           l.width, l.height, std::max(l.bufferWidth, 1u)};
    static std::map<std::array<uint32_t, 6>, std::vector<uint32_t>> maps; // game thread
    auto known = maps.find(shape);
    if (known != maps.end())
        return known->second;
    if (maps.size() >= 256u)
        maps.clear();
    std::vector<uint32_t> &byteOf = maps[shape];
    uint32_t maxWord = 0u;
    for (uint32_t y = 0u; y < shape[1]; ++y)
        for (uint32_t x = 0u; x < shape[0]; ++x)
            maxWord = std::max(maxWord, rg::swizzle::psmct32::addrPSMCT32(0u, shape[2], x, y) >> 2u);
    std::vector<uint32_t> pixelOfWord(size_t(maxWord) + 1u, ~0u);
    for (uint32_t y = 0u; y < shape[1]; ++y)
        for (uint32_t x = 0u; x < shape[0]; ++x)
            pixelOfWord[rg::swizzle::psmct32::addrPSMCT32(0u, shape[2], x, y) >> 2u] = y * shape[0] + x;
    byteOf.resize(size_t(shape[3]) * shape[4]);
    for (uint32_t y = 0u; y < shape[4]; ++y)
        for (uint32_t x = 0u; x < shape[3]; ++x)
        {
            const uint32_t address = rg::swizzle::psmt8::addrPSMT8(0u, shape[5], x, y);
            const uint32_t word = address >> 2u;
            if (word >= pixelOfWord.size() || pixelOfWord[word] == ~0u)
            {
                byteOf.clear();
                return byteOf;
            }
            byteOf[size_t(y) * shape[3] + x] = pixelOfWord[word] * 4u + (address & 3u);
        }
    return byteOf;
}
} // namespace

namespace
{
// What a level occupies in EE memory, as sent; 0 when it is not taken
// (`format`: for its format, otherwise its size or its memory).
size_t levelBytes(const TextureLevel &l, bool &format)
{
    format = false;
    const uint32_t psm = l.psm & 0x3Fu;
    const size_t texels = size_t(l.width) * l.height;
    size_t bytes = 0u;
    if (l.preSwizzled)
        bytes = size_t(l.transferWidth) * l.transferHeight * 4u;
    else if (psm == kCT32)
        bytes = texels * 4u;
    else if (psm == kCT16 || psm == kCT16S)
        bytes = texels * 2u;
    else if (psm == kT8)
        bytes = texels;
    else if (psm == kT4)
        bytes = (texels + 1u) / 2u;
    else
    {
        format = true;
        return 0u;
    }
    if (texels == 0u || !l.ram || l.ramBytes < bytes || l.width > 4096u || l.height > 4096u)
        return 0u;
    return bytes;
}

// The record of a level whose `bytes` hash to `hash`: its id, made if new
// (0: its layout is not one known). Takes s.mutex itself: the conversion
// runs outside it, which the draw thread takes for every mesh and piece
// (holding it through them put that thread to sleep on the lock some 20
// times a frame, each wake ~200 us; 2026-11-08). `note`: an upload follows,
// for takeProduced, and the game thread's table is kept. `palette`: the
// palette bound at its CBP, when the caller keeps that itself.
native::Id textureRecord(State &s, const TextureLevel &l, size_t bytes, uint64_t hash, size_t hashed, bool note,
                         const native::Id *palette)
{
    const uint32_t psm = l.psm & 0x3Fu;
    const size_t texels = size_t(l.width) * l.height;
    const uint64_t key = keyOf(hash, psm | (l.preSwizzled ? 0x100u : 0u), l.width, l.height);
    std::unique_lock lock(s.mutex);
    s.counts.bytesHashed += hashed;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
    {
        ++s.counts.texturesKnown;
        if (note)
        {
            s.textureAt[l.tbp] = {psm, known->second};
            noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false,
                         l.tbp, psm, known->second, hash, static_cast<uint32_t>(bytes));
        }
        return known->second;
    }
    lock.unlock();
    // New: converted unlocked. Should the draw thread make the same image
    // meanwhile (from a host upload), this record replaces its map entry:
    // one texture twice, never a wrong one.
    auto out = std::make_shared<std::vector<uint8_t>>();
    native::Texture record;
    record.scale = kTexelScale;
    record.width = static_cast<uint16_t>(l.width);
    record.height = static_cast<uint16_t>(l.height);
    if (psm == kCT32 && !l.preSwizzled)
    {
        record.format = native::PixelFormat::Rgba8;
        out->assign(l.ram, l.ram + bytes);
    }
    else if (psm == kCT16 || psm == kCT16S)
    {
        record.format = native::PixelFormat::Ct16;
        out->assign(l.ram, l.ram + texels * 2u);
    }
    else if (l.preSwizzled)
    {
        const std::vector<uint32_t> &byteOf = unswizzle(l);
        if (byteOf.empty())
        {
            lock.lock();
            ++s.counts.refusedLayout;
            return 0u;
        }
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        for (size_t i = 0; i < texels; ++i)
            (*out)[i] = byteOf[i] < bytes ? l.ram[byteOf[i]] : 0u;
    }
    else if (psm == kT8)
    {
        record.format = native::PixelFormat::Index8;
        out->assign(l.ram, l.ram + bytes);
    }
    else if ((l.width & 1u) == 0u) // T4 as the game has it: the shader takes the nibble
    {
        record.format = native::PixelFormat::Index4;
        out->assign(l.ram, l.ram + texels / 2u);
    }
    else // an odd width: widened to a byte each
    {
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        widenIndex4(l.ram, texels, out->data());
    }
    record.scale = texelScale(record.format);
    lock.lock();
    if (psm == kT8 || psm == kT4)
        record.palette = palette ? *palette : (s.paletteAt.count(l.cbp) ? s.paletteAt[l.cbp] : 0u);
    const native::Id id = record.id = s.nextTexture++;
    s.texels[record.id] = {out, record.width, record.height, record.format, record.palette};
    record.texels = std::move(out);
    s.textureIds[key] = record.id;
    s.textureKeys[record.id] = key;
    if (note)
    {
        s.textureAt[l.tbp] = {psm, record.id};
        noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false, l.tbp,
                     psm, record.id, hash, static_cast<uint32_t>(bytes));
    }
    ++s.counts.texturesNew;
    native::push(std::move(record));
    return id;
}

// Whether a palette is one taken, and its bytes in EE memory.
bool paletteUsable(const uint8_t *ram, uint32_t ramBytes, uint32_t cpsm, uint32_t entries, uint32_t &bytes)
{
    const bool is16 = cpsm == kCT16 || cpsm == kCT16S;
    bytes = entries * (is16 ? 2u : 4u);
    return ram && (entries == 16u || entries == 256u) && (cpsm == kCT32 || is16) && ramBytes >= bytes;
}

// Locked. The record of a usable palette whose bytes hash to `hash`: its id,
// made if new. `note`: as textureRecord's.
native::Id paletteRecord(State &s, const uint8_t *ram, uint32_t cbp, uint32_t cpsm, uint32_t entries, uint64_t hash,
                         uint32_t bytes, bool note)
{
    const bool is16 = cpsm == kCT16 || cpsm == kCT16S;
    const uint64_t key = keyOf(hash, cpsm, entries, 0u);
    if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
    {
        ++s.counts.palettesKnown;
        if (note)
        {
            s.paletteAt[cbp] = known->second;
            noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                         known->second, hash, bytes);
        }
        return known->second;
    }
    // CT16 entries stay as the game has them (the shader expands them with
    // the draw's TEXA).
    const uint32_t entryBytes = is16 ? 2u : 4u;
    auto colors = std::make_shared<std::vector<uint8_t>>(entries * entryBytes);
    for (uint32_t i = 0; i < entries; ++i)
    {
        // CSM1: a 256-entry palette's entries 8-15 and 16-23 of each 32 are
        // stored swapped.
        const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
        std::copy_n(ram + at * entryBytes, entryBytes, colors->data() + i * entryBytes);
    }
    native::Palette record;
    record.format = is16 ? native::PixelFormat::Ct16 : native::PixelFormat::Rgba8;
    record.scale = texelScale(record.format);
    const native::Id id = record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(entries);
    s.paletteColors[record.id] = colors;
    record.colors = std::move(colors);
    s.paletteIds[key] = record.id;
    if (note)
    {
        s.paletteAt[cbp] = record.id;
        noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                     record.id, hash, bytes);
    }
    ++s.counts.palettesNew;
    native::push(std::move(record));
    return id;
}

// The game thread's bindings: what bindResolve counted since bindBegin, and
// the palette it last bound at each CBP (a new texture's own palette).
struct BindCounts
{
    uint64_t levels = 0u, known = 0u, palettesKnown = 0u, bytesHashed = 0u;
};
thread_local BindCounts t_bind;
thread_local std::vector<native::Id> t_boundPalette;

// A level bound: its record's id. A source unchanged since its last binding
// (the write watch) takes no lock and reads no pixel.
native::Id bindTexture(State &s, const TextureLevel &l)
{
    bool format = false;
    const size_t bytes = levelBytes(l, format);
    if (bytes == 0u)
    {
        std::lock_guard lock(s.mutex);
        ++s.counts.levels;
        ++(format ? s.counts.refusedFormat : s.counts.refusedBytes);
        return 0u;
    }
    size_t hashed = 0u;
    SeenSource *seen = nullptr;
    const uint64_t hash = sourceHash(l.ram, bytes, hashed, &seen);
    const uint64_t key = keyOf(hash, (l.psm & 0x3Fu) | (l.preSwizzled ? 0x100u : 0u), l.width, l.height);
    const uint64_t epoch = g_evictEpoch.load(std::memory_order_relaxed);
    ++t_bind.levels;
    t_bind.bytesHashed += hashed;
    if (seen && seen->id != 0u && seen->key == key && seen->epoch == epoch)
    {
        ++t_bind.known;
        return seen->id;
    }
    if (t_boundPalette.empty())
        t_boundPalette.resize(0x4000u);
    const native::Id id = textureRecord(s, l, bytes, hash, 0u, false, &t_boundPalette[l.cbp & 0x3FFFu]);
    if (seen && id != 0u)
    {
        seen->key = key;
        seen->id = id;
        seen->epoch = epoch;
    }
    return id;
}

native::Id bindPalette(State &s, const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries)
{
    cpsm &= 0x3Fu;
    uint32_t bytes = 0u;
    if (!paletteUsable(ram, ramBytes, cpsm, entries, bytes))
    {
        std::lock_guard lock(s.mutex);
        ++s.counts.palettesRefused;
        return 0u;
    }
    size_t hashed = 0u;
    SeenSource *seen = nullptr;
    const uint64_t hash = sourceHash(ram, bytes, hashed, &seen);
    const uint64_t key = keyOf(hash, cpsm, entries, 0u);
    t_bind.bytesHashed += hashed;
    native::Id id = 0u;
    // (Palettes are never evicted: no epoch.)
    if (seen && seen->id != 0u && seen->key == key)
    {
        ++t_bind.palettesKnown;
        id = seen->id;
    }
    else
    {
        std::lock_guard lock(s.mutex);
        id = paletteRecord(s, ram, cbp, cpsm, entries, hash, bytes, false);
        if (seen)
        {
            seen->key = key;
            seen->id = id;
        }
    }
    if (t_boundPalette.empty())
        t_boundPalette.resize(0x4000u);
    t_boundPalette[cbp & 0x3FFFu] = id;
    return id;
}
} // namespace

// The game thread's, for every level it sends with its upload (the texture
// hook's old way: the upload follows and is withheld, takeProduced).
uint32_t colorReadKeep(uint32_t address, uint32_t width, uint32_t height)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    State::KeptRead &k = s.keptReads[address];
    if (k.id == 0u)
        k.id = s.nextTarget++;
    if (k.width != width || k.height != height)
    {
        k.width = width;
        k.height = height;
        native::Target record;
        record.id = k.id;
        record.role = native::TargetRole::Offscreen;
        record.width = static_cast<uint16_t>(std::min<uint32_t>(width, 4096u));
        record.height = static_cast<uint16_t>(std::min<uint32_t>(height, 4096u));
        native::push(native::Target(record));
        ++s.counts.targetsNew;
    }
    k.size = 0u;
    k.hash = 0u;
    return k.id;
}

void colorReadAnswered(uint32_t address, const uint8_t *bytes, size_t size)
{
    const uint64_t hash = bytes && size ? imageHash(bytes, size) : 0u;
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto k = s.keptReads.find(address);
    if (k == s.keptReads.end())
        return;
    k->second.size = size;
    k->second.hash = hash;
}

namespace
{
// Locked. The kept read whose answer `ram` still holds, or null.
const State::KeptRead *keptReadAt(State &s, uint32_t address, const uint8_t *ram, size_t ramBytes)
{
    if (address == 0u || !ram)
        return nullptr;
    const auto k = s.keptReads.find(address);
    if (k == s.keptReads.end() || k->second.size == 0u || ramBytes < k->second.size)
        return nullptr;
    return imageHash(ram, k->second.size) == k->second.hash ? &k->second : nullptr;
}
} // namespace

bool keptColorRead(uint32_t address, const uint8_t *ram, size_t ramBytes)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    return keptReadAt(s, address, ram, ramBytes) != nullptr;
}

void texture(const TextureLevel &l)
{
    State &s = state();
    {
        // A captured frame uploaded back, unchanged: a copy of the kept
        // target, at the renderer's resolution (the bytes stand in).
        std::lock_guard lock(s.mutex);
        if (const State::KeptRead *k = keptReadAt(s, l.address, l.ram, l.ramBytes);
            k && l.width == k->width && l.height == k->height && !l.preSwizzled)
        {
            const uint32_t psm = l.psm & 0x3Fu;
            const uint64_t key = keyOf(k->hash, psm | 0x800u, l.width, l.height);
            native::Id id = 0u;
            if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
                id = known->second;
            else
            {
                native::Texture record;
                record.id = id = s.nextTexture++;
                record.width = static_cast<uint16_t>(l.width);
                record.height = static_cast<uint16_t>(l.height);
                record.fromTarget = k->id;
                record.format = native::PixelFormat::Rgba8;
                record.scale = kTexelScale;
                if (psm == kCT32)
                    record.texels = std::make_shared<std::vector<uint8_t>>(l.ram, l.ram + k->size);
                s.textureIds[key] = id;
                s.textureKeys[id] = key;
                native::push(std::move(record));
                ++s.counts.texturesFromReads;
            }
            s.textureAt[l.tbp] = {psm, id};
            noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false,
                         l.tbp, psm, id, k->hash, static_cast<uint32_t>(k->size));
            return;
        }
    }
    bool format = false;
    const size_t bytes = levelBytes(l, format);
    {
        std::lock_guard lock(s.mutex);
        forgetEvicted(s);
        ++s.counts.levels;
        if (bytes == 0u)
        {
            ++(format ? s.counts.refusedFormat : s.counts.refusedBytes);
            return;
        }
    }
    size_t hashed = 0u;
    const uint64_t hash = sourceHash(l.ram, bytes, hashed);
    textureRecord(s, l, bytes, hash, hashed, true, nullptr);
}

void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries)
{
    State &s = state();
    cpsm &= 0x3Fu;
    uint32_t bytes = 0u;
    const bool usable = paletteUsable(ram, ramBytes, cpsm, entries, bytes);
    // Hashed unlocked, as a texture is.
    const uint64_t hash = usable ? imageHash(ram, bytes) : 0u;
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    if (!usable)
    {
        ++s.counts.palettesRefused;
        return;
    }
    s.counts.bytesHashed += bytes;
    paletteRecord(s, ram, cbp, cpsm, entries, hash, bytes, true);
}

bool bindable(const TextureLevel &l)
{
    bool format = false;
    return levelBytes(l, format) != 0u && (!l.preSwizzled || !unswizzle(l).empty());
}

uint64_t retainedEpoch()
{
    return native::retainedEpoch();
}

void bindBegin()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    s.counts.levels += t_bind.levels;
    s.counts.texturesKnown += t_bind.known;
    s.counts.palettesKnown += t_bind.palettesKnown;
    s.counts.bytesHashed += t_bind.bytesHashed;
    t_bind = {};
}

uint64_t bindResolve(const uint8_t *rdram, uint32_t ramSize, const BindEntry &e)
{
    const bool isPalette = (e.flags & kBindPalette) != 0u;
    const uint64_t place = (uint64_t(e.at & 0x3FFFu) << 32) | (uint64_t(e.psm & 0x3Fu) << 46) |
                           (uint64_t(isPalette ? 1u : 0u) << 52);
    if (e.source >= ramSize)
        return place;
    State &s = state();
    native::Id id = 0u;
    if (isPalette)
        id = bindPalette(s, rdram + e.source, ramSize - e.source, e.at, e.psm, e.width);
    else
    {
        TextureLevel l;
        l.ram = rdram + e.source;
        l.ramBytes = ramSize - e.source;
        l.tbp = e.at & 0x3FFFu;
        l.psm = e.psm;
        l.preSwizzled = (e.flags & kBindPreSwizzled) != 0u;
        l.transferWidth = e.width;
        l.transferHeight = e.height;
        l.width = l.preSwizzled ? l.transferWidth * 2u : l.transferWidth;
        l.height = l.preSwizzled ? l.transferHeight * 2u : l.transferHeight;
        l.bufferWidth = std::max<uint32_t>(e.bufferWidth, 1u);
        l.transferBufferWidth = l.preSwizzled ? std::max<uint32_t>(l.bufferWidth >> 1, 1u) : l.bufferWidth;
        l.transferPsm = l.preSwizzled ? 0u : e.psm;
        l.cbp = e.cbp & 0x3FFFu;
        id = bindTexture(s, l);
    }
    return place | id;
}

void bound(const uint64_t *values, size_t count)
{
    if (count == 0u)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    for (size_t i = 0; i < count; ++i)
    {
        const native::Id id = static_cast<native::Id>(values[i]);
        const uint32_t at = static_cast<uint32_t>((values[i] >> 32) & 0x3FFFu);
        const uint32_t psm = static_cast<uint32_t>((values[i] >> 46) & 0x3Fu);
        if (id == 0u)
        {
            ++s.counts.bindRefused;
            continue;
        }
        // As takeProduced leaves a withheld upload.
        if ((values[i] >> 52) & 1u)
        {
            s.residentPalette[at] = id;
            s.takenPaletteSeq[at] = ++s.sequence;
            ++s.counts.boundPalettes;
        }
        else
        {
            s.residentTexture[at] = {psm, id};
            s.uploadedSeq[at] = s.takenSeq[at] = ++s.sequence;
            ++s.counts.boundTextures;
        }
    }
}

namespace
{
// Locked. target()'s declaration; the id.
native::Id declare(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    native::Target record;
    record.role = kind == 1 ? native::TargetRole::SceneColor
                  : kind == 2 ? native::TargetRole::Display
                              : native::TargetRole::Offscreen;
    // The scene is one buffer: the first the game names as its frame.
    // Another named so (CBlur's 02a40 is) is a buffer of its own; sharing
    // the scene's id, a copy into it overwrote the scene.
    if (record.role == native::TargetRole::SceneColor)
    {
        if (!s.sceneBaseKnown)
        {
            s.sceneBase = base;
            s.sceneBaseKnown = true;
        }
        else if (base != s.sceneBase)
            record.role = native::TargetRole::Offscreen;
    }
    record.width = static_cast<uint16_t>(std::min<uint32_t>(width, 4096u));
    record.height = static_cast<uint16_t>(std::min<uint32_t>(height, 4096u));
    record.format = (psm & 0x30u) == 0x30u ? native::PixelFormat::Depth32 : native::PixelFormat::Rgba8;
    auto at = s.targetAt.find(base);
    // A buffer other than the scene is used past the size it is named with
    // (draws below a layer's height): it keeps the largest it was named or
    // drawn at, so it never shrinks.
    if (const auto before = s.targets.find(base);
        before != s.targets.end() && record.role != native::TargetRole::SceneColor &&
        before->second.role != native::TargetRole::SceneColor)
    {
        record.width = std::max(record.width, before->second.width);
        record.height = std::max(record.height, before->second.height);
    }
    if (record.role == native::TargetRole::SceneColor)
        record.id = kSceneTarget;
    else if (at != s.targetAt.end() && at->second != kSceneTarget)
        record.id = at->second;
    else
        record.id = s.nextTarget++;
    const auto before = s.targets.find(base);
    const bool same = before != s.targets.end() && before->second.id == record.id &&
                      before->second.role == record.role && before->second.width == record.width &&
                      before->second.height == record.height && before->second.format == record.format;
    s.targetAt[base] = record.id;
    if (same)
        return record.id;
    s.targets[base] = record;
    ++s.counts.targetsNew;
    native::push(native::Target(record));
    return record.id;
}

// Locked. The target at `base`; one the game never named is declared from
// what draws into it (counted), and one drawn past its size grows to it.
native::Id targetForUnrouted(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm);

// Locked. The layer target, declared at least as large as the routed buffer.
native::Id layerTarget(State &s, uint32_t width, uint32_t height)
{
    if (s.layer == 0u)
        s.layer = s.nextTarget++;
    if (width > s.layerWidth || height > s.layerHeight)
    {
        s.layerWidth = static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(width, s.layerWidth), 4096u));
        s.layerHeight = static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(height, s.layerHeight), 4096u));
        native::Target record;
        record.id = s.layer;
        record.role = native::TargetRole::Offscreen;
        record.width = s.layerWidth;
        record.height = s.layerHeight;
        native::push(native::Target(record));
        ++s.counts.targetsNew;
    }
    return s.layer;
}

// Locked. A draw into the buffer at `base`: its target, or the object's
// layer while one is routed there.
native::Id targetFor(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm)
{
    const native::Id id = targetForUnrouted(s, base, width, height, psm);
    if (!s.routing || base != s.routeBase || id == 0u)
        return id;
    ++s.counts.drawsRouted;
    const auto declared = s.targets.find(base);
    return layerTarget(s, declared != s.targets.end() ? declared->second.width : width,
                       declared != s.targets.end() ? declared->second.height : height);
}

native::Id targetForUnrouted(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm)
{
    if (const auto at = s.targetAt.find(base); at != s.targetAt.end())
    {
        const auto declared = s.targets.find(base);
        if (declared != s.targets.end() && declared->second.role != native::TargetRole::SceneColor &&
            (width > declared->second.width || height > declared->second.height))
        {
            ++s.counts.targetsGrown;
            const int kind = declared->second.role == native::TargetRole::Display ? 2 : 0;
            return declare(s, base, std::max<uint32_t>(width, declared->second.width),
                           std::max<uint32_t>(height, declared->second.height), psm, kind);
        }
        return at->second;
    }
    if (width == 0u || height == 0u)
        return 0u;
    ++s.counts.targetsAuto;
    return declare(s, base, width, height, psm, 0);
}
uint32_t viewKey(uint32_t base, uint32_t fbw)
{
    return 0x80000000u | (std::min(std::max(fbw, 1u), 63u) << 16) | (base & 0xFFFFu);
}

// Locked. A draw or read at `base` with buffer width `fbw` (64-pixel
// units) narrower than the base's target: one for a view (State::views).
bool narrow(State &s, uint32_t base, uint32_t fbw)
{
    const auto t = s.targets.find(base);
    return t != s.targets.end() && t->second.role != native::TargetRole::SceneColor &&
           std::max(fbw, 1u) * 64u < t->second.width;
}

// Locked. The view at (base, fbw), declared, or grown to `width` x `height`.
native::Id viewTarget(State &s, uint32_t base, uint32_t fbw, uint32_t width, uint32_t height)
{
    fbw = std::max(fbw, 1u);
    width = std::min(std::min(width, fbw * 64u), 4096u);
    height = std::min(std::max(height, 1u), 4096u);
    native::Target &v = s.views[viewKey(base, fbw)];
    if (v.id != 0u && width <= v.width && height <= v.height)
        return v.id;
    if (v.id == 0u)
    {
        v.id = s.nextTarget++;
        s.viewWidths[base].push_back(fbw);
        ++s.counts.viewsNew;
    }
    v.role = native::TargetRole::Offscreen;
    v.format = native::PixelFormat::Rgba8;
    v.width = static_cast<uint16_t>(std::max<uint32_t>(width, v.width));
    v.height = static_cast<uint16_t>(std::max<uint32_t>(height, v.height));
    native::push(native::Target(v));
    return v.id;
}
} // namespace

void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    if (width == 0u || height == 0u)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    (void)bufferWidth;
    declare(s, base, width, height, psm, kind);
}

bool takeProduced(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool &palette,
                  uint64_t &hash, uint32_t &bytes)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    constexpr size_t kLookahead = 64u;
    const size_t limit = std::min(s.produced.size(), kLookahead);
    for (size_t i = 0; i < limit; ++i)
    {
        const State::Produced &p = s.produced[i];
        if (p.dbp == (dbp & 0x3FFFu) && p.dbw == std::max(dbw, 1u) && p.dpsm == (dpsm & 0x3Fu) && p.width == width &&
            p.height == height)
        {
            palette = p.palette;
            hash = p.hash;
            bytes = p.bytes;
            if (p.palette)
            {
                s.residentPalette[p.key] = p.id;
                s.takenPaletteSeq[p.key] = ++s.sequence;
            }
            else
            {
                s.residentTexture[p.key] = {p.psm, p.id};
                s.uploadedSeq[p.key] = s.takenSeq[p.key] = ++s.sequence;
            }
            // The ones before it were never sent (docs/NATIVE-PIPELINE.md
            // step 6, "produced levels never taken"): the first few, by shape.
            static uint32_t told = 0u;
            for (size_t j = 0; j < i && told < 8u; ++j, ++told)
            {
                const State::Produced &q = s.produced[j];
                std::fprintf(stderr,
                             "[native-producer] produced, never taken: DBP %04x DBW %u DPSM %02x %ux%u%s; the next "
                             "taken: DBP %04x DBW %u DPSM %02x %ux%u\n",
                             q.dbp, q.dbw, q.dpsm, q.width, q.height, q.palette ? " (palette)" : "", p.dbp, p.dbw,
                             p.dpsm, p.width, p.height);
            }
            s.produced.erase(s.produced.begin(), s.produced.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return true;
        }
    }
    return false;
}

void replaceUpload(bool palette, uint32_t dbp, uint32_t dpsm, uint32_t width, uint32_t height, const uint8_t *data,
                   uint32_t bytes)
{
    if (!data)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    dpsm &= 0x3Fu;
    const uint64_t hash = imageHash(data, bytes);
    if (palette)
    {
        const uint32_t entries = width * height;
        const bool is16 = dpsm == kCT16 || dpsm == kCT16S;
        if ((entries != 16u && entries != 256u) || (dpsm != kCT32 && !is16) || bytes < entries * (is16 ? 2u : 4u))
            return;
        const uint64_t key = keyOf(hash, dpsm, entries, 0u);
        native::Id id = 0u;
        if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
            id = known->second;
        else
        {
            const uint32_t entryBytes = is16 ? 2u : 4u;
            auto colors = std::make_shared<std::vector<uint8_t>>(entries * entryBytes);
            for (uint32_t i = 0; i < entries; ++i)
            {
                const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
                std::copy_n(data + at * entryBytes, entryBytes, colors->data() + i * entryBytes);
            }
            native::Palette record;
            record.format = is16 ? native::PixelFormat::Ct16 : native::PixelFormat::Rgba8;
            record.scale = texelScale(record.format);
            record.id = id = s.nextPalette++;
            record.entries = static_cast<uint16_t>(entries);
            s.paletteColors[record.id] = colors;
            record.colors = std::move(colors);
            s.paletteIds[key] = record.id;
            native::push(std::move(record));
        }
        s.residentPalette[dbp] = id;
        s.takenPaletteSeq[dbp] = ++s.sequence;
        ++s.counts.palettesReplaced;
        return;
    }
    // A texture: only one sent as the format it is read in (not pre-swizzled).
    const auto resident = s.residentTexture.find(dbp);
    if (resident == s.residentTexture.end() || resident->second.first != dpsm)
        return;
    const size_t texels = size_t(width) * height;
    native::Texture record;
    record.scale = kTexelScale;
    auto out = std::make_shared<std::vector<uint8_t>>();
    if (dpsm == kCT32 && bytes >= texels * 4u)
    {
        record.format = native::PixelFormat::Rgba8;
        out->assign(data, data + texels * 4u);
    }
    else if ((dpsm == kCT16 || dpsm == kCT16S) && bytes >= texels * 2u)
    {
        record.format = native::PixelFormat::Ct16;
        out->assign(data, data + texels * 2u);
    }
    else if (dpsm == kT8 && bytes >= texels)
    {
        record.format = native::PixelFormat::Index8;
        out->assign(data, data + texels);
    }
    else if (dpsm == kT4 && bytes >= (texels + 1u) / 2u && (width & 1u) == 0u)
    {
        record.format = native::PixelFormat::Index4;
        out->assign(data, data + texels / 2u);
    }
    else if (dpsm == kT4 && bytes >= (texels + 1u) / 2u)
    {
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        widenIndex4(data, texels, out->data());
    }
    else
        return;
    const uint64_t key = keyOf(hash, dpsm, width, height);
    native::Id id = 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
        id = known->second;
    else
    {
        record.width = static_cast<uint16_t>(width);
        record.height = static_cast<uint16_t>(height);
        record.scale = texelScale(record.format);
        record.id = id = s.nextTexture++;
        s.texels[record.id] = {out, record.width, record.height, record.format};
        record.texels = std::move(out);
        s.textureIds[key] = record.id;
        s.textureKeys[record.id] = key;
        native::push(std::move(record));
    }
    resident->second.second = id;
    s.uploadedSeq[dbp] = s.takenSeq[dbp] = ++s.sequence;
    ++s.counts.texturesReplaced;
}

bool sourceUnchanged(const uint8_t *at, size_t size, uint64_t &hash)
{
    if (!rg::writewatch::enabled())
        return false;
    const auto it = t_seenSources.find(seenKey(at, size));
    if (it == t_seenSources.end() || it->second.at != at || it->second.size != size || it->second.stamp == 0u ||
        !rg::writewatch::unchanged(at, size, it->second.stamp))
        return false;
    hash = it->second.hash;
    return true;
}

void noteElided(uint32_t imageBytes, bool fetched)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++(fetched ? s.counts.elidedFetched : s.counts.elided);
    if (!fetched)
        s.counts.elidedBytes += imageBytes;
}

void noteWithheld(bool palette, uint32_t imageBytes)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++(palette ? s.counts.withheldPalettes : s.counts.withheldTextures);
    s.counts.withheldBytes += imageBytes;
}

void notePassed(Passed why)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    (void)why;
    ++s.counts.passedNotProduced;
}

void noteSplit()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++s.counts.split;
}

void noteContinuedInPacket()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++s.counts.continuedInPacket;
}

namespace
{
// A 4x4 of row vectors from VU1 data memory, quads `first` to `first + 3`.
void quadsMatrix(const uint8_t *vu1, uint32_t first, float out[16])
{
    std::memcpy(out, vu1 + first * 16u, 16u * sizeof(float));
}

void multiply(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}

// Gauss-Jordan with partial pivoting; false when `m` is singular.
bool invert(const float m[16], float out[16])
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12)
            return false;
        if (pivot != c)
            for (int j = 0; j < 8; ++j)
                std::swap(a[c][j], a[pivot][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j)
            a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = static_cast<float>(a[i][j + 4]);
    return true;
}

// `m` followed by the window's offset: x - ofx and y - ofy after the divide.
void foldOffset(float m[16], float ofx, float ofy)
{
    for (int i = 0; i < 4; ++i)
    {
        m[i * 4 + 0] -= m[i * 4 + 3] * ofx;
        m[i * 4 + 1] -= m[i * 4 + 3] * ofy;
    }
}

// Whether model x view x projection x viewport is the program's own matrix
// (68-71), column by column within 2e-3 of the column's largest entry.
bool sameMatrix(const float a[16], const float (&b)[4][4])
{
    for (int j = 0; j < 4; ++j)
    {
        float scale = 0.0f, error = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            scale = std::max(scale, std::fabs(b[i][j]));
            error = std::max(error, std::fabs(a[i * 4 + j] - b[i][j]));
        }
        if (!(error <= 2e-3f * scale + 1e-6f))
            return false;
    }
    return true;
}

native::Compare alphaCompare(uint32_t atst)
{
    static constexpr native::Compare table[8] = {
        native::Compare::Never,        native::Compare::Always,  native::Compare::Less,
        native::Compare::LessEqual,    native::Compare::Equal,   native::Compare::GreaterEqual,
        native::Compare::Greater,      native::Compare::NotEqual};
    return table[atst & 7u];
}

bool indexedPsm(uint32_t psm)
{
    return psm == kT8 || psm == kT4 || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu;
}

// The blend ALPHA means, (A - B) * C + D; A, B, D: 0 Cs, 1 Cd, 2 zero; C:
// 0 As, 1 Ad, 2 FIX. Every such equation is Cs * Fs + Cd * Fd, each factor
// 0, 1, C, 1 - C or -C: a GPU blend, a negative factor its op's subtract.
// False for C 3 (reserved) and a factor of 1 + C (A and D both Cs or Cd),
// which a GPU blend has no factor for.
bool blendOf(uint64_t alpha, native::Material &m)
{
    const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
    m.blending = true;
    m.blendConstant = float((alpha >> 32) & 0xFFu) * kUnit;
    // The coefficient of a color (0 Cs, 1 Cd): k1 * C + k0.
    const auto coefficient = [&](uint32_t color, int &k1, int &k0) {
        k1 = (a == color ? 1 : 0) - (b == color ? 1 : 0);
        k0 = d == color ? 1 : 0;
    };
    const native::BlendFactor withC = c == 0u ? native::BlendFactor::SrcAlpha
                                      : c == 1u ? native::BlendFactor::DstAlpha
                                                : native::BlendFactor::Fixed;
    const native::BlendFactor withoutC = c == 0u ? native::BlendFactor::InvSrcAlpha
                                         : c == 1u ? native::BlendFactor::InvDstAlpha
                                                   : native::BlendFactor::InvFixed;
    bool known = c != 3u;
    bool negative[2] = {};
    native::BlendFactor factors[2];
    for (uint32_t color = 0; color < 2u; ++color)
    {
        int k1, k0;
        coefficient(color, k1, k0);
        native::BlendFactor &f = factors[color];
        if (k1 == 0)
            f = k0 ? native::BlendFactor::One : native::BlendFactor::Zero;
        else if (k1 == 1 && k0 == 0)
            f = withC;
        else if (k1 == -1 && k0 == 1)
            f = withoutC;
        else if (k1 == -1 && k0 == 0)
        {
            f = withC;
            negative[color] = true;
        }
        else
        {
            f = native::BlendFactor::One; // 1 + C
            known = false;
        }
    }
    m.srcFactor = factors[0];
    m.dstFactor = factors[1];
    m.blendOp = negative[0] ? native::BlendOp::ReverseSubtract
                : negative[1] ? native::BlendOp::Subtract
                              : native::BlendOp::Add;
    return known;
}

// The alpha, destination alpha and depth tests TEST means, with ZBUF's mask.
bool testOf(uint64_t test, bool zmask, native::Material &m)
{
    if (test & 1u)
    {
        m.alphaTest = alphaCompare(static_cast<uint32_t>(test >> 1));
        m.alphaReference = float((test >> 4) & 0xFFu) * kUnit;
        static constexpr native::AlphaFail fails[4] = {native::AlphaFail::Nothing, native::AlphaFail::ColorOnly,
                                                      native::AlphaFail::DepthOnly, native::AlphaFail::RgbOnly};
        m.alphaFail = fails[(test >> 12) & 3u];
    }
    const bool zte = ((test >> 16) & 1u) != 0u;
    static constexpr native::Compare depths[4] = {native::Compare::Never, native::Compare::Always,
                                                  native::Compare::GreaterEqual, native::Compare::Greater};
    m.depthTest = zte ? depths[(test >> 17) & 3u] : native::Compare::Always;
    m.depthWrite = !zmask && m.depthTest != native::Compare::Never;
    // DATE, by DATM: the pixels whose alpha bit 7 is DATM's.
    m.destinationAlphaTest = ((test >> 14) & 1u) ? static_cast<int8_t>((test >> 15) & 1u) : int8_t(-1);
    return true;
}

// Locked. The material the draw state means; false where some part of it
// has no exact meaning here (counted by reason).
} // namespace

namespace
{
// Whether a draw reads CT16 texels: a CT16 texture, or a palette of CT16
// entries. Their alpha is the draw's TEXA, applied by the shader.
bool ct16Read(uint32_t psm, uint32_t cpsm)
{
    psm &= 0x3Fu;
    cpsm &= 0x3Fu;
    const bool is16 = psm == kCT16 || psm == kCT16S;
    return is16 || (indexedPsm(psm) && (cpsm == kCT16 || cpsm == kCT16S));
}

void texa16(native::Material &m, const rg::TexaReg &texa)
{
    m.texaLow = float(texa.ta0) * kUnit;
    m.texaHigh = float(texa.ta1) * kUnit;
    m.alphaZeroOnBlack = texa.aem;
}
} // namespace
bool sourceOf(State &s, const rg::Tex0Reg &t, native::Material &m);
void replaceWhole(State &s, const rg::Tex0Reg &t, native::Material &m);
void cutRegion(State &s, uint64_t clamp, native::Material &m);
void replaceRegion(State &s, const rg::DrawState &state, const rg::NativeMesh &mesh, native::Material &m);
namespace
{
// Locked. The `count` entries (16 or 256) of palette `id` from `first` as a
// palette record of their own (a CSA window); 0 when it has fewer.
native::Id paletteWindow(State &s, native::Id id, uint32_t first, uint32_t count = 16u)
{
    const uint64_t key = (uint64_t(id) << 32) | (uint64_t(count) << 16) | first;
    if (const auto known = s.paletteWindows.find(key); known != s.paletteWindows.end())
        return known->second;
    const auto colors = s.paletteColors.find(id);
    if (colors == s.paletteColors.end() || !colors->second)
        return 0u;
    // The entry size from the record's bytes: CT16 2, CT32 4 (256 or 16 entries).
    const size_t bytes = colors->second->size();
    const uint32_t entryBytes = (bytes == 512u * 2u || bytes == 256u * 2u || bytes == 16u * 2u) ? 2u : 4u;
    if (bytes < size_t(first + count) * entryBytes)
        return 0u;
    auto window = std::make_shared<std::vector<uint8_t>>(colors->second->begin() + first * entryBytes,
                                                         colors->second->begin() + (first + count) * entryBytes);
    native::Palette record;
    record.format = entryBytes == 2u ? native::PixelFormat::Ct16 : native::PixelFormat::Rgba8;
    record.scale = texelScale(record.format);
    record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(count);
    s.paletteColors[record.id] = window;
    record.colors = std::move(window);
    s.paletteWindows[key] = record.id;
    const native::Id made = record.id;
    native::push(std::move(record));
    return made;
}

bool materialOf(State &s, const rg::DrawState &state, native::Material &m)
{
    const rg::DrawContext &c = state.context;
    bool known = true;
    m.colorClamp = kColorClamp;
    if (state.prim.tme)
    {
        const rg::Tex0Reg &t = c.tex0;
        // As a 2D piece's: a target drawn there, a produced texture, or a
        // host upload's image, whichever was written last (sourceOf).
        if (!sourceOf(s, t, m))
        {
            ++s.counts.textureMiss;
            const auto at = s.residentTexture.find(t.tbp0);
            if (at != s.residentTexture.end() && at->second.first != (t.psm & 0x3Fu))
            {
                ++s.counts.textureMissPsm;
                static uint64_t seen[64] = {};
                const uint32_t pair = at->second.first & 0x3Fu;
                if (std::getenv("RG_NATIVE_MESH_DUMP") && !(seen[pair] & (1ull << (t.psm & 0x3Fu))))
                {
                    seen[pair] |= 1ull << (t.psm & 0x3Fu);
                    std::fprintf(stderr, "[mesh-miss] tbp %x recorded psm %x, sampled psm %x tw %u th %u\n", t.tbp0,
                                 pair, t.psm, t.tw, t.th);
                }
            }
            else if (indexedPsm(t.psm) && at != s.residentTexture.end())
                ++s.counts.paletteMiss;
            known = false;
        }
        else if ((t.psm & 0x3Fu) == 0x01u)
        {
            // CT24, a target or a CT32 texture: alpha from TEXA (as piece()).
            m.sourceAlpha = float(state.texa.ta0) * kUnit;
            m.alphaZeroOnBlack = state.texa.aem;
        }
        else if (ct16Read(t.psm, t.cpsm))
            texa16(m, state.texa);
        else if (m.sourceTarget && (t.psm & 0x3Fu) != 0x00u)
            known = false; // a target read in another format: not modelled
        m.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(t.tw, 10u));
        m.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(t.th, 10u));
        m.linear = state.linearFilter;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        // Region clamp and repeat (a tile of a sheet): a texture of its own.
        const bool region = wms > 1u || wmt > 1u;
        if (region && !m.sourceTarget)
            cutRegion(s, c.clamp, m);
        else
        {
            m.clampU = wms != 0u;
            m.clampV = wmt != 0u;
        }
        if ((wms > 1u || wmt > 1u) && m.sourceTarget)
        {
            ++s.counts.unknownClamp; // a target's region: not fetched so
            known = false;
        }
        static constexpr native::TextureFunction functions[4] = {
            native::TextureFunction::Modulate, native::TextureFunction::Decal, native::TextureFunction::Highlight,
            native::TextureFunction::Highlight2};
        m.function = functions[t.tfx & 3u];
        m.textureAlpha = t.tcc != 0u;
        if (!region)
            replaceWhole(s, t, m);
    }

    if (state.prim.abe && !blendOf(c.alpha, m))
    {
        ++s.counts.unknownBlend;
        known = false;
    }

    if (!testOf(c.test, c.zbuf.zmask, m) || (c.fba & 1u))
    {
        ++s.counts.unknownOther;
        known = false;
    }

    // FBMSK: a channel is written when none of its bits are masked.
    const uint32_t mask = c.frame.fbmsk;
    m.colorWrite = 0u;
    bool partial = false;
    for (uint32_t ch = 0; ch < 4u; ++ch)
    {
        const uint32_t bits = (mask >> (ch * 8u)) & 0xFFu;
        if (bits == 0u)
            m.colorWrite |= static_cast<uint8_t>(1u << ch);
        else if (bits != 0xFFu)
            partial = true;
    }
    if (partial)
    {
        ++s.counts.unknownWriteMask;
        known = false;
    }
    m.fog = state.prim.fge;
    m.cullBack = false; // the producer's indices already leave out what the program culls
    return known;
}

// Locked. The id of the depth buffer at `base`, declared like the color
// target it is drawn with.
native::Id depthTarget(State &s, uint32_t base, uint32_t colorBase, native::Id color)
{
    // Its first use in the frame clears it (to the far plane).
    const auto firstUse = [&s](native::Id id) {
        if (s.depthsThisFrame.insert(id).second)
        {
            native::Clear clear;
            clear.target = id;
            clear.depth = true;
            clear.depthValue = 0.0f;
            native::push(std::move(clear));
        }
        return id;
    };
    const auto at = s.depthAt.find(base);
    if (at != s.depthAt.end())
        return firstUse(at->second);
    native::Target record;
    record.id = s.nextTarget++;
    record.role = color == kSceneTarget ? native::TargetRole::SceneDepth : native::TargetRole::Offscreen;
    record.format = native::PixelFormat::Depth32;
    record.width = 640u;
    record.height = 448u;
    if (const auto declared = s.targets.find(colorBase); declared != s.targets.end())
    {
        record.width = declared->second.width;
        record.height = declared->second.height;
    }
    s.depthAt.emplace(base, record.id);
    ++s.counts.targetsNew;
    native::push(native::Target(record));
    return firstUse(record.id);
}

// The game's window depth Z (GS units) through a camera's window transform
// (Z = v22 z/w + v32) as the renderer's depth d = (1 - z/w) / 2: reversed,
// 1 at the near plane. 0.5 without a camera.
float standardDepth(float z, float v22, float v32)
{
    return v22 != 0.0f ? float(0.5 * (1.0 - (double(z) - v32) / v22)) : 0.5f;
}

// RG_NATIVE_DECAL_UNITS=N: an overlay laid on a surface drawn before it
// (tests >=, writes no depth or blends: a candle's light on a wall, the
// field's path and grass edges over the ground) is pulled toward the camera
// by N of the GS's Z units (4 unless set; 0 off). The GS gives both the same
// integer Z and the test passes; here each mesh's depth comes from its own
// matrices, and the overlay lost whole triangles to the surface. One Z unit
// is 1 / (2 |v22|) of the renderer's depth.
float decalBias(const native::Material &m, float v22)
{
    static const float units = [] {
        const char *v = std::getenv("RG_NATIVE_DECAL_UNITS");
        const float n = v && *v ? std::strtof(v, nullptr) : 4.0f;
        std::fprintf(stderr, "[native-producer] overlays pulled forward by %g Z units (RG_NATIVE_DECAL_UNITS)\n", n);
        return n;
    }();
    if (units == 0.0f || v22 == 0.0f || !(!m.depthWrite || m.blending) || m.depthTest != native::Compare::GreaterEqual)
        return 0.0f;
    return units / (2.0f * std::fabs(v22));
}

// The game's camera as the renderer's (native_records.h): `projection` (its
// clip space, VU quads 8-11) and `window` (clip to the target's pixels and
// GS Z, made from quads 4-7) folded into one standard projection over the
// target's `width` by `height`, its z the reversed depth (w - z) / 2 of the
// game's clip space, made in double so a wall and a light laid on it get
// the same depth.
void standardProjection(const float projection[16], const float window[16], float width, float height,
                        float out[16])
{
    double pw[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += double(projection[i * 4 + k]) * double(window[k * 4 + j]);
            pw[i * 4 + j] = sum;
        }
    for (int i = 0; i < 4; ++i)
    {
        const double *r = pw + i * 4;
        out[i * 4 + 0] = float(r[0] * 2.0 / width - r[3]);  // x' = 2x / W - w
        out[i * 4 + 1] = float(r[3] - r[1] * 2.0 / height); // y' = w - 2y / H (y up)
        out[i * 4 + 2] = float(0.5 * (double(projection[i * 4 + 3]) - double(projection[i * 4 + 2])));
        out[i * 4 + 3] = projection[i * 4 + 3];
    }
}

// Locked. The game's size of the target `id`, as declared.
bool declaredSize(const State &s, native::Id id, float &width, float &height)
{
    for (const auto &[base, t] : s.targets)
        if (t.id == id)
        {
            width = t.width;
            height = t.height;
            return true;
        }
    for (const auto &[key, t] : s.views)
        if (t.id == id)
        {
            width = t.width;
            height = t.height;
            return true;
        }
    return false;
}

// Locked. The camera's id, a record sent when it is new: `projection` is
// standard (standardProjection), `viewport` its rectangle of the target.
native::Id cameraOf(State &s, const float view[16], const float projection[16], const float viewport[4])
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const float *m) {
        for (int i = 0; i < 16; ++i)
        {
            uint32_t bits;
            std::memcpy(&bits, &m[i], 4u);
            hash = (hash ^ bits) * 1099511628211ull;
        }
    };
    mix(view);
    mix(projection);
    for (int i = 0; i < 4; ++i)
    {
        uint32_t bits;
        std::memcpy(&bits, &viewport[i], 4u);
        hash = (hash ^ bits) * 1099511628211ull;
    }
    const auto at = s.cameraIds.find(hash);
    if (at != s.cameraIds.end())
        return at->second;
    if (s.cameraIds.size() >= native::kCameraWindow) // the renderer keeps no more (native_records.h)
        s.cameraIds.clear();
    native::Camera record;
    record.id = s.nextCamera++;
    std::memcpy(record.view, view, sizeof(record.view));
    std::memcpy(record.projection, projection, sizeof(record.projection));
    std::memcpy(record.viewport, viewport, sizeof(record.viewport));
    s.cameraIds.emplace(hash, record.id);
    ++s.counts.camerasNew;
    native::push(native::Camera(record));
    return record.id;
}
// RG_NATIVE_HIDE=hex,hex (debug): draws (2D pieces and effect ops) into or
// reading from a buffer at one of these bases are dropped, to bisect which
// pass a visual fault belongs to. A `z` suffix (2a00z) drops only the
// depth-tested draws into that base (e.g. light volumes), nothing else; a
// `t` suffix (2a40t) only the draws texturing from it, 3D meshes included.
struct HiddenBase
{
    uint32_t base;
    bool depthTestedOnly;
    bool readsOnly;
};
const std::vector<HiddenBase> &hiddenBases()
{
    static const std::vector<HiddenBase> hidden = [] {
        std::vector<HiddenBase> out;
        const char *value = std::getenv("RG_NATIVE_HIDE");
        for (const char *cursor = value ? value : ""; *cursor;)
        {
            char *end = nullptr;
            const unsigned long b = std::strtoul(cursor, &end, 16);
            if (end == cursor)
                break;
            const bool z = *end == 'z' || *end == 'Z';
            const bool t = *end == 't' || *end == 'T';
            if (z || t)
                ++end;
            out.push_back({static_cast<uint32_t>(b & 0x3FFFu), z, t});
            cursor = *end == ',' ? end + 1 : end;
        }
        for (const HiddenBase &h : out)
            std::fprintf(stderr, "[native] RG_NATIVE_HIDE: %04x%s\n", h.base, h.depthTestedOnly ? " (depth-tested draws into it)" : h.readsOnly ? " (draws texturing from it)" : "");
        return out;
    }();
    return hidden;
}
// A draw into `target` (depth tested or not), reading `source` (or none).
bool hiddenDraw(uint32_t target, bool depthTested, bool reads, uint32_t source)
{
    for (const HiddenBase &h : hiddenBases())
    {
        if (!h.readsOnly && (target & 0x3FFFu) == h.base && (depthTested || !h.depthTestedOnly))
            return true;
        if (reads && !h.depthTestedOnly && (source & 0x3FFFu) == h.base)
            return true;
    }
    return false;
}

} // namespace


void nameSource(uint32_t source, const char *name)
{
    native::nameSource(source, name);
}

bool mesh(const rg::NativeMesh &mesh, const rg::DrawState *drawState, const uint8_t *vu1Data, uint32_t source,
          uint32_t retained, const ModelDraw *modelChunk)
{
    const ModelDraw *const chunkModel = modelChunk;
    // Every VU1 mesh is the model vertex stage's (M5): its row, and its
    // inputs or its retained id.
    if (!vu1Data || !chunkModel || !chunkModel->row)
        return false;
    // A model chunk's inputs, once, whatever becomes of this draw (M5).
    if (chunkModel && retained != 0u && chunkModel->define && chunkModel->inputs && !chunkModel->inputs->empty())
    {
        native::RetainedMesh kept;
        kept.id = retained;
        kept.list = chunkModel->list;
        kept.model = *chunkModel->inputs;
        native::push(std::move(kept));
    }

    // A chunk the program clips (h.clips: its slot E clips at z = -w and
    // z = w) keeps the triangles crossing the guard band: the GS backend
    // draws them clipped by the planes, and here the GPU clips them in clip
    // space. Skipping them left holes in floors near the camera.
    // RG_NATIVE_GUARD_CLIPPED=1 skips them as before (for comparing).
    static const bool guardClipped = [] {
        const char *v = std::getenv("RG_NATIVE_GUARD_CLIPPED");
        return v && std::strcmp(v, "0") != 0;
    }();
    if (drawState && !hiddenBases().empty())
    {
        const rg::DrawContext &hc = drawState->context;
        if (hiddenDraw(hc.frame.fbp * 32u, ((hc.test >> 16) & 1u) != 0u && ((hc.test >> 17) & 3u) >= 2u,
                       drawState->prim.tme != 0u, hc.tex0.tbp0))
            return true; // dropped
    }
    rg::NativeMesh::Header h = mesh.header;
    if (h.clips && !guardClipped)
        h.guardBand = 0u;

    native::MeshDraw draw;
    native::reuseMeshBuffers(draw);
    draw.source = source;

    std::memcpy(draw.modelColorMax, h.colorMax, sizeof(draw.modelColorMax));
    draw.modelColorScale = kUnit;
    {
        // The renderer draws it from its own buffer or the inputs it
        // brings (a chunk drawn this once, with no id) and judges the
        // program's cull on the GPU from these.
        draw.retained = retained;
        if (retained == 0u && chunkModel->inputs)
        {
            draw.modelInputs = *chunkModel->inputs;
            draw.modelList = chunkModel->list;
            draw.modelEdge = chunkModel->edge;
            draw.modelVolume = chunkModel->volume;
        }
        std::memcpy(draw.cullMatrix, h.matrix, sizeof(draw.cullMatrix));
        std::memcpy(draw.bandScale, h.bandScale, sizeof(draw.bandScale));
        std::memcpy(draw.bandOffset, h.bandOffset, sizeof(draw.bandOffset));
        draw.winding = h.triangleList && h.backface ? -h.winding : h.winding;
        draw.guardBand = h.guardBand != 0u;
        draw.backface = h.backface != 0u;
        draw.clips = h.clips != 0u;
        draw.hasModelRow = true;
        draw.modelRow = *chunkModel->row;
        if (chunkModel->palette)
            draw.palette = *chunkModel->palette;
    }
    for (int i = 0; i < 4; ++i)
        draw.fog[i] = h.fog[i] * kFogUnit;

    // The camera and the model, as the game keeps them in VU1 data memory.
    float model[16], view[16], projection[16], viewport[16], whole[16];
    quadsMatrix(vu1Data, 60u, model);
    quadsMatrix(vu1Data, 24u, view);
    quadsMatrix(vu1Data, 8u, projection);
    quadsMatrix(vu1Data, 4u, viewport);
    // Quads 4-7 are the projection all the way to the window (8-11 times
    // the screen's scale and offset), not a viewport after 8-11: the
    // program's matrix is model x view x (4-7). The camera keeps 8-11 as
    // its clip space and the rest, inverse(8-11) x (4-7), as its viewport.
    float toWindow[16], inverse[16];
    std::memcpy(toWindow, viewport, sizeof(toWindow));
    multiply(model, view, whole);
    multiply(whole, toWindow, whole);
    // The camera is the frame's, the same for mesh after mesh: its
    // projection's inverse times the window matrix is kept from the last
    // mesh that had these two (the inverse was a tenth of this function's
    // caller's thread, once a mesh).
    struct CameraSplit
    {
        float projection[16], toWindow[16], viewport[16];
        bool set = false, invertible = false;
    };
    thread_local CameraSplit last;
    if (!last.set || std::memcmp(last.projection, projection, sizeof(projection)) != 0 ||
        std::memcmp(last.toWindow, toWindow, sizeof(toWindow)) != 0)
    {
        std::memcpy(last.projection, projection, sizeof(projection));
        std::memcpy(last.toWindow, toWindow, sizeof(toWindow));
        last.invertible = invert(projection, inverse);
        if (last.invertible)
            multiply(inverse, toWindow, last.viewport);
        last.set = true;
    }
    const bool split = sameMatrix(whole, h.matrix) && last.invertible;
    if (split)
        std::memcpy(viewport, last.viewport, sizeof(viewport));

    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.meshes;
    if (!drawState)
    {
        ++s.counts.meshesNoState;
        return false;
    }
    const rg::DrawContext &c = drawState->context;
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    if (split)
    {
        // The camera is made below, once the target's size is known.
        foldOffset(viewport, ofx, ofy);
        draw.hasModel = true;
        std::memcpy(draw.model, model, sizeof(draw.model));
        ++s.counts.meshesSplit;
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                draw.model[i * 4 + j] = h.matrix[i][j];
        foldOffset(draw.model, ofx, ofy);
        draw.hasModel = true;
        ++s.counts.meshesWhole;
    }

    bool known = materialOf(s, *drawState, draw.material);
    // RG_NATIVE_OVERLAY_NO_DEPTH=1 (debug): blended or depth-unwritten
    // meshes (lights on walls, ground layers) skip the depth test, to tell
    // a lost depth test from a dropped triangle. Overlays will show through
    // whatever stands in front of them.
    static const bool overlayNoDepth = [] {
        const char *v = std::getenv("RG_NATIVE_OVERLAY_NO_DEPTH");
        const bool on = v && *v && *v != '0';
        if (on)
            std::fprintf(stderr, "[native] RG_NATIVE_OVERLAY_NO_DEPTH: overlays skip the depth test\n");
        return on;
    }();
    if (overlayNoDepth && (draw.material.blending || !draw.material.depthWrite))
        draw.material.depthTest = native::Compare::Always;
    const uint32_t frameBase = c.frame.fbp * 32u;
    uint32_t seqKey = frameBase; // the buffer's, or its narrow view's
    if (narrow(s, frameBase, c.frame.fbw))
    {
        seqKey = viewKey(frameBase, c.frame.fbw);
        draw.target = viewTarget(s, frameBase, c.frame.fbw, c.scissor.x1 + 1u, c.scissor.y1 + 1u);
        draw.material.depthTest = native::Compare::Always; // a view has no depth of its own
        draw.material.depthWrite = false;
        ++s.counts.viewWrites;
    }
    else
        draw.target = s.targetAt.count(frameBase)
                          ? targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm)
                          : 0u;
    if (draw.target == 0u)
    {
        // A buffer the game never named: the renderer has nowhere to draw
        // it, so the GS keeps it (review of #41).
        ++s.counts.targetMiss;
        return false;
    }
    if (draw.material.depthTest != native::Compare::Always || draw.material.depthWrite)
        draw.depthTarget = depthTarget(s, c.zbuf.zbp * 32u, frameBase, draw.target);
    if (draw.target == kSceneTarget && draw.depthTarget != 0u)
        s.sceneDepth = draw.depthTarget;
    if (split)
    {
        // The camera over the target, its depth standard (reversed). The
        // program scales its window depth and caps it (VU quad 87.z, quad
        // 1.y): decals at 1.01 and 1.02 sit in front of their surfaces; in
        // the renderer's depth that is a scale, a bias and a cap.
        const float v22 = viewport[10], v32 = viewport[14];
        float width = 640.0f, height = 448.0f;
        declaredSize(s, draw.target, width, height);
        float standard[16];
        standardProjection(projection, viewport, width, height, standard);
        const float rect[4] = {0.0f, 0.0f, width, height};
        draw.camera = cameraOf(s, view, standard, rect);
        s.lastCamera = draw.camera;
        s.lastDepthScale = v22;
        s.lastDepthOffset = v32;
        const float scaleBy = mesh.header.depthScale;
        if (scaleBy != 1.0f && v22 != 0.0f)
        {
            draw.depthScale = scaleBy;
            draw.depthBias = float((1.0 - double(scaleBy)) * (1.0 + double(v32) / v22) * 0.5);
        }
        if (v22 != 0.0f && mesh.header.depthMax < 3.0e38f)
            draw.depthCap = standardDepth(mesh.header.depthMax, v22, v32);
        draw.material.decalBias = decalBias(draw.material, v22);
    }
    draw.scissor = {c.scissor.x0, c.scissor.y0, c.scissor.x1 + 1, c.scissor.y1 + 1};
    draw.fogColor[0] = float(drawState->fogR) * kUnit;
    draw.fogColor[1] = float(drawState->fogG) * kUnit;
    draw.fogColor[2] = float(drawState->fogB) * kUnit;
    draw.material.known = known;
    if (known)
        ++s.counts.materialKnown;
    if (draw.target != s.layer || s.layer == 0u) // a routed draw leaves the buffer as it was
        s.drawnSeq[seqKey] = ++s.sequence;
    if (const uint64_t dumpFlip = triggeredDumpFlip(); dumpFlip != 0u && rg::graphics::guestFlips() == dumpFlip)
    {
        const rg::DrawContext &c = drawState->context;
        const native::Material &m = draw.material;
        uint32_t declaredW = 0u, declaredH = 0u;
        if (m.sourceTarget)
            for (const auto &[base, t] : s.targets)
                if (t.id == m.sourceTarget)
                {
                    declaredW = t.width;
                    declaredH = t.height;
                }
        std::fprintf(stderr,
                     "[mesh-dump] source declared %ux%u | fbp %04x -> target %u depth %u cam %u | tme %d tbp %05x tbw %u psm %02x %ux%u cbp "
                     "%05x csa %u tfx %u tcc %u -> %s %u palette %u | alpha %llx test %llx clamp %llx | %zu vertices%s\n",
                     declaredW, declaredH, c.frame.fbp * 32u, draw.target, draw.depthTarget, draw.camera,
                     int(drawState->prim.tme),
                     c.tex0.tbp0, c.tex0.tbw, c.tex0.psm & 0x3Fu, 1u << c.tex0.tw, 1u << c.tex0.th, c.tex0.cbp,
                     c.tex0.csa, c.tex0.tfx, c.tex0.tcc, m.sourceTarget ? "target" : "texture",
                     m.sourceTarget ? m.sourceTarget : m.texture, m.palette, (unsigned long long)c.alpha,
                     (unsigned long long)c.test, (unsigned long long)c.clamp, draw.vertices.size(),
                     m.known ? "" : " (unknown)");
    }
    native::push(native::MeshDraw(std::move(draw)));
    return true;
}

namespace
{
// Bits a host upload sends a pixel of, by DPSM; 0: not one kept.
uint32_t hostBits(uint32_t psm)
{
    switch (psm)
    {
    case kCT32: return 32u;
    case kCT16: case kCT16S: return 16u;
    case kT8: case 0x1Bu: return 8u;               // T8, T8H
    case kT4: case 0x24u: case 0x2Cu: return 4u;   // T4, T4HL, T4HH
    default: return 0u;
    }
}

// The bits of each 32-bit word a format in a 32-bit word holds; the others
// (CT16, T8, T4) lay memory out their own way: all of it.
uint32_t wordBits(uint32_t psm)
{
    switch (psm)
    {
    case kCT32: return 0xFFFFFFFFu;
    case 0x1Bu: return 0xFF000000u;
    case 0x24u: return 0x0F000000u;
    case 0x2Cu: return 0xF0000000u;
    default: return 0u;
    }
}

// The bytes of GS memory an image covers, roughly (rows of whole pages
// aside), from its base.
uint64_t hostSpan(uint32_t psm, uint32_t dbw, uint32_t height)
{
    const uint64_t texels = uint64_t(dbw) * 64u * height;
    if (wordBits(psm))
        return texels * 4u;
    return texels * hostBits(psm) / 8u;
}

// Locked. The host image a texturing read names: its format, at its base,
// at the buffer width it was written with (a palette's: any). Null when
// none.
State::HostImage *hostImageFor(State &s, uint32_t base, uint32_t psm, uint32_t bufferWidth, bool anyWidth = false)
{
    const auto at = s.hostImages.find({base, psm});
    if (at == s.hostImages.end())
        return nullptr;
    if (!anyWidth && at->second.dbw != std::max(bufferWidth, 1u))
    {
        ++s.counts.hostReadsRefused;
        return nullptr;
    }
    return &at->second;
}

// Locked. A record of a host image as a texture of the read's size (texels
// past what was written are 0): the one made before while unchanged.
native::Id hostTexture(State &s, const State::HostImage &image, const rg::Tex0Reg &t)
{
    const uint32_t psm = t.psm & 0x3Fu;
    const uint32_t width = 1u << std::min<uint32_t>(t.tw, 10u), height = 1u << std::min<uint32_t>(t.th, 10u);
    const uint64_t read = (uint64_t(t.tbp0) << 24) | (uint64_t(psm) << 8) | (uint64_t(t.tw & 15u) << 4) | (t.th & 15u);
    State::HostRecord &made = s.hostTextures[read];
    if (made.id != 0u && made.version == image.version && s.textureKeys.count(made.id))
        return made.id;
    const uint32_t stride = image.dbw * 64u;
    const uint32_t bytesPer = psm == kCT32 ? 4u : (psm == kCT16 || psm == kCT16S) ? 2u : 1u;
    const native::PixelFormat format = bytesPer == 4u   ? native::PixelFormat::Rgba8
                                       : bytesPer == 2u ? native::PixelFormat::Ct16
                                                        : native::PixelFormat::Index8;
    auto out = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * bytesPer, uint8_t(0));
    for (uint32_t y = 0; y < height && y < image.height; ++y)
    {
        const uint8_t *row = image.pixels.data() + size_t(y) * stride * bytesPer;
        uint8_t *dst = out->data() + size_t(y) * width * bytesPer;
        std::memcpy(dst, row, size_t(std::min(width, stride)) * bytesPer);
    }
    const uint64_t hash = imageHash(out->data(), out->size());
    s.counts.bytesHashed += out->size();
    made.version = image.version;
    const uint64_t key = keyOf(hash, psm | 0x200u, width, height);
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
    {
        ++s.counts.hostTexturesKnown;
        made.id = known->second;
        return made.id;
    }
    native::Texture record;
    record.format = format;
    record.scale = texelScale(format);
    record.width = static_cast<uint16_t>(width);
    record.height = static_cast<uint16_t>(height);
    record.id = s.nextTexture++;
    s.texels[record.id] = {out, record.width, record.height, record.format};
    record.texels = std::move(out);
    s.textureIds[key] = record.id;
    s.textureKeys[record.id] = key;
    made.id = record.id;
    ++s.counts.hostTexturesNew;
    native::push(std::move(record));
    return made.id;
}

// Locked. A host image read as a palette (CSM1, CSA 0): 16 entries from
// an 8x2 rectangle, 256 from 16x16 (entries 8-15 and 16-23 of each 32
// swapped). 0 when it holds less.
native::Id hostPalette(State &s, const State::HostImage &image, uint32_t cbp, uint32_t entries)
{
    const uint32_t columns = entries == 256u ? 16u : 8u, rows = entries == 256u ? 16u : 2u;
    const uint32_t stride = image.dbw * 64u;
    const bool is16 = image.psm != kCT32;
    if (image.height < rows || stride < columns)
        return 0u;
    const uint64_t read = (uint64_t(cbp) << 24) | (uint64_t(image.psm) << 12) | entries;
    State::HostRecord &made = s.hostPalettes[read];
    if (made.id != 0u && made.version == image.version)
        return made.id;
    const uint32_t entryBytes = is16 ? 2u : 4u;
    auto colors = std::make_shared<std::vector<uint8_t>>(entries * entryBytes);
    for (uint32_t i = 0; i < entries; ++i)
    {
        const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
        const size_t texel = size_t(at / columns) * stride + at % columns;
        std::copy_n(image.pixels.data() + texel * entryBytes, entryBytes, colors->data() + i * entryBytes);
    }
    const uint64_t hash = imageHash(colors->data(), colors->size());
    made.version = image.version;
    const uint64_t key = keyOf(hash, image.psm | 0x200u, entries, 0u);
    if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
    {
        made.id = known->second;
        return made.id;
    }
    native::Palette record;
    record.format = is16 ? native::PixelFormat::Ct16 : native::PixelFormat::Rgba8;
    record.scale = texelScale(record.format);
    record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(entries);
    s.paletteColors[record.id] = colors;
    record.colors = std::move(colors);
    s.paletteIds[key] = record.id;
    made.id = record.id;
    ++s.counts.hostPalettes;
    native::push(std::move(record));
    return made.id;
}

} // namespace

// Locked. The source a texturing draw state names: a target (whose base
// TBP0 is), a texture the VIF1 thread last sent there (and its palette), or
// what a host upload nobody took left there (step 6). False when none.
bool sourceOf(State &s, const rg::Tex0Reg &t, native::Material &m)
{
    const uint32_t psm = t.psm & 0x3Fu;
    // The game uploads textures into the same memory some of its buffers
    // use (02a00, 02a40): whichever wrote there last, in the stream's
    // order, is what the GS would read.
    const auto tex = s.residentTexture.find(t.tbp0);
    // A CT24 read of a CT32 texture is the same texels, its alpha TEXA's
    // (the caller's): an effect's 64x64 upload read so.
    bool texture = tex != s.residentTexture.end() &&
                   (tex->second.first == psm || (psm == 0x01u && tex->second.first == kCT32));
    State::HostImage *host = hostImageFor(s, t.tbp0, psm, t.tbw);
    if (host && texture)
    {
        const auto taken = s.takenSeq.find(t.tbp0);
        if (taken != s.takenSeq.end() && taken->second > host->seq)
            host = nullptr;
        else
            texture = false;
    }
    // A narrow read of a buffer with narrow views: the view of its width,
    // else whichever of the views and the buffer was drawn last.
    if (const auto widths = s.viewWidths.find(t.tbp0); widths != s.viewWidths.end() && narrow(s, t.tbp0, t.tbw))
    {
        const auto seqOf = [&](uint32_t key) {
            const auto q = s.drawnSeq.find(key);
            return q == s.drawnSeq.end() ? uint64_t(0) : q->second;
        };
        native::Id id = 0u;
        uint64_t seq = 0u;
        for (const uint32_t w : widths->second)
        {
            const uint32_t key = viewKey(t.tbp0, w);
            const uint64_t q = seqOf(key);
            const auto v = s.views.find(key);
            if (v == s.views.end())
                continue;
            if (w == std::max<uint32_t>(t.tbw, 1u))
            {
                id = v->second.id;
                seq = q;
                break;
            }
            if (q > seq)
            {
                id = v->second.id;
                seq = q;
            }
        }
        const auto uploaded = s.uploadedSeq.find(t.tbp0);
        const bool uploadedLast = uploaded != s.uploadedSeq.end() && uploaded->second > seq;
        const auto own = s.views.find(viewKey(t.tbp0, t.tbw));
        const bool ownWidth = own != s.views.end() && own->second.id == id;
        if (id != 0u && (seq >= seqOf(t.tbp0) || ownWidth) &&
            !((texture || host) && uploadedLast))
        {
            m.sourceTarget = id;
            ++s.counts.viewReads;
            return true;
        }
    }
    const auto at = s.targetAt.find(t.tbp0);
    if (at != s.targetAt.end())
    {
        const auto drawn = s.drawnSeq.find(t.tbp0), uploaded = s.uploadedSeq.find(t.tbp0);
        const bool uploadedLast = uploaded != s.uploadedSeq.end() &&
                                  (drawn == s.drawnSeq.end() || uploaded->second > drawn->second);
        if (!(texture || host) || !uploadedLast)
        {
            m.sourceTarget = at->second;
            return true;
        }
        ++s.counts.texturesOverTargets;
    }
    if (host)
    {
        if (psm == 0x01u) // CT24: its alpha is TEXA's, not kept here
            return false;
        m.texture = hostTexture(s, *host, t);
    }
    else if (texture)
        m.texture = tex->second.second;
    else
        return false;
    if (indexedPsm(psm))
    {
        // A CLUT offset (CSA) reads 16 entries of the CLUT buffer from
        // there: a T4 whose TEX0 loads its CLUT puts its CBP's first 16
        // there (the window is those); one that does not reads what an
        // earlier load left, taken as the CBP's 256-entry CLUT's entries
        // from 16 CSA on (sub-palettes of one CLUT, the common use). A T8
        // reads 256 entries the same way (from its CBP's first when its
        // TEX0 loads them, else from 16 CSA on).
        const bool t4 = !(psm == kT8 || psm == 0x1Bu);
        const uint32_t entries = (psm == kT8 || psm == 0x1Bu) ? 256u : 16u;
        const auto p = s.residentPalette.find(t.cbp);
        State::HostImage *hostClut = hostImageFor(s, t.cbp, t.cpsm & 0x3Fu, 0u, true);
        if (hostClut && p != s.residentPalette.end())
        {
            const auto taken = s.takenPaletteSeq.find(t.cbp);
            if (taken != s.takenPaletteSeq.end() && taken->second > hostClut->seq)
                hostClut = nullptr;
        }
        if (hostClut)
            m.palette = hostPalette(s, *hostClut, t.cbp, entries);
        else if (p != s.residentPalette.end())
            m.palette = p->second;
        if (m.palette == 0u)
            return false;
        if (t.csa != 0u && t.cld == 0u)
        {
            m.palette = paletteWindow(s, m.palette, 16u * t.csa, t4 ? 16u : 256u);
            if (m.palette == 0u)
                return false;
        }
    }
    return m.texture != 0u;
}

namespace
{
// RG_TEXTURE_DUMP_WHOLE=DIR: each new pair of an indexed texture and its
// palette drawn, whole and in colour, named as a PCSX2 pack names it
// (TEX0HASH-CLUTHASH-BITS.pam, the hashes in hex without leading zeros), so
// a pack can be made for the textures the game really uses. A PAM is RGBA
// with the game's alpha (0x80 opaque).
const char *wholeDumpDirectory()
{
    static const char *directory = [] {
        const char *value = std::getenv("RG_TEXTURE_DUMP_WHOLE");
        return value && *value ? value : nullptr;
    }();
    return directory;
}

void dumpWhole(const uint8_t *indices, const std::vector<uint8_t> &colors, uint32_t entries, uint32_t width,
               uint32_t height, uint32_t psm, uint64_t texels, uint64_t clut)
{
    char name[512];
    std::snprintf(name, sizeof name, "%s/%llx-%llx-%08x.pam", wholeDumpDirectory(),
                  static_cast<unsigned long long>(texels), static_cast<unsigned long long>(clut),
                  rg::pcsx2::nameBits(psm, width, height));
    std::FILE *file = std::fopen(name, "wb");
    if (!file)
        return;
    std::fprintf(file, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height);
    std::vector<uint8_t> rgba(size_t(width) * height * 4u);
    for (size_t i = 0; i < size_t(width) * height; ++i)
        std::copy_n(colors.data() + std::min<uint32_t>(indices[i], entries - 1u) * 4u, 4u, rgba.data() + i * 4u);
    std::fwrite(rgba.data(), 1u, rgba.size(), file);
    std::fclose(file);
}

native::PixelFormat blockFormat(int blocks)
{
    switch (blocks)
    {
    case rg::texturePack::kBc1: return native::PixelFormat::Bc1;
    case rg::texturePack::kBc2: return native::PixelFormat::Bc2;
    case rg::texturePack::kBc3: return native::PixelFormat::Bc3;
    case rg::texturePack::kBc7: return native::PixelFormat::Bc7;
    default: return native::PixelFormat::Rgba8;
    }
}
} // namespace

// Locked. Texture packs (texture_pack.h): an indexed texture with its
// palette, drawn from the pack's picture of it when it has one, as the GS
// backend's texture cache drew it until M1 of docs/NATIVE-ONLY.md: on the 3D
// alone (a mesh's material), not on the 2D pieces (sprites: the menus, the
// icons, the minimap), which stay as the game draws them. The
// picture is a texture record of its own (RGBA8, or the pack's BC blocks),
// larger than the game's, addressed at the game's size and filtered unless
// the pack says "nearest". Not for a tile of a sheet (region clamp or
// repeat), fetched by the game's texel.
void replaceWhole(State &s, const rg::Tex0Reg &t, native::Material &m)
{
    const bool dumping = wholeDumpDirectory() != nullptr;
    if ((!rg::texturePack::active() && !dumping) || m.texture == 0u || m.palette == 0u || m.sourceTarget != 0u)
        return;
    const uint32_t psm = t.psm & 0x3Fu;
    const auto tex = s.texels.find(m.texture);
    if (tex == s.texels.end() || !indexedFormat(tex->second.format) || !tex->second.data)
        return;
    const uint32_t width = tex->second.width, height = tex->second.height;
    if (!rg::pcsx2::hashable(psm, width, height) || width != (1u << std::min<uint32_t>(t.tw, 10u)) ||
        height != (1u << std::min<uint32_t>(t.th, 10u)))
        return;
    const uint32_t entries = psm == kT4 ? 16u : 256u;
    const auto colors = s.paletteColors.find(m.palette);
    if (colors == s.paletteColors.end() || !colors->second || colors->second->size() < entries * 2u)
        return;
    if (const uint64_t generation = rg::texturePack::generation(); generation != s.packGeneration)
    {
        s.wholes.clear();
        s.wholeOf.clear();
        s.packGeneration = generation;
    }
    const uint64_t key = (uint64_t(m.texture) << 32u) | m.palette;
    State::Whole &w = s.wholes[key];
    if (!w.hashed)
    {
        w.hashed = true;
        // PCSX2 hashes the CLUT expanded to RGBA8; a CT16 one is kept as
        // the game has it.
        std::vector<uint8_t> expanded;
        const std::vector<uint8_t> *rgba = colors->second.get();
        if (rgba->size() < entries * 4u)
        {
            expanded.resize(entries * 4u);
            for (uint32_t i = 0; i < entries; ++i)
                ct16(uint16_t((*rgba)[i * 2u] | ((*rgba)[i * 2u + 1u] << 8)), expanded.data() + i * 4u);
            rgba = &expanded;
        }
        std::vector<uint8_t> widened;
        const uint8_t *indices = tex->second.data->data();
        if (tex->second.format == native::PixelFormat::Index4)
        {
            widened.resize(size_t(width) * height);
            widenIndex4(indices, widened.size(), widened.data());
            indices = widened.data();
        }
        w.texels = rg::pcsx2::texelHash(indices, width, height, psm);
        const auto known = s.clutHashes.find(m.palette);
        w.clut = known != s.clutHashes.end() ? known->second
                                              : (s.clutHashes[m.palette] =
                                                     rg::pcsx2::clutHash(rgba->data(), entries));
        ++s.counts.wholesAsked;
        if (dumping && s.dumped.insert(w.texels * 1099511628211ull ^ w.clut).second)
            dumpWhole(indices, *rgba, entries, width, height, psm, w.texels, w.clut);
    }
    if (w.id == 0u && !w.none)
    {
        rg::texturePack::Picture picture;
        switch (rg::texturePack::whole(w.texels, w.clut, width, height, native::blockFormats(), picture))
        {
        case rg::texturePack::Lookup::Pending:
            return;
        case rg::texturePack::Lookup::None:
            w.none = true;
            ++s.counts.wholesNone;
            return;
        case rg::texturePack::Lookup::Ready:
            break;
        }
        // A whole multiple of the game's size, 1 to 16 times; a block
        // format's in whole blocks.
        const uint32_t factor = picture.width / width;
        if (factor < 1u || factor > 16u || picture.width != width * factor || picture.height != height * factor ||
            (picture.blocks != 0 && (picture.width % 4u || picture.height % 4u)) || picture.width > 16384u ||
            picture.height > 16384u)
        {
            std::fprintf(stderr, "[native-producer] texture pack: a %ux%u picture for a %ux%u texture, left out\n",
                         picture.width, picture.height, width, height);
            w.none = true;
            ++s.counts.wholesNone;
            return;
        }
        native::Texture record;
        record.scale = kTexelScale;
        record.id = s.nextTexture++;
        record.format = blockFormat(picture.blocks);
        record.width = static_cast<uint16_t>(picture.width);
        record.height = static_cast<uint16_t>(picture.height);
        record.logicalWidth = static_cast<uint16_t>(width);
        record.logicalHeight = static_cast<uint16_t>(height);
        record.texels = std::make_shared<const std::vector<uint8_t>>(std::move(picture.data));
        w.id = record.id;
        w.nearest = picture.nearest;
        s.wholeOf[record.id] = key;
        ++s.counts.wholesMade;
        native::push(std::move(record));
    }
    if (w.id == 0u)
        return;
    m.texture = w.id;
    m.palette = 0u;
    m.linear = !w.nearest;
    ++s.counts.wholesDrawn;
}

// Locked. A region pack's picture (M4 of docs/NATIVE-ONLY.md; the 2D art by
// region, TEXELS_CLUT_WxH, collected under the GS backend): a UV sprite's
// texel rectangle of an indexed texture, named as gs-path-final's
// GsTextureCache named it: the texels FNV-1a hashed after the texture's
// size (2^TW, 2^TH) and PSM, row by row (T8 a byte a texel; T4 two texels
// a byte, the low first), and the CLUT window after CPSM and CSM, each
// entry's raw value in index order. Not for what reads GS memory the
// producer does not hold (a CLUT offset by CSA or TEXCLUT, a T8's CT16 CLUT
// of 512 entries): counted.
void replaceRegion(State &s, const rg::DrawState &state, const rg::NativeMesh &mesh, native::Material &m)
{
    if (!rg::texturePack::active() || m.texture == 0u || m.palette == 0u || m.sourceTarget != 0u ||
        mesh.vertices.empty())
        return;
    const rg::Tex0Reg &t = state.context.tex0;
    const uint32_t psm = t.psm & 0x3Fu, cpsm = t.cpsm, csm = t.csm;
    if (psm != kT8 && psm != kT4)
        return;
    const auto tex = s.texels.find(m.texture);
    const auto colors = s.paletteColors.find(m.palette);
    if (tex == s.texels.end() || !tex->second.data || colors == s.paletteColors.end() || !colors->second)
        return;
    const bool sixteen = cpsm == kCT16 || cpsm == kCT16S;
    const uint32_t entries = psm == kT4 ? 16u : (sixteen ? 512u : 256u);
    // (A CSA window is m.palette already: materialOf's paletteWindow.)
    if ((t.csa != 0u && psm != kT4) || state.texclut.cou != 0u || state.texclut.cov != 0u || entries == 512u ||
        colors->second->size() < size_t(entries) * (sixteen ? 2u : 4u))
    {
        ++s.counts.regionsUnhashable;
        return;
    }
    // The UVs' rectangle (spriteTexelRect's), in texels.
    float u0 = mesh.vertices[0].u, u1 = u0, v0 = mesh.vertices[0].v, v1 = v0;
    for (const rg::NativeMesh::Vertex &v : mesh.vertices)
    {
        u0 = std::min(u0, v.u);
        u1 = std::max(u1, v.u);
        v0 = std::min(v0, v.v);
        v1 = std::max(v1, v.v);
    }
    const uint32_t texW = 1u << std::min<uint32_t>(t.tw, 10u), texH = 1u << std::min<uint32_t>(t.th, 10u);
    if (u0 < 0.0f || v0 < 0.0f)
        return;
    uint32_t x0 = uint32_t(u0), y0 = uint32_t(v0), x1 = uint32_t(std::ceil(u1)), y1 = uint32_t(std::ceil(v1));
    if (x1 == texW + 1u)
        x1 = texW;
    if (y1 == texH + 1u)
        y1 = texH;
    const State::Kept &k = tex->second;
    if (x1 <= x0 || y1 <= y0 || x1 > texW || y1 > texH || x1 > k.width || y1 > k.height)
        return;
    const uint32_t rw = x1 - x0, rh = y1 - y0;
    constexpr uint64_t kSeed = 1469598103934665603ull, kPrime = 1099511628211ull;
    const auto step = [&](uint64_t hash, uint64_t value) {
        for (int b = 0; b < 8; ++b)
            hash = (hash ^ ((value >> (b * 8)) & 0xFFull)) * kPrime;
        return hash;
    };
    const auto bytes = [&](uint64_t hash, const uint8_t *data, size_t size) {
        size_t i = 0;
        for (; i + 8u <= size; i += 8u)
        {
            uint64_t word;
            std::memcpy(&word, data + i, sizeof(word));
            hash = (hash ^ word) * kPrime;
        }
        for (; i < size; ++i)
            hash = (hash ^ data[i]) * kPrime;
        return hash;
    };
    uint64_t texels = step(step(step(kSeed, texW), texH), psm);
    thread_local std::vector<uint8_t> row;
    for (uint32_t y = y0; y < y1; ++y)
    {
        row.resize(rw);
        for (uint32_t x = 0; x < rw; ++x)
        {
            const size_t at = size_t(y) * k.width + (x0 + x);
            row[x] = k.format == native::PixelFormat::Index4 ? (((*k.data)[at / 2u] >> ((at & 1u) * 4u)) & 0x0Fu)
                                                             : (*k.data)[at];
        }
        if (psm == kT4)
        {
            const uint32_t pairs = (rw + 1u) / 2u;
            for (uint32_t p = 0; p < pairs; ++p)
                row[p] = uint8_t(row[p * 2u] | ((p * 2u + 1u < rw ? row[p * 2u + 1u] : 0u) << 4u));
            texels = bytes(texels, row.data(), pairs);
        }
        else
            texels = bytes(texels, row.data(), rw);
    }
    uint64_t clut = step(step(kSeed, cpsm), csm);
    const uint8_t *entry = colors->second->data();
    for (uint32_t i = 0; i < entries; ++i)
    {
        uint32_t value = 0u;
        if (sixteen)
            value = uint32_t(entry[i * 2u]) | (uint32_t(entry[i * 2u + 1u]) << 8);
        else
            std::memcpy(&value, entry + i * 4u, 4u);
        clut = step(clut, value);
    }
    const uint64_t key = texels ^ (clut * 0x9E3779B97F4A7C15ull) ^ (uint64_t(rw) << 48) ^ (uint64_t(rh) << 32);
    if (const uint64_t generation = rg::texturePack::generation(); generation != s.regionGeneration)
    {
        s.regions.clear();
        s.regionOf.clear();
        s.regionGeneration = generation;
    }
    State::Whole &w = s.regions[key];
    if (w.id == 0u && !w.none)
    {
        if (!w.hashed)
        {
            w.hashed = true;
            ++s.counts.regionsAsked;
        }
        rg::texturePack::Picture picture;
        switch (rg::texturePack::whole(texels, clut, rw, rh, native::blockFormats(), picture))
        {
        case rg::texturePack::Lookup::Pending:
            return;
        case rg::texturePack::Lookup::None:
            w.none = true;
            return;
        case rg::texturePack::Lookup::Ready:
            break;
        }
        const uint32_t factor = picture.width / rw;
        if (factor < 1u || factor > 16u || picture.width != rw * factor || picture.height != rh * factor ||
            (picture.blocks != 0 && (picture.width % 4u || picture.height % 4u)))
        {
            w.none = true;
            return;
        }
        native::Texture record;
        record.scale = kTexelScale;
        record.id = s.nextTexture++;
        record.format = blockFormat(picture.blocks);
        record.width = static_cast<uint16_t>(picture.width);
        record.height = static_cast<uint16_t>(picture.height);
        record.logicalWidth = static_cast<uint16_t>(rw);
        record.logicalHeight = static_cast<uint16_t>(rh);
        record.texels = std::make_shared<const std::vector<uint8_t>>(std::move(picture.data));
        w.id = record.id;
        w.nearest = picture.nearest;
        s.regionOf[record.id] = key;
        ++s.counts.regionsMade;
        native::push(std::move(record));
    }
    if (w.id == 0u)
        return;
    // The region's picture, addressed from the region's corner.
    m.texture = w.id;
    m.palette = 0u;
    m.texelOffset[0] += float(x0);
    m.texelOffset[1] += float(y0);
    m.clampU = m.clampV = true;
    m.linear = !w.nearest;
    ++s.counts.regionsDrawn;
}

// Locked. The GS's region clamp and region repeat (CLAMP's WMS and WMT 2
// and 3: a tile of a sheet, a texture the game addresses through a mask) as
// a texture record of its own: per axis, clamp keeps the texels from MIN to
// MAX, drawn clamped and offset by MIN; repeat keeps, at each texel u of the
// texture, its texel (u & MSK) | FIX, drawn repeated. The renderer then
// knows nothing of the GS's CLAMP. A texture whose texels the producer does
// not hold keeps its own (counted).
void cutRegion(State &s, uint64_t clamp, native::Material &m)
{
    const uint32_t wms = uint32_t(clamp & 3u), wmt = uint32_t((clamp >> 2) & 3u);
    m.clampU = wms == 1u || wms == 2u;
    m.clampV = wmt == 1u || wmt == 2u;
    const auto source = s.texels.find(m.texture);
    if (m.texture == 0u || source == s.texels.end() || !source->second.data)
    {
        ++s.counts.unknownClamp;
        return;
    }
    // Copied: the table may grow below. A packed T4 is cut from its
    // indices widened (the tile is Index8).
    State::Kept from = source->second;
    if (from.format == native::PixelFormat::Index4)
    {
        auto widened = std::make_shared<std::vector<uint8_t>>(size_t(from.width) * from.height);
        if (from.data->size() * 2u < widened->size())
        {
            ++s.counts.unknownClamp;
            return;
        }
        widenIndex4(from.data->data(), widened->size(), widened->data());
        from.data = std::move(widened);
        from.format = native::PixelFormat::Index8;
    }
    const uint32_t width = from.width, height = from.height;
    const size_t texel = texelBytes(from.format);
    if (width == 0u || height == 0u || from.data->size() < size_t(width) * height * texel)
    {
        ++s.counts.unknownClamp;
        return;
    }
    // The texels kept per axis, and the offset they are addressed at.
    const auto axis = [](uint32_t mode, uint32_t a, uint32_t b, uint32_t size, std::vector<uint32_t> &keep,
                         float &offset) {
        keep.clear();
        offset = 0.0f;
        if (mode == 2u)
        {
            const uint32_t lo = std::min(a, size - 1u), hi = std::clamp(b, lo, size - 1u);
            for (uint32_t u = lo; u <= hi; ++u)
                keep.push_back(u);
            offset = float(lo);
        }
        else if (mode == 3u)
            for (uint32_t u = 0; u < size; ++u)
                keep.push_back(((u & a) | b) % size);
        else
            for (uint32_t u = 0; u < size; ++u)
                keep.push_back(u);
    };
    const uint32_t minU = uint32_t((clamp >> 4) & 0x3FFu), maxU = uint32_t((clamp >> 14) & 0x3FFu);
    const uint32_t minV = uint32_t((clamp >> 24) & 0x3FFu), maxV = uint32_t((clamp >> 34) & 0x3FFu);
    thread_local std::vector<uint32_t> columns, rows;
    axis(wms, minU, maxU, width, columns, m.texelOffset[0]);
    axis(wmt, minV, maxV, height, rows, m.texelOffset[1]);
    const std::pair<native::Id, uint64_t> key{m.texture, clamp & 0xFFFFFFFFFFull};
    if (const auto cut = s.cuts.find(key); cut != s.cuts.end())
    {
        m.texture = cut->second;
        return;
    }
    auto out = std::make_shared<std::vector<uint8_t>>(columns.size() * rows.size() * texel);
    for (size_t y = 0; y < rows.size(); ++y)
        for (size_t x = 0; x < columns.size(); ++x)
            std::memcpy(out->data() + (y * columns.size() + x) * texel,
                        from.data->data() + (size_t(rows[y]) * width + columns[x]) * texel, texel);
    native::Texture record;
    record.format = from.format;
    record.scale = texelScale(record.format);
    record.id = s.nextTexture++;
    record.width = static_cast<uint16_t>(columns.size());
    record.height = static_cast<uint16_t>(rows.size());
    record.palette = from.palette;
    s.texels[record.id] = {out, record.width, record.height, from.format, from.palette};
    record.texels = std::move(out);
    s.cuts[key] = record.id;
    s.cutOf[record.id] = key;
    ++s.counts.tilesCut;
    m.texture = record.id;
    native::push(std::move(record));
}

namespace
{
// Locked. What the producer holds at a GS address, for the logs.
std::string holdingAt(State &s, uint32_t base)
{
    std::string out;
    char buf[96];
    if (const auto t = s.residentTexture.find(base); t != s.residentTexture.end())
    {
        std::snprintf(buf, sizeof(buf), " texture psm %02x id %u (taken seq %llu)", t->second.first, t->second.second,
                      (unsigned long long)(s.takenSeq.count(base) ? s.takenSeq[base] : 0u));
        out += buf;
    }
    if (const auto t = s.targetAt.find(base); t != s.targetAt.end())
    {
        std::snprintf(buf, sizeof(buf), " target %u (drawn seq %llu)", t->second,
                      (unsigned long long)(s.drawnSeq.count(base) ? s.drawnSeq[base] : 0u));
        out += buf;
    }
    for (const auto &[key, image] : s.hostImages)
        if (key.first == base)
        {
            std::snprintf(buf, sizeof(buf), " host image psm %02x dbw %u h %u", key.second, image.dbw, image.height);
            out += buf;
        }
    if (const auto t = s.textureAt.find(base); t != s.textureAt.end())
    {
        std::snprintf(buf, sizeof(buf), " (game thread: texture psm %02x id %u)", t->second.first, t->second.second);
        out += buf;
    }
    return out.empty() ? " nothing" : out;
}

// Locked. Texels of GS memory at a base as one format, in buffer rows:
// the host image there or the resident texture, whichever was written
// last (as sourceOf). Bytes a texel as the record keeps them (RGBA8 for
// the 16-bit formats' textures, raw for their host images).
struct Plane
{
    std::vector<uint8_t> *pixels = nullptr; // host image: written in place
    const State::Kept *kept = nullptr;      // texture record: remade
    State::HostImage *host = nullptr;
    uint32_t stride = 0u, height = 0u, bytesPer = 0u;
};
Plane planeAt(State &s, uint32_t base, uint32_t psm, uint32_t bufferWidth)
{
    Plane p;
    State::HostImage *host = nullptr;
    if (const auto at = s.hostImages.find({base, psm});
        at != s.hostImages.end() && at->second.dbw == std::max(bufferWidth, 1u))
        host = &at->second;
    const auto resident = s.residentTexture.find(base);
    const State::Kept *kept = nullptr;
    if (resident != s.residentTexture.end() && resident->second.first == psm)
        if (const auto k = s.texels.find(resident->second.second); k != s.texels.end() && k->second.data)
            kept = &k->second;
    if (host && kept)
    {
        const auto taken = s.takenSeq.find(base);
        if (taken != s.takenSeq.end() && taken->second > host->seq)
            host = nullptr;
    }
    if (host)
    {
        p.host = host;
        p.pixels = &host->pixels;
        p.stride = host->dbw * 64u;
        p.height = host->height;
        p.bytesPer = psm == kCT32 ? 4u : (psm == kCT16 || psm == kCT16S) ? 2u : 1u;
    }
    else if (kept)
    {
        p.kept = kept;
        p.stride = kept->width;
        p.height = kept->height;
        p.bytesPer = texelBytes(kept->format);
    }
    return p;
}

// Locked. A local copy (TRXDIR 2) done on what the producer holds: between
// the same format, from a host image or texture into one, the
// destination's texture made again with the rectangle written (clipped to
// both). Copies between formats reinterpret GS memory's layout and are
// counted, not done (docs/NATIVE-PIPELINE.md step 6).
void mirrorCopy(State &s, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp,
                uint32_t dbw, uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height)
{
    spsm &= 0x3Fu;
    dpsm &= 0x3Fu;
    if (spsm != dpsm)
    {
        ++s.counts.copiesOtherFormat;
        return;
    }
    const Plane from = planeAt(s, sbp, spsm, sbw);
    if (!from.pixels && !from.kept)
    {
        ++s.counts.copiesNoSource;
        return;
    }
    if (from.bytesPer == 0u) // a packed T4's half bytes: not mirrored
    {
        ++s.counts.copiesOtherFormat;
        return;
    }
    Plane to = planeAt(s, dbp, dpsm, dbw);
    if ((!to.pixels && !to.kept) || to.bytesPer != from.bytesPer)
    {
        ++s.counts.copiesNoDestination;
        return;
    }
    const uint8_t *src = from.pixels ? from.pixels->data() : from.kept->data->data();
    const size_t srcSize = from.pixels ? from.pixels->size() : from.kept->data->size();
    // A copy of the source's rows first: source and destination may be one.
    const uint32_t w = std::min({width, sx < from.stride ? from.stride - sx : 0u, dx < to.stride ? to.stride - dx : 0u});
    const uint32_t h = std::min({height, sy < from.height ? from.height - sy : 0u,
                                 dy < (to.pixels ? 2048u : to.height) ? (to.pixels ? 2048u : to.height) - dy : 0u});
    if (w == 0u || h == 0u)
    {
        ++s.counts.copiesNoSource;
        return;
    }
    const uint32_t bpp = from.bytesPer;
    std::vector<uint8_t> rect(size_t(w) * h * bpp);
    for (uint32_t y = 0; y < h; ++y)
    {
        const size_t at = (size_t(sy + y) * from.stride + sx) * bpp;
        if (at + size_t(w) * bpp <= srcSize)
            std::memcpy(rect.data() + size_t(y) * w * bpp, src + at, size_t(w) * bpp);
    }
    if (to.pixels)
    {
        State::HostImage &image = *to.host;
        if (dy + h > image.height)
        {
            image.height = dy + h;
            image.pixels.resize(size_t(image.height) * to.stride * bpp, uint8_t(0));
        }
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(image.pixels.data() + (size_t(dy + y) * to.stride + dx) * bpp,
                        rect.data() + size_t(y) * w * bpp, size_t(w) * bpp);
        ++image.version;
        image.seq = s.uploadedSeq[dbp] = ++s.sequence;
        ++s.counts.copiesMirrored;
        return;
    }
    auto out = std::make_shared<std::vector<uint8_t>>(*to.kept->data);
    for (uint32_t y = 0; y < h; ++y)
    {
        const size_t at = (size_t(dy + y) * to.stride + dx) * bpp;
        if (at + size_t(w) * bpp <= out->size())
            std::memcpy(out->data() + at, rect.data() + size_t(y) * w * bpp, size_t(w) * bpp);
    }
    const uint32_t texW = to.kept->width, texH = to.kept->height;
    const native::PixelFormat format = to.kept->format;
    auto resident = s.residentTexture.find(dbp);
    const uint64_t hash = imageHash(out->data(), out->size());
    const uint64_t key = keyOf(hash, dpsm | 0x400u, texW, texH);
    native::Id id = 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
        id = known->second;
    else
    {
        native::Texture record;
        record.format = format;
        record.scale = texelScale(format);
        record.width = static_cast<uint16_t>(texW);
        record.height = static_cast<uint16_t>(texH);
        record.id = id = s.nextTexture++;
        s.texels[record.id] = {out, record.width, record.height, format};
        record.texels = std::move(out);
        s.textureIds[key] = record.id;
        s.textureKeys[record.id] = key;
        native::push(std::move(record));
    }
    resident->second.second = id;
    s.uploadedSeq[dbp] = s.takenSeq[dbp] = ++s.sequence;
    ++s.counts.copiesMirrored;
}

} // namespace


void noteLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp, uint32_t dbw,
                   uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    static uint32_t told = 0u;
    if (told < 8u)
    {
        ++told;
        std::fprintf(stderr,
                 "[native-copy] SBP %04x SBW %u SPSM %02x (%u,%u) -> DBP %04x DBW %u DPSM %02x (%u,%u) %ux%u, seq "
                 "%llu\n  source:%s\n  destination:%s\n",
                 sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, width, height, (unsigned long long)s.sequence,
                 holdingAt(s, sbp).c_str(), holdingAt(s, dbp).c_str());
    }
    s.copiedTo.insert(dbp);
    mirrorCopy(s, sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, width, height);
}

void hostUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const uint8_t *data, uint32_t bytes)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const uint32_t psm = dpsm & 0x3Fu, bits = hostBits(psm);
    dbw = std::max(dbw, 1u);
    const uint32_t stride = dbw * 64u;
    const uint64_t need = (uint64_t(width) * height * bits + 7u) / 8u;
    ++s.counts.hostUploads;
    if (bits == 0u || width == 0u || height == 0u || x + width > stride || y + height > 2048u || !data ||
        bytes < need)
    {
        ++s.counts.hostRefused;
        return;
    }
    s.counts.hostBytes += need;
    // What it overwrites: an image of another format whose memory and bits
    // it shares is gone (T4HL and T4HH share a word, not its bits).
    const uint64_t from = uint64_t(dbp) * 256u, to = from + hostSpan(psm, dbw, y + height);
    for (auto it = s.hostImages.begin(); it != s.hostImages.end();)
    {
        const State::HostImage &other = it->second;
        const uint64_t otherFrom = uint64_t(it->first.first) * 256u;
        const uint64_t otherTo = otherFrom + hostSpan(other.psm, other.dbw, other.height);
        const uint32_t a = wordBits(psm), b = wordBits(other.psm);
        const bool shares = otherFrom < to && from < otherTo && (a == 0u || b == 0u || (a & b) != 0u);
        if (shares && !(it->first.first == dbp && other.psm == psm))
            it = s.hostImages.erase(it);
        else
            ++it;
    }
    State::HostImage &image = s.hostImages[{dbp, psm}];
    if (image.dbw != dbw || image.psm != psm)
    {
        image = {};
        image.dbw = dbw;
        image.psm = psm;
    }
    const uint32_t bytesPer = bits >= 16u ? bits / 8u : 1u;
    if (y + height > image.height)
    {
        image.height = y + height;
        image.pixels.resize(size_t(stride) * image.height * bytesPer, 0u);
    }
    for (uint32_t row = 0; row < height; ++row)
    {
        uint8_t *dst = image.pixels.data() + (size_t(y + row) * stride + x) * bytesPer;
        if (bits >= 8u)
            std::memcpy(dst, data + size_t(row) * width * bytesPer, size_t(width) * bytesPer);
        else
            for (uint32_t col = 0; col < width; ++col)
            {
                const size_t i = size_t(row) * width + col; // low nibble first
                dst[col] = (data[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
            }
    }
    ++image.version;
    image.seq = s.uploadedSeq[dbp] = ++s.sequence;
}

bool piece(const rg::NativeMesh &mesh, const rg::DrawState *drawState, bool texels, uint8_t layer,
           const std::vector<uint32_t> &indices)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.pieces;
    if (!drawState)
    {
        ++s.counts.piecesNoState;
        return false;
    }
    const rg::DrawContext &c = drawState->context;
    if (!hiddenBases().empty() &&
        hiddenDraw(c.frame.fbp * 32u, ((c.test >> 16) & 1u) != 0u && ((c.test >> 17) & 3u) >= 2u,
                   drawState->prim.tme != 0u, c.tex0.tbp0))
        return true; // dropped
    const rg::NativeMesh::Header &h = mesh.header;
    native::Draw2D draw;
    native::reuseMeshBuffers(draw);
    draw.source = kSource2d;
    draw.layer = static_cast<native::Layer>(std::min<uint8_t>(layer, uint8_t(native::Layer::Overlay)));
    const uint32_t frameBase = c.frame.fbp * 32u;
    uint32_t seqKey = frameBase; // the buffer's, or its narrow view's
    const bool view = narrow(s, frameBase, c.frame.fbw);
    if (view)
    {
        seqKey = viewKey(frameBase, c.frame.fbw);
        draw.target = viewTarget(s, frameBase, c.frame.fbw, c.scissor.x1 + 1u, c.scissor.y1 + 1u);
        ++s.counts.viewWrites;
    }
    else
        draw.target = s.targetAt.count(frameBase)
                          ? targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm)
                          : targetFor(s, frameBase, std::max(c.frame.fbw, 1u) * 64u, c.scissor.y1 + 1u, c.frame.psm);
    if (draw.target == 0u)
    {
        ++s.counts.piecesNoTarget;
        return false;
    }
    // A CT16 copy of a 32-bit buffer onto itself (shifted 8 pixels: on the
    // GS's memory a pixel's RG half lands on its BA half): green into alpha,
    // as the colour filter's kGreenToAlpha op. The shadow map's blur ends so
    // (2a00 at FBW 1): the decal that projects it reads that alpha.
    if (drawState->prim.tme && (c.tex0.psm & 0x3Fu) == kCT16 && (c.frame.psm & 0x3Fu) == kCT16 &&
        c.tex0.tbp0 == frameBase && c.tex0.tbw == c.frame.fbw)
    {
        native::Material &g = draw.material;
        g.colorClamp = kColorClamp;
        g.sourceTarget = draw.target;
        g.textureWidth = g.textureHeight = 1u;
        g.function = native::TextureFunction::Decal;
        g.textureAlpha = true;
        g.shuffle = native::ChannelShuffle::GreenToAlpha;
        g.shuffleScale = 128.0f; // a byte a unit over 0x80
        g.colorWrite = 0x8u;
        g.known = true;
        const float x1 = float(std::min<uint32_t>(std::max(c.frame.fbw, 1u) * 64u, c.scissor.x1 + 1u));
        const float y1 = float((c.scissor.y1 + 1u) / 2u); // the CT16 view is twice the 32-bit rows
        const float corners[4][2] = {{0.0f, 0.0f}, {x1, 0.0f}, {0.0f, y1}, {x1, y1}};
        draw.vertices.resize(4);
        for (int v = 0; v < 4; ++v)
        {
            native::Vertex &out = draw.vertices[v];
            out.position[0] = out.uv[0] = corners[v][0];
            out.position[1] = out.uv[1] = corners[v][1];
            for (int ch = 0; ch < 4; ++ch)
                out.color[ch] = 1.0f;
        }
        draw.indices = {0u, 1u, 2u, 1u, 3u, 2u};
        draw.scissor = {0, 0, int32_t(x1), int32_t(y1)};
        ++s.counts.piecesMade;
        ++s.counts.opsGreenToAlpha;
        s.drawnSeq[seqKey] = ++s.sequence;
        native::push(std::move(draw));
        return true;
    }
    native::Material &m = draw.material;
    m.colorClamp = kColorClamp;
    bool known = true;
    uint32_t why = 0u; // RG_NATIVE_UNKNOWN_LOG: 1 source psm, 2 clamp, 4 blend, 8 FBA, 16 write mask, 32 fog
    if (drawState->prim.tme)
    {
        if (!sourceOf(s, c.tex0, m))
        {
            ++s.counts.piecesNoSource;
            if (s.targetAt.count(c.tex0.tbp0 & ~31u))
                ++s.counts.piecesSubTarget;
            // The first few, by what they read (docs/NATIVE-PIPELINE.md step 6).
            static std::vector<uint64_t> told;
            const uint64_t what = (uint64_t(c.tex0.tbp0) << 32) | (uint64_t(c.tex0.psm & 0x3Fu) << 24) | c.tex0.cbp;
            if (told.size() < 12u && std::find(told.begin(), told.end(), what) == told.end())
            {
                told.push_back(what);
                const bool texture = s.residentTexture.count(c.tex0.tbp0) != 0u;
                std::fprintf(stderr,
                             "[native-producer] a 2D piece's source is unknown: TBP0 %05x PSM %02x CBP %05x CSA %u "
                             "%ux%u, %s, %zu vertices\n",
                             c.tex0.tbp0, c.tex0.psm & 0x3Fu, c.tex0.cbp, c.tex0.csa, 1u << c.tex0.tw,
                             1u << c.tex0.th,
                             texture ? "a texture of another format or no palette there" : "nothing uploaded there",
                             mesh.vertices.size());
            }
            return false;
        }
        if (m.sourceTarget)
            ++s.counts.piecesFromTarget;
        // A buffer read as CT24 (the character layers are, by the scene):
        // the GS takes its alpha from TEXA, not the buffer, 0 for black
        // texels with AEM. A 16-bit view of a 32-bit buffer has no meaning
        // here yet.
        if ((m.sourceTarget || m.texture) && (c.tex0.psm & 0x3Fu) == 0x01u)
        {
            m.sourceAlpha = float(drawState->texa.ta0) * kUnit;
            m.alphaZeroOnBlack = drawState->texa.aem;
            ++s.counts.piecesCt24;
        }
        else if (m.texture && ct16Read(c.tex0.psm, c.tex0.cpsm))
            texa16(m, drawState->texa);
        else if (m.sourceTarget && (c.tex0.psm & 0x3Fu) != 0x00u)
        {
            known = false;
            why |= 1u;
        }
        m.textureWidth = texels ? 1u : static_cast<uint16_t>(1u << std::min<uint32_t>(c.tex0.tw, 10u));
        m.textureHeight = texels ? 1u : static_cast<uint16_t>(1u << std::min<uint32_t>(c.tex0.th, 10u));
        m.linear = drawState->linearFilter;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        // Region clamp and repeat (a tile of a sheet): a texture of its own.
        const bool region = wms > 1u || wmt > 1u;
        if (region && !m.sourceTarget)
            cutRegion(s, c.clamp, m);
        else
        {
            m.clampU = wms != 0u;
            m.clampV = wmt != 0u;
        }
        if ((wms > 1u || wmt > 1u) && m.sourceTarget)
        {
            known = false; // a target's region: not fetched so
            why |= 2u;
        }
        // M4: a region pack's picture of the art this draw takes (FST UVs).
        if (texels && !region)
            replaceRegion(s, *drawState, mesh, m);
        static constexpr native::TextureFunction functions[4] = {
            native::TextureFunction::Modulate, native::TextureFunction::Decal, native::TextureFunction::Highlight,
            native::TextureFunction::Highlight2};
        m.function = functions[c.tex0.tfx & 3u];
        m.textureAlpha = c.tex0.tcc != 0u;
    }
    if (drawState->prim.abe)
        if (!blendOf(c.alpha, m))
        {
            known = false;
            why |= 4u;
        }
    testOf(c.test, c.zbuf.zmask, m);
    if (c.fba & 1u)
    {
        known = false;
        why |= 8u;
    }
    const uint32_t mask = c.frame.fbmsk;
    m.colorWrite = 0u;
    for (uint32_t ch = 0; ch < 4u; ++ch)
    {
        const uint32_t bits = (mask >> (ch * 8u)) & 0xFFu;
        if (bits == 0u)
            m.colorWrite |= static_cast<uint8_t>(1u << ch);
        else if (bits != 0xFFu)
        {
            known = false;
            why |= 16u;
        }
    }
    if (view)
    {
        m.depthTest = native::Compare::Always; // a view has no depth of its own
        m.depthWrite = false;
    }
    if (m.depthTest != native::Compare::Always || m.depthWrite)
    {
        draw.depthTarget = depthTarget(s, c.zbuf.zbp * 32u, frameBase, draw.target);
        draw.camera = s.lastCamera;
        m.decalBias = decalBias(m, s.lastDepthScale);
    }

    // build2D's encoding: a fogged piece has w = 1 + F / 255 with x, y (and
    // texel UVs) multiplied by it; positions keep XYOFFSET; the Z is `depth`.
    const bool fogged = h.matrix[2][3] == 1.0f && h.matrix[3][3] == 0.0f;
    if (fogged)
    {
        ++s.counts.piecesFogged; // its F per vertex (Vertex::fog)
    }
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    draw.vertices.resize(mesh.vertices.size());
    for (size_t k = 0; k < mesh.vertices.size(); ++k)
    {
        const rg::NativeMesh::Vertex &in = mesh.vertices[k];
        native::Vertex &out = draw.vertices[k];
        const float w = fogged && in.z != 0.0f ? in.z : 1.0f;
        out.position[0] = in.x / w - ofx;
        out.position[1] = in.y / w - ofy;
        out.position[2] = standardDepth(in.depth >= 0.0f ? in.depth : 0.0f, s.lastDepthScale, s.lastDepthOffset);
        out.uv[0] = fogged && texels ? in.u / w : in.u;
        out.uv[1] = fogged && texels ? in.v / w : in.v;
        out.color[0] = in.r * kUnit;
        out.color[1] = in.g * kUnit;
        out.color[2] = in.b * kUnit;
        out.color[3] = in.a * kUnit;
        // build2D's fogged form: w = 1 + (F + 0.25) / 255.
        if (fogged)
            out.fog = std::clamp((w - 1.0f) * 255.0f - 0.25f, 0.0f, 255.0f) * kFogUnit;
    }
    if (!indices.empty())
        draw.indices.assign(indices.begin(), indices.begin() + indices.size() / 3u * 3u);
    else
    {
        const uint32_t count = static_cast<uint32_t>(draw.vertices.size()) / 3u * 3u;
        draw.indices.resize(count);
        for (uint32_t k = 0; k < count; ++k)
            draw.indices[k] = k;
    }
    draw.scissor = {c.scissor.x0, c.scissor.y0, c.scissor.x1 + 1, c.scissor.y1 + 1};
    m.fog = fogged;
    draw.fogColor[0] = float(drawState->fogR) * kUnit;
    draw.fogColor[1] = float(drawState->fogG) * kUnit;
    draw.fogColor[2] = float(drawState->fogB) * kUnit;
    m.known = known;
    if (why && std::getenv("RG_NATIVE_UNKNOWN_LOG"))
    {
        static uint64_t seen = 0u;
        if (!(seen & (1ull << why)))
        {
            seen |= 1ull << why;
            std::fprintf(stderr,
                         "[unknown] why %x: frame fbp %x psm %x fbmsk %08x fba %llx tex0 tbp %x psm %x tfx %u tcc %u "
                         "clamp %llx alpha %llx test %llx texa %u/%d\n",
                         why, c.frame.fbp, c.frame.psm, c.frame.fbmsk, (unsigned long long)c.fba, c.tex0.tbp0,
                         c.tex0.psm, c.tex0.tfx, c.tex0.tcc, (unsigned long long)c.clamp,
                         (unsigned long long)c.alpha, (unsigned long long)c.test, drawState->texa.ta0,
                         int(drawState->texa.aem));
        }
    }
    ++s.counts.piecesMade;
    if (draw.target != s.layer || s.layer == 0u) // a routed draw leaves the buffer as it was
        s.drawnSeq[seqKey] = ++s.sequence;
    native::push(std::move(draw));
    return true;
}

namespace
{
// Locked. A window-space rectangle into `target` (x1, y1 exclusive): a clear
// to transparent black, or, with `source`, that target's same pixels copied.
native::Draw2D rectDraw(native::Id target, const int32_t box[4], native::Id source)
{
    native::Draw2D draw;
    native::reuseMeshBuffers(draw);
    draw.source = kSourceLayer;
    draw.target = target;
    native::Material &m = draw.material;
    m.colorClamp = kColorClamp;
    if (source != 0u)
    {
        m.sourceTarget = source;
        m.textureWidth = m.textureHeight = 1u; // texel UVs
        m.function = native::TextureFunction::Decal;
        m.textureAlpha = true;
    }
    m.colorWrite = 0xFu;
    m.known = true;
    const float x0 = float(box[0]), y0 = float(box[1]), x1 = float(box[2]), y1 = float(box[3]);
    const float corners[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    draw.vertices.resize(4);
    for (int v = 0; v < 4; ++v)
    {
        native::Vertex &out = draw.vertices[v];
        out.position[0] = out.uv[0] = corners[v][0];
        out.position[1] = out.uv[1] = corners[v][1];
        for (float &c : out.color)
            c = source != 0u ? 1.0f : 0.0f;
    }
    draw.indices = {0u, 1u, 2u, 1u, 3u, 2u};
    draw.scissor = {box[0], box[1], box[2], box[3]};
    return draw;
}

} // namespace

void routeLayer(uint32_t base, uint32_t fbw, const int32_t box[4])
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const uint32_t width = std::max(fbw, 1u) * 64u;
    const auto declared = s.targets.find(base);
    const uint32_t height = declared != s.targets.end() ? declared->second.height : 448u;
    s.routing = false; // the buffer itself, for its size
    targetForUnrouted(s, base, width, height, 0u);
    const native::Id layer = layerTarget(s, declared != s.targets.end() ? declared->second.width : width, height);
    s.layerBox[0] = std::max<int32_t>(box[0], 0);
    s.layerBox[1] = std::max<int32_t>(box[1], 0);
    s.layerBox[2] = std::min<int32_t>(box[2], s.layerWidth);
    s.layerBox[3] = std::min<int32_t>(box[3], s.layerHeight);
    if (s.layerBox[2] > s.layerBox[0] && s.layerBox[3] > s.layerBox[1])
        native::push((rectDraw(layer, s.layerBox, 0u)));
    s.routing = true;
    s.routeBase = base;
    ++s.counts.layersRouted;
}

void routeEnd()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    s.routing = false;
}

uint64_t uploadHash(const uint8_t *data, size_t size)
{
    return imageHash(data, size);
}

uint32_t paletteAt(uint32_t cbp)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.paletteAt.find(cbp);
    return it != s.paletteAt.end() ? it->second : 0u;
}

uint32_t captureTarget(uint32_t base)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto at = s.targetAt.find(base);
    const auto declared = s.targets.find(base);
    // The display buffer is the frame shown: natively, the scene as last drawn.
    if (at == s.targetAt.end() ||
        (declared != s.targets.end() && declared->second.role == native::TargetRole::Display))
        return kSceneTarget;
    return at->second;
}

bool readDepth(uint32_t base, uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               std::vector<uint8_t> &out)
{
    psm &= 0x3Fu;
    const uint32_t bytesPerPixel = psm == 0x30u ? 4u : psm == 0x31u ? 3u : (psm == 0x32u || psm == 0x3Au) ? 2u : 0u;
    if (bytesPerPixel == 0u || width == 0u || height == 0u || width > native::kMaxDepthSide ||
        height > native::kMaxDepthSide)
        return false;
    native::DepthBlock block;
    float scale = 0.0f, offset = 0.0f;
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ++s.depthReads;
        const auto at = s.depthAt.find(base);
        if (at != s.depthAt.end())
            block.target = at->second;
        else
            ++s.depthNoTarget;
        scale = s.lastDepthScale;
        offset = s.lastDepthOffset;
    }
    block.x = static_cast<int32_t>(x);
    block.y = static_cast<int32_t>(y);
    block.width = width;
    block.height = height;
    // Until there is an answer (or with no depth drawn there): the clear's
    // 0, as the GS's memory would hold.
    thread_local std::vector<float> depth;
    depth.assign(size_t(width) * height, 0.0f);
    if (block.target != 0u && native::readDepth(block, depth))
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ++s.depthAnswered;
    }
    const double maximum = bytesPerPixel == 4u ? 4294967295.0 : bytesPerPixel == 3u ? 16777215.0 : 65535.0;
    out.resize(size_t(width) * height * bytesPerPixel);
    for (size_t i = 0; i < depth.size(); ++i)
    {
        // Reversed depth d is (1 - z/w) / 2 of the projection's z; the GS's
        // Z is v22 z/w + v32 through the camera's viewport.
        const float d = depth[i];
        double z = d > 0.0f ? double(scale) * (1.0 - 2.0 * double(d)) + double(offset) : 0.0;
        z = std::clamp(z, 0.0, maximum);
        const uint32_t value = static_cast<uint32_t>(z);
        for (uint32_t b = 0; b < bytesPerPixel; ++b)
            out[i * bytesPerPixel + b] = static_cast<uint8_t>(value >> (8u * b));
    }
    return true;
}
bool pass(const PassRequest &request)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    native::Pass record;
    record.kind = static_cast<native::PassKind>(request.kind);
    record.frame = kSceneTarget;
    if (request.frameBase != 0u)
    {
        const auto at = s.targetAt.find(request.frameBase);
        record.frame = at != s.targetAt.end() ? at->second : 0u;
    }
    // An object layer's pass reads the object's layer, where its head's
    // draws went, in place of the buffer (step 5c).
    const bool fromLayer = request.kind == 13u && request.params[0] != 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (i == 0 && fromLayer)
        {
            record.work[0] = s.layer;
            if (record.work[0] == 0u)
                record.frame = 0u;
            continue;
        }
        if (request.workBases[i] == 0u)
            continue;
        auto at = s.targetAt.find(request.workBases[i]);
        if (at == s.targetAt.end() && request.workSizes[i][0] != 0u && request.workSizes[i][1] != 0u)
        {
            declare(s, request.workBases[i], request.workSizes[i][0], request.workSizes[i][1], kCT32, 0);
            at = s.targetAt.find(request.workBases[i]);
        }
        record.work[i] = at != s.targetAt.end() ? at->second : 0u;
        if (record.work[i] == 0u)
            record.frame = 0u;
    }
    if (record.frame == 0u)
    {
        ++s.counts.passesRefused;
        return false;
    }
    std::copy(std::begin(request.params), std::end(request.params), record.params);
    for (int i = 0; i < 32; ++i)
        if (request.depthParams & (1u << i))
            record.params[i] = standardDepth(record.params[i], s.lastDepthScale, s.lastDepthOffset);
    if (request.sceneDepth)
    {
        record.depth = s.sceneDepth;
        if (record.depth == 0u)
        {
            ++s.counts.passesRefused;
            return false;
        }
    }
    ++s.counts.passesMade;
    native::push(std::move(record));
    return true;
}

bool clear(const rg::DrawState &drawState, float x0, float y0, float x1, float y1, const uint8_t rgba[4], bool depth)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const rg::DrawContext &c = drawState.context;
    const uint32_t frameBase = c.frame.fbp * 32u;
    if (!s.targetAt.count(frameBase))
        return false;
    const native::Id target =
        targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm);
    if (target == 0u)
        return false;
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    native::Clear record;
    record.target = target;
    record.rect.x0 = static_cast<int32_t>(std::floor(std::min(x0, x1) - ofx));
    record.rect.y0 = static_cast<int32_t>(std::floor(std::min(y0, y1) - ofy));
    record.rect.x1 = static_cast<int32_t>(std::ceil(std::max(x0, x1) - ofx));
    record.rect.y1 = static_cast<int32_t>(std::ceil(std::max(y0, y1) - ofy));
    record.color = c.frame.fbmsk != 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i)
        record.rgba[i] = float(rgba[i]) * kUnit;
    native::push(native::Clear(record));
    if (depth && !c.zbuf.zmask)
    {
        native::Clear depthRecord;
        depthRecord.target = depthTarget(s, c.zbuf.zbp * 32u, frameBase, target);
        depthRecord.rect = record.rect;
        depthRecord.depth = true;
        depthRecord.depthValue = 0.0f;
        native::push(std::move(depthRecord));
    }
    return true;
}

void frameBoundary()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    s.depthsThisFrame.clear();
    // The scene starts each frame cleared to black, opaque.
    native::Clear clear;
    clear.target = kSceneTarget;
    clear.color = true;
    clear.rgba[3] = 1.0f;
    native::push(std::move(clear));
}

} // namespace rg::nativeproducer


