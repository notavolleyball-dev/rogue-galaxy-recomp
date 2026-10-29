#include "font_trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace rg::fonttrace
{
namespace
{

// The field's border, in texels: room for the zero line outside the glyph.
constexpr int kPad = 2;
constexpr double kSqrt2 = 1.4142135623730951;
// The arc either side of a point over which its turn is measured, and the
// turn that makes it a corner.
constexpr double kTurnSpan = 0.8;
constexpr double kCornerTurn = 60.0 * 3.14159265358979323846 / 180.0;
// Contour points this close (in arc) to a corner give way to it.
constexpr double kCut = 0.5;
// Schneider's tolerance, in squared texels.
constexpr double kFitError = 0.10 * 0.10;

struct P
{
    double x, y;
};
P operator-(P a, P b)
{
    return {a.x - b.x, a.y - b.y};
}
P operator+(P a, P b)
{
    return {a.x + b.x, a.y + b.y};
}
P operator*(P a, double k)
{
    return {a.x * k, a.y * k};
}
double dot(P a, P b)
{
    return a.x * b.x + a.y * b.y;
}
double cross(P a, P b)
{
    return a.x * b.y - a.y * b.x;
}
double dist(P a, P b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}
P unit(P a)
{
    const double l = std::hypot(a.x, a.y);
    return l > 1e-12 ? P{a.x / l, a.y / l} : P{0.0, 0.0};
}

// ------------------------------------------------------------ the field

struct Grid
{
    int w = 0, h = 0;
    std::vector<double> v;
    double &at(int y, int x)
    {
        return v[size_t(y) * w + x];
    }
    double at(int y, int x) const
    {
        return v[size_t(y) * w + x];
    }
};

// The distance from a texel's centre to an edge crossing it with coverage
// `a`, the edge's normal (gx, gy) (Gustavson and Strand).
double edgeDistance(double gx, double gy, double a)
{
    if (gx == 0.0 || gy == 0.0)
    {
        return 0.5 - a;
    }
    const double l = std::hypot(gx, gy);
    gx = std::fabs(gx / l);
    gy = std::fabs(gy / l);
    if (gx < gy)
    {
        std::swap(gx, gy);
    }
    const double a1 = 0.5 * gy / gx;
    if (a < a1)
    {
        return 0.5 * (gx + gy) - std::sqrt(2.0 * gx * gy * a);
    }
    if (a < 1.0 - a1)
    {
        return (0.5 - a) * gx;
    }
    return -0.5 * (gx + gy) + std::sqrt(2.0 * gx * gy * (1.0 - a));
}

// The distance from each texel to the shape (the texels with coverage), by
// brute force over its boundary: a glyph is a few hundred texels.
Grid distance(const Grid &img)
{
    const int w = img.w, h = img.h;
    Grid gx{w, h, std::vector<double>(img.v.size(), 0.0)};
    Grid gy = gx;
    for (int i = 1; i < h - 1; ++i)
    {
        for (int j = 1; j < w - 1; ++j)
        {
            const double a = img.at(i, j);
            if (!(a > 0.0 && a < 1.0))
            {
                continue;
            }
            double x = -img.at(i - 1, j - 1) - kSqrt2 * img.at(i, j - 1) - img.at(i + 1, j - 1) +
                       img.at(i - 1, j + 1) + kSqrt2 * img.at(i, j + 1) + img.at(i + 1, j + 1);
            double y = -img.at(i - 1, j - 1) - kSqrt2 * img.at(i - 1, j) - img.at(i - 1, j + 1) +
                       img.at(i + 1, j - 1) + kSqrt2 * img.at(i + 1, j) + img.at(i + 1, j + 1);
            const double l = std::hypot(x, y);
            if (l > 0.0)
            {
                x /= l;
                y /= l;
            }
            gx.at(i, j) = x;
            gy.at(i, j) = y;
        }
    }
    struct Source
    {
        int i, j;
        double a;
    };
    std::vector<Source> sources;
    for (int i = 0; i < h; ++i)
    {
        for (int j = 0; j < w; ++j)
        {
            const double a = img.at(i, j);
            if (a <= 0.0)
            {
                continue;
            }
            if (a >= 1.0)
            {
                bool interior = true;
                const int steps[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
                for (const auto &step : steps)
                {
                    const int ni = i + step[0], nj = j + step[1];
                    if (ni >= 0 && ni < h && nj >= 0 && nj < w && img.at(ni, nj) < 1.0)
                    {
                        interior = false;
                    }
                }
                if (interior)
                {
                    continue;
                }
            }
            sources.push_back({i, j, std::min(a, 1.0)});
        }
    }
    Grid d{w, h, std::vector<double>(img.v.size(), 0.0)};
    for (int i = 0; i < h; ++i)
    {
        for (int j = 0; j < w; ++j)
        {
            const double a = img.at(i, j);
            if (a >= 1.0)
            {
                continue;
            }
            double best = a > 0.0 ? edgeDistance(gx.at(i, j), gy.at(i, j), a) : 1e6;
            for (const Source &q : sources)
            {
                const int dx = j - q.j, dy = i - q.i;
                if (dx == 0 && dy == 0)
                {
                    continue;
                }
                const double v = std::sqrt(double(dx * dx + dy * dy)) + edgeDistance(dx, dy, q.a);
                best = std::min(best, v);
            }
            d.at(i, j) = std::max(0.0, best);
        }
    }
    return d;
}

// Outside positive, inside negative.
Grid signedDistance(const Grid &img)
{
    const Grid out = distance(img);
    Grid flipped = img;
    for (double &v : flipped.v)
    {
        v = 1.0 - v;
    }
    const Grid in = distance(flipped);
    Grid sd = out;
    for (size_t k = 0; k < sd.v.size(); ++k)
    {
        sd.v[k] = out.v[k] - in.v[k];
    }
    return sd;
}

// ------------------------------------------------------------ the zero line

// Its closed loops, in texels from the glyph's corner (marching squares; a
// saddle cell joins as its centre's average says).
std::vector<std::vector<P>> isolines(const Grid &sd)
{
    const int w = sd.w, h = sd.h;
    // An edge between two texel centres: horizontal (to the right of x, y)
    // or vertical (below it).
    auto edgeKey = [w, h](int vertical, int x, int y) { return (vertical * h + y) * w + x; };
    std::unordered_map<int, P> points;
    auto point = [&](int vertical, int x, int y) {
        const int key = edgeKey(vertical, x, y);
        if (!points.count(key))
        {
            if (!vertical)
            {
                const double v0 = sd.at(y, x), v1 = sd.at(y, x + 1);
                points[key] = {x + v0 / (v0 - v1) + 0.5 - kPad, y + 0.5 - kPad};
            }
            else
            {
                const double v0 = sd.at(y, x), v1 = sd.at(y + 1, x);
                points[key] = {x + 0.5 - kPad, y + v0 / (v0 - v1) + 0.5 - kPad};
            }
        }
        return key;
    };
    std::vector<std::pair<int, int>> segments;
    std::unordered_map<int, std::vector<int>> adjacent;
    for (int y = 0; y < h - 1; ++y)
    {
        for (int x = 0; x < w - 1; ++x)
        {
            const double c[4] = {sd.at(y, x), sd.at(y, x + 1), sd.at(y + 1, x + 1), sd.at(y + 1, x)};
            bool in[4];
            int count = 0;
            for (int k = 0; k < 4; ++k)
            {
                in[k] = c[k] < 0.0;
                count += in[k];
            }
            if (count == 0 || count == 4)
            {
                continue;
            }
            const int edges[4][3] = {{0, x, y}, {1, x + 1, y}, {0, x, y + 1}, {1, x, y}};
            int crossings[4], n = 0;
            for (int k = 0; k < 4; ++k)
            {
                if (in[k] != in[(k + 1) % 4])
                {
                    crossings[n++] = k;
                }
            }
            std::pair<int, int> pairs[2];
            int pairCount = 0;
            if (n == 2)
            {
                pairs[pairCount++] = {crossings[0], crossings[1]};
            }
            else if (in[0] == ((c[0] + c[1] + c[2] + c[3]) / 4.0 < 0.0))
            {
                pairs[pairCount++] = {0, 1};
                pairs[pairCount++] = {2, 3};
            }
            else
            {
                pairs[pairCount++] = {3, 0};
                pairs[pairCount++] = {1, 2};
            }
            for (int k = 0; k < pairCount; ++k)
            {
                const auto &ea = edges[pairs[k].first];
                const auto &eb = edges[pairs[k].second];
                const int a = point(ea[0], ea[1], ea[2]);
                const int b = point(eb[0], eb[1], eb[2]);
                segments.push_back({a, b});
                adjacent[a].push_back(int(segments.size()) - 1);
                adjacent[b].push_back(int(segments.size()) - 1);
            }
        }
    }
    std::vector<bool> used(segments.size(), false);
    std::vector<std::vector<P>> loops;
    for (size_t s = 0; s < segments.size(); ++s)
    {
        if (used[s])
        {
            continue;
        }
        used[s] = true;
        const int start = segments[s].first;
        int current = segments[s].second;
        std::vector<P> loop{points[start]};
        while (current != start)
        {
            loop.push_back(points[current]);
            int next = -1;
            for (const int k : adjacent[current])
            {
                if (!used[k])
                {
                    next = k;
                    break;
                }
            }
            if (next < 0)
            {
                break;
            }
            used[next] = true;
            current = segments[next].first == current ? segments[next].second : segments[next].first;
        }
        std::vector<P> clean{loop[0]};
        for (size_t k = 1; k < loop.size(); ++k)
        {
            if (dist(loop[k], clean.back()) > 0.02)
            {
                clean.push_back(loop[k]);
            }
        }
        if (clean.size() > 2 && dist(clean.front(), clean.back()) <= 0.02)
        {
            clean.pop_back();
        }
        if (clean.size() >= 3)
        {
            loops.push_back(std::move(clean));
        }
    }
    return loops;
}

// ------------------------------------------------------------ the curves

// A closed contour by arc length.
struct Loop
{
    std::vector<P> p;
    std::vector<double> s;
    double length = 0.0;

    explicit Loop(std::vector<P> points) : p(std::move(points))
    {
        s.push_back(0.0);
        for (size_t i = 1; i < p.size(); ++i)
        {
            s.push_back(s.back() + dist(p[i - 1], p[i]));
        }
        length = s.back() + dist(p.back(), p.front());
    }
    size_t n() const
    {
        return p.size();
    }
    P at(double arc) const
    {
        arc = std::fmod(arc, length);
        if (arc < 0.0)
        {
            arc += length;
        }
        const size_t i = size_t(std::upper_bound(s.begin(), s.end(), arc) - s.begin()) - 1u;
        const P a = p[i], b = p[(i + 1) % n()];
        const double end = i + 1 < n() ? s[i + 1] : length;
        const double t = end > s[i] ? (arc - s[i]) / (end - s[i]) : 0.0;
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }
    double arcDistance(double s0, double s1) const
    {
        const double d = std::fmod(std::fabs(s0 - s1), length);
        return std::min(d, length - d);
    }
    // (arc - from) taken forward round the loop.
    double ahead(double arc, double from) const
    {
        const double d = std::fmod(arc - from, length);
        return d < 0.0 ? d + length : d;
    }
};

// The centroid and direction of travel of the contour between two arcs.
void fitLine(const Loop &lp, double s0, double s1, P &centre, P &direction)
{
    const int k = std::max(3, int((s1 - s0) / 0.1));
    std::vector<P> pts;
    for (int i = 0; i < k; ++i)
    {
        pts.push_back(lp.at(s0 + (s1 - s0) * i / (k - 1)));
    }
    double cx = 0.0, cy = 0.0;
    for (const P &q : pts)
    {
        cx += q.x;
        cy += q.y;
    }
    cx /= k;
    cy /= k;
    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (const P &q : pts)
    {
        sxx += (q.x - cx) * (q.x - cx);
        syy += (q.y - cy) * (q.y - cy);
        sxy += (q.x - cx) * (q.y - cy);
    }
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    direction = {std::cos(angle), std::sin(angle)};
    if (dot(direction, pts.back() - pts.front()) < 0.0)
    {
        direction = direction * -1.0;
    }
    centre = {cx, cy};
}

struct Corner
{
    double arc;
    P position;
    bool tangents = false;
    P in, out;
};

// The points where the contour turns sharply, each moved to where the lines
// of its two sides meet, so a stem's corner is square.
std::vector<Corner> corners(const Loop &lp)
{
    const size_t n = lp.n();
    std::vector<double> turn(n);
    for (size_t i = 0; i < n; ++i)
    {
        const P a = lp.at(lp.s[i] - kTurnSpan), c = lp.at(lp.s[i] + kTurnSpan);
        const P u = unit(lp.p[i] - a), v = unit(c - lp.p[i]);
        turn[i] = std::atan2(std::fabs(cross(u, v)), dot(u, v));
    }
    std::vector<size_t> candidates;
    for (size_t i = 0; i < n; ++i)
    {
        if (turn[i] <= kCornerTurn)
        {
            continue;
        }
        bool peak = true;
        for (size_t j = 0; j < n && peak; ++j)
        {
            if (j != i && lp.arcDistance(lp.s[i], lp.s[j]) < kTurnSpan &&
                !(turn[i] >= turn[j] && (turn[i] > turn[j] || j > i)))
            {
                peak = false;
            }
        }
        if (peak)
        {
            candidates.push_back(i);
        }
    }
    std::vector<Corner> out;
    const size_t m = candidates.size();
    for (size_t k = 0; k < m; ++k)
    {
        const size_t i = candidates[k];
        const double previousGap = m > 1 ? lp.arcDistance(lp.s[i], lp.s[candidates[(k + m - 1) % m]]) : lp.length;
        const double nextGap = m > 1 ? lp.arcDistance(lp.s[i], lp.s[candidates[(k + 1) % m]]) : lp.length;
        const double reachIn = std::min(2.0, 0.5 * previousGap), reachOut = std::min(2.0, 0.5 * nextGap);
        Corner corner{lp.s[i], lp.p[i], false, {}, {}};
        if (reachIn > kCut + 0.3 && reachOut > kCut + 0.3)
        {
            P a, b, in, outward;
            fitLine(lp, lp.s[i] - reachIn, lp.s[i] - kCut, a, in);
            fitLine(lp, lp.s[i] + kCut, lp.s[i] + reachOut, b, outward);
            const double cr = cross(in, outward);
            if (std::fabs(cr) > 0.3)
            {
                const double t = cross(b - a, outward) / cr;
                const P x = a + in * t;
                if (dist(x, lp.p[i]) < 1.0)
                {
                    corner.position = x;
                    corner.tangents = true;
                    corner.in = in;
                    corner.out = outward;
                }
            }
        }
        out.push_back(corner);
    }
    return out;
}

using Bezier = std::array<P, 4>;

P bez(const Bezier &b, double t)
{
    const double mt = 1.0 - t;
    return b[0] * (mt * mt * mt) + b[1] * (3.0 * mt * mt * t) + b[2] * (3.0 * mt * t * t) + b[3] * (t * t * t);
}

P bezD(const Bezier &b, double t)
{
    const double mt = 1.0 - t;
    return ((b[1] - b[0]) * (mt * mt) + (b[2] - b[1]) * (2.0 * mt * t) + (b[3] - b[2]) * (t * t)) * 3.0;
}

P bezDD(const Bezier &b, double t)
{
    const double mt = 1.0 - t;
    return ((b[2] - b[1] * 2.0 + b[0]) * mt + (b[3] - b[2] * 2.0 + b[1]) * t) * 6.0;
}

Bezier generate(const std::vector<P> &d, const std::vector<double> &u, P t1, P t2)
{
    const P first = d.front(), last = d.back();
    double c00 = 0.0, c01 = 0.0, c11 = 0.0, x0 = 0.0, x1 = 0.0;
    for (size_t i = 0; i < d.size(); ++i)
    {
        const double t = u[i], mt = 1.0 - t;
        const P a1 = t1 * (3.0 * mt * mt * t);
        const P a2 = t2 * (3.0 * mt * t * t);
        c00 += dot(a1, a1);
        c01 += dot(a1, a2);
        c11 += dot(a2, a2);
        const P base = first * (mt * mt * mt + 3.0 * mt * mt * t) + last * (3.0 * mt * t * t + t * t * t);
        const P rest = d[i] - base;
        x0 += dot(a1, rest);
        x1 += dot(a2, rest);
    }
    const double det = c00 * c11 - c01 * c01;
    const double chord = dist(first, last);
    double left = 0.0, right = 0.0;
    if (std::fabs(det) > 1e-12)
    {
        left = (x0 * c11 - x1 * c01) / det;
        right = (c00 * x1 - c01 * x0) / det;
    }
    // A handle that runs away (or back) gives way to a third of the chord.
    if (left < 1e-6 * chord || right < 1e-6 * chord || left > 1.5 * chord || right > 1.5 * chord)
    {
        left = right = chord / 3.0;
    }
    return {first, first + t1 * left, last + t2 * right, last};
}

double maxError(const std::vector<P> &d, const Bezier &b, const std::vector<double> &u, size_t &split)
{
    double worst = 0.0;
    split = d.size() / 2;
    for (size_t i = 1; i + 1 < d.size(); ++i)
    {
        const double e = std::pow(dist(bez(b, u[i]), d[i]), 2.0);
        if (e >= worst)
        {
            worst = e;
            split = i;
        }
    }
    return worst;
}

std::vector<double> reparameterize(const std::vector<P> &d, const Bezier &b, const std::vector<double> &u)
{
    std::vector<double> out;
    for (size_t i = 0; i < d.size(); ++i)
    {
        const double t = u[i];
        const P q = bez(b, t), q1 = bezD(b, t), q2 = bezDD(b, t);
        const double num = dot(q - d[i], q1);
        const double den = dot(q1, q1) + dot(q - d[i], q2);
        out.push_back(std::fabs(den) > 1e-12 ? std::clamp(t - num / den, 0.0, 1.0) : t);
    }
    return out;
}

void fitCubic(const std::vector<P> &d, P t1, P t2, std::vector<Bezier> &out, int depth = 0)
{
    if (d.size() == 2 || depth > 24)
    {
        const double k = dist(d.front(), d.back()) / 3.0;
        out.push_back({d.front(), d.front() + t1 * k, d.back() + t2 * k, d.back()});
        return;
    }
    std::vector<double> u{0.0};
    for (size_t i = 1; i < d.size(); ++i)
    {
        u.push_back(u.back() + dist(d[i - 1], d[i]));
    }
    if (u.back() <= 0.0)
    {
        return;
    }
    for (double &x : u)
    {
        x /= u.back();
    }
    Bezier b = generate(d, u, t1, t2);
    size_t split = 0;
    double error = maxError(d, b, u, split);
    if (error < kFitError)
    {
        out.push_back(b);
        return;
    }
    if (error < kFitError * 16.0)
    {
        for (int k = 0; k < 8; ++k)
        {
            u = reparameterize(d, b, u);
            b = generate(d, u, t1, t2);
            error = maxError(d, b, u, split);
            if (error < kFitError)
            {
                out.push_back(b);
                return;
            }
        }
    }
    const P tc = unit(unit(d[split - 1] - d[split]) + unit(d[split] - d[split + 1]));
    fitCubic(std::vector<P>(d.begin(), d.begin() + split + 1), t1, tc, out, depth + 1);
    fitCubic(std::vector<P>(d.begin() + split, d.end()), tc * -1.0, t2, out, depth + 1);
}

bool straight(const std::vector<P> &d, double tolerance)
{
    const P a = d.front(), b = d.back();
    if (dist(a, b) < 0.5)
    {
        return false;
    }
    const P v = unit(b - a);
    for (size_t i = 1; i + 1 < d.size(); ++i)
    {
        if (std::fabs(cross(v, d[i] - a)) >= tolerance)
        {
            return false;
        }
    }
    return true;
}

// A Gaussian average along the arc; an open run's ends stay put.
std::vector<P> smooth(const std::vector<P> &d, bool closed, double sigma)
{
    if (sigma <= 0.0 || d.size() < 3)
    {
        return d;
    }
    std::vector<double> s{0.0};
    for (size_t i = 1; i < d.size(); ++i)
    {
        s.push_back(s.back() + dist(d[i - 1], d[i]));
    }
    const double length = s.back() + (closed ? dist(d.back(), d.front()) : 0.0);
    std::vector<P> out;
    for (size_t i = 0; i < d.size(); ++i)
    {
        if (!closed && (i == 0 || i + 1 == d.size()))
        {
            out.push_back(d[i]);
            continue;
        }
        double wx = 0.0, wy = 0.0, ws = 0.0;
        for (size_t j = 0; j < d.size(); ++j)
        {
            double ds = std::fabs(s[j] - s[i]);
            if (closed)
            {
                ds = std::min(ds, length - ds);
            }
            else if (ds > 3.0 * sigma)
            {
                continue;
            }
            const double weight = std::exp(-0.5 * (ds / sigma) * (ds / sigma));
            wx += weight * d[j].x;
            wy += weight * d[j].y;
            ws += weight;
        }
        out.push_back({wx / ws, wy / ws});
    }
    return out;
}

std::vector<Bezier> traceLoop(const std::vector<P> &points, const Params &params)
{
    const Loop lp(points);
    const std::vector<Corner> cs = corners(lp);
    std::vector<Bezier> curves;
    if (cs.empty())
    {
        std::vector<P> d = smooth(points, true, params.smooth);
        const Loop smoothed(d);
        const P t = unit(smoothed.at(0.5) - smoothed.at(-0.5));
        d.push_back(d.front());
        fitCubic(d, t, t * -1.0, curves);
        return curves;
    }
    const size_t m = cs.size();
    for (size_t k = 0; k < m; ++k)
    {
        const Corner &a = cs[k];
        const Corner &b = cs[(k + 1) % m];
        const double span = m > 1 ? lp.ahead(b.arc, a.arc) : lp.length;
        std::vector<size_t> order;
        for (size_t i = 0; i < lp.n(); ++i)
        {
            const double ahead = lp.ahead(lp.s[i], a.arc);
            if (kCut < ahead && ahead < span - kCut)
            {
                order.push_back(i);
            }
        }
        std::sort(order.begin(), order.end(),
                  [&](size_t x, size_t y) { return lp.ahead(lp.s[x], a.arc) < lp.ahead(lp.s[y], a.arc); });
        std::vector<P> run{a.position};
        for (const size_t i : order)
        {
            run.push_back(lp.p[i]);
        }
        run.push_back(b.position);
        const std::vector<P> d = smooth(run, false, params.smooth);
        if (d.size() == 2 || straight(d, params.straight))
        {
            const P v = b.position - a.position;
            curves.push_back({a.position, a.position + v * (1.0 / 3.0), a.position + v * (2.0 / 3.0), b.position});
            continue;
        }
        const P t1 = a.tangents ? a.out : unit(d[1] - a.position);
        const P t2 = b.tangents ? b.in * -1.0 : unit(d[d.size() - 2] - b.position);
        fitCubic(d, t1, t2, curves);
    }
    return curves;
}

} // namespace

std::vector<Outline> trace(const std::vector<float> &coverage, int width, int height, const Params &params)
{
    Grid img{width + 2 * kPad, height + 2 * kPad, {}};
    img.v.assign(size_t(img.w) * img.h, 0.0);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            img.at(y + kPad, x + kPad) = coverage[size_t(y) * width + x];
        }
    }
    Grid sd = signedDistance(img);
    for (double &v : sd.v)
    {
        v += params.thin;
    }
    std::vector<Outline> outlines;
    for (const std::vector<P> &loop : isolines(sd))
    {
        const std::vector<Bezier> curves = traceLoop(loop, params);
        Outline outline;
        constexpr int kSteps = 16;
        for (const Bezier &b : curves)
        {
            for (int i = 0; i < kSteps; ++i)
            {
                const P q = bez(b, double(i) / kSteps);
                outline.push_back({float(q.x), float(q.y)});
            }
        }
        if (outline.size() >= 3)
        {
            outlines.push_back(std::move(outline));
        }
    }
    return outlines;
}

