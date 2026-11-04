#include "native_producer.h"

#if defined(RG_HAS_SDLGPU)

#include <algorithm>
#include <array>
#include <deque>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gfx/gs/gs_named_textures.h"
#include "gfx/native/native_pipeline.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/gs_types.h"
#include "native_effects.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace rg::nativeproducer
{
namespace
{
namespace native = dq8::gfx::native;

constexpr uint32_t kCT32 = 0x00u, kCT16 = 0x02u, kCT16S = 0x0Au, kT8 = 0x13u, kT4 = 0x14u;

struct Counts
{
    uint64_t levels = 0, texturesNew = 0, texturesKnown = 0;
    uint64_t refusedFormat = 0, refusedBytes = 0, refusedLayout = 0;
    uint64_t palettesNew = 0, palettesKnown = 0, palettesRefused = 0;
    uint64_t targetsNew = 0, targetsAuto = 0, targetsGrown = 0, texturesOverTargets = 0;
    uint64_t bytesHashed = 0;
    uint64_t withheldTextures = 0, withheldPalettes = 0, withheldBytes = 0;
    uint64_t passedNotProduced = 0, split = 0, continuedInPacket = 0;
    // Step 4a: VU1 meshes made mesh draws.
    uint64_t meshes = 0, meshesNoState = 0, meshesSplit = 0, meshesWhole = 0, camerasNew = 0;
    uint64_t materialKnown = 0, textureMiss = 0, textureMissPsm = 0, textureMissTarget = 0, paletteMiss = 0, targetMiss = 0;
    uint64_t unknownBlend = 0, unknownClamp = 0, unknownWriteMask = 0, unknownOther = 0;
    uint64_t triangles = 0, trianglesSkipped = 0;
    // Step 5a's survey.
    uint64_t effects[8] = {}; // by command (kSprites ... kComposite, the rest last)
    uint64_t effectOps = 0, opsIntoScene = 0, opsIntoDeclared = 0, opsIntoUndeclared = 0;
    uint64_t opsFromNothing = 0, opsFromTarget = 0, opsFromTexture = 0, opsFromUnknown = 0, opsFromLayer = 0;
    // Step 5b: 2D pieces and effects made window-space mesh draws, and why not.
    uint64_t pieces = 0, piecesMade = 0, piecesNoState = 0, piecesNoTarget = 0, piecesNoSource = 0;
    uint64_t piecesFogged = 0, piecesFromTarget = 0, piecesSubTarget = 0, piecesCt24 = 0;
    uint64_t effectsMade = 0, effectsRefused = 0, opsMade = 0, opsNoTarget = 0, opsNoSource = 0, opsLayer = 0;
    uint64_t opsGreenToAlpha = 0, opsFeedback = 0, opsDepth = 0;
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
    Counts counts, reported;
    bool statsSet = false;
};
State &state()
{
    static State instance;
    return instance;
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
                       ", refused: format " + d(n.refusedFormat, r.refusedFormat) + ", bytes " +
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
                       ", other " + d(n.unknownOther, r.unknownOther) + "), triangles " +
                       d(n.triangles, r.triangles) + " (skipped " + d(n.trianglesSkipped, r.trianglesSkipped) + ")" +
                       " | to the GS: effects " + d(n.effects[0], r.effects[0]) + " sprites, " +
                       d(n.effects[1], r.effects[1]) + " route, " + d(n.effects[2], r.effects[2]) + " route end, " +
                       d(n.effects[3], r.effects[3]) + " object tail, " + d(n.effects[4], r.effects[4]) +
                       " composite, " + d(n.effects[7], r.effects[7]) + " other; ops " + d(n.effectOps, r.effectOps) +
                       " into scene " + d(n.opsIntoScene, r.opsIntoScene) + " / declared " +
                       d(n.opsIntoDeclared, r.opsIntoDeclared) + " / undeclared " +
                       d(n.opsIntoUndeclared, r.opsIntoUndeclared) + ", from nothing " +
                       d(n.opsFromNothing, r.opsFromNothing) + " / a target " + d(n.opsFromTarget, r.opsFromTarget) +
                       " / a texture " + d(n.opsFromTexture, r.opsFromTexture) + " / a layer " +
                       d(n.opsFromLayer, r.opsFromLayer) + " / unknown " + d(n.opsFromUnknown, r.opsFromUnknown) +
                       " | sampled a texture uploaded over a target " +
                       d(n.texturesOverTargets, r.texturesOverTargets) + " | 2D pieces " + d(n.pieces, r.pieces) + ": made " + d(n.piecesMade, r.piecesMade) +
                       " (from a target " + d(n.piecesFromTarget, r.piecesFromTarget) + ", as CT24 " +
                       d(n.piecesCt24, r.piecesCt24) + ", fogged " +
                       d(n.piecesFogged, r.piecesFogged) + "), to the GS: no state " +
                       d(n.piecesNoState, r.piecesNoState) + ", target not declared " +
                       d(n.piecesNoTarget, r.piecesNoTarget) + ", source unknown " +
                       d(n.piecesNoSource, r.piecesNoSource) + " (inside a target " +
                       d(n.piecesSubTarget, r.piecesSubTarget) + ") | effects made " +
                       d(n.effectsMade, r.effectsMade) + " (" + d(n.opsMade, r.opsMade) + " ops, " +
                       d(n.opsDepth, r.opsDepth) + " depth tested), to the GS " +
                       d(n.effectsRefused, r.effectsRefused) + ": an op into an undeclared buffer " +
                       d(n.opsNoTarget, r.opsNoTarget) + ", from an unknown one " + d(n.opsNoSource, r.opsNoSource) +
                       ", from an object layer " + d(n.opsLayer, r.opsLayer) + ", green to alpha " +
                       d(n.opsGreenToAlpha, r.opsGreenToAlpha) + "; made reading their own target " +
                       d(n.opsFeedback, r.opsFeedback) +
                       " | withheld from the GS: textures " + d(n.withheldTextures, r.withheldTextures) +
                       ", palettes " + d(n.withheldPalettes, r.withheldPalettes) + " (" +
                       std::to_string((n.withheldBytes - r.withheldBytes) >> 10) + " KiB); uploads sent on (no record) " +
                       d(n.passedNotProduced, r.passedNotProduced) + "; A+D blocks split (other registers kept) " +
                       d(n.split, r.split) + "; image continued in a packet " +
                       d(n.continuedInPacket, r.continuedInPacket) + " | depth reads (all so far) " +
                       std::to_string(s.depthReads) + ": answered " + std::to_string(s.depthAnswered) +
                       ", no depth target there " + std::to_string(s.depthNoTarget);
    s.reported = s.counts;
    return line;
}

// Locked. An upload made into a record (or known as one), for takeProduced.
void noteProduced(State &s, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool palette,
                  uint32_t key, uint32_t psm, native::Id id)
{
    // Never sent ones age out; a bound in case the VIF1 thread takes none.
    if (s.produced.size() >= 8192u)
        s.produced.pop_front();
    s.produced.push_back({dbp & 0x3FFFu, std::max(dbw, 1u), dpsm & 0x3Fu, width, height, palette, key, psm, id});
}

// Locked. The renderer's evictions, forgotten here so they are sent again.
void forgetEvicted(State &s)
{
    for (const native::Id id : native::takeEvicted())
    {
        const auto key = s.textureKeys.find(id);
        if (key == s.textureKeys.end())
            continue;
        s.textureIds.erase(key->second);
        s.textureKeys.erase(key);
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

// A CT16 texel as RGBA8 with the game's alpha: A set reads as 0x80. (The GS
// takes TEXA's TA1 and TA0 for set and clear; the game's are 0x80 and 0 where
// it uses CT16, not yet read from its TEXA.)
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
            maxWord = std::max(maxWord, GSPSMCT32::addrPSMCT32(0u, shape[2], x, y) >> 2u);
    std::vector<uint32_t> pixelOfWord(size_t(maxWord) + 1u, ~0u);
    for (uint32_t y = 0u; y < shape[1]; ++y)
        for (uint32_t x = 0u; x < shape[0]; ++x)
            pixelOfWord[GSPSMCT32::addrPSMCT32(0u, shape[2], x, y) >> 2u] = y * shape[0] + x;
    byteOf.resize(size_t(shape[3]) * shape[4]);
    for (uint32_t y = 0u; y < shape[4]; ++y)
        for (uint32_t x = 0u; x < shape[3]; ++x)
        {
            const uint32_t address = GSPSMT8::addrPSMT8(0u, shape[5], x, y);
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

void texture(const TextureLevel &l)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.levels;
    const uint32_t psm = l.psm & 0x3Fu;
    const size_t texels = size_t(l.width) * l.height;
    // What it occupies in EE memory, as sent.
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
        ++s.counts.refusedFormat;
        return;
    }
    if (texels == 0u || bytes == 0u || !l.ram || l.ramBytes < bytes || l.width > 4096u || l.height > 4096u)
    {
        ++s.counts.refusedBytes;
        return;
    }
    const uint64_t hash = dq8::gfx::namedtextures::imageHash(l.ram, bytes);
    s.counts.bytesHashed += bytes;
    const uint64_t key = keyOf(hash, psm | (l.preSwizzled ? 0x100u : 0u), l.width, l.height);
    const native::Id palette = (psm == kT8 || psm == kT4) ? (s.paletteAt.count(l.cbp) ? s.paletteAt[l.cbp] : 0u) : 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
    {
        ++s.counts.texturesKnown;
        s.textureAt[l.tbp] = {psm, known->second};
        noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false, l.tbp,
                     psm, known->second);
        return;
    }
    auto out = std::make_shared<std::vector<uint8_t>>();
    native::Texture record;
    record.width = static_cast<uint16_t>(l.width);
    record.height = static_cast<uint16_t>(l.height);
    record.palette = palette;
    if (psm == kCT32 && !l.preSwizzled)
    {
        record.format = native::PixelFormat::Rgba8;
        out->assign(l.ram, l.ram + bytes);
    }
    else if (psm == kCT16 || psm == kCT16S)
    {
        record.format = native::PixelFormat::Rgba8;
        out->resize(texels * 4u);
        for (size_t i = 0; i < texels; ++i)
            ct16(uint16_t(l.ram[i * 2u] | (l.ram[i * 2u + 1u] << 8)), out->data() + i * 4u);
    }
    else if (l.preSwizzled)
    {
        const std::vector<uint32_t> &byteOf = unswizzle(l);
        if (byteOf.empty())
        {
            ++s.counts.refusedLayout;
            return;
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
    else // T4: the low nibble first, widened to a byte each
    {
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        for (size_t i = 0; i < texels; ++i)
            (*out)[i] = (l.ram[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
    }
    record.id = s.nextTexture++;
    record.texels = std::move(out);
    s.textureIds[key] = record.id;
    s.textureKeys[record.id] = key;
    s.textureAt[l.tbp] = {psm, record.id};
    noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false, l.tbp, psm,
                 record.id);
    ++s.counts.texturesNew;
    native::push(std::move(record));
}

void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    cpsm &= 0x3Fu;
    const bool is16 = cpsm == kCT16 || cpsm == kCT16S;
    const uint32_t bytes = entries * (is16 ? 2u : 4u);
    if (!ram || (entries != 16u && entries != 256u) || (cpsm != kCT32 && !is16) || ramBytes < bytes)
    {
        ++s.counts.palettesRefused;
        return;
    }
    const uint64_t hash = dq8::gfx::namedtextures::imageHash(ram, bytes);
    s.counts.bytesHashed += bytes;
    const uint64_t key = keyOf(hash, cpsm, entries, 0u);
    if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
    {
        ++s.counts.palettesKnown;
        s.paletteAt[cbp] = known->second;
        noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                     known->second);
        return;
    }
    auto colors = std::make_shared<std::vector<uint8_t>>(entries * 4u);
    for (uint32_t i = 0; i < entries; ++i)
    {
        // CSM1: a 256-entry palette's entries 8-15 and 16-23 of each 32 are
        // stored swapped.
        const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
        if (is16)
            ct16(uint16_t(ram[at * 2u] | (ram[at * 2u + 1u] << 8)), colors->data() + i * 4u);
        else
            std::copy_n(ram + at * 4u, 4u, colors->data() + i * 4u);
    }
    native::Palette record;
    record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(entries);
    record.colors = std::move(colors);
    s.paletteIds[key] = record.id;
    s.paletteAt[cbp] = record.id;
    noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                 record.id);
    ++s.counts.palettesNew;
    native::push(std::move(record));
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
native::Id targetFor(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm)
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
} // namespace

void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    if (!native::enabled() || width == 0u || height == 0u)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    (void)bufferWidth;
    declare(s, base, width, height, psm, kind);
}

bool takeProduced(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool &palette)
{
    if (!native::enabled())
        return false;
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
            if (p.palette)
                s.residentPalette[p.key] = p.id;
            else
            {
                s.residentTexture[p.key] = {p.psm, p.id};
                s.uploadedSeq[p.key] = ++s.sequence;
            }
            s.produced.erase(s.produced.begin(), s.produced.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return true;
        }
    }
    return false;
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
    m.blendFactor = static_cast<uint8_t>((alpha >> 32) & 0xFFu);
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
        m.alphaRef = static_cast<uint8_t>((test >> 4) & 0xFFu);
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
bool materialOf(State &s, const GSDrawState &state, native::Material &m)
{
    const GSContext &c = state.context;
    bool known = true;
    if (state.prim.tme)
    {
        const GSTex0Reg &t = c.tex0;
        const auto at = s.residentTexture.find(t.tbp0);
        m.texture = at != s.residentTexture.end() && at->second.first == (t.psm & 0x3Fu) ? at->second.second : 0u;
        if (m.texture == 0u)
        {
            ++s.counts.textureMiss;
            if (at != s.residentTexture.end())
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
            else if (s.targetAt.count(t.tbp0 & ~31u))
                ++s.counts.textureMissTarget;
            known = false;
        }
        if (indexedPsm(t.psm))
        {
            const auto p = s.residentPalette.find(t.cbp);
            m.palette = p != s.residentPalette.end() && t.csa == 0u ? p->second : 0u;
            if (m.palette == 0u)
            {
                ++s.counts.paletteMiss;
                known = false;
            }
        }
        m.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(t.tw, 10u));
        m.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(t.th, 10u));
        m.linear = state.linearFilter;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        m.clampU = wms != 0u;
        m.clampV = wmt != 0u;
        if (wms > 1u || wmt > 1u)
        {
            ++s.counts.unknownClamp;
            known = false;
        }
        static constexpr native::TextureFunction functions[4] = {
            native::TextureFunction::Modulate, native::TextureFunction::Decal, native::TextureFunction::Highlight,
            native::TextureFunction::Highlight2};
        m.function = functions[t.tfx & 3u];
        m.textureAlpha = t.tcc != 0u;
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
    const auto at = s.depthAt.find(base);
    if (at != s.depthAt.end())
        return at->second;
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
    return record.id;
}

