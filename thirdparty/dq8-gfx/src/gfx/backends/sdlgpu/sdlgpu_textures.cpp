#include "gfx/backends/sdlgpu/sdlgpu_textures.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "gfx/gs/gs_memory_uses.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <map>
#include <algorithm>
#include <set>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {
namespace {
// Names are read once the host names anything; under the native bypass
// from the start, uploads before the first record naming themselves.
bool namedOn() {
    return namedtextures::active() || gsNativeBypass();
}
} // namespace


namespace {

// Beyond this the cache is cleared wholesale rather than evicting cleverly.
// A GS scene addresses far fewer distinct textures than this; the cap exists
// so a pathological stream cannot exhaust GPU memory.
constexpr size_t kMaxEntries = 2048u;

uint32_t rgba5551To8888(uint32_t value) {
    const uint32_t r = (value & 0x1fu) << 3u;
    const uint32_t g = ((value >> 5u) & 0x1fu) << 3u;
    const uint32_t b = ((value >> 10u) & 0x1fu) << 3u;
    const uint32_t a = ((value >> 15u) & 0x01u) << 7u;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

struct TexaState {
    uint8_t ta0 = 0u;
    bool aem = false;
    uint8_t ta1 = 0u;
};

TexaState unpackTexa(uint32_t packed) {
    return {static_cast<uint8_t>(packed & 0xffu),
            ((packed >> 8u) & 0x1u) != 0u,
            static_cast<uint8_t>((packed >> 16u) & 0xffu)};
}

// TEXA supplies the alpha a format has no room for: CT24 has none at all, and
// CT16's single bit selects between TA0 and TA1. AEM additionally forces alpha
// to zero when RGB is black, which is how a colour-keyed texture is done.
uint32_t applyTexa(const TexaState &texa, uint32_t psm, uint32_t texel) {
    if ((psm & 0x3fu) == GS_PSM_CT32)
        return texel;

    const bool rgbZero = (texel & 0x00ffffffu) == 0u;
    uint8_t alpha = static_cast<uint8_t>((texel >> 24u) & 0xffu);
    switch (psm & 0x3fu) {
    case GS_PSM_CT24:
        alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        if ((alpha & 0x80u) != 0u)
            alpha = texa.ta1;
        else
            alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    default:
        break;
    }
    return (texel & 0x00ffffffu) | (static_cast<uint32_t>(alpha) << 24u);
}

// CSM1 swaps address bits 3 and 4 of the palette index; a 16-bit CLUT exposes
// a ninth bit through CSA[4].
uint32_t swizzleClutIndexCsm1(uint32_t index) {
    return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
}

uint32_t resolveClutIndex(uint32_t index, uint32_t cpsm, uint32_t csm, uint32_t csa,
                          uint32_t sourcePsm) {
    const bool fourBitSource = (sourcePsm & 0x3fu) == GS_PSM_T4 ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HH ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HL;
    // CSM2 addresses the palette directly through TEXCLUT, and CSA must be
    // zero there, so it does not offset anything.
    if (csm != 0u)
        return fourBitSource ? (index & 0x0fu) : index;

    const bool sixteenBitClut = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBitClut ? 0x1fu : 0x0fu;
    const uint32_t indexMask = sixteenBitClut ? 0x1ffu : 0x0ffu;
    const uint32_t base = (csa & csaMask) << 4u;

    uint32_t resolved = index;
    switch (sourcePsm & 0x3fu) {
    case GS_PSM_T4:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        resolved = base + (index & 0x0fu);
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        resolved = base + index;
        break;
    default:
        return index;
    }
    return swizzleClutIndexCsm1(resolved & indexMask);
}

// Writes an expanded texture as a PPM next to a one-line description, when
// DQ8_GFX_DUMP_TEXTURES names a directory. Expansion is where PSM, CLUT and
// TEXA all meet, so "is the texture wrong or is the shader wrong" is the first
// question every texturing bug asks.
void dumpTexture(const GsTextureKey &key,
                 const GsTextureIdentity &identity,
                 const std::vector<uint8_t> &rgba,
                 uint64_t index) {
    const char *directory = std::getenv("DQ8_GFX_DUMP_TEXTURES");
    if (!directory || *directory == '\0')
        return;

    // Named by identity, not by address: that is the name a replacement pack
    // would use, so a dump doubles as the pack's index.
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%016llx_%016llx_%ux%u_psm%02x.ppm", directory,
                  static_cast<unsigned long long>(identity.texels),
                  static_cast<unsigned long long>(identity.clut), key.width, key.height,
                  key.psm);
    std::FILE *file = std::fopen(path, "wb");
    if (!file)
        return;
    std::fprintf(file, "P6\n%u %u\n255\n", key.width, key.height);
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u)
        std::fwrite(rgba.data() + i, 1u, 3u, file);
    std::fclose(file);

    uint64_t opaque = 0u;
    uint64_t nonBlack = 0u;
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u) {
        if (rgba[i] != 0u || rgba[i + 1u] != 0u || rgba[i + 2u] != 0u)
            ++nonBlack;
        if (rgba[i + 3u] != 0u)
            ++opaque;
    }
    std::fprintf(stderr,
                 "[texture %llu] id=%016llx/%016llx tbp0=%04x psm=%02x %ux%u cbp=%04x "
                 "cpsm=%02x csa=%u non-black=%llu alpha>0=%llu\n",
                 static_cast<unsigned long long>(index),
                 static_cast<unsigned long long>(identity.texels),
                 static_cast<unsigned long long>(identity.clut), key.tbp0, key.psm,
                 key.width, key.height, key.cbp, key.cpsm, key.csa,
                 static_cast<unsigned long long>(nonBlack),
                 static_cast<unsigned long long>(opaque));
}

// FNV-1a. Not cryptographic, and does not need to be: a texture pack holds
// thousands of entries, not billions.
constexpr uint64_t kHashSeed = 1469598103934665603ull;

uint64_t hashStep(uint64_t hash, uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
    return hash;
}

// The same FNV-1a over a byte string, one machine word at a time. The
// per-pixel hashStep it replaces cost eight multiply-xor rounds per texel,
// which at 512x448 was a third of what expansion cost.
uint64_t hashBytes(uint64_t hash, const uint8_t *data, size_t size) {
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

} // namespace

size_t GsTextureKeyHash::operator()(const GsTextureKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    };
    mix(key.tbp0);
    mix(static_cast<uint64_t>(key.tbw) | (static_cast<uint64_t>(key.psm) << 16u) |
        (static_cast<uint64_t>(key.width) << 24u) |
        (static_cast<uint64_t>(key.height) << 40u));
    mix(key.cbp);
    mix(static_cast<uint64_t>(key.cpsm) | (static_cast<uint64_t>(key.csm) << 8u) |
        (static_cast<uint64_t>(key.csa) << 16u));
    mix(key.texa);
    mix(key.texclut);
    return static_cast<size_t>(hash);
}

GsTextureCache::GsTextureCache(SdlGpuDevice &device, GsVram &vram, GsTargetCache &targets)
    : m_device(device), m_vram(vram), m_targets(targets) {
    m_src = &m_vram;
    const char *audit = std::getenv("DQ8_GFX_UPLOAD_AUDIT");
    m_auditOn = audit && *audit && *audit != '0';
    const char *store = std::getenv("DQ8_GFX_UPLOAD_STORE");
    m_storeOn = !store || (*store && *store != '0'); // on unless DQ8_GFX_UPLOAD_STORE=0
    m_tracking = m_auditOn || m_storeOn;
    if (m_tracking) {
        // The sparse store's stand-in: a local memory that only host writes
        // reach, which a texture or palette must read the same texels from.
        m_shadowStorage.assign(kGsVramBytes, 0u);
        m_shadow.attach(m_shadowStorage.data(), static_cast<uint32_t>(m_shadowStorage.size()));
        m_shadowTransfer = std::make_unique<GsTransferEngine>(m_shadow);
    }
}

GsTextureCache::~GsTextureCache() {
    reset();
    releaseLarger();
    if (m_device.valid())
        for (auto &[size, texture] : m_indexTextures) {
            (void)size;
            SDL_ReleaseGPUTexture(m_device.handle(), texture);
        }
    m_indexTextures.clear();
}

void GsTextureCache::releaseLarger() {
    if (m_device.valid())
        for (auto &[key, larger] : m_larger) {
            (void)key;
            SDL_ReleaseGPUTexture(m_device.handle(), larger.texture);
        }
    m_larger.clear();
    m_largerBytes = 0u;
}

void GsTextureCache::reset() {
    if (!m_entries.empty())
        beforeRewrite(nullptr);
    if (m_device.valid()) {
        for (auto &[key, entry] : m_entries) {
            (void)key;
            if (entry.texture)
                SDL_ReleaseGPUTexture(m_device.handle(), entry.texture);
        }
    }
    m_entries.clear();
    m_sourcePagesAll.reset();
    m_drawnBlocks.reset();
    m_drawMemo.clear();
    ++m_drawnGeneration;
    for (auto &entries : m_entriesByPage)
        entries.clear();
    releaseUploadPool();
}

void GsTextureCache::noteUpload(const GSTransferCommand &command) {
    beginNamedUpload(command);
    if (!m_tracking)
        return;
    UploadRecord record;
    record.dbp = command.bitbltbuf.dbp;
    record.dbw = std::max<uint32_t>(command.bitbltbuf.dbw, 1u);
    record.dpsm = command.bitbltbuf.dpsm;
    record.w = command.trxreg.rrw;
    record.h = command.trxreg.rrh;
    record.x = command.trxpos.dsax;
    record.y = command.trxpos.dsay;
    gsMarkPages(record.pages, record.dbp, record.dbw, record.dpsm, record.w, record.h,
                command.trxpos.dsax, command.trxpos.dsay);
    gsMarkCoveredPages(record.covered, record.dbp, record.dbw, record.dpsm, record.w, record.h,
                       command.trxpos.dsax, command.trxpos.dsay);
    // Only the upload audit reads back past records (auditPages); without
    // it the latest is all auditStamp needs, and keeping every one grew
    // the vector by several MB a second at the first boss (Rogue Galaxy:
    // fixed).
    if (!m_auditOn)
        m_uploads.clear();
    m_uploads.push_back(record);
    ++m_uploadSerial;
    const uint32_t psm = record.dpsm & 0x3fu;
    static const bool decodeStore = std::getenv("DQ8_GFX_DECODE_STORE") != nullptr;
    m_uploadCopied = !decodeStore && !gsNativeBypass() && m_vram.attached() &&
                     (psm == GS_PSM_CT32 || psm == GS_PSM_T8 || psm == GS_PSM_T4);
    m_shadowTransfer->begin(command);
    m_auditPending = static_cast<uint32_t>(m_uploads.size()) + 15u;
}

void GsTextureCache::noteUploadData(const uint8_t *data, uint32_t sizeBytes) {
    if (m_namedOpen && data && sizeBytes) {
        const size_t room = m_namedExpected - m_namedPending.bytes.size();
        m_namedPending.bytes.edit().insert(m_namedPending.bytes.edit().end(), data, data + std::min<size_t>(room, sizeBytes));
        if (m_namedPending.bytes.size() >= m_namedExpected)
            finishNamedUpload();
    }
    if (m_tracking && m_shadowTransfer && !m_uploadCopied && !m_namedSkipShadow)
        m_shadowTransfer->upload(data, sizeBytes);
}

void GsTextureCache::noteUploadLanded(const GSTransferCommand &command, uint32_t copiedBefore, uint32_t copiedAfter) {
    if (!m_tracking || !m_uploadCopied || copiedAfter <= copiedBefore)
        return;
    m_shadow.copyLanded(m_vram, command.bitbltbuf.dpsm, command.bitbltbuf.dbp,
                        std::max<uint32_t>(command.bitbltbuf.dbw, 1u), command.trxpos.dsax, command.trxpos.dsay,
                        command.trxreg.rrw, copiedBefore, copiedAfter);
}

void GsTextureCache::auditStamp(const GsPageSet &pages, InvalidationSource source) {
    if (m_writer.empty())
        m_writer.assign(kGsPageCount, 0u);
    const UploadRecord *record =
        source == InvalidationSource::HostWrite && m_auditPending >= 16u ? &m_uploads[m_auditPending - 16u] : nullptr;
    // Blocks a copy or an unrecorded write touched are not the host writes'
    // alone until an upload covers them whole; a draw's are marked as it
    // draws (noteDrawnBlocks), at its own extent.
    if (source != InvalidationSource::Draw) {
        if (!record) {
            gsBlocksOfPages(m_drawnBlocks, pages);
            ++m_drawnGeneration;
        } else {
            const GsPageSet outside = pages & ~record->pages;
            if (outside.any())
                gsBlocksOfPages(m_drawnBlocks, outside);
            // The record's whole blocks, once per upload rather than once
            // per payload (Rogue Galaxy: changed).
            if (m_coveredBlocksFor != m_uploadSerial) {
                m_coveredBlocks.reset();
                gsMarkCoveredBlocks(m_coveredBlocks, record->dbp, record->dbw, record->dpsm, record->w, record->h,
                                    record->x, record->y);
                m_coveredBlocksFor = m_uploadSerial;
            }
            // Once per upload: no draw lands between one transfer's payloads.
            if (m_coveredAppliedFor != m_uploadSerial) {
                m_coveredAppliedFor = m_uploadSerial;
                if ((m_drawnBlocks & m_coveredBlocks).any()) {
                    m_drawnBlocks &= ~m_coveredBlocks;
                    ++m_drawnGeneration;
                }
            }
        }
    }
    if (!m_auditOn)
        return; // the writers by page are the audit's report alone
    gsForEachPage(pages, [&](uint32_t page) {
        uint32_t &writer = m_writer[page];
        if (source == InvalidationSource::Draw)
            writer = 1u;
        else if (!record || !record->pages.test(page))
            writer = 3u;
        else if (record->covered.test(page) || writer == 0u || writer == m_auditPending)
            writer = m_auditPending;
        else
            writer = 2u;
    });
}

// One word for what wrote `pages` last, against what the texture wants.
std::string GsTextureCache::auditPages(const GsPageSet &pages, const GsTextureKey &key, bool clut) const {
    bool none = false, draw = false, mixed = false, unknown = false;
    uint32_t upload = 0u;
    bool several = false;
    gsForEachPage(pages, [&](uint32_t page) {
        const uint32_t writer = m_writer.empty() ? 0u : m_writer[page];
        if (writer == 0u) none = true;
        else if (writer == 1u) draw = true;
        else if (writer == 2u) mixed = true;
        else if (writer == 3u) unknown = true;
        else if (upload == 0u || upload == writer) upload = writer;
        else several = true;
    });
    if (draw) return "draw-or-copy";
    if (mixed) return "partial-page";
    if (unknown) return "unrecorded-write";
    if (several) return "several-uploads";
    if (none && upload == 0u) return "never-written";
    if (none) return "partly-never-written";
    const UploadRecord &record = m_uploads[upload - 16u];
    const uint32_t base = clut ? key.cbp : key.tbp0;
    const uint32_t width = clut ? std::max<uint32_t>(key.texclut & 0xffu, 1u) : key.tbw;
    const uint32_t psm = (clut ? key.cpsm : key.psm) & 0x3fu;
    char buffer[96];
    if ((record.dpsm & 0x3fu) != psm) {
        std::snprintf(buffer, sizeof buffer, "reinterpreted upload %02x as %02x", record.dpsm & 0x3fu, psm);
        return buffer;
    }
    if (record.dbp == base && record.dbw == width)
        return "exact";
    if (record.dbw == width)
        return "same-width-other-base";
    return "other-geometry";
}

