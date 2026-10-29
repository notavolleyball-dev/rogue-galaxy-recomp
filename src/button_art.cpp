#include "button_art.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace rg::buttons::art
{
namespace
{

// Letters two pixels thick: the face buttons' 8 rows high, the plates' 10.
const char *const kFaceA[] = {"..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX",
                              "XXXXXXXX", "XXXXXXXX", "XX....XX", "XX....XX"};
const char *const kFaceB[] = {"XXXXXX.", "XX...XX", "XX...XX", "XXXXXX.",
                              "XXXXXXX", "XX...XX", "XX...XX", "XXXXXX."};
const char *const kFaceX[] = {"XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
                              "..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX"};
const char *const kFaceY[] = {"XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
                              "...XX...", "...XX...", "...XX...", "...XX..."};
constexpr int kFaceRows = 8;
const char *const kTallL[] = {"XX....", "XX....", "XX....", "XX....", "XX....",
                              "XX....", "XX....", "XX....", "XXXXXX", "XXXXXX"};
const char *const kTallR[] = {"XXXXXX.", "XXXXXXX", "XX...XX", "XX...XX", "XXXXXXX",
                              "XXXXXX.", "XX.XX..", "XX..XX.", "XX...XX", "XX...XX"};
const char *const kTallB[] = {"XXXXXX.", "XXXXXXX", "XX...XX", "XX...XX", "XXXXXX.",
                              "XXXXXXX", "XX...XX", "XX...XX", "XXXXXXX", "XXXXXX."};
const char *const kTallT[] = {"XXXXXXXX", "XXXXXXXX", "...XX...", "...XX...", "...XX...",
                              "...XX...", "...XX...", "...XX...", "...XX...", "...XX..."};
constexpr int kTallRows = 10;
constexpr int kPlateGap = 3;

constexpr Rgb kGreen{110, 204, 104};
constexpr Rgb kRed{232, 92, 80};
constexpr Rgb kBlue{84, 148, 244};
constexpr Rgb kYellow{244, 204, 76};

bool on(const Bitmap &letter, int x, int y)
{
    return letter.rows && x >= 0 && y >= 0 && x < letter.width() && y < letter.count && letter.rows[y][x] == 'X';
}

// The first letter of a plate's two, and the second.
char plateChar(Glyph glyph, bool second)
{
    if (!second)
    {
        return glyph == Glyph::LB || glyph == Glyph::LT ? 'L' : 'R';
    }
    return glyph == Glyph::LB || glyph == Glyph::RB ? 'B' : 'T';
}

char faceChar(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::A:
        return 'A';
    case Glyph::B:
        return 'B';
    case Glyph::X:
        return 'X';
    case Glyph::Y:
        return 'Y';
    default:
        return 0;
    }
}

// The region as handed over, read in its cell's texels.
struct View
{
    const uint8_t *rgba;
    int width, height;
    // The cell's corner in the region's texels.
    int offsetX, offsetY;

    const uint8_t *at(int cellX, int cellY) const
    {
        const int x = cellX + offsetX, y = cellY + offsetY;
        if (x < 0 || y < 0 || x >= width || y >= height)
        {
            return nullptr;
        }
        const uint8_t *texel = rgba + (size_t(y) * width + x) * 4u;
        return texel[3] != 0u ? texel : nullptr;
    }
};

bool same(const uint8_t *a, const uint8_t *b)
{
    return a && b && a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

long distance2(const uint8_t *texel, Rgb rgb)
{
    const long dr = texel[0] - rgb.r, dg = texel[1] - rgb.g, db = texel[2] - rgb.b;
    return dr * dr + dg * dg + db * db;
}

// Whether the texels in the cell that `letterAt` says are a letter's are
// all one colour, near `colour`, and no other texel of the cell inside
// [x0, x1) by [y0, y1) is: as button_glyphs.cpp stamps them, one palette
// index for all.
template <typename LetterAt>
bool letterShown(const View &view, int cellWidth, int cellHeight, LetterAt letterAt, Rgb colour, int x0, int y0,
                 int x1, int y1)
{
    const uint8_t *seen = nullptr;
    for (int y = 0; y < cellHeight; ++y)
    {
        for (int x = 0; x < cellWidth; ++x)
        {
            if (!letterAt(x, y))
            {
                continue;
            }
            const uint8_t *texel = view.at(x, y);
            if (!texel || (seen && !same(texel, seen)))
            {
                return false;
            }
            seen = texel;
        }
    }
    constexpr long kNear = 60L * 60L;
    if (!seen || distance2(seen, colour) > kNear)
    {
        return false;
    }
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (!letterAt(x, y) && same(view.at(x, y), seen))
            {
                return false;
            }
        }
    }
    return true;
}