// Locked. The camera's id, a record sent when it is new.
native::Id cameraOf(State &s, const float view[16], const float projection[16], const float viewport[16])
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
    mix(viewport);
    const auto at = s.cameraIds.find(hash);
    if (at != s.cameraIds.end())
        return at->second;
    if (s.cameraIds.size() >= 4096u)
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
} // namespace

bool mesh(const GSNativeMesh &mesh, const GSDrawState *drawState, const uint8_t *vu1Data, uint32_t source)
{
    if (!native::enabled() || !vu1Data)
        return false;
    const GSNativeMesh::Header &h = mesh.header;
    const uint32_t count = static_cast<uint32_t>(mesh.vertices.size());

    native::MeshDraw draw;
    draw.source = source;

    // The triangles the program draws: a strip's every vertex from the
    // third on, a list's every third, less those the program culls (the
    // GS front end's rule for this mesh, gsNativeMeshSkips).
    const bool culls = h.guardBand || h.backface;
    thread_local std::vector<GSNativeMeshPoint> points;
    if (culls)
    {
        points.resize(count);
        for (uint32_t k = 0; k < count; ++k)
            points[k] = gsNativeMeshPoint(h, mesh.vertices[k]);
    }
    const bool lists = h.triangleList != 0u;
    const bool listCull = lists && h.backface;
    float winding = listCull ? -h.winding : h.winding;
    uint64_t skipped = 0u;
    draw.indices.reserve(count >= 2u ? (count - 2u) * 3u : 0u);
    for (uint32_t k = 1; k < count; ++k)
    {
        const bool judged = !listCull || k % 3u == 2u;
        const bool skip = culls && judged && gsNativeMeshSkips(h, points.data(), k, winding);
        if (culls && !listCull)
            winding = -winding;
        const bool drawn = k >= 2u && (lists ? k % 3u == 2u : true);
        if (!drawn)
            continue;
        if (skip)
        {
            ++skipped;
            continue;
        }
        draw.indices.push_back(k - 2u);
        draw.indices.push_back(k - 1u);
        draw.indices.push_back(k);
    }

    draw.vertices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        const GSNativeMesh::Vertex &in = mesh.vertices[k];
        native::Vertex &out = draw.vertices[k];
        out.position[0] = in.x;
        out.position[1] = in.y;
        out.position[2] = in.z;
        out.uv[0] = in.u;
        out.uv[1] = in.v;
        out.color[0] = std::fmin(in.r, h.colorMax[0]);
        out.color[1] = std::fmin(in.g, h.colorMax[1]);
        out.color[2] = std::fmin(in.b, h.colorMax[2]);
        out.color[3] = std::fmin(in.a, h.colorMax[3]);
    }
    std::memcpy(draw.fog, h.fog, sizeof(draw.fog));

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
    const bool split = sameMatrix(whole, h.matrix) && invert(projection, inverse);
    if (split)
        multiply(inverse, toWindow, viewport);

    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.meshes;
    s.counts.triangles += draw.indices.size() / 3u;
    s.counts.trianglesSkipped += skipped;
    if (!drawState)
    {
        ++s.counts.meshesNoState;
        return false;
    }
    const GSContext &c = drawState->context;
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    if (split)
    {
        foldOffset(viewport, ofx, ofy);
        draw.camera = cameraOf(s, view, projection, viewport);
        s.lastCamera = draw.camera;
        s.lastDepthScale = viewport[10];
        s.lastDepthOffset = viewport[14];
        draw.hasModel = true;
        std::memcpy(draw.model, model, sizeof(draw.model));
        draw.floatDepth = true;
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
    const uint32_t frameBase = c.frame.fbp * 32u;
    draw.target = s.targetAt.count(frameBase)
                      ? targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm)
                      : 0u;
    if (draw.target == 0u)
    {
        ++s.counts.targetMiss;
        known = false;
    }
    if (draw.material.depthTest != native::Compare::Always || draw.material.depthWrite)
        draw.depthTarget = depthTarget(s, c.zbuf.zbp * 32u, frameBase, draw.target);
    draw.scissor = {c.scissor.x0, c.scissor.y0, c.scissor.x1 + 1, c.scissor.y1 + 1};
    draw.fogColor[0] = drawState->fogR;
    draw.fogColor[1] = drawState->fogG;
    draw.fogColor[2] = drawState->fogB;
    draw.material.known = known;
    if (known)
        ++s.counts.materialKnown;
    s.drawnSeq[frameBase] = ++s.sequence;
    native::push(native::MeshDraw(std::move(draw)));
    return true;
}

