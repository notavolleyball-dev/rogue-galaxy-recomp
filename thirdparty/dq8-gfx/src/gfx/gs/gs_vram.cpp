#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace dq8::gfx {

namespace {

// GSMem's swizzle tables are computed once at runtime, not constant-initialised.
void ensureLookupTables() {
    static std::once_flag once;
    std::call_once(once, [] { GSMem::InitLookupTables(); });
}

// Page tables for the run writers. GSMem keeps its own private to its
// translation unit, which is why a write() per pixel cannot inline; these
// are built by the same PixelStorageTraits from the same block and column
// layouts (copied from ps2_gs_memory.cpp), so every address is identical.
using P8Traits = GSMem::PixelStorageTraits<GSMem::P8>;
using P4Traits = GSMem::PixelStorageTraits<GSMem::P4>;
using C32Traits = GSMem::PixelStorageTraits<GSMem::C32>;
using Z32Traits = GSMem::PixelStorageTraits<GSMem::Z32>;

constexpr C32Traits::BlockLookupTableT kBlockTableC32{{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
}};

constexpr Z32Traits::BlockLookupTableT kBlockTableZ32{{
    {24, 25, 28, 29, 8, 9, 12, 13},
    {26, 27, 30, 31, 10, 11, 14, 15},
    {16, 17, 20, 21, 0, 1, 4, 5},
    {18, 19, 22, 23, 2, 3, 6, 7},
}};

// Shared by C32 and Z32.
constexpr C32Traits::ColumnLookupTableT kColumnTable32{{
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},
    {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},
    {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},
    {50, 51, 54, 55, 58, 59, 62, 63},
}};

constexpr P8Traits::BlockLookupTableT kBlockTableP8{{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
}};

constexpr P4Traits::BlockLookupTableT kBlockTableP4{{
    {0, 2, 8, 10},
    {1, 3, 9, 11},
    {4, 6, 12, 14},
    {5, 7, 13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31},
}};

constexpr P8Traits::ColumnLookupTableT kColumnTable8{{
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
}};

constexpr P4Traits::ColumnLookupTableT kColumnTable4{{
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
}};

const P8Traits::PageLookupTableT &pageTableP8() {
    static const auto *table = [] {
        auto *t = new P8Traits::PageLookupTableT{};
        P8Traits::InitPageLookupTable(*t, kBlockTableP8, kColumnTable8);
        return t;
    }();
    return *table;
}

const P4Traits::PageLookupTableT &pageTableP4() {
    static const auto *table = [] {
        auto *t = new P4Traits::PageLookupTableT{};
        P4Traits::InitPageLookupTable(*t, kBlockTableP4, kColumnTable4);
        return t;
    }();
    return *table;
}

const C32Traits::PageLookupTableT &pageTableC32() {
    static const auto *table = [] {
        auto *t = new C32Traits::PageLookupTableT{};
        C32Traits::InitPageLookupTable(*t, kBlockTableC32, kColumnTable32);
        return t;
    }();
    return *table;
}

const Z32Traits::PageLookupTableT &pageTableZ32() {
    static const auto *table = [] {
        auto *t = new Z32Traits::PageLookupTableT{};
        Z32Traits::InitPageLookupTable(*t, kBlockTableZ32, kColumnTable32);
        return t;
    }();
    return *table;
}

} // namespace

