// GS local memory: typed pixel access and page-range tracking.
//
// The 4 MiB the GS calls local memory stays the backing store for every
// backend. A hardware backend caches parts of it as GPU textures, but the bytes
// here remain the definition of what those caches hold, so cache lookup,
// invalidation and resolve all speak in terms of this file's page sets.
//
// Nothing here knows about SDL or any graphics API.
#pragma once

#include <bit>
#include <bitset>
#include <cstdint>
#include <cstring>

namespace dq8::gfx {

// Entries into the GS page bookkeeping on this thread: page and block sets
// built (gsMarkPages, gsMarkBlocks and their covered forms), invalidation
// by pages (both caches), palette page reads, and reads served from the host
// writes' memory (m_shadow). A running count the backend reads around a
// converted draw, for docs/NATIVE-SUBMIT-STATUS.md's decoupled counter (c)
// (Rogue Galaxy: added).
inline thread_local uint64_t t_gsPageBookkeeping = 0u;
inline void gsNotePageBookkeeping() { ++t_gsPageBookkeeping; }

// 4 MiB in 8 KiB pages. A page is the GS's own allocation granularity and the
// finest unit at which two resources can be said not to overlap.
inline constexpr uint32_t kGsVramBytes = 4u * 1024u * 1024u;
inline constexpr uint32_t kGsPageBytes = 8192u;
inline constexpr uint32_t kGsPageCount = kGsVramBytes / kGsPageBytes;

using GsPageSet = std::bitset<kGsPageCount>;

// A block is 256 bytes, 32 to a page: the unit two formats' layouts of the
// same memory can be told apart at.
inline constexpr uint32_t kGsBlockBytes = 256u;
inline constexpr uint32_t kGsBlockCount = kGsVramBytes / kGsBlockBytes;
using GsBlockSet = std::bitset<kGsBlockCount>;

// The set as 64-bit words, page p at bit p % 64 of word p / 64. Every
// standard library lays a 512-bit bitset out so (libc++, MSVC and libstdc++
// alike); the bitset's own shift and mask go through bit iterators and cost
// as much as a test per page (Rogue Galaxy: added).
inline constexpr uint32_t kGsPageWords = kGsPageCount / 64u;
inline void gsPageWords(const GsPageSet &pages, uint64_t (&words)[kGsPageWords]) {
    static_assert(sizeof(GsPageSet) == sizeof(uint64_t) * kGsPageWords, "GsPageSet is not packed as 64-bit words");
    std::memcpy(words, &pages, sizeof(words));
}

// Calls visit(page) for each set page, lowest first (Rogue Galaxy: added).
template <typename Visit>
inline void gsForEachPage(const GsPageSet &pages, Visit &&visit) {
    uint64_t words[kGsPageWords];
    gsPageWords(pages, words);
    for (uint32_t index = 0u; index < kGsPageWords; ++index) {
        for (uint64_t word = words[index]; word != 0u; word &= word - 1u)
            visit(index * 64u + static_cast<uint32_t>(std::countr_zero(word)));
    }
}

// Pixel dimensions of one 8 KiB page in a given PSM. Zero for PSMs with no
// defined page geometry, which callers must treat as "assume everything".
struct GsPageExtent {
    uint32_t width = 0u;
    uint32_t height = 0u;

    bool valid() const { return width != 0u && height != 0u; }
};

GsPageExtent gsPageExtent(uint32_t psm);

// Pixel dimensions of one 256-byte block in a given PSM, for the formats
// with a block table here; zero otherwise (Rogue Galaxy: added).
struct GsBlockExtent {
    uint32_t width = 0u;
    uint32_t height = 0u;