void noteEffect(const GSNativeEffect &e)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++s.counts.effects[e.command < 5u ? e.command : 7u];
    if (e.command != GSNativeEffect::kSprites && e.command != GSNativeEffect::kObjectTail)
        return;
    const auto bufferIs = [&](uint32_t index, bool &scene, bool &declared) {
        scene = declared = false;
        if (index >= e.bufferCount)
            return;
        const auto at = s.targetAt.find(e.buffers[index].base);
        declared = at != s.targetAt.end();
        scene = declared && at->second == kSceneTarget;
    };
    for (uint32_t k = 0; k < e.opCount && k < GSNativeEffect::kMaxOps; ++k)
    {
        const GSNativeEffect::Op &op = e.ops[k];
        ++s.counts.effectOps;
        bool scene, declared;
        bufferIs(op.target, scene, declared);
        ++(scene ? s.counts.opsIntoScene : declared ? s.counts.opsIntoDeclared : s.counts.opsIntoUndeclared);
        if (op.source < 0)
        {
            ++s.counts.opsFromNothing;
            continue;
        }
        const uint32_t source = static_cast<uint32_t>(op.source);
        if (source < e.bufferCount && e.buffers[source].layer)
        {
            ++s.counts.opsFromLayer;
            continue;
        }
        bufferIs(source, scene, declared);
        if (declared)
            ++s.counts.opsFromTarget;
        else if (source < e.bufferCount && s.residentTexture.count(e.buffers[source].base))
            ++s.counts.opsFromTexture;
        else
            ++s.counts.opsFromUnknown;
    }
}

