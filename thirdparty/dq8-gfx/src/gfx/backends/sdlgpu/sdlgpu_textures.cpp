#include "gfx/backends/sdlgpu/sdlgpu_textures.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <set>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

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
    m_uploadCopied = !decodeStore && m_vram.attached() &&
                     (psm == GS_PSM_CT32 || psm == GS_PSM_T8 || psm == GS_PSM_T4);
    m_shadowTransfer->begin(command);
    m_auditPending = static_cast<uint32_t>(m_uploads.size()) + 15u;
}

void GsTextureCache::noteUploadData(const uint8_t *data, uint32_t sizeBytes) {
    if (m_tracking && m_shadowTransfer && !m_uploadCopied)
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
            if ((m_drawnBlocks & m_coveredBlocks).any()) {
                m_drawnBlocks &= ~m_coveredBlocks;
                ++m_drawnGeneration;
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
    if (m_tracking)
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
    if (!m_tracking || region.empty())
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
    if ((blocks & m_drawnBlocks).none()) {
        ++m_stats.uploadStoreReads;
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
    const uint64_t texels = expand(key, region, true);
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
        if (entry.clutDirty) {
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
    SDL_GPUDevice *device = m_device.handle();
    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;

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
    std::memcpy(mapped, m_staging.data(), bytes);
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
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
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

bool GsTextureCache::expandFromTarget(const GsTextureKey &key, Entry &entry, GsRegion region,
                                      bool &handled, std::string &error) {
    handled = false;
    // An 8-bit indexed texture through its palette, or (Rogue Galaxy: added)
    // a CT32 one texel for texel, from the target that drew its pages. The
    // flip's copy of the frame into the display buffer reads the frame so,
    // every frame; through local memory that was a readback of the whole
    // frame, an expansion and an upload.
    const bool indexed = (key.psm & 0x3fu) == GS_PSM_T8;
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
    if (!indexed && std::getenv("DQ8_GFX_DEBUG_DIRECT"))
        std::fprintf(stderr, "[direct] tbp %05x tbw %u %ux%u region %u,%u-%u,%u target %s\n", key.tbp0, key.tbw,
                     key.width, key.height, region.x0, region.y0, region.x1, region.y1, target ? "yes" : "no");
    if (!target)
        return true;

    // The palette is still read on the CPU; only the texels come from the
    // target, so only their cells of it need be current (its other queued
    // patches, the textures uploaded into its other pages, can wait: Rogue
    // Galaxy, refreshRegion).
    GsPageSet clutPages;
    if (indexed)
        markClutReads(key, clutPages);
    if (!m_targets.copyOut(clutPages, error) ||
        !m_targets.refreshRegion(*target, m_targets.regionForPages(*target, texelPages), error))
        return false;
    if (indexed)
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
    const std::array<uint32_t, 4> pages{key.tbp0 >> 5u, indexed ? std::max<uint32_t>(key.tbw >> 1u, 1u) : key.tbw,
                                        target->base >> 5u,
                                        target->bufferWidth | (std::max(target->scale, 1u) << 16u)};
    const bool done = indexed ? m_device.expandIndexed8(target->texture, m_clutRgba.data(), entry.texture, key.width,
                                                        key.height, rect, pages, error)
                              : m_device.copyFromTarget(target->texture, entry.texture, key.width, key.height, rect,
                                                        pages, preserve, error);
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
    const uint64_t blockHash =
        blockHashOn && wholeTexture ? gsHashBlocks(m_src->data(), key.tbp0, key.tbw, key.psm, key.width, key.height)
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
    const uint64_t texelHash = expand(key, region);
    // A whole-texture re-expand whose raw texels hash to what we already hold,
    // with no palette change waiting, is the title screen re-uploading its own
    // UI: the GPU texture is already right, so skip the transfer entirely.
    const bool unchanged = wholeTexture && !entry.clutUploadPending &&
                           texelHash == entry.identity.texels;
    if (!unchanged) {
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
    if (expanded) {
        entry.identity = {};
        entry.identity.clut = hashClutWindow(key);
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
    entry.blockHash = gsHashBlocks(m_src->data(), key.tbp0, key.tbw, key.psm, width, height);
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

} // namespace dq8::gfx
