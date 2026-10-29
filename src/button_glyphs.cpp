#include "button_glyphs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "button_art.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"

namespace rg::buttons
{
namespace
{

// GaijiLoadImage(archive) copies the picture to 0x4C3180 afresh (at boot
// and with every map's resources; the texture registered stays in use while
// a loading screen shows), and GaijiGetTex answers its texture for each
// special character drawn, which the game sends from that memory.
constexpr uint32_t kGaijiLoadImage = 0x002705A0u;
constexpr uint32_t kGaijiGetTex = 0x00270510u;
constexpr uint32_t kGaijiImage = 0x004C3180u;
constexpr uint32_t kGaijiImageSize = 0x10800u;
// gaiji2.img: an IM3 archive holding one TIM2 picture, its header at +0x60,
// 256x256 8-bit indexed (stored in the GS's PSMT8 order), its 256-colour
// CLUT (CSM1 order) after the pixels.
constexpr uint32_t kTim2 = 0x50u;
constexpr uint32_t kPicture = 0x60u;
constexpr int kWidth = 256;
constexpr int kHeight = 256;
constexpr uint32_t kPixelBytes = kWidth * kHeight;
constexpr uint32_t kClutBytes = 256u * 4u;

// The buttons' corner of the picture: L2, L1, R1, R2 along the top, the
// face buttons below.
constexpr int kRegionWidth = 128;
constexpr int kRegionHeight = 42;
using art::Bitmap;
using art::Cell;
using art::kCells;
using art::kFaceSize;
using art::kPlateHeight;
using art::kPlateWidth;
using art::Rgb;
// The templates: the cross's circle and L1's plate, emptied.
constexpr Cell kCircle = kCells[0];
constexpr Cell kPlate = kCells[4];

PS2Runtime::RecompiledFunction g_loadOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getTexOriginal = nullptr;
const char *g_dumpPath = nullptr;

// Packed Glyphs, four bits a button: what the presenting thread wants, and
// what the picture has (the game's thread).
std::atomic<uint32_t> g_wanted{0u};
// RG_BUTTONS set what is drawn: the menu's choice does not change it.
bool g_forced = false;
uint32_t g_applied = 0u;
bool g_loaded = false;
// The buttons' corner as loaded, unswizzled.
std::vector<uint8_t> g_original;

uint32_t pack(const Glyphs &glyphs)
{
    uint32_t packed = 0u;
    for (size_t i = 0; i < kButtons; ++i)
    {
        packed |= static_cast<uint32_t>(glyphs[i]) << (i * 4u);
    }
    return packed;
}

Glyph glyphAt(uint32_t packed, size_t i)
{
    return static_cast<Glyph>((packed >> (i * 4u)) & 0xFu);
}

// Where pixel (x, y) of a PSMT8 picture stored in the GS's order is.
uint32_t swizzled(int x, int y)
{
    const int block = (y & ~0xF) * kWidth + (x & ~0xF) * 2;
    const int swap = (((y + 2) >> 2) & 1) * 4;
    const int row = (((y & ~3) >> 1) + (y & 1)) & 7;
    const int column = row * kWidth * 2 + ((x + swap) & 7) * 4;
    const int byte = ((y >> 1) & 1) + ((x >> 2) & 2);
    return static_cast<uint32_t>(block + column + byte);
}

// CSM1 keeps a CLUT's entries 8-15 and 16-23 of every 32 swapped.
int clutEntry(int index)
{
    const int row = index & 31;
    if (row >= 8 && row < 16)
    {
        return index + 8;
    }
    if (row >= 16 && row < 24)
    {
        return index - 8;
    }
    return index;
}

struct Picture
{
    uint8_t *pixels = nullptr;
    const uint8_t *clut = nullptr;
};

// The loaded picture, if it is the one this knows.
bool picture(uint8_t *rdram, Picture &out)
{
    uint8_t *image = getMemPtr(rdram, kGaijiImage);
    if (!image || std::memcmp(image + kTim2, "TIM2", 4) != 0)
    {
        return false;
    }
    uint32_t clutBytes = 0u, pixelBytes = 0u;
    uint16_t headerBytes = 0u, colours = 0u, width = 0u, height = 0u;
    std::memcpy(&clutBytes, image + kPicture + 0x04, 4);
    std::memcpy(&pixelBytes, image + kPicture + 0x08, 4);
    std::memcpy(&headerBytes, image + kPicture + 0x0C, 2);
    std::memcpy(&colours, image + kPicture + 0x0E, 2);
    std::memcpy(&width, image + kPicture + 0x14, 2);
    std::memcpy(&height, image + kPicture + 0x16, 2);
    const uint8_t imageType = image[kPicture + 0x13];
    const uint32_t pixels = kPicture + headerBytes;
    if (clutBytes != kClutBytes || pixelBytes != kPixelBytes || colours != 256u || imageType != 5u ||
        width != kWidth || height != kHeight || pixels + kPixelBytes + kClutBytes > kGaijiImageSize)
    {
        return false;
    }
    out.pixels = image + pixels;
    out.clut = image + pixels + kPixelBytes;
    return true;
}

// The picture's colour nearest to `rgb` among its opaque ones.
uint8_t nearest(const Picture &pic, Rgb rgb)
{
    int best = 0;
    long bestError = -1;
    for (int index = 0; index < 256; ++index)
    {
        const uint8_t *c = pic.clut + clutEntry(index) * 4;
        if (c[3] < 0x40)
        {
            continue;
        }
        const long dr = c[0] - rgb.r, dg = c[1] - rgb.g, db = c[2] - rgb.b;
        const long error = dr * dr + dg * dg + db * db;
        if (bestError < 0 || error < bestError)
        {
            best = index;
            bestError = error;
        }
    }
    return static_cast<uint8_t>(best);
}

// The region, unswizzled, to draw in.
struct Region
{
    uint8_t at[kRegionHeight][kRegionWidth];
};

uint8_t originalAt(int x, int y)
{
    return g_original[static_cast<size_t>(y) * kRegionWidth + x];
}

// The most common index of the template's pixels that `keep` accepts.
template <typename Keep>
uint8_t commonest(const Cell &cell, int width, int height, Keep keep)
{
    std::map<uint8_t, int> counts;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (keep(x, y))
            {
                ++counts[originalAt(cell.x + x, cell.y + y)];
            }
        }
    }
    uint8_t best = 0u;
    int bestCount = -1;
    for (const auto &[index, count] : counts)
    {
        if (count > bestCount)
        {
            best = index;
            bestCount = count;
        }
    }
    return best;
}