// Locked. The source a texturing draw state names: a target (whose base
// TBP0 is), or a texture the VIF1 thread last sent there (and its palette).
// False when neither.
bool sourceOf(State &s, const GSTex0Reg &t, native::Material &m)
{
    // The game uploads textures into the same memory some of its buffers
    // use (02a00, 02a40): whichever wrote there last, in the stream's
    // order, is what the GS would read.
    const auto tex = s.residentTexture.find(t.tbp0);
    const bool texture = tex != s.residentTexture.end() && tex->second.first == (t.psm & 0x3Fu);
    const auto at = s.targetAt.find(t.tbp0);
    if (at != s.targetAt.end())
    {
        const auto drawn = s.drawnSeq.find(t.tbp0), uploaded = s.uploadedSeq.find(t.tbp0);
        const bool uploadedLast = uploaded != s.uploadedSeq.end() &&
                                  (drawn == s.drawnSeq.end() || uploaded->second > drawn->second);
        if (!texture || !uploadedLast)
        {
            m.sourceTarget = at->second;
            return true;
        }
        ++s.counts.texturesOverTargets;
    }
    if (!texture)
        return false;
    m.texture = tex->second.second;
    if (indexedPsm(t.psm))
    {
        const auto p = s.residentPalette.find(t.cbp);
        if (p == s.residentPalette.end() || t.csa != 0u)
            return false;
        m.palette = p->second;
    }
    return true;
}

