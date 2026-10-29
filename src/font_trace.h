#pragma once

#include <vector>

// Letters redrawn from their texels: the outline a glyph's coverage
// describes, traced into curves and drawn again at any size.
//
// A glyph of the game's fonts is a few hundred texels whose 16 alpha levels
// say how much of each the letter covers. Their anti-aliased distance field
// (Gustavson and Strand's, which reads where an edge crosses a texel from
// its coverage) gives the edge to a fraction of a texel. Its zero line,
// moved `thin` texels inward, is followed cell by cell (marching squares),
// its corners found where it turns sharply and squared off, and the runs
// between them fitted with cubic Bezier curves (Schneider's method), first
// smoothed along their length. Drawn back with exact coverage, the letter
// is as sharp as the screen at any scale.
namespace rg::fonttrace
{

struct Point
{
    float x, y;
};
// A closed outline, its curves flattened, in texels from the glyph's corner.
using Outline = std::vector<Point>;

struct Params
{
    // The zero line moved this far inside, in texels: a glyph's
    // semi-transparent edge looks thinner than its half-covered line.
    float thin = 0.15f;
    // The contour smoothed along its length (a Gaussian's sigma, in
    // texels), and how close to its chord a run stays to be a straight line.
    float smooth = 0.4f;
    float straight = 0.12f;
    // The turn, in degrees over 0.8 texels either side, that makes a point
    // of the contour a corner.
    float corner = 60.0f;
};

// `coverage` row by row, 0 to 1.
std::vector<Outline> trace(const std::vector<float> &coverage, int width, int height, const Params &params = {});

// The outlines' coverage (even-odd) on a grid `scale` times finer than the
// glyph's and `margin` texels wider on each side: (width + 2 margin) scale
// by (height + 2 margin) scale values, row by row.
std::vector<float> raster(const std::vector<Outline> &outlines, int width, int height, int scale, int margin);

// That raster (`inside`) grown by `radius` texels all around, round, as a
// pen of that radius run along the outlines draws it: the black border the
// game draws as copies of the glyph moved a pixel each way.
std::vector<float> grown(const std::vector<Outline> &outlines, const std::vector<float> &inside, int width,
                         int height, int scale, int margin, float radius);

} // namespace rg::fonttrace