void GsTextureCache::auditBuild(const GsTextureKey &key, const Entry &entry) {
    for (const bool clut : {false, true}) {
        const GsPageSet &pages = clut ? entry.clutPages : entry.texelPages;
        if (pages.none())
            continue;
        // The texels (their indices) and the palette window, from local
        // memory and from the host writes alone.
        const GsRegion whole{0u, 0u, key.width, key.height};
        bool same = true;
        GsVram *const saved = m_src;
        if (!clut) {
            m_src = &m_vram;
            const uint64_t real = expand(key, whole, true);
            m_src = &m_shadow;
            const uint64_t shadow = expand(key, whole, true);
            m_src = saved;
            same = real == shadow;
        } else {
            m_src = &m_vram;
            const uint64_t real = hashClutWindow(key);
            m_src = &m_shadow;
            const uint64_t shadow = hashClutWindow(key);
            m_src = saved;
            same = real == shadow;
        }
        // The rule step 2 would use: the host writes' memory serves a build
        // when no page of it was touched by anything else.
        const bool served = !blocksDrawn(key);
        const std::string cls = std::string(clut ? "clut " : "texture ") + (served ? "uploads-only " : "needs-targets ") +
                                (same ? "SAME: " : "DIFFERENT: ") + auditPages(pages, key, clut);
        AuditClass &entryClass = m_audit[cls];
        ++entryClass.count;
        entryClass.texels += clut ? 256u : uint64_t(key.width) * key.height;
        if (entryClass.example.empty()) {
            char buffer[128];
            std::snprintf(buffer, sizeof buffer, "tbp %05x tbw %u psm %02x %ux%u cbp %05x cpsm %02x", key.tbp0, key.tbw,
                          key.psm, key.width, key.height, key.cbp, key.cpsm);
            entryClass.example = buffer;
        }
    }
}

std::string GsTextureCache::auditReport() const {
    std::string out;
    for (const auto &[name, entryClass] : m_audit) {
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "[upload-audit] %-44s %7llu builds %9llu Ktexels  e.g. %s\n", name.c_str(),
                      static_cast<unsigned long long>(entryClass.count),
                      static_cast<unsigned long long>(entryClass.texels / 1000u), entryClass.example.c_str());
        out += buffer;
    }
    return out;
}

void GsTextureCache::invalidate(const GsPageSet &pages, InvalidationSource source) {
    // A copy into a named image's blocks makes it something else (uploads
    // and draws drop names at their own extent).
    if (!m_named.empty() && source != InvalidationSource::HostWrite && source != InvalidationSource::Draw) {
        GsBlockSet blocks;
        gsBlocksOfPages(blocks, pages);
        dropNamed(blocks, m_namedCopyKeep);
    }
    // The drawn blocks only choose between the two memories, which the
    // bypass does not (sourceFor).
    if (m_tracking && !gsNativeBypass())
        auditStamp(pages, source);
    // Draws mostly land in pages no texture reads; the walk below, over up
    // to 2048 entries, ran for every flush and every host write.
    const GsPageSet hit = m_sourcePagesAll & pages;
    if (hit.none())
        return;
    // Mapping pages to texels belongs at consumption, not at each of the
    // movie's 896 tiny writes. Bitsets merge those writes without a page walk.
    // Only the entries over the written pages, each once (Rogue Galaxy:
    // added; the walk over every entry is kept in the comment above the
    // index).
    const uint64_t generation = ++m_invalidateGeneration;
    gsForEachPage(hit, [&](uint32_t page) {
        for (Entry *entry : m_entriesByPage[page]) {
            if (entry->invalidated == generation)
                continue;
            entry->invalidated = generation;
            entry->dirtyPages |= entry->texelPages & pages;
            entry->clutDirty = entry->clutDirty || (entry->clutPages & pages).any();
        }
    });
}

void GsTextureCache::markTexelPages(const GsTextureKey &key, GsPageSet &pages) const {
    gsMarkPages(pages, key.tbp0, key.tbw, key.psm, key.width, key.height);
}

void GsTextureCache::markClutPages(const GsTextureKey &key, GsPageSet &pages) const {
    if (!gsIsIndexedPsm(key.psm))
        return;

    // The palette is a separate region and just as invalidatable: a game that
    // animates a CLUT rewrites it every frame while the indices stay put.
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = (key.csm == 0u && sixteenBitClut) ? 512u : 256u;
    const uint32_t clutWidth = (key.texclut & 0xffu) + 16u;
    const uint32_t clutHeight = ((key.texclut >> 16u) & 0xffffu) + (entries + 15u) / 16u;
    gsMarkPages(pages, key.cbp, std::max<uint32_t>(key.texclut & 0xffu, 1u), key.cpsm,
                clutWidth, clutHeight);
}

void GsTextureCache::markSourcePages(const GsTextureKey &key, GsPageSet &pages) const {
    markTexelPages(key, pages);
    markClutPages(key, pages);
}

bool GsTextureCache::evictionImminent() const {
    return m_entries.size() >= kMaxEntries;
}

GsPageSet GsTextureCache::sourcePagesFor(const GSDrawState &state) const {
    // The pages depend on the key alone, so the memo never goes stale.
    const GsTextureKey key = keyFor(state);
    if (!m_lastSourceValid || !(key == m_lastSourceKey)) {
        m_lastSourcePages.reset();
        markSourcePages(key, m_lastSourcePages);
        m_lastSourceKey = key;
        m_lastSourceValid = true;
    }
    return m_lastSourcePages;
}

void GsTextureCache::noteDrawnBlocks(uint32_t base, uint32_t bufferWidth, uint32_t psm, const GsRegion &region) {
    // A bound on the blocks the draw reaches (a page of 32 blocks is at
    // least 32 rows and at most 64 pixels wide): most draws miss every named
    // image by it, without marking their blocks.
    const uint32_t pageRows = (region.y1 + 31u) / 32u;
    const uint32_t reach = base + pageRows * std::max<uint32_t>(bufferWidth, 1u) * 32u;
    if (!m_named.empty() && !region.empty() && namedWithin(base, reach == base ? base : reach - 1u)) {
        GsBlockSet drawn;
        gsMarkBlocks(drawn, base, std::max<uint32_t>(bufferWidth, 1u), psm, region.x1, region.y1);
        m_dropFromDraw = true;
        dropNamed(drawn);
        m_dropFromDraw = false;
    }
    if (!m_tracking || region.empty() || gsNativeBypass())
        return;
    DrawMemo &memo = m_drawMemo[base];
    if (memo.generation == m_drawnGeneration && memo.bw == bufferWidth && memo.psm == psm &&
        memo.region.contains(region))
        return;
    gsMarkBlocks(m_drawnBlocks, base, bufferWidth, psm, region.width(), region.height(), region.x0, region.y0);
    memo.region = region;
    memo.bw = bufferWidth;
    memo.psm = psm;
    memo.generation = m_drawnGeneration;
}

GsBlockSet GsTextureCache::blocksFor(const GsTextureKey &key) const {
    GsBlockSet blocks;
    gsMarkBlocks(blocks, key.tbp0, key.tbw, key.psm, key.width, key.height);
    if (gsIsIndexedPsm(key.psm)) {
        const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
        const uint32_t entries = (key.csm == 0u && sixteenBitClut) ? 512u : 256u;
        const uint32_t clutWidth = (key.texclut & 0xffu) + 16u;
        const uint32_t clutHeight = ((key.texclut >> 16u) & 0xffffu) + (entries + 15u) / 16u;
        gsMarkBlocks(blocks, key.cbp, std::max<uint32_t>(key.texclut & 0xffu, 1u), key.cpsm, clutWidth, clutHeight);
    }
    return blocks;
}

GsVram *GsTextureCache::sourceFor(const GsTextureKey &key) {
    if (!m_storeOn)
        return &m_vram;
    const GsBlockSet blocks = blocksFor(key);
    // Under the bypass local memory is never written: a texture names do
    // not serve reads the host writes' memory, filled from the named images
    // it reaches (settleShadow), whatever drew there since.
    if (gsNativeBypass()) {
        if (m_namedOwed != 0u && !namedServes(key))
            settleShadow(blocks);
        return &m_shadow;
    }
    if ((blocks & m_drawnBlocks).none()) {
        ++m_stats.uploadStoreReads;
        if (m_namedOwed != 0u && !namedServes(key))
            settleShadow(blocks);
        return &m_shadow;
    }
    ++m_stats.localMemoryReads;
    // DQ8_GFX_POISON_DRAWN=1: a read of drawn pages that is not served from
    // a target sees garbage (a loud colour), so the picture says whether
    // anything depends on local memory the draws would have written.
    static const bool poison = std::getenv("DQ8_GFX_POISON_DRAWN") != nullptr;
    if (poison) {
        if (m_poisonStorage.empty()) {
            m_poisonStorage.assign(kGsVramBytes, 0xA5u);
            m_poison.attach(m_poisonStorage.data(), static_cast<uint32_t>(m_poisonStorage.size()));
        }
        return &m_poison;
    }
    static const bool warn = std::getenv("DQ8_GFX_WARN_LOCAL_READS") != nullptr;
    if (warn) {
        // Draws wrote blocks this read covers: with no local memory kept
        // for them, whatever is read here is stale. Once per first block.
        static std::set<std::string> seen;
        const GsBlockSet drawn = blocks & m_drawnBlocks;
        char head[160];
        std::snprintf(head, sizeof head, "tbp %05x tbw %u psm %02x %ux%u cbp %05x cpsm %02x", key.tbp0, key.tbw, key.psm,
                      key.width, key.height, key.cbp, key.cpsm);
        std::string line = head;
        m_targets.forEachSurface([&](const GsSurface &surface) {
            if (surface.depth || surface.undefined)
                return;
            GsBlockSet own;
            gsMarkBlocks(own, surface.base, surface.bufferWidth, surface.psm, surface.width, surface.height);
            if ((own & drawn).none())
                return;
            char buffer[96];
            std::snprintf(buffer, sizeof buffer, " | target %05x fbw %u psm %02x %ux%u (%zu blocks)", surface.base,
                          surface.bufferWidth, surface.psm, surface.width, surface.height, (own & drawn).count());
            line += buffer;
        });
        if (seen.insert(line).second)
            std::fprintf(stderr, "[local-read] %zu of %zu blocks drawn: %s\n", drawn.count(), blocks.count(),
                         line.c_str());
    }
    return &m_vram;
}

GsTextureIdentity GsTextureCache::regionIdentity(const GSDrawState &state, const GsRegion &region) {
    const GsTextureKey key = keyFor(state);
    const bool identityRead = m_identityRead;
    m_identityRead = true;
    struct Restore { bool &flag; bool value; ~Restore() { flag = value; } } restore{m_identityRead, identityRead};
    SourceScope scope(*this, key);
    GsTextureIdentity identity;
    identity.texels = expand(key, region, true);
    if (gsIsIndexedPsm(key.psm))
        identity.clut = hashClutWindow(key);
    return identity;
}