namespace {

// The run writers below are Traits::Write unrolled over spans: the address
// of a texel is page * PixelsPerPage + table[block][y % pageH][x % pageW],
// and along a row only x changes until the row or the page ends. Traits::Write
// redid the page arithmetic and a lookup in a megabyte-sized table for every
// texel; with a thousand uploads a frame that was a quarter of the GS worker.
// Same addresses (the selftest checks them against GSMem).
template <typename Traits, typename Table, typename Texel>
void writeRunSpans(const Table &table, uint8_t *data, uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd,
                   uint32_t x, uint32_t y, uint32_t count, Texel texel) {
    constexpr uint32_t pageW = Traits::PageExtent().x;
    constexpr uint32_t pageH = Traits::PageExtent().y;
    constexpr uint32_t pixelsPerPage = static_cast<uint32_t>(Traits::PixelsPerPage());
    constexpr uint32_t blocksPerPage = static_cast<uint32_t>(Traits::BlocksPerPage());
    const uint32_t block = base % blocksPerPage;
    const uint32_t basePage = base / blocksPerPage;
    const uint32_t pagesPerRow = (bw * 64u) / pageW;
    uint32_t i = 0;
    while (i < count) {
        if (x >= rowEnd) {
            x = dsax;
            ++y;
            if (dsax >= rowEnd)
                return; // an empty row never advances
            continue;
        }
        const uint32_t pageCol = x / pageW;
        const uint32_t page = basePage + (y / pageH) * pagesPerRow + pageCol;
        const uint32_t pageBase = page * pixelsPerPage;
        const auto &row = table[block][y % pageH];
        const uint32_t spanEnd = std::min(rowEnd, (pageCol + 1u) * pageW);
        const uint32_t n = std::min(count - i, spanEnd - x);
        uint32_t px = x % pageW;
        for (uint32_t k = 0; k < n; ++k, ++i, ++px)
            texel(data, pageBase + row[px], i);
        x += n;
    }
}

} // namespace

void GsVram::writeRunT8(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                        const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRunSpans<P8Traits>(pageTableP8(), m_data, base, bw, dsax, rowEnd, x, y, count,
                            [src](uint8_t *data, uint32_t pixel, uint32_t i) {
                                data[pixel & (GSMem::MEMORY_SIZE - 1u)] = src[i];
                            });
}

void GsVram::writeRunCT32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                          const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRunSpans<C32Traits>(pageTableC32(), m_data, base, bw, dsax, rowEnd, x, y, count,
                             [src](uint8_t *data, uint32_t pixel, uint32_t i) {
                                 std::memcpy(data + ((pixel * 4u) & (GSMem::MEMORY_SIZE - 4u)), src + i * 4u, 4u);
                             });
}

void GsVram::writeRunZ32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                         const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRunSpans<Z32Traits>(pageTableZ32(), m_data, base, bw, dsax, rowEnd, x, y, count,
                             [src](uint8_t *data, uint32_t pixel, uint32_t i) {
                                 std::memcpy(data + ((pixel * 4u) & (GSMem::MEMORY_SIZE - 4u)), src + i * 4u, 4u);
                             });
}

void GsVram::readRowT4(uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t count, uint8_t *out) const {
    if (!attached())
        return;
    const auto &table = pageTableP4();
    uint8_t *storage = const_cast<uint8_t *>(m_data);
    for (uint32_t i = 0; i < count; ++i)
        out[i] = static_cast<uint8_t>(P4Traits::Read(table, storage, base, bw, x + i, y));
}

void GsVram::writeRunT4(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                        const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRunSpans<P4Traits>(pageTableP4(), m_data, base, bw, dsax, rowEnd, x, y, count,
                            [src](uint8_t *data, uint32_t pixel, uint32_t i) {
                                // Two texels a byte on both sides; the source's low
                                // nibble comes first, the destination's shift is the
                                // address's own bit.
                                const uint8_t packed = src[i >> 1u];
                                const uint32_t value = (i & 1u) ? (packed >> 4u) : (packed & 0x0fu);
                                uint8_t &byte = data[(pixel >> 1u) & (GSMem::MEMORY_SIZE - 1u)];
                                const uint32_t shift = (pixel & 1u) * 4u;
                                byte = static_cast<uint8_t>((byte & ~(0x0fu << shift)) | (value << shift));
                            });
}

GsPageExtent gsPageExtent(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return {64u, 32u};
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return {64u, 64u};
    case GS_PSM_T8:
        return {128u, 64u};
    case GS_PSM_T4:
        return {128u, 128u};
    default:
        return {};
    }
}

bool gsIsIndexedPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_T8:
    case GS_PSM_T4:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return true;
    default:
        return false;
    }
}

bool gsIsDepthPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

bool gsIsColorFramePsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return true;
    default:
        return false;
    }
}

uint32_t gsBitsPerPixel(uint32_t psm) {
    return static_cast<uint32_t>(
        GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm & 0x3fu)));
}