// A letter's cells at (x0, y0), with a one-pixel edge around them.
void stamp(Region &region, const Bitmap &letter, int x0, int y0, uint8_t colour, uint8_t edge)
{
    const int width = letter.width();
    const auto on = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < letter.count && letter.rows[y][x] == 'X';
    };
    for (int y = -1; y <= letter.count; ++y)
    {
        for (int x = -1; x <= width; ++x)
        {
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= kRegionWidth || py >= kRegionHeight)
            {
                continue;
            }
            if (on(x, y))
            {
                region.at[py][px] = colour;
                continue;
            }
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy)
            {
                for (int dx = -1; dx <= 1 && !near; ++dx)
                {
                    near = on(x + dx, y + dy);
                }
            }
            if (near)
            {
                region.at[py][px] = edge;
            }
        }
    }
}

void drawFace(Region &region, const Picture &pic, const Cell &cell, Glyph glyph)
{
    const auto distance = [](int x, int y) { return std::hypot(x - 10.5, y - 10.5); };
    const uint8_t fill = commonest(kCircle, kFaceSize, kFaceSize, [&](int x, int y) {
        const double d = distance(x, y);
        return d > 5.5 && d <= 7.6;
    });
    for (int y = 0; y < kFaceSize; ++y)
    {
        for (int x = 0; x < kFaceSize; ++x)
        {
            region.at[cell.y + y][cell.x + x] =
                distance(x, y) <= 7.6 ? fill : originalAt(kCircle.x + x, kCircle.y + y);
        }
    }
    const Bitmap letter = art::faceLetter(glyph);
    stamp(region, letter, cell.x + art::faceLetterX(letter), cell.y + art::kFaceLetterY,
          nearest(pic, art::faceColour(glyph)), nearest(pic, art::kLetterEdge));
}