SDL_GPUTexture *GsTextureCache::replacement(const GSDrawState &state, const GsRegion &region, const GsRegion &sampled,
                                            uint32_t wantedScale, bool border, bool shadow, bool bold,
                                            uint32_t &scale, std::string &error) {
    if (!m_regionLookup || region.empty())
        return nullptr;
    const GsTextureKey key = keyFor(state);
    if (!gsIsIndexedPsm(key.psm))
        return nullptr;
    SourceScope scope(*this, keyFor(state));
    // The palette first: most regions drawn are art the host leaves alone,
    // and their texels need no hash.
    const uint64_t clut = hashClutWindow(key);
    if (!m_lastPaletteValid || clut != m_lastPalette) {
        m_lastPalette = clut;
        m_lastPaletteWanted = !m_regionPalettes || m_regionPalettes(clut);
        m_lastPaletteValid = true;
    }
    if (!m_lastPaletteWanted)
        return nullptr;
    const bool identityRead = m_identityRead;
    m_identityRead = true;
    const uint64_t texels = expand(key, region, true);
    m_identityRead = identityRead;
    uint64_t id = kHashSeed;
    for (const uint64_t value : {texels, clut, uint64_t(region.width()) << 32u | region.height(),
                                 uint64_t(wantedScale) << 3u | (shadow ? 4u : 0u) | (border ? 2u : 0u) |
                                     (bold ? 1u : 0u),
                                 uint64_t(sampled.x0) << 48u | uint64_t(sampled.y0) << 32u |
                                     uint64_t(sampled.x1) << 16u | sampled.y1}) {
        id ^= value;
        id *= 1099511628211ull;
    }
    if (const auto larger = m_larger.find(id); larger != m_larger.end()) {
        larger->second.used = ++m_largerClock;
        scale = larger->second.scale;
        return larger->second.texture;
    }
    if (m_largerMissing.count(id))
        return nullptr;
    expand(key, region);
    for (uint32_t y = region.y0; y < region.y1; ++y)
        for (uint32_t x = region.x0; x < region.x1; ++x)
            if (x < sampled.x0 || x >= sampled.x1 || y < sampled.y0 || y >= sampled.y1)
                m_staging[(size_t(y - region.y0) * region.width() + (x - region.x0)) * 4u + 3u] = 0u;
    RegionRequest request;
    request.texels = texels;
    request.clut = clut;
    request.width = region.width();
    request.height = region.height();
    request.x = region.x0;
    request.y = region.y0;
    request.rgba = m_staging.data();
    request.scale = wantedScale;
    request.border = border;
    request.shadow = shadow;
    request.bold = bold;
    uint32_t width = 0u, height = 0u;
    const int answer = m_regionLookup(request, width, height, m_regionPixels);
    if (answer == kRegionPending)
        return nullptr;
    const uint32_t margin = border || shadow ? 2u * kRegionBorderMargin : 0u;
    const uint32_t baseWidth = region.width() + margin;
    const uint32_t baseHeight = region.height() + margin;
    const uint32_t factor = width / baseWidth;
    if (answer != kRegionReady || factor < 2u || factor > 8u || width != baseWidth * factor ||
        height != baseHeight * factor || m_regionPixels.size() != size_t(width) * height * 4u) {
        m_largerMissing.insert(id);
        return nullptr;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    if (!texture) {
        error = std::string("SDL_CreateGPUTexture(larger): ") + SDL_GetError();
        return nullptr;
    }
    m_staging.swap(m_regionPixels);
    if (!uploadRegion(texture, {0u, 0u, width, height}, error)) {
        SDL_ReleaseGPUTexture(m_device.handle(), texture);
        return nullptr;
    }
    // Past the budget the least recently drawn go; a queued draw of one
    // lands first.
    constexpr uint64_t kLargerBudget = 512ull << 20u;
    m_largerBytes += uint64_t(width) * height * 4u;
    while (m_largerBytes > kLargerBudget && !m_larger.empty()) {
        auto oldest = m_larger.begin();
        for (auto it = m_larger.begin(); it != m_larger.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        beforeRewrite(oldest->second.texture);
        SDL_ReleaseGPUTexture(m_device.handle(), oldest->second.texture);
        m_largerBytes -= oldest->second.bytes;
        m_larger.erase(oldest);
    }
    m_larger[id] = {texture, factor, uint64_t(width) * height * 4u, ++m_largerClock};
    scale = factor;
    return texture;
}

GsTextureKey GsTextureCache::keyFor(const GSDrawState &state) {
    const GSTex0Reg &tex0 = state.context.tex0;

    GsTextureKey key{};
    key.tbp0 = tex0.tbp0;
    key.tbw = std::max<uint32_t>(tex0.tbw, 1u);
    key.psm = tex0.psm;
    key.width = std::max<uint32_t>(state.textureWidth, 1u);
    key.height = std::max<uint32_t>(state.textureHeight, 1u);
    key.cbp = tex0.cbp;
    key.cpsm = tex0.cpsm;
    key.csm = tex0.csm;
    key.csa = tex0.csa;
    key.texa = static_cast<uint32_t>(state.texa.ta0) |
               (state.texa.aem ? 0x100u : 0u) |
               (static_cast<uint32_t>(state.texa.ta1) << 16u);
    key.texclut = static_cast<uint32_t>(state.texclut.cbw) |
                  (static_cast<uint32_t>(state.texclut.cou) << 8u) |
                  (static_cast<uint32_t>(state.texclut.cov) << 16u);
    return key;
}

SDL_GPUTexture *GsTextureCache::acquire(const GSDrawState &state, std::string &error, const GsRegion *sampled) {
    const GsTextureKey key = keyFor(state);

    ++m_stats.lookups;
    if (auto found = m_entries.find(key); found != m_entries.end()) {
        ++m_stats.hits;
        Entry &entry = found->second;
        if (entry.dirtyPages.any()) {
            entry.dirty.merge(regionForPages(key, entry.dirtyPages));
            entry.dirtyPages.reset();
        }
        if (entry.clutDirty && entry.paletteFromTarget) {
            entry.clutDirty = false;
            entry.dirty.merge(GsRegion{0u, 0u, key.width, key.height});
        } else if (entry.clutDirty) {
            if (!resolveSource(key, {}, error)) return nullptr;
            refreshClut(key, entry);
        }
        // Texels never read back join the stale ones once a draw reaches
        // them, and stale texels wait while no draw does.
        if (!entry.unresolved.empty() && (!sampled || sampled->intersects(entry.unresolved))) {
            entry.dirty.merge(entry.unresolved);
            entry.unresolved.clear();
        }
        if (!entry.dirty.empty() && (!sampled || entry.dirty.intersects(*sampled)) &&
            !update(key, entry, error, sampled))
            return nullptr;
        return entry.texture;
    }

    if (m_entries.size() >= kMaxEntries) {
        reset();
        ++m_stats.evictions;
    }

    Entry entry{};
    if (!build(key, entry, error, sampled))
        return nullptr;
    markSourcePages(key, entry.pages);
    markTexelPages(key, entry.texelPages);
    markClutPages(key, entry.clutPages);
    if (m_auditOn)
        auditBuild(key, entry);
    SDL_GPUTexture *texture = entry.texture;
    m_sourcePagesAll |= entry.pages;
    Entry &stored = m_entries.emplace(key, std::move(entry)).first->second;
    gsForEachPage(stored.pages, [&](uint32_t page) { m_entriesByPage[page].push_back(&stored); });
    return texture;
}

// Expands one texel rectangle into m_staging as tightly packed RGBA8 rows of
// region.width() pixels, and returns the hash of the raw texels it read.
//
// Three fast paths carry everything DQ8 draws during boot: CT32/Z32 store
// straight into staging (the raw word is the RGBA byte sequence), P8 resolves
// the palette once and indexes it per texel, and the P8H/P4H* family reads
// its row through the CT32 layout and shifts the index out of each word. The
// per-pixel path below remains for every other PSM; it is correct for all of
// them and none of them is hot on this game.
void GsTextureCache::buildPalette(const GsTextureKey &key) {
    const TexaState texa = unpackTexa(key.texa);
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const uint32_t psm = key.psm & 0x3fu;
    const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    m_clutRgba.resize(fourBit ? 16u : 256u);
    for (uint32_t raw = 0u; raw < m_clutRgba.size(); ++raw) {
        const uint32_t clutIndex = resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
        const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
        const uint32_t entryValue = m_src->read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
        uint32_t color = 0u;
        switch (key.cpsm & 0x3fu) {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
            break;
        default:
            color = applyTexa(texa, key.cpsm, entryValue);
            break;
        }
        m_clutRgba[raw] = color;
    }
}

uint64_t GsTextureCache::expand(const GsTextureKey &key, const GsRegion &region, bool hashOnly) {
    if (!m_identityRead && namedOn()) {
        uint64_t namedHash = 0u;
        if (expandNamed(key, region, hashOnly, namedHash))
            return namedHash;
    }
    if (!hashOnly && !m_identityRead && gsNativeBypass()) {
        GsPageSet texelPages, clutPages;
        markTexelPages(key, texelPages);
        markClutPages(key, clutPages);
        const bool clutNamed = m_named.find(key.cbp) != m_named.end();
        char shape[224];
        std::snprintf(shape, sizeof shape,
                      "tbp %05x tbw %u psm %02x %ux%u cbp %05x cpsm %02x csm %u: %s; texels %s, palette %s%s",
                      key.tbp0, key.tbw, key.psm & 0x3fu, key.width, key.height, key.cbp, key.cpsm & 0x3fu, key.csm,
                      m_namedMissWhy, m_targets.ownsAny(texelPages) ? "in a target" : "in no target",
                      clutPages.none() ? "none" : (m_targets.ownsAny(clutPages) ? "in a target" : "in no target"),
                      clutPages.none() ? "" : (clutNamed ? " (named)" : " (unnamed)"));
        memoryuses::note(memoryuses::Kind::TextureFromMemory, shape);
    }
    const TexaState texa = unpackTexa(key.texa);
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool indexed = gsIsIndexedPsm(key.psm);
    const uint32_t regionWidth = region.width();
    const uint32_t regionHeight = region.height();
    const uint32_t psm = key.psm & 0x3fu;

    if (!hashOnly)
        m_staging.resize(static_cast<size_t>(regionWidth) * regionHeight * 4u);
    uint64_t texelHash =
        hashStep(hashStep(hashStep(kHashSeed, key.width), key.height), key.psm);

    // The palette resolved to final RGBA once per expand, so the texel loop is
    // a load and a store. A per-pixel swizzled palette read here was the
    // single most expensive thing in the old loop.
    if (indexed && !hashOnly)
        buildPalette(key);

    const bool rowIsWords = psm == GS_PSM_CT32 || psm == GS_PSM_Z32 ||
                            psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    m_rawRow.resize(static_cast<size_t>(regionWidth) * (rowIsWords ? 4u : 1u));
    m_idxRow.resize(regionWidth);

    for (uint32_t y = region.y0; y < region.y1; ++y) {
        // Hashing only, the texels are read and hashed exactly as below and
        // nothing is written (Rogue Galaxy: added).
        uint8_t *row = hashOnly ? nullptr
                                : m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        switch (psm) {
        case GS_PSM_CT32:
        case GS_PSM_Z32: {
            // The raw word is the RGBA byte sequence; TEXA does not apply.
            const auto readRow = psm == GS_PSM_Z32 ? GSMem::ReadRowZ32 : GSMem::ReadRowCT32;
            uint8_t *out = hashOnly ? m_rawRow.data() : row;
            readRow(m_src->data(), key.tbp0, key.tbw, region.x0, y, regionWidth, out);
            texelHash = hashBytes(texelHash, out, static_cast<size_t>(regionWidth) * 4u);
            continue;
        }
        case GS_PSM_T8: {
            uint8_t *raw = m_rawRow.data();
            GSMem::ReadRowP8(m_src->data(), key.tbp0, key.tbw, region.x0, y,
                             regionWidth, raw);
            texelHash = hashBytes(texelHash, raw, regionWidth);
            if (hashOnly)
                continue;
            for (uint32_t x = 0u; x < regionWidth; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[raw[x]], 4u);
            continue;
        }
        case GS_PSM_T4: {
            // Rogue Galaxy: added. Its textures are mostly 4-bit, and the
            // general loop below re-read every texel's palette entry from
            // local memory although buildPalette has already resolved them.
            uint8_t *raw = m_idxRow.data();
            m_src->readRowT4(key.tbp0, key.tbw, region.x0, y, regionWidth, raw);
            if (!hashOnly)
                for (uint32_t x = 0u; x < regionWidth; ++x)
                    std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[raw[x] & 0x0fu], 4u);
            // Hashed as two nibbles per byte, low first, like T4HL/T4HH.
            const uint32_t pairs = (regionWidth + 1u) / 2u;
            for (uint32_t p = 0u; p < pairs; ++p)
                raw[p] = static_cast<uint8_t>(raw[p * 2u] |
                                              ((p * 2u + 1u < regionWidth ? raw[p * 2u + 1u] : 0u) << 4u));
            texelHash = hashBytes(texelHash, raw, pairs);
            continue;
        }
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH: {
            // Indices ride in a 32-bit layout: read the row as CT32 words and
            // shift each index out. All the index bits are inside the pixel's
            // own word, so the +3-byte read offset the per-pixel path uses is
            // not needed here.
            uint32_t *words = reinterpret_cast<uint32_t *>(m_rawRow.data());
            uint8_t *raw = m_idxRow.data();
            GSMem::ReadRowCT32(m_src->data(), key.tbp0, key.tbw, region.x0, y,
                               regionWidth, reinterpret_cast<uint8_t *>(words));
            const uint32_t shift = psm == GS_PSM_T4HH ? 28u : 24u;
            const uint32_t mask = psm == GS_PSM_T8H ? 0xffu : 0x0fu;
            for (uint32_t x = 0u; x < regionWidth; ++x)
                raw[x] = static_cast<uint8_t>((words[x] >> shift) & mask);
            if (psm == GS_PSM_T8H) {
                texelHash = hashBytes(texelHash, raw, regionWidth);
            } else {
                // Two nibbles per byte, low first, so the packed stream stays
                // the hash input.
                const uint32_t pairs = (regionWidth + 1u) / 2u;
                for (uint32_t p = 0u; p < pairs; ++p)
                    raw[p] = static_cast<uint8_t>(raw[p * 2u] |
                                                  ((p * 2u + 1u < regionWidth
                                                        ? raw[p * 2u + 1u]
                                                        : 0u)
                                                   << 4u));
                texelHash = hashBytes(texelHash, raw, pairs);
            }
            if (hashOnly)
                continue;
            for (uint32_t x = 0u; x < regionWidth; ++x) {
                const uint32_t index = (words[x] >> shift) & mask;
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[index], 4u);
            }
            continue;
        }
        default:
            break;
        }

        // Formats with no fast path: one swizzled read and one hash word per
        // pixel, exactly as before.
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            const uint32_t raw = m_src->read(key.psm, key.tbp0, key.tbw, x, y);
            texelHash = hashStep(texelHash, raw);
            if (hashOnly)
                continue;
            uint32_t color = 0u;

            if (indexed) {
                const uint32_t clutIndex =
                    resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
                const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
                const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
                const uint32_t entryValue =
                    m_src->read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
                switch (key.cpsm & 0x3fu) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                    color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
                    break;
                default:
                    color = applyTexa(texa, key.cpsm, entryValue);
                    break;
                }
            } else {
                switch (psm) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                case GS_PSM_Z16:
                case GS_PSM_Z16S:
                    color = applyTexa(texa, GS_PSM_CT16, rgba5551To8888(raw));
                    break;
                default:
                    color = applyTexa(texa, key.psm, raw);
                    break;
                }
            }

            const size_t offset = static_cast<size_t>(x - region.x0) * 4u;
            row[offset] = static_cast<uint8_t>(color);
            row[offset + 1u] = static_cast<uint8_t>(color >> 8u);
            row[offset + 2u] = static_cast<uint8_t>(color >> 16u);
            row[offset + 3u] = static_cast<uint8_t>(color >> 24u);
        }
    }
    return texelHash;
}

// Which texels of a texture a set of local-memory pages covers. Same geometry
// as GsTargetCache::regionForPages, applied to a texture's own base and width.
GsRegion GsTextureCache::regionForPages(const GsTextureKey &key,
                                        const GsPageSet &pages) const {
    GsRegion region{};
    const GsPageExtent extent = gsPageExtent(key.psm);
    if (!extent.valid())
        return {0u, 0u, key.width, key.height};

    const uint32_t pixelsPerRow = std::max<uint32_t>(key.tbw, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t basePage = key.tbp0 >> 5u;
    const uint32_t pageRows = (key.height + extent.height - 1u) / extent.height;
    const uint32_t pageColumns = (key.width + extent.width - 1u) / extent.width;

    for (uint32_t row = 0u; row < pageRows; ++row) {
        for (uint32_t column = 0u; column < pageColumns; ++column) {
            const uint32_t page = (basePage + row * pagesPerRow + column) % kGsPageCount;
            const bool tailHit = (key.tbp0 & 31u) != 0u &&
                                 pages.test((page + 1u) % kGsPageCount);
            if (!pages.test(page) && !tailHit)
                continue;
            GsRegion cell{column * extent.width, row * extent.height,
                          std::min((column + 1u) * extent.width, key.width),
                          std::min((row + 1u) * extent.height, key.height)};
            region.merge(cell);
        }
    }
    return region;
}

// Transfer buffers come back once the submission that read them completes.
// Reaping happens on take; the common case never waits because by the time
// a buffer is wanted again the previous upload has long finished.
SDL_GPUTransferBuffer *GsTextureCache::takeUploadBuffer(size_t bytes) {
    std::string error;
    return m_uploadPool.take(m_device, bytes, error);
}

void GsTextureCache::recycleUploadBuffer(SDL_GPUTransferBuffer *buffer) {
    m_uploadPool.used(buffer, m_device.pendingSerial());
}

void GsTextureCache::releaseUploadPool() {
    m_uploadPool.release(m_device);
}

bool GsTextureCache::uploadRegion(SDL_GPUTexture *texture, const GsRegion &region,
                                  std::string &error) {
    return uploadBytes(texture, region, m_staging.data(), 4u, error);
}

bool GsTextureCache::uploadBytes(SDL_GPUTexture *texture, const GsRegion &region, const uint8_t *data,
                                 uint32_t bytesPerTexel, std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * bytesPerTexel;

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(bytes);
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "texture upload");
    if (!commands)
        return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        m_device.cancelPending();
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.x = region.x0;
    destination.y = region.y0;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    // cycle stays false: a cycled destination would come back with its
    // untouched texels undefined, which a partial update must not do. The
    // buffer side is guarded by the pool fence instead.
    SDL_UploadToGPUTexture(copy, &source, &destination, m_uploadCycle);
    SDL_EndGPUCopyPass(copy);
    recycleUploadBuffer(upload);
    return true;
}