void gsMarkPages(GsPageSet &pages,
                 uint32_t baseBlock,
                 uint32_t bufferWidth,
                 uint32_t psm,
                 uint32_t width,
                 uint32_t height,
                 uint32_t x,
                 uint32_t y) {
    if (width == 0u || height == 0u)
        return;
    const GsPageExtent extent = gsPageExtent(psm);
    if (!extent.valid()) {
        pages.set();
        return;
    }
    const uint32_t pixelsPerRow = std::max<uint32_t>(bufferWidth, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t lastColumn = (x + width - 1u) / extent.width;
    const uint32_t lastRow = (y + height - 1u) / extent.height;
    const uint32_t basePage = baseBlock >> 5u;

    for (uint32_t row = y / extent.height; row <= lastRow; ++row) {
        for (uint32_t column = x / extent.width; column <= lastColumn; ++column) {
            const uint32_t page = basePage + row * pagesPerRow + column;
            pages.set(page % kGsPageCount);
            // A block base inside a page pushes the tail of the rectangle into
            // the next one.
            if ((baseBlock & 31u) != 0u)
                pages.set((page + 1u) % kGsPageCount);
        }
    }
}

void gsMarkCoveredPages(GsPageSet &pages,
                        uint32_t baseBlock,
                        uint32_t bufferWidth,
                        uint32_t psm,
                        uint32_t width,
                        uint32_t height,
                        uint32_t x,
                        uint32_t y) {
    const GsPageExtent extent = gsPageExtent(psm);
    const uint32_t pixelsPerRow = std::max<uint32_t>(bufferWidth, 1u) * 64u;
    // Only where page cells tile the buffer exactly; anything else covers
    // no page with certainty.
    if (!extent.valid() || width == 0u || height == 0u || (baseBlock & 31u) != 0u ||
        pixelsPerRow % extent.width != 0u || x + width > pixelsPerRow)
        return;
    const uint32_t pagesPerRow = pixelsPerRow / extent.width;
    const uint32_t firstColumn = (x + extent.width - 1u) / extent.width;
    const uint32_t endColumn = (x + width) / extent.width;
    const uint32_t firstRow = (y + extent.height - 1u) / extent.height;
    const uint32_t endRow = (y + height) / extent.height;
    for (uint32_t row = firstRow; row < endRow; ++row)
        for (uint32_t column = firstColumn; column < endColumn; ++column)
            pages.set(((baseBlock >> 5u) + row * pagesPerRow + column) % kGsPageCount);
}

GsVram::GsVram() {
    ensureLookupTables();
}

void GsVram::attach(uint8_t *storage, uint32_t sizeBytes) {
    ensureLookupTables();
    m_data = storage;
    m_size = storage != nullptr ? sizeBytes : 0u;
}

uint32_t GsVram::read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const {
    if (!attached())
        return 0u;
    // GSMem's accessors take a mutable pointer even to read.
    uint8_t *storage = const_cast<uint8_t *>(m_data);
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: return ReadCT32(storage, base, bw, x, y);
    case GS_PSM_CT24: return ReadCT24(storage, base, bw, x, y);
    case GS_PSM_CT16: return ReadCT16(storage, base, bw, x, y);
    case GS_PSM_CT16S: return ReadCT16S(storage, base, bw, x, y);
    case GS_PSM_T8: return ReadP8(storage, base, bw, x, y);
    case GS_PSM_T4: return ReadP4(storage, base, bw, x, y);
    case GS_PSM_T8H: return ReadP8H(storage, base, bw, x, y);
    case GS_PSM_T4HL: return ReadP4HL(storage, base, bw, x, y);
    case GS_PSM_T4HH: return ReadP4HH(storage, base, bw, x, y);
    case GS_PSM_Z32: return ReadZ32(storage, base, bw, x, y);
    case GS_PSM_Z24: return ReadZ24(storage, base, bw, x, y);
    case GS_PSM_Z16: return ReadZ16(storage, base, bw, x, y);
    case GS_PSM_Z16S: return ReadZ16S(storage, base, bw, x, y);
    default: return 0u;
    }
}

void GsVram::write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) {
    if (!attached())
        return;
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: WriteCT32(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT24: WriteCT24(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16: WriteCT16(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16S: WriteCT16S(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8: WriteP8(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4: WriteP4(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8H: WriteP8H(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HL: WriteP4HL(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HH: WriteP4HH(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z32: WriteZ32(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z24: WriteZ24(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16: WriteZ16(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16S: WriteZ16S(m_data, base, bw, x, y, value); break;
    default: break;
    }
}

} // namespace dq8::gfx
