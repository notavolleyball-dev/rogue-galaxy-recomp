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
    uint64_t targetsNew = 0;
    uint64_t bytesHashed = 0;
    uint64_t withheldTextures = 0, withheldPalettes = 0, withheldBytes = 0;
    uint64_t passedNotProduced = 0, split = 0, continuedInPacket = 0;
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
    // Uploads made into records, in the game's order, for takeProduced.
    struct Produced
    {
        uint32_t dbp, dbw, dpsm, width, height;
        bool palette;
    };
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
                       " | targets new " + d(n.targetsNew, r.targetsNew) + " | hashed " +
                       std::to_string((n.bytesHashed - r.bytesHashed) >> 10) + " KiB" +
                       " | withheld from the GS: textures " + d(n.withheldTextures, r.withheldTextures) +
                       ", palettes " + d(n.withheldPalettes, r.withheldPalettes) + " (" +
                       std::to_string((n.withheldBytes - r.withheldBytes) >> 10) + " KiB); uploads sent on (no record) " +
                       d(n.passedNotProduced, r.passedNotProduced) + "; A+D blocks split (other registers kept) " +
                       d(n.split, r.split) + "; image continued in a packet " +
                       d(n.continuedInPacket, r.continuedInPacket);
    s.reported = s.counts;
    return line;
}

// Locked. An upload made into a record (or known as one), for takeProduced.
void noteProduced(State &s, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool palette)
{
    // Never sent ones age out; a bound in case the VIF1 thread takes none.
    if (s.produced.size() >= 8192u)
        s.produced.pop_front();
    s.produced.push_back({dbp & 0x3FFFu, std::max(dbw, 1u), dpsm & 0x3Fu, width, height, palette});
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
        noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false);
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
    noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false);
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
        noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true);
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
    noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true);
    ++s.counts.palettesNew;
    native::push(std::move(record));
}

void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    if (!native::enabled() || width == 0u || height == 0u)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    native::Target record;
    record.role = kind == 1 ? native::TargetRole::SceneColor
                  : kind == 2 ? native::TargetRole::Display
                              : native::TargetRole::Offscreen;
    record.width = static_cast<uint16_t>(std::min<uint32_t>(width, 4096u));
    record.height = static_cast<uint16_t>(std::min<uint32_t>(height, 4096u));
    record.format = (psm & 0x30u) == 0x30u ? native::PixelFormat::Depth32 : native::PixelFormat::Rgba8;
    (void)bufferWidth;
    auto at = s.targetAt.find(base);
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
        return;
    s.targets[base] = record;
    ++s.counts.targetsNew;
    native::push(native::Target(record));
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
uint32_t textureAt(uint32_t, uint32_t) { return 0u; }
uint32_t paletteAt(uint32_t) { return 0u; }
uint32_t targetAt(uint32_t) { return 0u; }
} // namespace rg::nativeproducer

#endif