// Hash of the palette window this texture's indices resolve into: only the
// entries the CLUT resolution can actually reach, CSA window included.
uint64_t GsTextureCache::hashClutWindow(const GsTextureKey &key) const {
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = ((key.psm & 0x3fu) == GS_PSM_T4 ||
                              (key.psm & 0x3fu) == GS_PSM_T4HL ||
                              (key.psm & 0x3fu) == GS_PSM_T4HH)
                                 ? 16u
                                 : (sixteenBitClut ? 512u : 256u);
    uint64_t clutHash = hashStep(hashStep(kHashSeed, key.cpsm), key.csm);
    for (uint32_t index = 0u; index < entries; ++index) {
        const uint32_t resolved =
            resolveClutIndex(index, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (resolved & 0x0fu);
        const uint32_t clutY = clutOriginV + (resolved >> 4u);
        clutHash = hashStep(clutHash, m_src->read(key.cpsm, key.cbp, clutWidth, clutX, clutY));
    }
    return clutHash;
}

// A palette-page write landed. Re-hash the window: when it is unchanged --
// games re-upload the same CLUT every frame -- this is the whole cost of the
// write. When it moved, every expanded colour is stale, so the whole texture
// re-expands on next use.
void GsTextureCache::refreshClut(const GsTextureKey &key, Entry &entry) {
    SourceScope scope(*this, key);
    entry.clutDirty = false;
    if (!gsIsIndexedPsm(key.psm))
        return;
    const uint64_t clutHash = hashClutWindow(key);
    if (clutHash == entry.identity.clut)
        return;
    entry.identity.clut = clutHash;
    entry.clutUploadPending = true;
    entry.dirty.merge(GsRegion{0u, 0u, key.width, key.height});
}

// Patches the texels a host write touched, instead of discarding a texture and
// re-expanding all of it. On the DQ8 movie path the written region is a small
// fraction of a 512x512 texture, and rebuilding it was the dominant cost.
GsPageSet GsTextureCache::staleTexelPages(const GsTextureKey &key, const GsRegion &region) const {
    GsPageSet pages;
    if (region.empty())
        return pages;
    if (key.psm == GS_PSM_CT32 || key.psm == GS_PSM_CT24 ||
        key.psm == GS_PSM_T8H || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH) {
        m_targets.markReadPagesCt32(pages, key.tbp0, key.tbw, region);
    } else {
        // Through the words the CPU wrote where it can tell (T8, T4, CT16):
        // a texture uploaded into pages a target still owns needs no
        // readback (Rogue Galaxy: markReadPages).
        m_targets.markReadPages(pages, key.tbp0, key.tbw, key.psm, region);
    }
    return pages;
}

bool GsTextureCache::resolveSource(const GsTextureKey &key, const GsRegion &region,
                                  std::string &error, const GsPageSet *exclude) {
    // The native bypass reads no target back into local memory: textures
    // come from names, the upload store or the targets on the GPU.
    if (gsNativeBypass()) {
        ++m_stats.resolvesBypassed;
        return true;
    }
    GsPageSet conservative;
    markSourcePages(key, conservative);
    if (!m_targets.ownsAny(conservative)) return true;
    static const bool blockwiseResolve = std::getenv("DQ8_GFX_NO_BLOCKWISE_TARGETS") == nullptr;
    if (m_storeOn && blockwiseResolve && !blocksDrawn(key)) {
        ++m_stats.targetReadsSkipped;
        return true;
    }

    GsPageSet pages = staleTexelPages(key, region);
    if (exclude)
        pages &= ~*exclude;
    GsPageSet clutPages;
    markClutReads(key, clutPages);
    // The palette is a few texels of whatever target holds it.
    return m_targets.resolve(pages, error) && m_targets.copyOut(clutPages, error);
}

void GsTextureCache::markClutReads(const GsTextureKey &key, GsPageSet &pages) const {
    if (!gsIsIndexedPsm(key.psm))
        return;
    if (key.cpsm != GS_PSM_CT32 && key.cpsm != GS_PSM_CT24) {
        markClutPages(key, pages);
        return;
    }
    // Palette words the CPU wrote itself are already right in local memory.
    const bool fourBit = key.psm == GS_PSM_T4 || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH;
    const uint32_t count = fourBit ? 16u : 256u;
    const uint32_t bw = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t index = resolveClutIndex(i, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t x = ((key.texclut >> 8u) & 0xffu) + (index & 15u);
        const uint32_t y = (key.texclut >> 16u) + (index >> 4u);
        m_targets.markReadPagesCt32(pages, key.cbp, bw, {x, y, x + 1u, y + 1u});
    }
}

// Where each palette entry of a CT32 CSM1 CLUT sits in the target that holds
// it, as the GPU reads it: x in the low 16 bits, y in the high, in GS pixels
// of the target (Rogue Galaxy: added; gs_index8.frag's palette-in-target
// mode).
void GsTextureCache::targetPaletteCoordinates(const GsTextureKey &key, const GsSurface &target,
                                              std::array<uint32_t, 256> &out) const {
    static const std::array<uint16_t, 2048> wordPixel = [] {
        std::array<uint16_t, 2048> table{};
        for (uint32_t y = 0u; y < 32u; ++y)
            for (uint32_t x = 0u; x < 64u; ++x)
                table[GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u] = static_cast<uint16_t>(x | (y << 8u));
        return table;
    }();
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const uint32_t psm = key.psm & 0x3fu;
    const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    const uint32_t targetPagesPerRow = std::max<uint32_t>(target.bufferWidth, 1u);
    out.fill(0u);
    for (uint32_t raw = 0u; raw < (fourBit ? 16u : 256u); ++raw) {
        const uint32_t clutIndex = resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
        const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
        const uint32_t word = GSPSMCT32::addrPSMCT32(key.cbp, clutWidth, clutX, clutY) >> 2u;
        const uint32_t page = (word >> 11u) & 511u;
        const uint32_t relative = (page + 512u - ((target.base >> 5u) & 511u)) & 511u;
        const uint16_t within = wordPixel[word & 2047u];
        const uint32_t x = (relative % targetPagesPerRow) * 64u + (within & 0xffu);
        const uint32_t y = (relative / targetPagesPerRow) * 32u + (within >> 8u);
        out[raw] = (x & 0xffffu) | (y << 16u);
    }
}

bool GsTextureCache::expandFromTarget(const GsTextureKey &key, Entry &entry, GsRegion region,
                                      bool &handled, std::string &error) {
    handled = false;
    // An 8-bit indexed texture through its palette, or (Rogue Galaxy: added)
    // a CT32 one texel for texel, from the target that drew its pages. The
    // flip's copy of the frame into the display buffer reads the frame so,
    // every frame; through local memory that was a readback of the whole
    // frame, an expansion and an upload.
    // Also (Rogue Galaxy: added) T4, and the alpha formats T8H, T4HL and
    // T4HH, which read a CT32 target's alpha byte: the frame read as T4 with
    // its palette (DQ8_GFX_NO_INDEXED4_TARGETS=1 reads those back instead).
    static const bool indexed4 = std::getenv("DQ8_GFX_NO_INDEXED4_TARGETS") == nullptr;
    const uint32_t texturePsm = key.psm & 0x3fu;
    uint32_t mode = 0u;
    switch (texturePsm) {
    case GS_PSM_T8: mode = 0u; break;
    case GS_PSM_T4: mode = 1u; break;
    case GS_PSM_T8H: mode = 2u; break;
    case GS_PSM_T4HL: mode = 3u; break;
    case GS_PSM_T4HH: mode = 4u; break;
    default: mode = 5u; break;
    }
    if (mode != 0u && mode != 5u && !indexed4)
        mode = 5u;
    const bool indexed = mode != 5u;
    static const bool direct = std::getenv("DQ8_GFX_NO_DIRECT_TARGET_TEXTURES") == nullptr;
    if ((!indexed && !((key.psm & 0x3fu) == GS_PSM_CT32 && direct)) || (key.tbp0 & 31u) != 0u)
        return true;
    GsPageSet texelPages;
    if (indexed)
        markTexelPages(key, texelPages);
    else if (region.empty())
        return true;
    else
        gsMarkPages(texelPages, key.tbp0, key.tbw, key.psm, region.width(), region.height(), region.x0, region.y0);
    GsSurface *target = m_targets.nativeOwner(texelPages, true);
    // A texture whose blocks no draw wrote is what the host wrote there, which
    // the host writes' memory has (the target only holds it as a patch, in
    // pages it shares with the texture): not a read of the target.
    static const bool blockwise = std::getenv("DQ8_GFX_NO_BLOCKWISE_TARGETS") == nullptr;
    if (target && m_storeOn && blockwise && !blocksDrawn(key)) {
        ++m_stats.targetReadsSkipped;
        target = nullptr;
    }
    // Pages of several targets (Rogue Galaxy: added; the 512-wide read of
    // 02ba0 over four): each page from the target that drew it, when every
    // page has one (DQ8_GFX_NO_MULTI_TARGET_TEXTURES=1 reads them back).
    std::vector<std::pair<GsSurface *, GsPageSet>> pieces;
    static const bool multi = std::getenv("DQ8_GFX_NO_MULTI_TARGET_TEXTURES") == nullptr;
    if (!target && multi && m_storeOn && blocksDrawn(key)) {
        bool all = true;
        gsForEachPage(texelPages, [&](uint32_t page) {
            if (!all)
                return;
            GsPageSet one;
            one.set(page);
            GsSurface *owner = m_targets.nativeOwner(one, true);
            if (!owner) {
                all = false;
                return;
            }
            auto it = std::find_if(pieces.begin(), pieces.end(), [&](const auto &p) { return p.first == owner; });
            if (it == pieces.end())
                it = pieces.insert(pieces.end(), {owner, GsPageSet{}});
            it->second.set(page);
        });
        if (all && pieces.size() > 1u)
            target = pieces.front().first;
        else
            pieces.clear();
    }
    for (size_t i = 1; i < pieces.size(); ++i)
        if (!m_targets.refreshRegion(*pieces[i].first, m_targets.regionForPages(*pieces[i].first, pieces[i].second), error))
            return false;
    if (!indexed && std::getenv("DQ8_GFX_DEBUG_DIRECT"))
        std::fprintf(stderr, "[direct] tbp %05x tbw %u %ux%u region %u,%u-%u,%u target %s\n", key.tbp0, key.tbw,
                     key.width, key.height, region.x0, region.y0, region.x1, region.y1, target ? "yes" : "no");
    if (!target)
        return true;

    // The palette is read on the CPU, unless (Rogue Galaxy: added) it is a
    // CT32 CSM1 palette inside the same target as the texels (the frame read
    // as T8/T4 with its CLUT in the frame): then the shader fetches each
    // entry from the target (DQ8_GFX_NO_TARGET_PALETTES=1 copies it out).
    // Only the cells read need be current (the target's other queued
    // patches, the textures uploaded into its other pages, can wait:
    // refreshRegion).
    GsPageSet clutPages;
    if (indexed)
        markClutReads(key, clutPages);
    static const bool targetPalettes = std::getenv("DQ8_GFX_NO_TARGET_PALETTES") == nullptr;
    // Also a palette in another CT32 target (Rogue Galaxy: the palettes at
    // 03f80), bound beside the texels' target.
    GsSurface *paletteTarget = nullptr;
    if (indexed && targetPalettes && (key.cpsm & 0x3fu) == GS_PSM_CT32 && key.csm == 0u && clutPages.any()) {
        paletteTarget = m_targets.nativeOwner(clutPages, true);
        if (paletteTarget && (clutPages & ~paletteTarget->pages).any())
            paletteTarget = nullptr;
        // Its blocks no draw wrote are the host's (the upload store has them).
        if (paletteTarget && m_storeOn && !clutBlocksDrawn(key))
            paletteTarget = nullptr;
    }
    const bool paletteInTarget = paletteTarget != nullptr;
    if (paletteInTarget) {
        if (!m_targets.refreshRegion(*target, m_targets.regionForPages(*target, texelPages), error) ||
            !m_targets.refreshRegion(*paletteTarget, m_targets.regionForPages(*paletteTarget, clutPages), error))
            return false;
    } else if (!m_targets.copyOut(clutPages, error) ||
               !m_targets.refreshRegion(*target, m_targets.regionForPages(*target, texelPages), error)) {
        return false;
    }
    if (indexed && !paletteInTarget)
        buildPalette(key);

    SDL_GPUTexture *preserve = nullptr;
    if (!entry.renderable) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = key.width;
        info.height = key.height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device.handle(), &info);
        if (!texture) {
            error = std::string("SDL_CreateGPUTexture(indexed from target): ") + SDL_GetError();
            return false;
        }
        if (entry.texture) {
            beforeRewrite(entry.texture);
            if (indexed)
                SDL_ReleaseGPUTexture(m_device.handle(), entry.texture);
            else
                preserve = entry.texture; // released once copied from
        }
        if (indexed)
            region = {0u, 0u, key.width, key.height};
        entry.texture = texture;
        entry.renderable = true;
    } else {
        beforeRewrite(entry.texture);
    }

    const SDL_Rect rect{int(region.x0), int(region.y0), int(region.width()), int(region.height())};
    // T8 and T4 pages are two buffer-width units wide; the alpha formats
    // are CT32's pages.
    const uint32_t texturePagesPerRow = !indexed ? key.tbw
                                        : mode < 2u ? std::max<uint32_t>(key.tbw >> 1u, 1u)
                                                    : std::max<uint32_t>(key.tbw, 1u);
    const std::array<uint32_t, 4> pages{key.tbp0 >> 5u,
                                        texturePagesPerRow | (indexed ? mode << 16u : 0u) |
                                            (paletteInTarget ? (1u << 24u) |
                                                                   (std::min(std::max(paletteTarget->scale, 1u), 15u) << 25u)
                                                             : 0u),
                                        target->base >> 5u,
                                        target->bufferWidth | (std::max(target->scale, 1u) << 16u)};
    std::array<uint32_t, 256> palette{};
    if (paletteInTarget)
        targetPaletteCoordinates(key, *paletteTarget, palette);
    else if (indexed)
        std::copy_n(m_clutRgba.begin(), std::min<size_t>(m_clutRgba.size(), palette.size()), palette.begin());
    entry.paletteFromTarget = paletteInTarget;
    const auto expandFrom = [&](GsSurface &from, const SDL_Rect &area, SDL_GPUTexture *keep) {
        std::array<uint32_t, 4> fromPages = pages;
        fromPages[2] = from.base >> 5u;
        fromPages[3] = from.bufferWidth | (std::max(from.scale, 1u) << 16u);
        return indexed ? m_device.expandIndexed8(from.texture, palette.data(), entry.texture, key.width, key.height,
                                                 area, fromPages, error,
                                                 paletteInTarget ? paletteTarget->texture : nullptr)
                       : m_device.copyFromTarget(from.texture, entry.texture, key.width, key.height, area, fromPages,
                                                 keep, error);
    };
    bool done = true;
    if (pieces.empty()) {
        done = expandFrom(*target, rect, preserve);
    } else {
        // A pass a page, each from its own target; the first keeps what
        // `preserve` held outside the region.
        bool first = true;
        for (const auto &[from, piecePages] : pieces) {
            gsForEachPage(piecePages, [&](uint32_t page) {
                if (!done)
                    return;
                GsPageSet one;
                one.set(page);
                GsRegion area = regionForPages(key, one);
                area.x0 = std::max(area.x0, region.x0);
                area.y0 = std::max(area.y0, region.y0);
                area.x1 = std::min(area.x1, region.x1);
                area.y1 = std::min(area.y1, region.y1);
                if (area.empty())
                    return;
                const SDL_Rect pageRect{int(area.x0), int(area.y0), int(area.width()), int(area.height())};
                done = expandFrom(*from, pageRect, first ? preserve : nullptr);
                first = false;
            });
        }
        if (done)
            ++m_stats.expandedFromSeveralTargets;
    }
    if (preserve) {
        // The copy was recorded; the old texture goes when it has run.
        SDL_ReleaseGPUTexture(m_device.handle(), preserve);
    }
    if (!done)
        return false;
    ++m_stats.expandedFromTargets;
    handled = true;
    checkAgainstCpu(key, entry, region);
    return true;
}

// DQ8_GFX_CHECK_GPU_TEXTURES=1 rebuilds every texture expandFromTarget made
// the old way, reading the target back, and reports texels that differ. It
// stalls on every readback, so it is for checking changes, not for play.
void GsTextureCache::checkAgainstCpu(const GsTextureKey &key, Entry &entry, const GsRegion &region) {
    static const bool enabled = std::getenv("DQ8_GFX_CHECK_GPU_TEXTURES") != nullptr;
    if (!enabled)
        return;
    std::vector<uint8_t> gpu;
    std::string error;
    if (!m_device.submitPending(error) ||
        !m_device.downloadTexture(entry.texture, key.width, key.height, gpu, error) ||
        !resolveSource(key, region, error)) {
        std::fprintf(stderr, "[texture-check] %s\n", error.c_str());
        return;
    }
    expand(key, region);
    uint64_t differing = 0u;
    for (uint32_t y = region.y0; y < region.y1; ++y)
        for (uint32_t x = region.x0; x < region.x1; ++x)
            differing += std::memcmp(gpu.data() + (size_t(y) * key.width + x) * 4u,
                                     m_staging.data() + (size_t(y - region.y0) * region.width() + (x - region.x0)) * 4u,
                                     4u) != 0;
    std::fprintf(stderr, "[texture-check] tbp=%05x %ux%u from target: %llu of %llu texels differ\n", key.tbp0,
                 key.width, key.height, static_cast<unsigned long long>(differing),
                 static_cast<unsigned long long>(region.width()) * region.height());
}

void GsTextureCache::restrictToSampled(const GsTextureKey &key, const GsRegion &region, const GsRegion *sampled,
                                       GsPageSet &exclude, GsRegion &unresolved) const {
    exclude.reset();
    if (!sampled || region.empty() || sampled->contains(region))
        return;
    // Only pages a target holds matter: stale words nobody owns resolve to
    // nothing anyway.
    GsPageSet stale = staleTexelPages(key, region);
    for (uint32_t page = 0u; page < kGsPageCount; ++page) {
        if (!stale.test(page))
            continue;
        GsPageSet one;
        one.set(page);
        if (!m_targets.ownsAny(one))
            stale.reset(page);
    }
    if (stale.none())
        return;
    GsRegion staleRegion = regionForPages(key, stale);
    staleRegion.x0 = std::max(staleRegion.x0, region.x0);
    staleRegion.y0 = std::max(staleRegion.y0, region.y0);
    staleRegion.x1 = std::min(staleRegion.x1, region.x1);
    staleRegion.y1 = std::min(staleRegion.y1, region.y1);
    if (staleRegion.empty() || sampled->intersects(staleRegion))
        return;
    exclude = stale;
    unresolved.merge(staleRegion);
}