bool piece(const GSNativeMesh &mesh, const GSDrawState *drawState)
{
    if (!native::enabled())
        return false;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.pieces;
    if (!drawState)
    {
        ++s.counts.piecesNoState;
        return false;
    }
    const GSContext &c = drawState->context;
    const GSNativeMesh::Header &h = mesh.header;
    native::MeshDraw draw;
    const uint32_t frameBase = c.frame.fbp * 32u;
    draw.target = s.targetAt.count(frameBase)
                      ? targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm)
                      : targetFor(s, frameBase, std::max(c.frame.fbw, 1u) * 64u, c.scissor.y1 + 1u, c.frame.psm);
    if (draw.target == 0u)
    {
        ++s.counts.piecesNoTarget;
        return false;
    }
    native::Material &m = draw.material;
    bool known = true;
    uint32_t why = 0u; // RG_NATIVE_UNKNOWN_LOG: 1 source psm, 2 clamp, 4 blend, 8 FBA, 16 write mask, 32 fog
    if (drawState->prim.tme)
    {
        if (!sourceOf(s, c.tex0, m))
        {
            ++s.counts.piecesNoSource;
            if (s.targetAt.count(c.tex0.tbp0 & ~31u))
                ++s.counts.piecesSubTarget;
            return false;
        }
        if (m.sourceTarget)
            ++s.counts.piecesFromTarget;
        // A buffer read as CT24 (the character layers are, by the scene):
        // the GS takes its alpha from TEXA, not the buffer, 0 for black
        // texels with AEM. A 16-bit view of a 32-bit buffer has no meaning
        // here yet.
        if (m.sourceTarget && (c.tex0.psm & 0x3Fu) == 0x01u)
        {
            m.fixedTextureAlpha = drawState->texa.ta0;
            m.alphaZeroOnBlack = drawState->texa.aem;
            ++s.counts.piecesCt24;
        }
        else if (m.sourceTarget && (c.tex0.psm & 0x3Fu) != 0x00u)
        {
            known = false;
            why |= 1u;
        }
        // build2D's texel UVs (FST) carry the flag in the source's top bit.
        const bool texels = (h.source & 0x80000000u) != 0u;
        m.textureWidth = texels ? 1u : static_cast<uint16_t>(1u << std::min<uint32_t>(c.tex0.tw, 10u));
        m.textureHeight = texels ? 1u : static_cast<uint16_t>(1u << std::min<uint32_t>(c.tex0.th, 10u));
        m.linear = drawState->linearFilter;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        m.clampU = wms != 0u;
        m.clampV = wmt != 0u;
        if (wms > 1u || wmt > 1u)
        {
            known = false;
            why |= 2u;
        }
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
    if (m.depthTest != native::Compare::Always || m.depthWrite)
    {
        draw.depthTarget = depthTarget(s, c.zbuf.zbp * 32u, frameBase, draw.target);
        draw.camera = s.lastCamera;
    }

    // build2D's encoding: a fogged piece has w = 1 + F / 255 with x, y (and
    // texel UVs) multiplied by it; positions keep XYOFFSET; the Z is `depth`.
    const bool fogged = h.matrix[2][3] == 1.0f && h.matrix[3][3] == 0.0f;
    const bool texels = (h.source & 0x80000000u) != 0u;
    if (fogged)
    {
        ++s.counts.piecesFogged; // its F per vertex (Vertex::fog)
    }
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    draw.vertices.resize(mesh.vertices.size());
    for (size_t k = 0; k < mesh.vertices.size(); ++k)
    {
        const GSNativeMesh::Vertex &in = mesh.vertices[k];
        native::Vertex &out = draw.vertices[k];
        const float w = fogged && in.z != 0.0f ? in.z : 1.0f;
        out.position[0] = in.x / w - ofx;
        out.position[1] = in.y / w - ofy;
        out.position[2] = in.depth >= 0.0f ? in.depth : 0.0f;
        out.uv[0] = fogged && texels ? in.u / w : in.u;
        out.uv[1] = fogged && texels ? in.v / w : in.v;
        out.color[0] = in.r;
        out.color[1] = in.g;
        out.color[2] = in.b;
        out.color[3] = in.a;
        // build2D's fogged form: w = 1 + (F + 0.25) / 255.
        if (fogged)
            out.fog = std::clamp((w - 1.0f) * 255.0f - 0.25f, 0.0f, 255.0f);
    }
    const uint32_t count = static_cast<uint32_t>(draw.vertices.size()) / 3u * 3u;
    draw.indices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
        draw.indices[k] = k;
    draw.scissor = {c.scissor.x0, c.scissor.y0, c.scissor.x1 + 1, c.scissor.y1 + 1};
    draw.fog[0] = draw.fog[1] = 255.0f; // a model's F: none (the vertices carry theirs)
    m.fog = fogged;
    draw.fogColor[0] = drawState->fogR;
    draw.fogColor[1] = drawState->fogG;
    draw.fogColor[2] = drawState->fogB;
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
    s.drawnSeq[frameBase] = ++s.sequence;
    native::push(native::MeshDraw(std::move(draw)));
    return true;
}