struct Vec
{
    float x, y;
};

// A letter's strokes: the lines a round pen of its weight runs along.
using Strokes = std::vector<std::pair<Vec, Vec>>;

void arc(Strokes &strokes, Vec centre, float radius, float from, float to)
{
    constexpr int kSteps = 12;
    Vec previous{centre.x + radius * std::cos(from), centre.y + radius * std::sin(from)};
    for (int i = 1; i <= kSteps; ++i)
    {
        const float angle = from + (to - from) * float(i) / float(kSteps);
        const Vec next{centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
        strokes.push_back({previous, next});
        previous = next;
    }
}

// A bowl off the stem at `stem`: along `top` to `reach`, round to
// `bottom` (y grows down) and back.
void bowl(Strokes &strokes, float stem, float top, float bottom, float reach)
{
    constexpr float kHalfTurn = 3.14159265f / 2.0f;
    const float radius = (bottom - top) / 2.0f;
    strokes.push_back({{stem, top}, {reach, top}});
    arc(strokes, {reach, top + radius}, radius, -kHalfTurn, kHalfTurn);
    strokes.push_back({{reach, bottom}, {stem, bottom}});
}

// The strokes of `letter` in a box `width` by `height` texels at (x0, y0),
// for a pen `pen` texels wide: the letters the PS2-sized picture has, as
// lines.
Strokes letterStrokes(char letter, float x0, float y0, float width, float height, float pen)
{
    Strokes strokes;
    const float e = pen / 2.0f;
    const auto at = [&](float x, float y) { return Vec{x0 + x, y0 + y}; };
    const float right = width - e, bottom = height - e, middle = width / 2.0f;
    const auto line = [&](Vec a, Vec b) { strokes.push_back({a, b}); };
    switch (letter)
    {
    case 'A': {
        const Vec apex = at(middle, e);
        line(apex, at(e, bottom));
        line(apex, at(right, bottom));
        const float barY = e + (bottom - e) * 0.68f;
        const float along = (barY - e) / (bottom - e);
        line(at(middle - (middle - e) * along, barY), at(middle + (right - middle) * along, barY));
        break;
    }
    case 'B': {
        line(at(e, e), at(e, bottom));
        const float waist = height / 2.0f;
        const float upper = (waist - e) / 2.0f, lower = (bottom - waist) / 2.0f;
        bowl(strokes, x0 + e, y0 + e, y0 + waist, x0 + width - 0.9f - upper - e);
        bowl(strokes, x0 + e, y0 + waist, y0 + bottom, x0 + width - 0.45f - lower - e);
        break;
    }
    case 'R': {
        line(at(e, e), at(e, bottom));
        const float waist = height * 0.52f;
        const float radius = (waist - e) / 2.0f;
        bowl(strokes, x0 + e, y0 + e, y0 + waist, x0 + width - 0.45f - radius - e);
        line(at(width * 0.43f, waist), at(right, bottom));
        break;
    }
    case 'X':
        line(at(e, e), at(right, bottom));
        line(at(right, e), at(e, bottom));
        break;
    case 'Y': {
        const Vec fork = at(middle, height * 0.54f);
        line(at(e, e), fork);
        line(at(right, e), fork);
        line(fork, at(middle, bottom));
        break;
    }
    case 'L':
        line(at(e, e), at(e, bottom));
        line(at(e, bottom), at(right, bottom));
        break;
    case 'T':
        line(at(e, e), at(right, e));
        line(at(middle, e), at(middle, bottom));
        break;
    default:
        break;
    }
    return strokes;
}

float distanceTo(const Strokes &strokes, Vec p)
{
    float best = 1e9f;
    for (const auto &[a, b] : strokes)
    {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float length2 = dx * dx + dy * dy;
        float t = length2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
        best = std::min(best, ex * ex + ey * ey);
    }
    return std::sqrt(best);
}

struct Colour
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

Colour colourOf(const uint8_t *texel)
{
    return {float(texel[0]), float(texel[1]), float(texel[2])};
}

Colour colourOf(Rgb rgb)
{
    return {float(rgb.r), float(rgb.g), float(rgb.b)};
}

Colour mix(Colour a, Colour b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// How much of an output pixel a shape covers that reaches `inside` texels
// past it: a pixel's width of ramp.
float cover(float inside, uint32_t scale)
{
    return std::clamp(inside * float(scale) + 0.5f, 0.0f, 1.0f);
}

// A letter (its strokes) over the button: its edge a texel past its pen,
// then the letter.
Colour letterOver(Colour base, const Strokes &strokes, Vec p, float pen, Rgb colour, Rgb edge, uint32_t scale)
{
    const float d = distanceTo(strokes, p);
    base = mix(base, colourOf(edge), cover(pen / 2.0f + 1.0f - d, scale));
    return mix(base, colourOf(colour), cover(pen / 2.0f - d, scale));
}

// The most common colour of the cell's texels that `keep` accepts.
template <typename Keep>
const uint8_t *commonest(const View &view, int cellWidth, int cellHeight, Keep keep)
{
    std::map<uint32_t, std::pair<int, const uint8_t *>> counts;
    for (int y = 0; y < cellHeight; ++y)
    {
        for (int x = 0; x < cellWidth; ++x)
        {
            const uint8_t *texel = view.at(x, y);
            if (texel && keep(x, y))
            {
                auto &count = counts[uint32_t(texel[0]) << 16 | uint32_t(texel[1]) << 8 | texel[2]];
                ++count.first;
                count.second = texel;
            }
        }
    }
    const uint8_t *best = nullptr;
    int bestCount = 0;
    for (const auto &[rgb, count] : counts)
    {
        if (count.first > bestCount)
        {
            bestCount = count.first;
            best = count.second;
        }
    }
    return best;
}

// The face buttons' circle, about the cell's middle: its black outline a
// texel wide out to 9.9 texels (the area of the PS2's), a bevel lit from
// above inside it, and the button's face inside 7.6, where
// button_glyphs.cpp clears the PlayStation's symbol.
constexpr float kFaceCentre = 11.0f;
constexpr float kFaceOuter = 9.9f;
constexpr float kFaceOutline = 8.95f;
constexpr float kFaceInner = 7.6f;
constexpr int kBevelSteps = 32;
constexpr float kFacePen = 1.8f;

bool drawFace(const View &view, uint32_t scale, uint8_t opaque, std::vector<uint8_t> &out)
{
    Glyph shown = Glyph::PlayStation;
    for (const Glyph glyph : {Glyph::A, Glyph::B, Glyph::X, Glyph::Y})
    {
        const Bitmap letter = faceLetter(glyph);
        const int lx = faceLetterX(letter);
        if (letterShown(
                view, kFaceSize, kFaceSize, [&](int x, int y) { return on(letter, x - lx, y - kFaceLetterY); },
                faceColour(glyph), 0, 0, kFaceSize, kFaceSize))
        {
            shown = glyph;
            break;
        }
    }
    if (shown == Glyph::PlayStation)
    {
        return false;
    }
    const auto radius = [](float x, float y) { return std::hypot(x - kFaceCentre, y - kFaceCentre); };
    const auto texelRadius = [&](int x, int y) { return radius(float(x) + 0.5f, float(y) + 0.5f); };
    const uint8_t *fill = commonest(view, kFaceSize, kFaceSize, [&](int x, int y) {
        const float r = texelRadius(x, y);
        return r > 5.5f && r <= kFaceInner;
    });
    const uint8_t *outline = commonest(view, kFaceSize, kFaceSize, [&](int x, int y) {
        const float r = texelRadius(x, y);
        return r > 9.0f && r < kFaceOuter;
    });
    if (!fill || !outline)
    {
        return false;
    }
    // The bevel's colour about the circle, from its texels, a step at a
    // time; a step with none takes its neighbours'.
    Colour bevel[kBevelSteps];
    int counts[kBevelSteps] = {};
    constexpr float kTurn = 6.2831853f;
    const auto step = [&](float x, float y) {
        const float angle = std::atan2(y - kFaceCentre, x - kFaceCentre);
        return (angle < 0.0f ? angle + kTurn : angle) / kTurn * float(kBevelSteps);
    };
    for (int y = 0; y < kFaceSize; ++y)
    {
        for (int x = 0; x < kFaceSize; ++x)
        {
            const float r = texelRadius(x, y);
            const uint8_t *texel = view.at(x, y);
            if (!texel || r <= kFaceInner + 0.1f || r >= kFaceOutline)
            {
                continue;
            }
            const int i = std::min(int(step(float(x) + 0.5f, float(y) + 0.5f)), kBevelSteps - 1);
            bevel[i].r += texel[0];
            bevel[i].g += texel[1];
            bevel[i].b += texel[2];
            ++counts[i];
        }
    }
    bool any = false;
    for (int i = 0; i < kBevelSteps; ++i)
    {
        if (counts[i] != 0)
        {
            bevel[i] = {bevel[i].r / counts[i], bevel[i].g / counts[i], bevel[i].b / counts[i]};
            any = true;
        }
    }
    if (!any)
    {
        return false;
    }
    for (int pass = 0; pass < kBevelSteps; ++pass)
    {
        bool filled = true;
        for (int i = 0; i < kBevelSteps; ++i)
        {
            if (counts[i] != 0)
            {
                continue;
            }
            const int before = (i + kBevelSteps - 1) % kBevelSteps, after = (i + 1) % kBevelSteps;
            if (counts[before] > 0 || counts[after] > 0)
            {
                const int from = counts[before] > 0 ? before : after;
                const int to = counts[after] > 0 ? after : before;
                bevel[i] = mix(bevel[from], bevel[to], 0.5f);
                counts[i] = -1;
            }
            else
            {
                filled = false;
            }
        }
        for (int &count : counts)
        {
            count = count < 0 ? 1 : count;
        }
        if (filled)
        {
            break;
        }
    }
    const Bitmap letter = faceLetter(shown);
    const float width = float(letter.width());
    const Strokes strokes = letterStrokes(faceChar(shown), (float(kFaceSize) - width) / 2.0f, float(kFaceLetterY),
                                          width, float(kFaceRows), kFacePen);
    const int outWidth = view.width * int(scale), outHeight = view.height * int(scale);
    for (int j = 0; j < outHeight; ++j)
    {
        for (int i = 0; i < outWidth; ++i)
        {
            const Vec p{(float(i) + 0.5f) / float(scale) - float(view.offsetX),
                        (float(j) + 0.5f) / float(scale) - float(view.offsetY)};
            const float r = radius(p.x, p.y);
            const float alpha = cover(kFaceOuter - r, scale);
            if (alpha <= 0.0f)
            {
                continue;
            }
            const float at = step(p.x, p.y) - 0.5f;
            const int below = (int(std::floor(at)) + kBevelSteps) % kBevelSteps;
            const Colour ring = mix(bevel[below], bevel[(below + 1) % kBevelSteps], at - std::floor(at));
            Colour colour = mix(colourOf(fill), ring, cover(r - kFaceInner, scale));
            colour = mix(colour, colourOf(outline), cover(r - kFaceOutline, scale));
            colour = letterOver(colour, strokes, p, kFacePen, faceColour(shown), kLetterEdge, scale);
            uint8_t *pixel = &out[(size_t(j) * outWidth + i) * 4u];
            pixel[0] = uint8_t(colour.r + 0.5f);
            pixel[1] = uint8_t(colour.g + 0.5f);
            pixel[2] = uint8_t(colour.b + 0.5f);
            pixel[3] = uint8_t(float(opaque) * alpha + 0.5f);
        }
    }
    return true;
}

// The plates: a rounded box over x 1-31, y 1-19 of the cell, its black
// outline a texel wide, then the PS2's own bevel and face texel by texel
// (straight edges, sharp as they are at any scale).
constexpr float kPlateLeft = 1.0f, kPlateTop = 1.0f, kPlateRight = 31.0f, kPlateBottom = 19.0f;
constexpr float kPlateRound = 2.5f;
constexpr float kPlatePen = 2.0f;

// How far inside the plate's box a point is (negative outside).
float insidePlate(Vec p)
{
    const float cx = (kPlateLeft + kPlateRight) / 2.0f, cy = (kPlateTop + kPlateBottom) / 2.0f;
    const float qx = std::abs(p.x - cx) - ((kPlateRight - kPlateLeft) / 2.0f - kPlateRound);
    const float qy = std::abs(p.y - cy) - ((kPlateBottom - kPlateTop) / 2.0f - kPlateRound);
    const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f);
    return kPlateRound - outside;
}

bool drawPlate(const View &view, uint32_t scale, uint8_t opaque, std::vector<uint8_t> &out)
{
    Glyph shown = Glyph::PlayStation;
    for (const Glyph glyph : {Glyph::LB, Glyph::RB, Glyph::LT, Glyph::RT})
    {
        const Bitmap first = plateLetter(glyph, false), second = plateLetter(glyph, true);
        const int x1 = plateLetterX(glyph, false), x2 = plateLetterX(glyph, true);
        const auto letterAt = [&](int x, int y) {
            return on(first, x - x1, y - kPlateLetterY) || on(second, x - x2, y - kPlateLetterY);
        };
        if (letterShown(view, kPlateWidth, kPlateHeight, letterAt, kPlateWhite, kPlateFaceLeft, kPlateFaceTop,
                        kPlateFaceRight + 1, kPlateFaceBottom + 1))
        {
            shown = glyph;
            break;
        }
    }
    const uint8_t *outline = view.at(16, 1);
    const uint8_t *face = view.at(kPlateFaceLeft, kPlateFaceTop);
    if (shown == Glyph::PlayStation || !outline || !face)
    {
        return false;
    }
    const auto outlineLike = [&](const uint8_t *texel) { return !texel || same(texel, outline); };
    Strokes strokes;
    for (const bool second : {false, true})
    {
        const Bitmap letter = plateLetter(shown, second);
        const Strokes more = letterStrokes(plateChar(shown, second), float(plateLetterX(shown, second)),
                                           float(kPlateLetterY), float(letter.width()), float(kTallRows), kPlatePen);
        strokes.insert(strokes.end(), more.begin(), more.end());
    }
    const int outWidth = view.width * int(scale), outHeight = view.height * int(scale);
    for (int j = 0; j < outHeight; ++j)
    {
        for (int i = 0; i < outWidth; ++i)
        {
            const Vec p{(float(i) + 0.5f) / float(scale) - float(view.offsetX),
                        (float(j) + 0.5f) / float(scale) - float(view.offsetY)};
            const float inside = insidePlate(p);
            const float alpha = cover(inside, scale);
            if (alpha <= 0.0f)
            {
                continue;
            }
            // The texel under it, kept inside the outline: the PS2's
            // corners are cut in steps, the box's round.
            int tx = std::clamp(int(std::floor(p.x)), 2, kPlateWidth - 3);
            int ty = std::clamp(int(std::floor(p.y)), 2, kPlateHeight - 3);
            const uint8_t *texel = view.at(tx, ty);
            for (int tries = 0; tries < 3 && outlineLike(texel); ++tries)
            {
                tx += tx < kPlateWidth / 2 ? 1 : -1;
                ty += ty < kPlateHeight / 2 ? 1 : -1;
                texel = view.at(tx, ty);
            }
            if (tx >= kPlateFaceLeft && tx <= kPlateFaceRight && ty >= kPlateFaceTop && ty <= kPlateFaceBottom)
            {
                texel = face;
            }
            Colour colour = colourOf(outline);
            if (texel && !outlineLike(texel))
            {
                colour = mix(colourOf(outline), colourOf(texel), cover(inside - 1.0f, scale));
            }
            colour = letterOver(colour, strokes, p, kPlatePen, kPlateWhite, kPlateEdge, scale);
            uint8_t *pixel = &out[(size_t(j) * outWidth + i) * 4u];
            pixel[0] = uint8_t(colour.r + 0.5f);
            pixel[1] = uint8_t(colour.g + 0.5f);
            pixel[2] = uint8_t(colour.b + 0.5f);
            pixel[3] = uint8_t(float(opaque) * alpha + 0.5f);
        }
    }
    return true;
}

// The registered mark: a dark oval (its edge the picture's soft one), a
// light ring a texel and a half inside it, and an R, its strokes about a
// texel wide. Placed and sized by the oval the texels show; the R's
// strokes as the smaller mark's are, in the ring's radii (5.5 by 6.5).
constexpr float kMarkPen = 1.15f;

bool drawMark(const uint8_t *rgba, int width, int height, uint32_t scale, uint8_t opaque, std::vector<uint8_t> &out)
{
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    const uint8_t *light = nullptr, *dark = nullptr;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const uint8_t *texel = rgba + (size_t(y) * width + x) * 4u;
            if (texel[3] < 0x40u)
            {
                continue;
            }
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
            if (!light || texel[0] + texel[1] + texel[2] > light[0] + light[1] + light[2])
            {
                light = texel;
            }
            if (texel[3] >= opaque && (!dark || texel[0] + texel[1] + texel[2] < dark[0] + dark[1] + dark[2]))
            {
                dark = texel;
            }
        }
    }
    if (!light || !dark || x1 - x0 < 8 || y1 - y0 < 8)
    {
        return false;
    }
    const float cx = (float(x0) + float(x1) + 1.0f) / 2.0f, cy = (float(y0) + float(y1) + 1.0f) / 2.0f;
    const float boxX = (float(x1 - x0) + 1.0f) / 2.0f, boxY = (float(y1 - y0) + 1.0f) / 2.0f;
    const float ringX = boxX - 1.5f, ringY = boxY - 1.5f;
    // The box's outer texels are half transparent: the oval inside them.
    const float outerX = boxX - 0.35f, outerY = boxY - 0.35f;
    // Distance to an oval's edge, near it (positive outside).
    const auto toOval = [](float dx, float dy, float rx, float ry) {
        const float f = std::sqrt((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry));
        const float gx = dx / (rx * rx), gy = dy / (ry * ry);
        const float gradient = std::sqrt(gx * gx + gy * gy) / std::max(f, 1e-4f);
        return gradient > 0.0f ? (f - 1.0f) / gradient : -std::min(rx, ry);
    };
    const float sx = ringX / 5.5f, sy = ringY / 6.5f;
    const auto at = [&](float x, float y) { return Vec{cx + x * sx, cy + y * sy}; };
    Strokes strokes;
    const float stem = -2.2f, top = -3.5f, waist = 0.5f, foot = 3.6f, reach = 0.5f;
    strokes.push_back({at(stem, top), at(stem, foot)});
    bowl(strokes, cx + stem * sx, cy + top * sy, cy + waist * sy, cx + reach * sx);
    strokes.push_back({at(0.6f, waist), at(2.4f, foot)});
    const int outWidth = width * int(scale), outHeight = height * int(scale);
    for (int j = 0; j < outHeight; ++j)
    {
        for (int i = 0; i < outWidth; ++i)
        {
            const Vec p{(float(i) + 0.5f) / float(scale), (float(j) + 0.5f) / float(scale)};
            const float dx = p.x - cx, dy = p.y - cy;
            const float alpha = cover(-toOval(dx, dy, outerX, outerY), scale);
            if (alpha <= 0.0f)
            {
                continue;
            }
            const float ring = std::abs(toOval(dx, dy, ringX, ringY));
            Colour colour = mix(colourOf(dark), colourOf(light), cover(kMarkPen / 2.0f - ring, scale));
            colour = mix(colour, colourOf(light), cover(kMarkPen / 2.0f - distanceTo(strokes, p), scale));
            uint8_t *pixel = &out[(size_t(j) * outWidth + i) * 4u];
            pixel[0] = uint8_t(colour.r + 0.5f);
            pixel[1] = uint8_t(colour.g + 0.5f);
            pixel[2] = uint8_t(colour.b + 0.5f);
            pixel[3] = uint8_t(float(opaque) * alpha + 0.5f);
        }
    }
    return true;
}

} // namespace