bool GsTextureCache::update(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled) {
    SourceScope scope(*this, key);
    const GsRegion region = entry.dirty;
    if (region.empty() || !entry.texture)
        return true;

    bool expanded = false;
    if (!expandFromTarget(key, entry, region, expanded, error))
        return false;
    if (!expanded && !expandNamedWithTargetPalette(key, entry, expanded, error))
        return false;
    if (expanded) {
        entry.identity.texels = 0u;
        entry.blockHash = 0u;
        entry.clutUploadPending = false;
        entry.dirty.clear();
        ++m_stats.partialUpdates;
        m_stats.texelsUpdated += static_cast<uint64_t>(region.width()) * region.height();
        return true;
    }

    // Pages of the target the draw reaches no texel of stay there; their
    // texels are re-expanded from local memory as it stands and stay
    // unresolved.
    GsPageSet exclude;
    restrictToSampled(key, region, sampled, exclude, entry.unresolved);
    if (!resolveSource(key, region, error, &exclude)) {
        return false;
    }

    if (m_auditOn) {
        // The same test as a build's, for the texels this update re-reads.
        GsVram *const saved = m_src;
        m_src = &m_vram;
        const uint64_t real = expand(key, region, true);
        m_src = &m_shadow;
        const uint64_t shadow = expand(key, region, true);
        m_src = saved;
        const bool served = !blocksDrawn(key);
        AuditClass &entryClass = m_audit[std::string("update ") + (served ? "uploads-only " : "needs-targets ") +
                                         (real == shadow ? "SAME" : "DIFFERENT")];
        ++entryClass.count;
        entryClass.texels += uint64_t(region.width()) * region.height();
        if (entryClass.example.empty()) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer, "tbp %05x tbw %u psm %02x %ux%u", key.tbp0, key.tbw, key.psm,
                          key.width, key.height);
            entryClass.example = buffer;
        }
    }
    const bool wholeTexture = region.x0 == 0u && region.y0 == 0u &&
                              region.x1 >= key.width && region.y1 >= key.height;
    // A whole texture dirty again with no palette change waiting is most
    // often back to what it held: another texture passed through its pages
    // and this one was uploaded again (Rogue Galaxy: added; 86% of the
    // re-expands in its streets). Hashing the raw texels first costs their
    // read; when they match, the expansion and the upload are skipped.
    // DQ8_GFX_NO_UNCHANGED_SKIP=1 expands them anyway, as before.
    static const bool skipUnchanged = std::getenv("DQ8_GFX_NO_UNCHANGED_SKIP") == nullptr;
    // The blocks' raw bytes first (Rogue Galaxy: added): the same hash says
    // unchanged without reading the texels out of their swizzle, and a
    // different one goes straight to the expansion, which still finds an
    // unchanged texture by its texel hash below; reading the texels twice,
    // to hash and then to expand, was most of the upload path's reads at
    // the first boss. DQ8_GFX_NO_BLOCK_HASH=1 hashes the texels instead.
    static const bool blockHashOn = std::getenv("DQ8_GFX_NO_BLOCK_HASH") == nullptr;
    const bool mayBeUnchanged = skipUnchanged && wholeTexture && !entry.clutUploadPending &&
                                entry.identity.texels != 0u;
    // Named textures are identified by their images' hashes (expand), and
    // their blocks in memory may not hold their bytes (owed under the
    // bypass): no block hash for them.
    const bool named = namedServes(key);
    const uint64_t blockHash =
        blockHashOn && wholeTexture && !named
            ? gsHashBlocks(m_src->data(), key.tbp0, key.tbw, key.psm, key.width, key.height)
            : 0u;
    if (mayBeUnchanged && blockHash != 0u && entry.blockHash != 0u) {
        if (blockHash == entry.blockHash) {
            entry.dirty.clear();
            ++m_stats.unchangedUpdates;
            ++m_stats.unchangedByBlocks;
            return true;
        }
    } else if (mayBeUnchanged && expand(key, region, true) == entry.identity.texels) {
        entry.dirty.clear();
        entry.blockHash = blockHash;
        ++m_stats.unchangedUpdates;
        return true;
    }
    uint64_t texelHash = 0u;
    bool onGpu = false;
    if (!expandNamedOnGpu(key, entry, region, onGpu, texelHash, error))
        return false;
    if (!onGpu)
        texelHash = expand(key, region);
    // A whole-texture re-expand whose raw texels hash to what we already hold,
    // with no palette change waiting, is the title screen re-uploading its own
    // UI: the GPU texture is already right, so skip the transfer entirely.
    const bool unchanged = wholeTexture && !entry.clutUploadPending &&
                           texelHash == entry.identity.texels;
    if (!unchanged && !onGpu) {
        beforeRewrite(entry.texture);
        if (!uploadRegion(entry.texture, region, error))
            return false;
    }

    // The stored hash described the whole texture before this patch. A whole-
    // texture expand re-derived it; a partial one would leave it wrong, and
    // rehashing would mean reading every texel, so the identity is cleared
    // rather than left wrong -- nothing should match a partially updated
    // texture against a replacement pack.
    if (wholeTexture) {
        entry.identity.texels = texelHash;
        entry.blockHash = blockHash;
        entry.clutUploadPending = false;
    } else {
        entry.identity.texels = 0u;
        entry.blockHash = 0u;
    }
    entry.dirty.clear();
    ++m_stats.partialUpdates;
    m_stats.texelsUpdated += static_cast<uint64_t>(region.width()) * region.height();
    return true;
}