bool effect(const GSNativeEffect &e)
{
    if (!native::enabled() || e.command != GSNativeEffect::kSprites)
        return false;
    // A CT24 read left for the GS to give TEXA (kTexaFromGs) takes the
    // converter's copy of it, the GS's at this point; read before this
    // file's lock, which the converter's own calls here take inside its.
    uint32_t gsTexa = 0u;
    for (uint32_t k = 0; k < std::min<uint32_t>(e.opCount, GSNativeEffect::kMaxOps); ++k)
        if (e.ops[k].flags & GSNativeEffect::kTexaFromGs)
        {
            const GSDrawState mirror = rg::nativefx::mirroredDrawState();
            gsTexa = mirror.texa.ta0 | (mirror.texa.aem ? 0x8000u : 0u);
            break;
        }
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    const auto refuse = [&](uint64_t &why) {
        ++why;
        ++s.counts.effectsRefused;
        return false;
    };
    // Everything is resolved before anything is sent: an effect is the
    // renderer's or the GS's, never split between them.
    const uint32_t ops = std::min<uint32_t>(e.opCount, GSNativeEffect::kMaxOps);
    thread_local std::vector<native::MeshDraw> made;
    thread_local std::vector<uint32_t> bases;
    made.clear();
    bases.clear();
    for (uint32_t k = 0; k < ops; ++k)
    {
        const GSNativeEffect::Op &op = e.ops[k];
        if (op.target >= e.bufferCount)
            return refuse(s.counts.opsNoTarget);
        const GSNativeEffect::Buffer &into = e.buffers[op.target];
        const native::Id target = targetFor(s, into.base, into.width ? into.width : std::max(into.fbw, 1u) * 64u,
                                            into.height, into.psm);
        if (target == 0u)
            return refuse(s.counts.opsNoTarget);
        if (op.flags & GSNativeEffect::kGreenToAlpha)
            return refuse(s.counts.opsGreenToAlpha);
        native::MeshDraw draw;
        draw.target = target;
        native::Material &m = draw.material;
        bool known = true;
        if (op.source >= 0)
        {
            const uint32_t source = static_cast<uint32_t>(op.source);
            if (source >= e.bufferCount)
                return refuse(s.counts.opsNoSource);
            if (e.buffers[source].layer)
                return refuse(s.counts.opsLayer);
            const auto from = s.targetAt.find(e.buffers[source].base);
            if (from == s.targetAt.end())
                return refuse(s.counts.opsNoSource);
            if (from->second == draw.target)
                ++s.counts.opsFeedback; // the renderer reads a copy
            m.sourceTarget = from->second;
            m.textureWidth = m.textureHeight = 1u; // the op's UVs are texels
            m.linear = (op.flags & GSNativeEffect::kLinear) != 0u;
            m.function = (op.flags & GSNativeEffect::kDecal)        ? native::TextureFunction::Decal
                         : (op.flags & GSNativeEffect::kHighlight)  ? native::TextureFunction::Highlight
                         : (op.flags & GSNativeEffect::kHighlight2) ? native::TextureFunction::Highlight2
                                                                    : native::TextureFunction::Modulate;
            m.textureAlpha = (op.flags & GSNativeEffect::kTextureAlpha) != 0u;
            if (op.flags & GSNativeEffect::kCt24)
            {
                const uint32_t texa = (op.flags & GSNativeEffect::kTexaFromGs) ? gsTexa : op.texa;
                m.fixedTextureAlpha = static_cast<int16_t>(texa & 0xFFu);
                m.alphaZeroOnBlack = (texa & 0x8000u) != 0u;
            }
        }
        if (op.flags & GSNativeEffect::kBlend)
            known = blendOf(op.alpha, m) && known;
        const bool zmask = true; // the op's depth buffer is never written
        known = testOf(op.test, zmask, m) && known;
        m.colorWrite = (op.flags & GSNativeEffect::kKeepAlpha)   ? 0x7u
                       : (op.flags & GSNativeEffect::kKeepColor) ? 0x8u
                                                                 : 0xFu;
        if (m.depthTest != native::Compare::Always)
        {
            ++s.counts.opsDepth;
            const uint32_t zbp = static_cast<uint32_t>(op.zbuf & 0x1FFu) * 32u;
            draw.depthTarget = depthTarget(s, zbp, e.buffers[op.target].base, draw.target);
            draw.camera = s.lastCamera;
        }
        const float x0 = op.rect[0], y0 = op.rect[1], x1 = op.rect[2], y1 = op.rect[3];
        const float z0 = static_cast<float>(op.z[0]), z1 = static_cast<float>(op.z[1]);
        const float corners[4][5] = {{x0, y0, z0, op.uv[0], op.uv[1]},
                                     {x1, y0, z1, op.uv[2], op.uv[1]},
                                     {x0, y1, z0, op.uv[0], op.uv[3]},
                                     {x1, y1, z1, op.uv[2], op.uv[3]}};
        draw.vertices.resize(4);
        for (int v = 0; v < 4; ++v)
        {
            native::Vertex &out = draw.vertices[v];
            out.position[0] = corners[v][0];
            out.position[1] = corners[v][1];
            out.position[2] = corners[v][2];
            out.uv[0] = corners[v][3];
            out.uv[1] = corners[v][4];
            for (int ch = 0; ch < 4; ++ch)
                out.color[ch] = op.rgba[ch];
        }
        draw.indices = {0u, 1u, 2u, 1u, 3u, 2u};
        draw.scissor = {static_cast<int32_t>(op.scissor[0]), static_cast<int32_t>(op.scissor[1]),
                        static_cast<int32_t>(op.scissor[2]) + 1, static_cast<int32_t>(op.scissor[3]) + 1};
        draw.fog[0] = draw.fog[1] = 255.0f;
        m.known = known;
        made.push_back(std::move(draw));
        bases.push_back(into.base);
    }
    for (size_t k = 0; k < made.size(); ++k)
    {
        s.drawnSeq[bases[k]] = ++s.sequence;
        native::push(native::MeshDraw(std::move(made[k])));
    }
    ++s.counts.effectsMade;
    s.counts.opsMade += made.size();
    return true;
}