int Bitmap::width() const
{
    return rows ? static_cast<int>(std::strlen(rows[0])) : 0;
}

Bitmap faceLetter(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::A:
        return {kFaceA, kFaceRows};
    case Glyph::B:
        return {kFaceB, kFaceRows};
    case Glyph::X:
        return {kFaceX, kFaceRows};
    case Glyph::Y:
        return {kFaceY, kFaceRows};
    default:
        return {nullptr, 0};
    }
}

int faceLetterX(const Bitmap &letter)
{
    return (kFaceSize - letter.width()) / 2;
}

Bitmap plateLetter(Glyph glyph, bool second)
{
    switch (plateChar(glyph, second))
    {
    case 'L':
        return {kTallL, kTallRows};
    case 'R':
        return {kTallR, kTallRows};
    case 'B':
        return {kTallB, kTallRows};
    default:
        return {kTallT, kTallRows};
    }
}

int plateLetterX(Glyph glyph, bool second)
{
    const int first = plateLetter(glyph, false).width();
    const int left = (kPlateWidth - (first + kPlateGap + plateLetter(glyph, true).width())) / 2;
    return second ? left + first + kPlateGap : left;
}

Rgb faceColour(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::B:
        return kRed;
    case Glyph::X:
        return kBlue;
    case Glyph::Y:
        return kYellow;
    default:
        return kGreen;
    }
}