bool GsTextureCache::build(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled) {
    GsPageSet sourcePages;
    markSourcePages(key, sourcePages);
    SourceScope scope(*this, key);
    if (!m_device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }

    // A texture may be sourced from a region a render target still owns --
    // render-to-texture, which DQ8 uses. Resolving costs the extra detail a
    // scaled target holds; sampling the target directly is the fix, and is not
    // done yet.
    const bool owned = m_targets.ownsAny(sourcePages);
    if (owned)
        ++m_stats.renderTargetSources;

    bool expanded = false;
    if (!expandFromTarget(key, entry, {0u, 0u, key.width, key.height}, expanded, error))
        return false;
    if (!expanded && !expandNamedWithTargetPalette(key, entry, expanded, error))
        return false;
    if (expanded && entry.paletteFromTarget && entry.identity.texels != 0u) {
        ++m_stats.builds;
        return true;
    }
    if (expanded) {
        entry.identity = {};
        entry.identity.clut = entry.paletteFromTarget ? 0u : hashClutWindow(key);
        ++m_stats.builds;
        return true;
    }

    // Pages of a target the draw reaches no texel of stay there; their
    // texels are expanded from whatever local memory holds and marked
    // unresolved (restrictToSampled).
    const GsRegion whole{0u, 0u, key.width, key.height};
    GsPageSet exclude;
    if (owned)
        restrictToSampled(key, whole, sampled, exclude, entry.unresolved);
    if (!resolveSource(key, whole, error, &exclude)) {
        return false;
    }

    const uint32_t width = key.width;
    const uint32_t height = key.height;
    m_staging.resize(static_cast<size_t>(width) * height * 4u);

    const bool indexed = gsIsIndexedPsm(key.psm);

    // Identity, computed off the same reads the expansion does. Dimensions and
    // format go into the texel hash because the same bytes read as 64x64 and as
    // 128x128 are different pictures.
    entry.identity = {};
    if (indexed) {
        // Only the palette window this texture can actually reach: CSA lets
        // many 4-bit textures share one CLUT, and folding in the whole thing
        // would make them all move together.
        entry.identity.clut = hashClutWindow(key);
    }

    entry.identity.texels = expand(key, {0u, 0u, width, height});
    entry.blockHash = namedServes(key) ? 0u : gsHashBlocks(m_src->data(), key.tbp0, key.tbw, key.psm, width, height);
    dumpTexture(key, entry.identity, m_staging, m_stats.builds);

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    entry.texture = SDL_CreateGPUTexture(device, &info);
    if (!entry.texture) {
        error = std::string("SDL_CreateGPUTexture(texture cache): ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(m_staging.size());
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    std::memcpy(mapped, m_staging.data(), m_staging.size());
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "texture build");
    if (!commands) {
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        m_device.cancelPending();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion region{};
    region.texture = entry.texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    recycleUploadBuffer(upload);

    ++m_stats.builds;
    m_stats.texelsExpanded += static_cast<uint64_t>(width) * height;
    return true;
}


// --- Textures by name (Rogue Galaxy: added) --------------------------------

namespace {
// The first and last block a set holds (false when it holds none).
bool blockSpan(const GsBlockSet &blocks, uint32_t &first, uint32_t &last) {
    // By 64-bit words (libstdc++'s bitset is its array of them).
    static_assert(sizeof(GsBlockSet) == GsBlockSet().size() / 8u);
    const uint64_t *words = reinterpret_cast<const uint64_t *>(&blocks);
    constexpr size_t count = sizeof(GsBlockSet) / sizeof(uint64_t);
    size_t low = 0u;
    while (low < count && words[low] == 0u)
        ++low;
    if (low == count)
        return false;
    size_t high = count - 1u;
    while (words[high] == 0u)
        --high;
    first = static_cast<uint32_t>(low * 64u + static_cast<size_t>(__builtin_ctzll(words[low])));
    last = static_cast<uint32_t>(high * 64u + 63u - static_cast<size_t>(__builtin_clzll(words[high])));
    return true;
}
} // namespace

void GsTextureCache::dropNamed(const GsBlockSet &blocks, uint32_t keep) {
    if (m_named.empty() && m_graves.empty())
        return;
    // Each image's span first: most writes (draws, uploads elsewhere) miss
    // every image without a look at their 16384-bit sets.
    uint32_t first = 0u, last = 0u;
    if (!blockSpan(blocks, first, last))
        return;
    GsBlockSet covered;
    bool coveredMarked = false;
    // What this write replaces whole is no grave's any more (a draw's
    // blocks are its target's: nothing reads them from memory), and a grave
    // with nothing left is gone.
    if (!m_graves.empty()) {
        const GsBlockSet *replaced = &blocks;
        if (m_dropCoveredBy) {
            const GSTransferCommand &c = *m_dropCoveredBy;
            gsMarkCoveredBlocks(covered, c.bitbltbuf.dbp, std::max<uint32_t>(c.bitbltbuf.dbw, 1u),
                                c.bitbltbuf.dpsm & 0x3fu, c.trxreg.rrw, c.trxreg.rrh, c.trxpos.dsax,
                                c.trxpos.dsay);
            coveredMarked = true;
            replaced = &covered;
        } else if (!m_dropFromDraw) {
            replaced = nullptr; // a copy's or a canvas's partial write: kept as it is
        }
        if (replaced) {
            thread_local std::vector<std::pair<uint32_t, uint64_t>> dead;
            dead.clear();
            eachGrave(first, last, [&](const std::pair<uint32_t, uint64_t> &key, Grave &grave) {
                grave.live &= ~*replaced;
                if (grave.live.none())
                    dead.push_back(key);
            });
            for (const auto &key : dead) {
                m_graves.erase(key);
                m_graveAges.erase(key.second);
                if (m_namedOwed)
                    --m_namedOwed;
            }
        }
    }
    if (m_named.empty())
        return;
    for (auto it = m_named.begin(); it != m_named.end();) {
        const NamedImage &image = it->second;
        if (it->first != keep && image.firstBlock <= last && first <= image.lastBlock &&
            (image.blocks & blocks).any()) {
            if (image.owedToShadow) {
                // Blocks of it the writer does not replace whole keep its bytes.
                if (m_dropCoveredBy && !coveredMarked) {
                    const GSTransferCommand &c = *m_dropCoveredBy;
                    gsMarkCoveredBlocks(covered, c.bitbltbuf.dbp, std::max<uint32_t>(c.bitbltbuf.dbw, 1u),
                                        c.bitbltbuf.dpsm & 0x3fu, c.trxreg.rrw, c.trxreg.rrh, c.trxpos.dsax,
                                        c.trxpos.dsay);
                    coveredMarked = true;
                }
                if (!m_dropCoveredBy || (image.blocks & ~covered).any()) {
                    if (!buryNamed(it->second))
                        writeNamedToShadow(it->second);
                } else
                    --m_namedOwed;
            }
            it = m_named.erase(it);
        }
        else
            ++it;
    }
}

// An owed named image into the host writes' memory, as its upload would
// have written it: the upload as sent, or (pre-swizzled art made linear) the
// same bytes as a T8 image, which lands in the same place.
template <typename Visit> void GsTextureCache::eachGrave(uint32_t first, uint32_t last, Visit visit) {
    const uint32_t from = first > m_graveSpan ? first - m_graveSpan : 0u;
    for (auto it = m_graves.lower_bound({from, 0u}); it != m_graves.end() && it->first.first <= last; ++it)
        if (first <= it->second.image.lastBlock)
            visit(it->first, it->second);
}

// Under the bypass an owed image dropped is buried, not written (its bytes
// are shared: no copy); false when it is written now instead.
bool GsTextureCache::buryNamed(NamedImage &image) {
    static const bool eager = std::getenv("DQ8_GFX_EAGER_WRITE_BACKS") != nullptr;
    if (eager || !gsNativeBypass() || !image.owedToShadow)
        return false;
    const uint64_t age = ++m_graveAge;
    const uint32_t first = image.firstBlock;
    m_graveSpan = std::max(m_graveSpan, image.lastBlock - image.firstBlock);
    Grave grave;
    grave.live = image.blocks;
    grave.image = std::move(image);
    m_graves.emplace(std::make_pair(first, age), std::move(grave));
    m_graveAges.emplace(age, first);
    // A bound on what waits: the oldest has nothing older to wait for.
    constexpr size_t kMaxGraves = 1024u;
    if (m_graves.size() > kMaxGraves) {
        const auto oldest = m_graveAges.begin();
        const auto key = std::make_pair(oldest->second, oldest->first);
        writeGrave(m_graves.at(key));
        m_graves.erase(key);
        m_graveAges.erase(oldest);
    }
    return true;
}

// A grave's bytes into the host writes' memory, its live blocks only: the
// others hold what was written after it.
void GsTextureCache::writeGrave(Grave &grave) {
    const GsBlockSet stale = grave.image.blocks & ~grave.live;
    uint8_t *memory = m_shadow.data();
    thread_local std::vector<std::pair<uint32_t, std::array<uint8_t, 256>>> saved;
    saved.clear();
    if (memory && stale.any()) {
        for (uint32_t block = grave.image.firstBlock; block <= grave.image.lastBlock; ++block)
            if (stale.test(block)) {
                saved.emplace_back();
                saved.back().first = block;
                std::memcpy(saved.back().second.data(), memory + block * 256u, 256u);
            }
    }
    writeShadowBytes(grave.image);
    for (const auto &[block, bytes] : saved)
        std::memcpy(memory + block * 256u, bytes.data(), 256u);
}

// The graves a write of these blocks has to follow, oldest first: those
// whose live blocks it overlaps, and those overlapping any of those.
void GsTextureCache::flushGraves(const GsBlockSet &blocks, uint32_t first, uint32_t last) {
    if (m_graves.empty())
        return;
    GsBlockSet reach = blocks;
    thread_local std::vector<std::pair<uint32_t, uint64_t>> picked;
    picked.clear();
    for (bool grew = true; grew;) {
        grew = false;
        eachGrave(first, last, [&](const std::pair<uint32_t, uint64_t> &key, Grave &grave) {
            if (std::find(picked.begin(), picked.end(), key) != picked.end() || (grave.live & reach).none())
                return;
            picked.push_back(key);
            reach |= grave.image.blocks;
            first = std::min(first, grave.image.firstBlock);
            last = std::max(last, grave.image.lastBlock);
            grew = true;
        });
    }
    std::sort(picked.begin(), picked.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &key : picked) {
        writeGrave(m_graves.at(key));
        m_graves.erase(key);
        m_graveAges.erase(key.second);
    }
}

void GsTextureCache::writeNamedToShadow(NamedImage &image) {
    if (!image.owedToShadow)
        return;
    flushGraves(image.blocks, image.firstBlock, image.lastBlock);
    writeShadowBytes(image);
}

void GsTextureCache::writeShadowBytes(NamedImage &image) {
    if (!image.owedToShadow)
        return;
    image.owedToShadow = false;
    if (m_namedOwed)
        --m_namedOwed;
    if (!m_shadowTransfer)
        return;
    const namedtextures::Upload &u = image.upload;
    GSTransferCommand command{};
    command.direction = 0u;
    command.bitbltbuf.dbp = u.dbp;
    if (u.preSwizzled && !image.swizzled) {
        command.bitbltbuf.dbw = std::max<uint32_t>(u.textureBufferWidth, 1u);
        command.bitbltbuf.dpsm = GS_PSM_T8;
        command.trxreg.rrw = u.textureWidth;
        command.trxreg.rrh = u.textureHeight;
    } else {
        command.bitbltbuf.dbw = std::max<uint32_t>(u.dbw, 1u);
        command.bitbltbuf.dpsm = u.dpsm;
        command.trxreg.rrw = u.width;
        command.trxreg.rrh = u.height;
    }
    m_shadowTransfer->begin(command);
    m_shadowTransfer->upload(image.bytes.data(), static_cast<uint32_t>(image.bytes.size()));
}

// Before a read of the host writes' memory: the owed images it reaches.
void GsTextureCache::settleShadow(const GsBlockSet &blocks) {
    if (m_namedOwed == 0u)
        return;
    uint32_t first = 0u, last = 0u;
    if (!blockSpan(blocks, first, last))
        return;
    flushGraves(blocks, first, last);
    for (auto &[base, image] : m_named) {
        (void)base;
        if (image.owedToShadow && image.firstBlock <= last && first <= image.lastBlock && (image.blocks & blocks).any())
            writeNamedToShadow(image);
    }
}

// Whether the named images give this whole texture (its texels, and its
// palette if it has one): then nothing reads its blocks from memory.
bool GsTextureCache::namedServes(const GsTextureKey &key) {
    if (m_identityRead || !namedOn())
        return false;
    uint64_t hash = 0u;
    return expandNamed(key, {0u, 0u, key.width, key.height}, true, hash);
}

// The canvas at a base and buffer width, made or grown to `height` rows; a
// named image of another kind there is dropped (as an upload over it would).
GsTextureCache::NamedImage *GsTextureCache::canvasFor(uint32_t dbp, uint32_t dbw, uint32_t height) {
    const uint32_t width = dbw * 64u;
    if (width == 0u || height == 0u || height > 2048u)
        return nullptr;
    auto found = m_named.find(dbp);
    if (found != m_named.end() && (!found->second.canvas || std::max<uint32_t>(found->second.upload.dbw, 1u) != dbw)) {
        writeNamedToShadow(found->second); // what it held stays where a read of memory finds it
        m_named.erase(found);
        found = m_named.end();
    }
    if (found == m_named.end()) {
        NamedImage image;
        image.canvas = true;
        namedtextures::Upload &u = image.upload;
        u.dbp = dbp;
        u.dbw = dbw;
        u.dpsm = u.psm = GS_PSM_CT32;
        u.textureBufferWidth = dbw;
        u.width = u.textureWidth = width;
        found = m_named.emplace(dbp, std::move(image)).first;
    }
    NamedImage &canvas = found->second;
    if (canvas.upload.textureHeight < height) {
        canvas.upload.height = canvas.upload.textureHeight = height;
        canvas.bytes.edit().resize(static_cast<size_t>(width) * height * 4u, 0u);
        canvas.blocks.reset();
        gsMarkBlocks(canvas.blocks, dbp, dbw, GS_PSM_CT32, width, height);
        blockSpan(canvas.blocks, canvas.firstBlock, canvas.lastBlock);
        canvas.wordToPixel.clear();
        canvas.contentHash = 0u;
    }
    return &canvas;
}

std::string GsTextureCache::describeNamed(uint32_t base) const {
    const auto found = m_named.find(base);
    if (found == m_named.end()) {
        // An image holding the block, if any.
        for (const auto &[at, image] : m_named)
            if (base < image.blocks.size() && image.blocks.test(base)) {
                char text[96];
                std::snprintf(text, sizeof text, "inside %05x (psm %02x dbw %u %ux%u)", at, image.upload.psm & 0x3fu,
                              image.upload.dbw, image.upload.textureWidth, image.upload.textureHeight);
                return text;
            }
        return "none";
    }
    const namedtextures::Upload &u = found->second.upload;
    char text[96];
    std::snprintf(text, sizeof text, "psm %02x dbw %u %ux%u%s", u.psm & 0x3fu, u.dbw, u.textureWidth, u.textureHeight,
                  u.preSwizzled ? " pre-swizzled" : "");
    return text;
}

bool GsTextureCache::namedWithin(uint32_t first, uint32_t last) const {
    for (const auto &[base, image] : m_named) {
        (void)base;
        if (image.firstBlock <= last && first <= image.lastBlock)
            return true;
    }
    return false;
}

// An upload no record names: what local memory must still hold for the
// native path. DQ8_GFX_WARN_UNNAMED=1 prints each new shape once.
void GsTextureCache::noteUnnamedUpload(const GSTransferCommand &command) {
    const uint32_t dpsm = command.bitbltbuf.dpsm & 0x3fu;
    uint32_t bits = 32u;
    switch (dpsm) {
    case GS_PSM_CT24: bits = 24u; break;
    case GS_PSM_CT16: case GS_PSM_CT16S: bits = 16u; break;
    case GS_PSM_T8: case GS_PSM_T8H: bits = 8u; break;
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: bits = 4u; break;
    default: break;
    }
    ++m_stats.uploadsUnnamed;
    if (gsNativeBypass()) {
        char shape[128];
        std::snprintf(shape, sizeof shape, "unnamed dbp %05x dbw %u dpsm %02x %ux%u%s", command.bitbltbuf.dbp,
                      command.bitbltbuf.dbw, command.bitbltbuf.dpsm & 0x3fu, command.trxreg.rrw, command.trxreg.rrh,
                      (command.trxpos.dsax | command.trxpos.dsay) ? " at an offset" : "");
        memoryuses::note(memoryuses::Kind::UploadToMemory, shape);
    }
    m_stats.unnamedUploadBytes += static_cast<uint64_t>(command.trxreg.rrw) * command.trxreg.rrh * bits / 8u;
    static const bool warn = std::getenv("DQ8_GFX_WARN_UNNAMED") != nullptr;
    if (warn) {
        static std::set<std::string> seen;
        char line[128];
        std::snprintf(line, sizeof line, "dbp %05x dbw %u dpsm %02x %ux%u at %u,%u", command.bitbltbuf.dbp,
                      command.bitbltbuf.dbw, dpsm, command.trxreg.rrw, command.trxreg.rrh, command.trxpos.dsax,
                      command.trxpos.dsay);
        if (seen.size() < 256u && seen.insert(line).second)
            std::fprintf(stderr, "[unnamed-upload] %s\n", line);
    }
}

namespace {
uint32_t namedBits(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: return 32u;
    case GS_PSM_CT24: return 24u;
    case GS_PSM_CT16: case GS_PSM_CT16S: return 16u;
    case GS_PSM_T8: case GS_PSM_T8H: return 8u;
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: return 4u;
    default: return 0u;
    }
}
} // namespace

void GsTextureCache::beginNamedUpload(const GSTransferCommand &command) {
    if (m_namedOpen)
        finishNamedUpload(); // a short one: dropped there
    m_namedSkipShadow = false;
    if (command.direction == 1u)
        return; // a read writes nothing
    const uint32_t dbw = std::max<uint32_t>(command.bitbltbuf.dbw, 1u);
    const uint32_t dpsm = command.bitbltbuf.dpsm & 0x3fu;
    const uint32_t x = command.trxpos.dsax, y = command.trxpos.dsay;
    const uint32_t w = command.trxreg.rrw, h = command.trxreg.rrh;
    GsBlockSet written;
    if (!m_named.empty())
        gsMarkBlocks(written, command.bitbltbuf.dbp, dbw, dpsm, x + w, y + h);
    // The drops below: an owed image this upload replaces whole needs no
    // write of its own (a local copy's or a read's drops always write it).
    // Its covered blocks are marked only if an owed image is in the way.
    struct CoveredScope {
        const GSTransferCommand *&slot;
        ~CoveredScope() { slot = nullptr; }
    } coveredScope{m_dropCoveredBy};
    if (command.direction == 0u)
        m_dropCoveredBy = &command;
    static const bool off = std::getenv("DQ8_GFX_NO_NAMED_TEXTURES") != nullptr;
    const uint32_t bits = namedBits(dpsm);
    if (command.direction != 0u || off || bits == 0u || w == 0u || h == 0u || !namedOn()) {
        dropNamed(written);
        if (command.direction == 0u)
            noteUnnamedUpload(command);
        return;
    }

    // Under the bypass, uploads GS memory would merge: CT32 rectangles at
    // an offset with no image of their own to patch, and the alpha formats,
    // which share CT32 words. They go into a canvas at their base.
    const bool alphaFormat = dpsm == GS_PSM_T8H || dpsm == GS_PSM_T4HL || dpsm == GS_PSM_T4HH;
    if (gsNativeBypass() && (alphaFormat || (dpsm == GS_PSM_CT32 && (x != 0u || y != 0u))) && x + w <= dbw * 64u &&
        (!alphaFormat || bits != 4u || (x % 2u == 0u && w % 2u == 0u))) {
        const auto found = m_named.find(command.bitbltbuf.dbp);
        const bool ownImage = found != m_named.end() && !found->second.canvas && !alphaFormat &&
                              !found->second.upload.preSwizzled && (found->second.upload.psm & 0x3fu) == dpsm &&
                              (found->second.upload.dpsm & 0x3fu) == dpsm &&
                              std::max<uint32_t>(found->second.upload.dbw, 1u) == dbw &&
                              x + w <= found->second.upload.textureWidth && y + h <= found->second.upload.textureHeight;
        if (!ownImage) {
            NamedImage *canvas = canvasFor(command.bitbltbuf.dbp, dbw, y + h);
            if (canvas) {
                dropNamed(written, command.bitbltbuf.dbp);
                m_namedPending = {};
                m_namedPending.upload = canvas->upload;
                m_namedPatch = {x, y, w, h};
                m_namedPatchFormat = dpsm;
                m_namedPatching = true;
                m_namedExpected = (static_cast<size_t>(w) * h * bits + 7u) / 8u;
                m_namedPending.bytes.edit().reserve(m_namedExpected);
                m_namedOpen = true;
                ++m_stats.uploadsPatched;
                return;
            }
        }
    }

    // Into part of a named image in its own format (glyphs, a region
    // redrawn on the CPU): the image is patched, not dropped.
    if (x != 0u || y != 0u) {
        const auto found = m_named.find(command.bitbltbuf.dbp);
        const bool patchable = found != m_named.end() && !found->second.upload.preSwizzled &&
                               (found->second.upload.dpsm & 0x3fu) == dpsm &&
                               (found->second.upload.psm & 0x3fu) == dpsm &&
                               std::max<uint32_t>(found->second.upload.dbw, 1u) == dbw &&
                               x + w <= found->second.upload.textureWidth &&
                               y + h <= found->second.upload.textureHeight && (bits != 4u || (x % 2u == 0u && w % 2u == 0u));
        if (!patchable) {
            dropNamed(written);
            noteUnnamedUpload(command);
            return;
        }
        dropNamed(written, command.bitbltbuf.dbp);
        m_namedPending = {};
        m_namedPending.upload = found->second.upload;
        m_namedPatch = {x, y, w, h};
        m_namedPatchFormat = dpsm;
        m_namedPatching = true;
        m_namedExpected = (static_cast<size_t>(w) * h * bits + 7u) / 8u;
        m_namedPending.bytes.edit().reserve(m_namedExpected);
        m_namedOpen = true;
        ++m_stats.uploadsPatched;
        return;
    }

    dropNamed(written);
    namedtextures::Upload upload;
    if (namedtextures::take(command.bitbltbuf.dbp, dbw, dpsm, w, h, upload)) {
        ++m_stats.uploadsNamed;
    } else {
        // No record: the transfer names itself, an image of its own format
        // and size, which serves the textures that read it the same way.
        upload = {};
        upload.dbp = command.bitbltbuf.dbp;
        upload.dbw = dbw;
        upload.dpsm = dpsm;
        upload.width = w;
        upload.height = h;
        upload.psm = dpsm;
        upload.textureBufferWidth = dbw;
        upload.textureWidth = w;
        upload.textureHeight = h;
        ++m_stats.uploadsSelfNamed;
    }
    m_namedPending = {};
    m_namedPending.upload = upload;
    m_namedPatching = false;
    m_namedExpected = (static_cast<size_t>(upload.width) * upload.height * namedBits(upload.dpsm) + 7u) / 8u;
    if (m_namedExpected == 0u)
        return;
    m_namedPending.bytes.edit().reserve(m_namedExpected);
    gsMarkBlocks(m_namedPending.blocks, upload.dbp, dbw, dpsm, upload.width, upload.height);
    m_namedOpen = true;
    // Under the bypass the bytes go to the named image alone; m_shadow gets
    // them when a read reaches them (settleShadow) or the image is dropped.
    if (m_tracking && !m_auditOn && gsNativeBypass()) {
        m_namedSkipShadow = true;
        m_namedSkipCommand = command;
    }
}

namespace {
// 8-bit art sent as CT32 at half the size: its bytes are the T8 page
// layout's, read as CT32 words. Per T8 texel, its byte in the upload,
// through the two layouts; null when the shape is not that layout. The game
// sends the same few shapes every frame, so each shape's map is made once.
const std::vector<uint32_t> *unswizzleMap(const namedtextures::Upload &u) {
    const uint32_t w32 = u.width, h32 = u.height, w8 = u.textureWidth, h8 = u.textureHeight;
    const uint32_t tbw32 = std::max<uint32_t>(u.dbw, 1u), tbw8 = std::max<uint32_t>(u.textureBufferWidth, 1u);
    const std::array<uint32_t, 6> shape = {w32, h32, tbw32, w8, h8, tbw8};
    static thread_local std::map<std::array<uint32_t, 6>, std::vector<uint32_t>> maps;
    auto known = maps.find(shape);
    if (known == maps.end()) {
        if (maps.size() >= 256u)
            maps.clear();
        // Word address to upload pixel, as a flat table over the
        // upload's own words; then each T8 texel's byte through it. An
        // empty map: not this layout after all.
        uint32_t maxWord = 0u;
        for (uint32_t y = 0u; y < h32; ++y)
            for (uint32_t x = 0u; x < w32; ++x)
                maxWord = std::max(maxWord, GSPSMCT32::addrPSMCT32(0u, tbw32, x, y) >> 2u);
        std::vector<uint32_t> pixelOfWord(static_cast<size_t>(maxWord) + 1u, ~0u);
        for (uint32_t y = 0u; y < h32; ++y)
            for (uint32_t x = 0u; x < w32; ++x)
                pixelOfWord[GSPSMCT32::addrPSMCT32(0u, tbw32, x, y) >> 2u] = y * w32 + x;
        std::vector<uint32_t> byteOf(static_cast<size_t>(w8) * h8);
        for (uint32_t y = 0u; y < h8 && !byteOf.empty(); ++y) {
            for (uint32_t x = 0u; x < w8; ++x) {
                const uint32_t address = GSPSMT8::addrPSMT8(0u, tbw8, x, y);
                const uint32_t word = address >> 2u;
                if (word >= pixelOfWord.size() || pixelOfWord[word] == ~0u) {
                    byteOf.clear();
                    break;
                }
                byteOf[static_cast<size_t>(y) * w8 + x] = pixelOfWord[word] * 4u + (address & 3u);
            }
        }
        known = maps.emplace(shape, std::move(byteOf)).first;
    }
    return known->second.empty() ? nullptr : &known->second;
}
} // namespace

void GsTextureCache::finishNamedUpload() {
    m_namedOpen = false;
    NamedImage image = std::move(m_namedPending);
    m_namedPending = {};
    if (m_namedPatching) {
        m_namedPatching = false;
        const auto found = m_named.find(image.upload.dbp);
        if (found == m_named.end())
            return;
        if (image.bytes.size() < m_namedExpected) {
            GsBlockSet blocks = found->second.blocks;
            dropNamed(blocks);
            return;
        }
        NamedImage &target = found->second;
        const uint32_t patchFormat = m_namedPatchFormat & 0x3fu;
        if (target.canvas && (patchFormat == GS_PSM_T8H || patchFormat == GS_PSM_T4HL || patchFormat == GS_PSM_T4HH)) {
            // Into the top byte of the canvas's words, as the GS keeps them.
            const uint32_t stride = target.upload.textureWidth;
            for (uint32_t row = 0u; row < m_namedPatch[3]; ++row)
                for (uint32_t col = 0u; col < m_namedPatch[2]; ++col) {
                    const size_t i = static_cast<size_t>(row) * m_namedPatch[2] + col;
                    const size_t at = ((static_cast<size_t>(m_namedPatch[1]) + row) * stride + m_namedPatch[0] + col) * 4u + 3u;
                    if (at >= target.bytes.size())
                        continue;
                    uint8_t &top = target.bytes.edit()[at];
                    if (patchFormat == GS_PSM_T8H)
                        top = image.bytes[i];
                    else {
                        const uint8_t v = (image.bytes[i >> 1u] >> ((i & 1u) * 4u)) & 0x0fu;
                        top = patchFormat == GS_PSM_T4HL ? uint8_t((top & 0xf0u) | v) : uint8_t((top & 0x0fu) | (v << 4u));
                    }
                }
            target.contentHash = 0u;
            return;
        }
        // Rows of the patch into the linear image, in its own format.
        const uint32_t bits = namedBits(target.upload.psm);
        const size_t rowBytes = static_cast<size_t>(m_namedPatch[2]) * bits / 8u;
        const size_t stride = static_cast<size_t>(target.upload.textureWidth) * bits / 8u;
        for (uint32_t row = 0u; row < m_namedPatch[3]; ++row) {
            const size_t to = (static_cast<size_t>(m_namedPatch[1]) + row) * stride +
                              static_cast<size_t>(m_namedPatch[0]) * bits / 8u;
            if (to + rowBytes > target.bytes.size())
                break;
            std::memcpy(target.bytes.edit().data() + to, image.bytes.data() + row * rowBytes, rowBytes);
        }
        target.contentHash = 0u;
        return;
    }
    // Not kept as a named image: the bytes the shadow skipped go there now.
    const auto unkept = [&] {
        if (m_namedSkipShadow && m_shadowTransfer) {
            m_shadowTransfer->begin(m_namedSkipCommand);
            m_shadowTransfer->upload(image.bytes.data(), static_cast<uint32_t>(image.bytes.size()));
        }
        m_namedSkipShadow = false;
    };
    if (image.bytes.size() < m_namedExpected)
        return unkept();
    const namedtextures::Upload &u = image.upload;
    if (u.preSwizzled) {
        // Kept as uploaded; made linear when a texture first reads it
        // (linearize): most uploads are sent again before any draw reads them.
        if (!unswizzleMap(u))
            return unkept(); // not this layout after all: leave it unnamed
        image.swizzled = true;
    }
    if (!blockSpan(image.blocks, image.firstBlock, image.lastBlock))
        return unkept();
    storeNamed(std::move(image));
}

void GsTextureCache::storeNamed(NamedImage &&image) {
    const uint32_t dbp = image.upload.dbp;
    if (m_namedSkipShadow) {
        image.owedToShadow = true;
        ++m_namedOwed;
    }
    const auto previous = m_named.find(dbp);
    if (previous != m_named.end() && previous->second.owedToShadow) {
        // Replaced in place: its blocks not this image's keep its bytes.
        if ((previous->second.blocks & ~image.blocks).any()) {
            if (!buryNamed(previous->second))
                writeNamedToShadow(previous->second);
        } else
            --m_namedOwed;
    }
    m_named[dbp] = std::move(image);
}

namespace {
// How a kept image was named, with its bytes' hash: the same bytes named
// otherwise (another node, format or shape) are another image.
uint64_t keptKey(const namedtextures::Upload &u, uint64_t hash) {
    uint64_t key = hashStep(hashStep(hash, u.node), u.dbw | (u.dpsm << 8u) | (u.psm << 16u) |
                                                         (u.textureBufferWidth << 24u) |
                                                         (u.preSwizzled ? 1ull << 40u : 0u) |
                                                         (u.palette ? 1ull << 41u : 0u));
    key = hashStep(key, u.width | (static_cast<uint64_t>(u.height) << 16u) |
                            (static_cast<uint64_t>(u.textureWidth) << 32u) |
                            (static_cast<uint64_t>(u.textureHeight) << 48u));
    return key;
}
} // namespace

bool GsTextureCache::reuploadUnchanged(const GSTransferCommand &command, const namedtextures::Held &held) {
    static const bool off = std::getenv("DQ8_GFX_NO_KEPT_IMAGES") != nullptr;
    if (off || command.direction != 0u || command.trxpos.dsax != 0u || command.trxpos.dsay != 0u || !namedOn())
        return false;
    const auto found = m_named.find(command.bitbltbuf.dbp);
    if (found == m_named.end())
        return false;
    const NamedImage &image = found->second;
    const namedtextures::Upload &was = image.upload;
    const uint32_t dbw = std::max<uint32_t>(command.bitbltbuf.dbw, 1u);
    const uint32_t dpsm = command.bitbltbuf.dpsm & 0x3fu;
    const uint32_t w = command.trxreg.rrw, h = command.trxreg.rrh;
    if (image.canvas || image.contentHash != held.hash || std::max<uint32_t>(was.dbw, 1u) != dbw ||
        (was.dpsm & 0x3fu) != dpsm || was.width != w || was.height != h)
        return false;
    namedtextures::Upload record;
    if (!namedtextures::peek(command.bitbltbuf.dbp, dbw, dpsm, w, h, &record) || record.node != was.node ||
        record.psm != was.psm || record.preSwizzled != was.preSwizzled || record.palette != was.palette ||
        record.textureWidth != was.textureWidth || record.textureHeight != was.textureHeight ||
        record.textureBufferWidth != was.textureBufferWidth)
        return false;
    namedtextures::take(command.bitbltbuf.dbp, dbw, dpsm, w, h, record);
    ++m_stats.uploadsNamed;
    ++m_stats.uploadsUnchanged;
    return true;
}

bool GsTextureCache::noteHeldUpload(namedtextures::Held &held) {
    // Only a whole named upload whose bytes the host writes' memory does not
    // want now (the bypass owes them); anything else takes its payload.
    if (!m_namedOpen || m_namedPatching || !held.bytes || held.bytes->size() != m_namedExpected ||
        (m_tracking && !m_namedSkipShadow))
        return false;
    static const bool keepOff = std::getenv("DQ8_GFX_NO_KEPT_IMAGES") != nullptr;
    const uint64_t key = keptKey(m_namedPending.upload, held.hash);
    if (!keepOff) {
        if (auto kept = m_kept.find(key); kept != m_kept.end()) {
            NamedImage &source = kept->second;
            linearize(source); // once, for every later upload of it
            NamedImage image;
            image.upload = m_namedPending.upload;
            image.blocks = m_namedPending.blocks;
            image.bytes = source.bytes;
            image.swizzled = source.swizzled;
            image.contentHash = source.contentHash;
            m_namedOpen = false;
            m_namedPending = {};
            if (!blockSpan(image.blocks, image.firstBlock, image.lastBlock))
                return true; // no blocks: nothing to name, and nothing owed
            storeNamed(std::move(image));
            ++m_stats.uploadsKept;
            return true;
        }
    }
    m_namedPending.bytes.adopt(std::move(held.bytes));
    m_namedPending.contentHash = held.hash;
    const uint32_t dbp = m_namedPending.upload.dbp;
    finishNamedUpload();
    ++m_stats.uploadsAdopted;
    if (keepOff)
        return true;
    const auto stored = m_named.find(dbp);
    if (stored == m_named.end() || stored->second.contentHash != held.hash)
        return true; // not kept as a named image (finishNamedUpload's reasons)
    // A budget well over the walk's few hundred images (~4 MB); past it the
    // kept ones start again.
    constexpr size_t kKeptBudget = 64u * 1024u * 1024u;
    if (m_keptBytes + stored->second.bytes.size() > kKeptBudget) {
        m_kept.clear();
        m_keptBytes = 0u;
    }
    NamedImage &kept = m_kept[key];
    kept.upload = stored->second.upload;
    kept.bytes = stored->second.bytes;
    kept.swizzled = stored->second.swizzled;
    kept.contentHash = stored->second.contentHash;
    m_keptBytes += kept.bytes.size();
    return true;
}


uint64_t GsTextureCache::contentHashOf(const NamedImage &image) {
    if (image.contentHash == 0u)
        image.contentHash = namedtextures::imageHash(image.bytes.data(), image.bytes.size());
    return image.contentHash;
}

void GsTextureCache::linearize(NamedImage &image) {
    if (!image.swizzled)
        return;
    image.swizzled = false;
    const std::vector<uint32_t> *map = unswizzleMap(image.upload);
    if (!map)
        return;
    std::vector<uint8_t> linear(map->size());
    const uint8_t *const from = image.bytes.data();
    const uint32_t *const index = map->data();
    uint8_t *const to = linear.data();
    const size_t count = map->size();
    for (size_t i = 0; i < count; ++i)
        to[i] = from[index[i]];
    image.bytes.assign(std::move(linear)); // the same picture: contentHash stands
}

bool GsTextureCache::expandNamed(const GsTextureKey &key, const GsRegion &region, bool hashOnly, uint64_t &hash) {
    const auto miss = [&](uint64_t &counter) {
        if (!hashOnly)
            ++counter;
        m_namedMissWhy = &counter == &m_stats.namedMissNoName ? "no name"
                         : &counter == &m_stats.namedMissShape ? "shape"
                                                                : "palette";
        return false;
    };
    m_namedLastImage = nullptr;
    m_namedLastDirect = false;
    const auto found = m_named.find(key.tbp0);
    if (found == m_named.end())
        return miss(m_stats.namedMissNoName);
    NamedImage &image = found->second;
    const namedtextures::Upload &u = image.upload;
    const uint32_t psm = key.psm & 0x3fu;
    // A CT32 image read as 8 or 4 bits, through the GS's layouts: the
    // texel's byte address, then the CT32 word holding it in the image.
    const bool indexedRead = psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H || psm == GS_PSM_T4HL ||
                             psm == GS_PSM_T4HH;
    const uint32_t keyTbw = std::max<uint32_t>(key.tbw, 1u);
    const uint32_t imageTbw = std::max<uint32_t>(u.dbw, 1u);
    // Under the bypass any buffer width: the layouts take the texel to its
    // word wherever it is (texels past the image read as 0).
    const bool reinterpret = !u.palette && !u.preSwizzled && (u.psm & 0x3fu) == GS_PSM_CT32 && indexedRead &&
                             (gsNativeBypass() ||
                              ((psm == GS_PSM_T8 || psm == GS_PSM_T4) ? std::max<uint32_t>(keyTbw >> 1u, 1u) == imageTbw
                                                                      : keyTbw == imageTbw));
    if (!reinterpret &&
        (u.palette || (u.psm & 0x3fu) != psm || keyTbw != std::max<uint32_t>(u.textureBufferWidth, 1u)))
        return miss(m_stats.namedMissShape);
    const uint32_t w = reinterpret ? key.width : u.textureWidth, h = reinterpret ? key.height : u.textureHeight;
    const auto pow2 = [](uint32_t v) { uint32_t p = 1u; while (p < v) p <<= 1u; return p; };
    if (!reinterpret && (key.width > pow2(w) || key.height > pow2(h)))
        return miss(m_stats.namedMissShape);
    if (reinterpret && image.wordToPixel.empty()) {
        const uint32_t rows = (u.height + 31u) / 32u;
        image.wordToPixel.assign(static_cast<size_t>(rows) * imageTbw * 2048u, 0u);
        for (uint32_t py = 0u; py < u.height; ++py)
            for (uint32_t px = 0u; px < u.width; ++px) {
                const uint32_t word = GSPSMCT32::addrPSMCT32(0u, imageTbw, px, py) >> 2u;
                if (word < image.wordToPixel.size())
                    image.wordToPixel[word] = py * u.width + px + 1u;
            }
    }
    // The byte at a byte address of the image, or 0 past it.
    const auto byteAt = [&](uint32_t address) -> uint32_t {
        const uint32_t word = address >> 2u;
        if (word >= image.wordToPixel.size() || image.wordToPixel[word] == 0u)
            return 0u;
        return image.bytes[static_cast<size_t>(image.wordToPixel[word] - 1u) * 4u + (address & 3u)];
    };

    const bool indexed = gsIsIndexedPsm(key.psm);
    const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    const TexaState texa = unpackTexa(key.texa);
    std::array<uint32_t, 256> palette{};
    const NamedImage *clut = nullptr;
    if (m_wantIndices) {
        if (!indexed)
            return false;
        for (uint32_t i = 0u; i < 256u; ++i)
            palette[i] = i | 0xff000000u; // the index itself, in R
    } else if (indexed) {
        if (key.csm != 0u)
            return miss(m_stats.namedMissPalette);
        const auto clutFound = m_named.find(key.cbp);
        if (clutFound == m_named.end() || clutFound->second.upload.preSwizzled ||
            (clutFound->second.upload.dpsm & 0x3fu) != (key.cpsm & 0x3fu))
            return miss(m_stats.namedMissPalette);
        clut = &clutFound->second;
        const uint32_t cpsm = key.cpsm & 0x3fu;
        const bool sixteen = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        if (!sixteen && cpsm != GS_PSM_CT32)
            return miss(m_stats.namedMissPalette);
        const uint32_t cw = clut->upload.width, ch = clut->upload.height;
        // How far the indices reach into the palette depends on the formats
        // and CSA alone: worked out once, so the identity check (hashOnly)
        // needs no palette (resolving 256 indices for it every call was
        // 0.7% of the samples).
        static thread_local std::unordered_map<uint32_t, uint32_t> reach;
        const uint32_t reachKey = cpsm | (key.csm << 6u) | ((key.csa & 0x1fu) << 7u) | (psm << 12u);
        auto known = reach.find(reachKey);
        if (known == reach.end()) {
            uint32_t maxX = 0u, maxY = 0u;
            for (uint32_t raw = 0u; raw < (fourBit ? 16u : 256u); ++raw) {
                const uint32_t index = resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
                maxX = std::max(maxX, index & 0x0fu);
                maxY = std::max(maxY, index >> 4u);
            }
            known = reach.emplace(reachKey, maxX | (maxY << 16u)).first;
        }
        if ((known->second & 0xffffu) >= cw || (known->second >> 16u) >= ch)
            return miss(m_stats.namedMissPalette);
        for (uint32_t raw = 0u; raw < (fourBit ? 16u : 256u) && (!hashOnly || m_namedWantPalette); ++raw) {
            const uint32_t index = resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
            const uint32_t cx = index & 0x0fu, cy = index >> 4u;
            if (cx >= cw || cy >= ch)
                return miss(m_stats.namedMissPalette);
            const size_t at = static_cast<size_t>(cy) * cw + cx;
            uint32_t value = 0u;
            if (sixteen) {
                value = static_cast<uint32_t>(clut->bytes[at * 2u]) | (static_cast<uint32_t>(clut->bytes[at * 2u + 1u]) << 8u);
                palette[raw] = applyTexa(texa, key.cpsm, rgba5551To8888(value));
            } else {
                std::memcpy(&value, clut->bytes.data() + at * 4u, 4u);
                palette[raw] = applyTexa(texa, key.cpsm, value);
            }
        }
    }

    m_namedLastImage = &image;
    m_namedLastDirect = !reinterpret && !m_wantIndices && (psm == GS_PSM_T8 || psm == GS_PSM_T4);
    m_namedLastWidth = w;
    m_namedLastHeight = h;
    m_namedLastPalette = palette;
    // The identity: the image's and the palette's bytes (each hashed once
    // per upload), how they are read, and the region. The game uploads its
    // textures again every frame and each one in use is checked; a pass
    // over every texel to hash it was the named path's main cost.
    uint64_t identity = hashStep(hashStep(hashStep(kHashSeed ^ 0x6e616d6564ull, key.width), key.height), key.psm);
    identity = hashStep(hashStep(identity, u.node), contentHashOf(image));
    identity = hashStep(hashStep(identity, key.tbw), key.texa);
    identity = hashStep(identity, key.cpsm | (key.csm << 8u) | (key.csa << 16u) | (m_wantIndices ? 1u << 24u : 0u));
    identity = hashStep(hashStep(identity, region.x0 | (region.y0 << 16u)), region.x1 | (region.y1 << 16u));
    if (clut)
        identity = hashStep(hashStep(identity, clut->upload.node), contentHashOf(*clut));
    if (hashOnly) {
        hash = identity;
        return true;
    }
    linearize(image); // after the identity: its hash is of the bytes as uploaded
    const uint32_t regionWidth = region.width();
    m_staging.assign(static_cast<size_t>(regionWidth) * region.height() * 4u, 0u);
    for (uint32_t y = region.y0; y < region.y1; ++y) {
        uint8_t *row = m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            uint32_t color = 0u;
            uint32_t raw = 0u;
            if (reinterpret) {
                switch (psm) {
                case GS_PSM_T8: raw = byteAt(GSPSMT8::addrPSMT8(0u, keyTbw, x, y)); break;
                case GS_PSM_T4: {
                    const uint32_t nibble = GSPSMT4::addrPSMT4(0u, keyTbw, x, y);
                    raw = (byteAt(nibble >> 1u) >> ((nibble & 1u) * 4u)) & 0x0fu;
                    break;
                }
                default: {
                    const uint32_t alpha = byteAt((GSPSMCT32::addrPSMCT32(0u, keyTbw, x, y) & ~3u) + 3u);
                    raw = psm == GS_PSM_T8H ? alpha : (psm == GS_PSM_T4HL ? (alpha & 0x0fu) : (alpha >> 4u));
                    break;
                }
                }
                color = palette[raw];
            } else if (x < w && y < h) {
                const size_t i = static_cast<size_t>(y) * w + x;
                switch (psm) {
                case GS_PSM_CT32:
                    std::memcpy(&raw, image.bytes.data() + i * 4u, 4u);
                    color = raw;
                    break;
                case GS_PSM_CT24:
                    raw = image.bytes[i * 3u] | (image.bytes[i * 3u + 1u] << 8u) | (image.bytes[i * 3u + 2u] << 16u);
                    color = applyTexa(texa, key.psm, raw);
                    break;
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                    raw = image.bytes[i * 2u] | (image.bytes[i * 2u + 1u] << 8u);
                    color = applyTexa(texa, key.psm, rgba5551To8888(raw));
                    break;
                case GS_PSM_T8:
                case GS_PSM_T8H:
                    raw = image.bytes[i];
                    color = palette[raw];
                    break;
                case GS_PSM_T4:
                case GS_PSM_T4HL:
                case GS_PSM_T4HH:
                    raw = (image.bytes[i >> 1u] >> ((i & 1u) * 4u)) & 0x0fu;
                    color = palette[raw];
                    break;
                default:
                    return false;
                }
            }
            std::memcpy(row + (x - region.x0) * 4u, &color, 4u);
        }
    }
    ++m_stats.namedExpands;
    hash = identity;
    return true;
}