    bool valid() const { return width != 0u && height != 0u; }
};

GsBlockExtent gsBlockExtent(uint32_t psm);
// The block number within its page of the block at (column, row) of a page's
// block grid, from the format's block table.
uint32_t gsBlockInPage(uint32_t psm, uint32_t column, uint32_t row);

bool gsIsIndexedPsm(uint32_t psm);
bool gsIsDepthPsm(uint32_t psm);
// True for the PSMs a FRAME register may legally name.
bool gsIsColorFramePsm(uint32_t psm);
uint32_t gsBitsPerPixel(uint32_t psm);

// Marks every page a (base, bufferWidth, psm) rectangle can touch.
//
// Conservative where it has to be: an unknown PSM marks everything, and a
// block base that is not page aligned marks the next physical page for
// formats without a block table here (with one, the carry is exact).
// Over-marking costs a spurious cache invalidation; under-marking silently
// renders stale data.
void gsMarkPages(GsPageSet &pages,
                 uint32_t baseBlock,
                 uint32_t bufferWidth,
                 uint32_t psm,
                 uint32_t width,
                 uint32_t height,
                 uint32_t x = 0u,
                 uint32_t y = 0u);

// The pages a rectangle writes completely, so none of their old contents
// survive it. A subset of gsMarkPages; empty where page cells do not tile the
// buffer exactly.
void gsMarkCoveredPages(GsPageSet &pages,
                        uint32_t baseBlock,
                        uint32_t bufferWidth,
                        uint32_t psm,
                        uint32_t width,
                        uint32_t height,
                        uint32_t x = 0u,
                        uint32_t y = 0u);

// The same at block granularity: every block a rectangle touches, and the
// blocks it writes whole. An unknown block table marks every block of the
// pages it touches (covered: none).
void gsMarkBlocks(GsBlockSet &blocks,
                  uint32_t baseBlock,
                  uint32_t bufferWidth,
                  uint32_t psm,
                  uint32_t width,
                  uint32_t height,
                  uint32_t x = 0u,
                  uint32_t y = 0u);
void gsMarkCoveredBlocks(GsBlockSet &blocks,
                         uint32_t baseBlock,
                         uint32_t bufferWidth,
                         uint32_t psm,
                         uint32_t width,
                         uint32_t height,
                         uint32_t x = 0u,
                         uint32_t y = 0u);
// Every block of the pages given.
void gsBlocksOfPages(GsBlockSet &blocks, const GsPageSet &pages);
// A hash of the raw bytes of every block a width x height rectangle at
// (0, 0) touches, as they sit in local memory, swizzled (Rogue Galaxy:
// added). Cheap next to reading the texels out: a texture whose blocks hash
// the same holds the same texels. Blocks the rectangle shares with other
// data make it change more often than the texels, never less. 0 for a
// format with no block table.
uint64_t gsHashBlocks(const uint8_t *data, uint32_t baseBlock, uint32_t bufferWidth, uint32_t psm, uint32_t width,
                      uint32_t height);

// A view over externally owned GS local memory. GS::init() hands the backend
// the frontend's allocation and other code may read it, so backends write
// through to it rather than keeping a private copy that can silently diverge.
class GsVram {
public:
    GsVram();

    void attach(uint8_t *storage, uint32_t sizeBytes);
    bool attached() const { return m_data != nullptr && m_size != 0u; }

    uint8_t *data() { return m_data; }
    const uint8_t *data() const { return m_data; }
    uint32_t size() const { return m_size; }

    uint32_t read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    // The texels of a w x h rectangle at (x, y) copied from `source`, as
    // write(read()) for each would (Rogue Galaxy: added): the blocks the
    // rectangle covers whole as 256-byte copies, the texels of the others
    // one by one. For PSMCT32, PSMT8 and PSMT4; false for another format,
    // with nothing copied.
    bool copyRect(const GsVram &source, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h);
    // Pixels [first, end) of a host-to-local transfer's rectangle (rows of
    // rrw from (dsax, dsay)), copied by copyRect as up to three rectangles.
    bool copyLanded(const GsVram &source, uint32_t psm, uint32_t base, uint32_t bw, uint32_t dsax, uint32_t dsay,
                    uint32_t rrw, uint32_t first, uint32_t end);

    // A host-to-local run of PSMT8 or PSMT4 pixels: `count` pixels from
    // (x, y), wrapping to dsax at rowEnd, exactly as `count` calls to write()
    // would store them. T4 takes two pixels per byte, low nibble first.
    // (Rogue Galaxy: added. Its texture uploads are mostly 4- and 8-bit, and
    // a write() per pixel was a fifth of the GS worker.)
    void writeRunT8(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                    const uint8_t *src, uint32_t count);
    void writeRunT4(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                    const uint8_t *src, uint32_t count);
    // The same for PSMCT32 and PSMZ32, four bytes a pixel (Rogue Galaxy:
    // added; GSMem's own run writers redo the page arithmetic per pixel).
    void writeRunCT32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                      const uint8_t *src, uint32_t count);
    void writeRunZ32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                     const uint8_t *src, uint32_t count);
    // `count` PSMT4 indices of one row from (x, y), one per byte, as read()
    // returns them (Rogue Galaxy: added, for texture expansion).
    void readRowT4(uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t count, uint8_t *out) const;

private:
    uint8_t *m_data = nullptr;
    uint32_t m_size = 0u;
};

} // namespace dq8::gfx