std::vector<float> raster(const std::vector<Outline> &outlines, int width, int height, int scale, int margin)
{
    const int ow = (width + 2 * margin) * scale, oh = (height + 2 * margin) * scale;
    struct Edge
    {
        double y0, y1, x0, slope;
    };
    std::vector<std::vector<Edge>> rows(static_cast<size_t>(oh));
    for (const Outline &poly : outlines)
    {
        for (size_t k = 0; k < poly.size(); ++k)
        {
            double x0 = (poly[k].x + margin) * scale, y0 = (poly[k].y + margin) * scale;
            double x1 = (poly[(k + 1) % poly.size()].x + margin) * scale;
            double y1 = (poly[(k + 1) % poly.size()].y + margin) * scale;
            if (y0 == y1)
            {
                continue;
            }
            if (y0 > y1)
            {
                std::swap(x0, x1);
                std::swap(y0, y1);
            }
            const Edge e{y0, y1, x0, (x1 - x0) / (y1 - y0)};
            for (int r = std::max(0, int(y0)); r < std::min(oh, int(y1) + 1); ++r)
            {
                rows[size_t(r)].push_back(e);
            }
        }
    }
    std::vector<float> cov(size_t(ow) * oh, 0.0f);
    constexpr int kSubRows = 4;
    constexpr double kWeight = 1.0 / kSubRows;
    std::vector<double> xs;
    for (int oy = 0; oy < oh; ++oy)
    {
        float *row = &cov[size_t(oy) * ow];
        for (int k = 0; k < kSubRows; ++k)
        {
            const double sy = oy + (k + 0.5) / kSubRows;
            xs.clear();
            for (const Edge &e : rows[size_t(oy)])
            {
                if (e.y0 <= sy && sy < e.y1)
                {
                    xs.push_back(e.x0 + (sy - e.y0) * e.slope);
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2)
            {
                const double a = std::max(0.0, xs[i]), b = std::min(double(ow), xs[i + 1]);
                if (b <= a)
                {
                    continue;
                }
                const int ia = int(a), ib = int(b);
                if (ia == ib)
                {
                    row[ia] += float((b - a) * kWeight);
                    continue;
                }
                row[ia] += float((ia + 1 - a) * kWeight);
                for (int x = ia + 1; x < ib; ++x)
                {
                    row[x] += float(kWeight);
                }
                if (ib < ow)
                {
                    row[ib] += float((b - ib) * kWeight);
                }
            }
        }
    }
    for (float &v : cov)
    {
        v = std::min(v, 1.0f);
    }
    return cov;
}

std::vector<float> grown(const std::vector<Outline> &outlines, const std::vector<float> &inside, int width,
                         int height, int scale, int margin, float radius)
{
    const int ow = (width + 2 * margin) * scale, oh = (height + 2 * margin) * scale;
    // The distance from each pixel's centre to the outlines, near them.
    std::vector<float> nearest(size_t(ow) * oh, 1e9f);
    const double reach = radius + 1.0 / scale;
    for (const Outline &poly : outlines)
    {
        for (size_t k = 0; k < poly.size(); ++k)
        {
            const P a{poly[k].x, poly[k].y};
            const P b{poly[(k + 1) % poly.size()].x, poly[(k + 1) % poly.size()].y};
            const P ab = b - a;
            const double ll = dot(ab, ab);
            const int px0 = std::max(0, int(std::floor((std::min(a.x, b.x) - reach + margin) * scale)));
            const int px1 = std::min(ow - 1, int(std::ceil((std::max(a.x, b.x) + reach + margin) * scale)));
            const int py0 = std::max(0, int(std::floor((std::min(a.y, b.y) - reach + margin) * scale)));
            const int py1 = std::min(oh - 1, int(std::ceil((std::max(a.y, b.y) + reach + margin) * scale)));
            for (int py = py0; py <= py1; ++py)
            {
                for (int px = px0; px <= px1; ++px)
                {
                    const P c{(px + 0.5) / scale - margin, (py + 0.5) / scale - margin};
                    const double t = ll > 0.0 ? std::clamp(dot(c - a, ab) / ll, 0.0, 1.0) : 0.0;
                    const float d = float(dist(c, a + ab * t));
                    float &slot = nearest[size_t(py) * ow + px];
                    slot = std::min(slot, d);
                }
            }
        }
    }
    std::vector<float> out(inside.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float pen = std::clamp((radius - nearest[i]) * scale + 0.5f, 0.0f, 1.0f);
        out[i] = std::max(inside[i], pen);
    }
    return out;
}

} // namespace rg::fonttrace