bool GsTextureCache::expandNamedOnGpu(const GsTextureKey &key, Entry &entry, const GsRegion &region, bool &handled,
                                      uint64_t &hash, std::string &error) {
    handled = false;
    static const bool on = std::getenv("DQ8_GFX_NO_NAMED_GPU") == nullptr;
    if (!on || m_identityRead || !namedOn() || region.empty() || !entry.texture)
        return true;
    const uint32_t psm = key.psm & 0x3fu;
    if (psm != GS_PSM_T8 && psm != GS_PSM_T4)
        return true;
    // Whole textures, or parts of one already drawn into (a new texture
    // would lose the texels outside the part).
    const bool whole = region.x0 == 0u && region.y0 == 0u && region.x1 == key.width && region.y1 == key.height;
    if (!whole && !entry.renderable)
        return true;
    m_namedWantPalette = true;
    const bool named = expandNamed(key, region, true, hash);
    m_namedWantPalette = false;
    if (!named || !m_namedLastDirect || !m_namedLastImage)
        return true;
    // Texels past the image read as 0, not palette[0]: those stay on the CPU.
    if (region.x1 > m_namedLastWidth || region.y1 > m_namedLastHeight)
        return true;
    linearize(*m_namedLastImage); // the identity read it as uploaded
    const NamedImage &image = *m_namedLastImage;
    const bool fourBit = psm == GS_PSM_T4;
    const uint32_t w = m_namedLastWidth;

    // The index bytes of the region's rows (T4: from an even texel, two a byte).
    const uint32_t x0 = fourBit ? region.x0 & ~1u : region.x0;
    const uint32_t rowBytes = fourBit ? (region.x1 - x0 + 1u) / 2u : region.x1 - x0;
    const size_t stride = fourBit ? (w + 1u) / 2u : w;
    m_indexStaging.resize(static_cast<size_t>(rowBytes) * region.height());
    for (uint32_t y = region.y0; y < region.y1; ++y) {
        const size_t from = static_cast<size_t>(y) * stride + (fourBit ? x0 / 2u : x0);
        if (from + rowBytes > image.bytes.size())
            return true;
        std::memcpy(m_indexStaging.data() + static_cast<size_t>(y - region.y0) * rowBytes, image.bytes.data() + from,
                    rowBytes);
    }

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    info.width = rowBytes;
    info.height = region.height();
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    // One texture a size, kept: uploaded cycled, so a use still in flight
    // keeps its contents (creating one per update was 1.6% of the samples).
    SDL_GPUTexture *&indices = m_indexTextures[rowBytes | (region.height() << 16u)];
    if (!indices) {
        indices = SDL_CreateGPUTexture(device, &info);
        if (!indices) {
            error = std::string("SDL_CreateGPUTexture(named indices): ") + SDL_GetError();
            return false;
        }
    }
    m_uploadCycle = true;
    const bool uploaded = uploadBytes(indices, {0u, 0u, rowBytes, region.height()}, m_indexStaging.data(), 1u, error);
    m_uploadCycle = false;
    if (!uploaded)
        return false;
    if (!entry.renderable) {
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = key.width;
        info.height = key.height;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
        if (!texture) {
            error = std::string("SDL_CreateGPUTexture(named on GPU): ") + SDL_GetError();
            return false;
        }
        beforeRewrite(entry.texture);
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = texture;
        entry.renderable = true;
    } else {
        beforeRewrite(entry.texture);
    }
    // Mode 5 (T8) or 6 (T4); pages.x: the index texture's origin in texels.
    const std::array<uint32_t, 4> pages{x0 | (region.y0 << 16u), 1u | ((fourBit ? 6u : 5u) << 16u), 0u,
                                        1u | (1u << 16u)};
    const SDL_Rect rect{int(region.x0), int(region.y0), int(region.width()), int(region.height())};
    const bool done = m_device.expandIndexed8(indices, m_namedLastPalette.data(), entry.texture, key.width,
                                              key.height, rect, pages, error);
    if (!done)
        return false;
    entry.paletteFromTarget = false;
    ++m_stats.namedOnGpu;
    handled = true;
    return true;
}