bool drawLarger(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t scale,
                std::vector<uint8_t> &out)
{
    if (width > 40u || height > 40u || scale < 2u)
    {
        return false;
    }
    uint8_t opaque = 0u;
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        opaque = std::max(opaque, rgba[i * 4u + 3u]);
    }
    // The registered mark: two sizes at the picture's bottom left.
    if (y >= 232u && x + width <= 34u && y + height <= 256u)
    {
        out.assign(size_t(width) * scale * height * scale * 4u, 0u);
        return drawMark(rgba, int(width), int(height), scale, opaque, out);
    }
    // The cell its middle is in.
    const int middleX = int(x + width / 2u), middleY = int(y + height / 2u);
    const Cell *cell = nullptr;
    for (const Cell &candidate : kCells)
    {
        const int w = candidate.face ? kFaceSize : kPlateWidth, h = candidate.face ? kFaceSize : kPlateHeight;
        if (middleX >= candidate.x && middleX < candidate.x + w && middleY >= candidate.y &&
            middleY < candidate.y + h)
        {
            cell = &candidate;
        }
    }
    if (!cell)
    {
        return false;
    }
    const View view{rgba, int(width), int(height), cell->x - int(x), cell->y - int(y)};
    out.assign(size_t(width) * scale * height * scale * 4u, 0u);
    return cell->face ? drawFace(view, scale, opaque, out) : drawPlate(view, scale, opaque, out);
}