void drawPlate(Region &region, const Picture &pic, const Cell &cell, Glyph glyph)
{
    const auto onFace = [](int x, int y) {
        return x >= art::kPlateFaceLeft && x <= art::kPlateFaceRight && y >= art::kPlateFaceTop &&
               y <= art::kPlateFaceBottom;
    };
    const uint8_t body = commonest(kPlate, kPlateWidth, kPlateHeight, onFace);
    const uint8_t shade = nearest(pic, art::kPlateShade);
    for (int y = 0; y < kPlateHeight; ++y)
    {
        for (int x = 0; x < kPlateWidth; ++x)
        {
            uint8_t index = originalAt(kPlate.x + x, kPlate.y + y);
            if (onFace(x, y))
            {
                index = body;
            }
            else if (x >= art::kPlateFaceLeft && x <= art::kPlateFaceRight && y == art::kPlateFaceBottom + 1)
            {
                index = shade;
            }
            region.at[cell.y + y][cell.x + x] = index;
        }
    }
    const uint8_t white = nearest(pic, art::kPlateWhite), edge = nearest(pic, art::kPlateEdge);
    for (const bool second : {false, true})
    {
        stamp(region, art::plateLetter(glyph, second), cell.x + art::plateLetterX(glyph, second),
              cell.y + art::kPlateLetterY, white, edge);
    }
}

bool faceGlyph(Glyph glyph)
{
    return glyph == Glyph::A || glyph == Glyph::B || glyph == Glyph::X || glyph == Glyph::Y;
}

bool plateGlyph(Glyph glyph)
{
    return glyph == Glyph::LB || glyph == Glyph::RB || glyph == Glyph::LT || glyph == Glyph::RT;
}

void apply(uint8_t *rdram, uint32_t packed)
{
    Picture pic;
    if (!picture(rdram, pic))
    {
        return;
    }
    Region region;
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            region.at[y][x] = originalAt(x, y);
        }
    }
    for (size_t i = 0; i < kButtons; ++i)
    {
        const Glyph glyph = glyphAt(packed, i);
        const Cell &cell = kCells[i];
        if (cell.face && faceGlyph(glyph))
        {
            drawFace(region, pic, cell, glyph);
        }
        else if (!cell.face && plateGlyph(glyph))
        {
            drawPlate(region, pic, cell, glyph);
        }
    }
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            pic.pixels[swizzled(x, y)] = region.at[y][x];
        }
    }
    g_applied = packed;
}

void afterLoad(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    Picture pic;
    if (!picture(rdram, pic))
    {
        std::fprintf(stderr, "[buttons] gaiji2.img is not the picture expected, left as it is\n");
        g_loaded = false;
        return;
    }
    if (g_dumpPath)
    {
        if (FILE *out = std::fopen(g_dumpPath, "wb"))
        {
            std::fwrite(getConstMemPtr(rdram, kGaijiImage), 1, kGaijiImageSize, out);
            std::fclose(out);
        }
    }
    // Loaded afresh: the PlayStation's buttons.
    g_original.resize(static_cast<size_t>(kRegionWidth) * kRegionHeight);
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            g_original[static_cast<size_t>(y) * kRegionWidth + x] = pic.pixels[swizzled(x, y)];
        }
    }
    g_loaded = true;
    g_applied = 0u;
    if (const uint32_t wanted = g_wanted.load(std::memory_order_relaxed); wanted != 0u)
    {
        apply(rdram, wanted);
    }
}

void gaijiLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_loadOriginal, rdram, ctx, runtime, &afterLoad, 0u);
}

void gaijiGetTex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_loaded)
    {
        if (const uint32_t wanted = g_wanted.load(std::memory_order_relaxed); wanted != g_applied)
        {
            apply(rdram, wanted);
        }
    }
    g_getTexOriginal(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *dump = std::getenv("RG_GAIJI_DUMP"); dump && *dump)
    {
        g_dumpPath = dump;
    }
    // RG_BUTTONS=xbox draws an Xbox pad's from the start, for tests.
    if (const char *buttons = std::getenv("RG_BUTTONS"); buttons && std::strcmp(buttons, "xbox") == 0)
    {
        setGlyphs({Glyph::A, Glyph::B, Glyph::X, Glyph::Y, Glyph::LB, Glyph::RB, Glyph::LT, Glyph::RT});
        g_forced = true;
    }
    if (!rg::timing::hookFunction(runtime, kGaijiLoadImage, &gaijiLoadImage, g_loadOriginal) ||
        !rg::timing::hookFunction(runtime, kGaijiGetTex, &gaijiGetTex, g_getTexOriginal))
    {
        std::fprintf(stderr, "[rg] the button symbols stay the PlayStation's\n");
    }
}

void setGlyphs(const Glyphs &glyphs)
{
    if (!g_forced)
    {
        g_wanted.store(pack(glyphs), std::memory_order_relaxed);
    }
}

} // namespace rg::buttons