bool GsTextureCache::expandNamedWithTargetPalette(const GsTextureKey &key, Entry &entry, bool &handled,
                                                  std::string &error) {
    handled = false;
    static const bool on = std::getenv("DQ8_GFX_NO_TARGET_PALETTES") == nullptr;
    if (!on || !gsIsIndexedPsm(key.psm) || !namedOn() || (key.cpsm & 0x3fu) != GS_PSM_CT32 ||
        key.csm != 0u || !m_storeOn || !clutBlocksDrawn(key))
        return true;
    GsPageSet clutPages;
    markClutReads(key, clutPages);
    GsSurface *paletteTarget = clutPages.any() ? m_targets.nativeOwner(clutPages, true) : nullptr;
    if (!paletteTarget || (clutPages & ~paletteTarget->pages).any())
        return true;
    // The indices from the named texels, into m_staging (index in R).
    const GsRegion whole{0u, 0u, key.width, key.height};
    uint64_t hash = 0u;
    m_wantIndices = true;
    const bool named = expandNamed(key, whole, false, hash);
    m_wantIndices = false;
    if (!named)
        return true;
    if (!m_targets.refreshRegion(*paletteTarget, m_targets.regionForPages(*paletteTarget, clutPages), error))
        return false;

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = key.width;
    info.height = key.height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *indices = SDL_CreateGPUTexture(device, &info);
    if (!indices) {
        error = std::string("SDL_CreateGPUTexture(indices): ") + SDL_GetError();
        return false;
    }
    if (!uploadRegion(indices, whole, error)) {
        SDL_ReleaseGPUTexture(device, indices);
        return false;
    }
    if (!entry.renderable) {
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
        if (!texture) {
            SDL_ReleaseGPUTexture(device, indices);
            error = std::string("SDL_CreateGPUTexture(target palette): ") + SDL_GetError();
            return false;
        }
        if (entry.texture) {
            beforeRewrite(entry.texture);
            SDL_ReleaseGPUTexture(device, entry.texture);
        }
        entry.texture = texture;
        entry.renderable = true;
    } else {
        beforeRewrite(entry.texture);
    }
    std::array<uint32_t, 256> palette{};
    targetPaletteCoordinates(key, *paletteTarget, palette);
    const std::array<uint32_t, 4> pages{0u,
                                        1u | (5u << 16u) | (1u << 24u) |
                                            (std::min(std::max(paletteTarget->scale, 1u), 15u) << 25u),
                                        0u, 1u | (1u << 16u)};
    const SDL_Rect rect{0, 0, int(key.width), int(key.height)};
    const bool done = m_device.expandIndexed8(indices, palette.data(), entry.texture, key.width, key.height, rect,
                                              pages, error, paletteTarget->texture);
    SDL_ReleaseGPUTexture(device, indices); // released once the expansion has run
    if (!done)
        return false;
    entry.paletteFromTarget = true;
    entry.identity = {};
    entry.identity.texels = hash;
    ++m_stats.namedWithTargetPalettes;
    handled = true;
    return true;
}


void GsTextureCache::noteTransferStart(const GSTransferCommand &command) {
    m_namedSkipShadow = false; // set again by beginNamedUpload for the upload it names
    m_namedCopyKeep = ~0u;
    if (command.direction != 2u || m_named.empty())
        return;
    static const bool off = std::getenv("DQ8_GFX_NO_NAMED_COPIES") != nullptr;
    const GSBitBltBuf &b = command.bitbltbuf;
    const auto from = m_named.find(b.sbp);
    const auto to = m_named.find(b.dbp);
    const uint32_t spsm = b.spsm & 0x3fu, dpsm = b.dpsm & 0x3fu;
    // Pre-swizzled 8-bit art is T8 at its texture buffer width once made
    // linear (the texture animations copy between such images).
    const auto usable = [](const NamedImage &image, uint32_t psm, uint32_t bw) {
        const namedtextures::Upload &u = image.upload;
        if (u.preSwizzled)
            return (u.psm & 0x3fu) == psm && std::max<uint32_t>(u.textureBufferWidth, 1u) == std::max<uint32_t>(bw, 1u);
        return (u.psm & 0x3fu) == psm && (u.dpsm & 0x3fu) == psm &&
               std::max<uint32_t>(u.dbw, 1u) == std::max<uint32_t>(bw, 1u);
    };
    const uint32_t w = command.trxreg.rrw, h = command.trxreg.rrh;
    const uint32_t sx = command.trxpos.ssax, sy = command.trxpos.ssay;
    const uint32_t dx = command.trxpos.dsax, dy = command.trxpos.dsay;
    if (off || from == m_named.end() || to == m_named.end() || spsm != dpsm || !usable(from->second, spsm, b.sbw) ||
        !usable(to->second, dpsm, b.dbw) || sx + w > from->second.upload.textureWidth ||
        sy + h > from->second.upload.textureHeight || dx + w > to->second.upload.textureWidth ||
        dy + h > to->second.upload.textureHeight)
        return;
    const uint32_t bits = namedBits(spsm);
    if (bits == 0u)
        return;
    linearize(from->second);
    linearize(to->second);
    // Through a copy of the source rows, so an overlapping copy within one
    // image reads what was there before.
    const std::vector<uint8_t> source(from->second.bytes.data(), from->second.bytes.data() + from->second.bytes.size());
    std::vector<uint8_t> &dest = to->second.bytes.edit();
    to->second.contentHash = 0u; // written below
    const uint32_t sw = from->second.upload.textureWidth, dw = to->second.upload.textureWidth;
    const auto get = [&](uint32_t x, uint32_t y) -> uint32_t {
        const size_t i = static_cast<size_t>(y) * sw + x;
        switch (bits) {
        case 4u: return (source[i >> 1u] >> ((i & 1u) * 4u)) & 0x0fu;
        case 8u: return source[i];
        case 16u: return source[i * 2u] | (source[i * 2u + 1u] << 8u);
        case 24u: return source[i * 3u] | (source[i * 3u + 1u] << 8u) | (source[i * 3u + 2u] << 16u);
        default: { uint32_t v; std::memcpy(&v, source.data() + i * 4u, 4u); return v; }
        }
    };
    const auto put = [&](uint32_t x, uint32_t y, uint32_t v) {
        const size_t i = static_cast<size_t>(y) * dw + x;
        switch (bits) {
        case 4u: {
            const uint32_t shift = (i & 1u) * 4u;
            dest[i >> 1u] = static_cast<uint8_t>((dest[i >> 1u] & ~(0x0fu << shift)) | ((v & 0x0fu) << shift));
            break;
        }
        case 8u: dest[i] = static_cast<uint8_t>(v); break;
        case 16u: dest[i * 2u] = uint8_t(v); dest[i * 2u + 1u] = uint8_t(v >> 8u); break;
        case 24u: dest[i * 3u] = uint8_t(v); dest[i * 3u + 1u] = uint8_t(v >> 8u); dest[i * 3u + 2u] = uint8_t(v >> 16u); break;
        default: std::memcpy(dest.data() + i * 4u, &v, 4u); break;
        }
    };
    for (uint32_t y = 0u; y < h; ++y)
        for (uint32_t x = 0u; x < w; ++x)
            put(dx + x, dy + y, get(sx + x, sy + y));
    to->second.wordToPixel.clear();
    // Other named images under the written rectangle are something else now.
    GsBlockSet written;
    gsMarkBlocks(written, b.dbp, std::max<uint32_t>(b.dbw, 1u), dpsm, dx + w, dy + h);
    dropNamed(written, b.dbp);
    m_namedCopyKeep = b.dbp;
    ++m_stats.namedCopies;
}

} // namespace dq8::gfx