bool drawLoadingLarger(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *rgba,
                       uint32_t scale, const std::vector<uint8_t> &crossCell, std::vector<uint8_t> &out)
{
    const int cellX = kLoadingCrossX - int(x), cellY = kLoadingCrossY - int(y);
    const int w = int(width), h = int(height), s = int(scale);
    std::vector<uint8_t> face;
    if (scale < 2u || cellX < 0 || cellY < 0 || cellX + kLoadingCross > w || cellY + kLoadingCross > h ||
        crossCell.size() != size_t(kFaceSize) * kFaceSize * 4u ||
        !drawLarger(uint32_t(kCells[0].x), uint32_t(kCells[0].y), kFaceSize, kFaceSize, crossCell.data(), scale,
                    face))
    {
        return false;
    }
    const int outWidth = w * s, outHeight = h * s;
    out.assign(size_t(outWidth) * outHeight * 4u, 0u);
    const auto texel = [&](int tx, int ty) {
        return &rgba[(size_t(std::clamp(ty, 0, h - 1)) * w + std::clamp(tx, 0, w - 1)) * 4u];
    };
    // The face's circle is the cross's, a texel nearer its cell's corner
    // (its centre at 11 of 22, the cross's at 10 of 20).
    const float centreX = float(cellX) + 10.0f, centreY = float(cellY) + 10.0f;
    const int faceX = (cellX - 1) * s, faceY = (cellY - 1) * s, faceSize = kFaceSize * s;
    for (int j = 0; j < outHeight; ++j)
    {
        for (int i = 0; i < outWidth; ++i)
        {
            uint8_t *pixel = &out[(size_t(j) * outWidth + i) * 4u];
            const float px = (float(i) + 0.5f) / float(s), py = (float(j) + 0.5f) / float(s);
            if (std::hypot(px - centreX, py - centreY) > 10.5f)
            {
                const float u = px - 0.5f, v = py - 0.5f;
                const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
                const float fu = u - float(u0), fv = v - float(v0);
                const uint8_t *a = texel(u0, v0), *b = texel(u0 + 1, v0), *c = texel(u0, v0 + 1),
                              *d = texel(u0 + 1, v0 + 1);
                for (int k = 0; k < 4; ++k)
                {
                    const float top = float(a[k]) + (float(b[k]) - float(a[k])) * fu;
                    const float bottom = float(c[k]) + (float(d[k]) - float(c[k])) * fu;
                    pixel[k] = uint8_t(top + (bottom - top) * fv + 0.5f);
                }
            }
            const int fi = i - faceX, fj = j - faceY;
            if (fi < 0 || fj < 0 || fi >= faceSize || fj >= faceSize)
            {
                continue;
            }
            // The face over the picture, both with straight alpha.
            const uint8_t *over = &face[(size_t(fj) * faceSize + fi) * 4u];
            const float top = std::min(float(over[3]) / 128.0f, 1.0f);
            const float below = std::min(float(pixel[3]) / 128.0f, 1.0f) * (1.0f - top);
            const float alpha = top + below;
            if (alpha <= 0.0f)
            {
                continue;
            }
            for (int k = 0; k < 3; ++k)
            {
                pixel[k] = uint8_t((float(over[k]) * top + float(pixel[k]) * below) / alpha + 0.5f);
            }
            pixel[3] = uint8_t(alpha * 128.0f + 0.5f);
        }
    }
    return true;
}

} // namespace rg::buttons::art