bool pipelineOn()
{
    return native::enabled();
}

uint32_t textureAt(uint32_t tbp, uint32_t psm)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.textureAt.find(tbp);
    return it != s.textureAt.end() && it->second.first == (psm & 0x3Fu) ? it->second.second : 0u;
}

uint32_t paletteAt(uint32_t cbp)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.paletteAt.find(cbp);
    return it != s.paletteAt.end() ? it->second : 0u;
}

uint32_t targetAt(uint32_t base)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.targetAt.find(base);
    return it != s.targetAt.end() ? it->second : 0u;
}

bool readDepth(uint32_t base, uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               std::vector<uint8_t> &out)
{
    if (!native::enabled())
        return false;
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
} // namespace rg::nativeproducer

#else

namespace rg::nativeproducer
{
void texture(const TextureLevel &) {}
void palette(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t) {}
void target(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int) {}
void noteWithheld(bool, uint32_t) {}
void notePassed(Passed) {}
void noteSplit() {}
bool takeProduced(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool &) { return false; }
void noteContinuedInPacket() {}
bool mesh(const GSNativeMesh &, const GSDrawState *, const uint8_t *, uint32_t) { return false; }
bool pipelineOn() { return false; }
bool readDepth(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::vector<uint8_t> &) { return false; }
void noteEffect(const GSNativeEffect &) {}
bool piece(const GSNativeMesh &, const GSDrawState *) { return false; }
bool effect(const GSNativeEffect &) { return false; }
uint32_t textureAt(uint32_t, uint32_t) { return 0u; }
uint32_t paletteAt(uint32_t) { return 0u; }
uint32_t targetAt(uint32_t) { return 0u; }
} // namespace rg::nativeproducer

#endif
