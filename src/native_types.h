#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// The draw path's own types (M6 of docs/NATIVE-ONLY.md: src/ takes none of
// the runtime's GS headers for drawing; only its stubs for the game's
// waits stay). The draw state the game's register writes describe (the
// draw-state mirror's, src/draw_state_mirror.h), and a VU1 chunk's or a 2D
// piece's mesh (src/vu1_native.h, src/native_2d.h). Field for field what
// the runtime's GS front end had, under the game's names for them.
namespace rg
{

enum PrimType : uint8_t
{
    kPrimPoint = 0,
    kPrimLine = 1,
    kPrimLineStrip = 2,
    kPrimTriangle = 3,
    kPrimTriStrip = 4,
    kPrimTriFan = 5,
    kPrimSprite = 6,
};

struct FrameReg
{
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint8_t psm = 0;
    uint32_t fbmsk = 0;
};

struct ZbufReg
{
    uint32_t zbp = 0;
    uint8_t psm = 0;
    bool zmask = false;
};

struct ScissorReg
{
    uint16_t x0 = 0;
    uint16_t x1 = 0;
    uint16_t y0 = 0;
    uint16_t y1 = 0;
};

struct Tex0Reg
{
    uint32_t tbp0 = 0;
    uint8_t tbw = 0;
    uint8_t psm = 0;
    uint8_t tw = 0;
    uint8_t th = 0;
    uint8_t tcc = 0;
    uint8_t tfx = 0;
    uint32_t cbp = 0;
    uint8_t cpsm = 0;
    uint8_t csm = 0;
    uint8_t csa = 0;
    uint8_t cld = 0;
};

struct XYOffsetReg
{
    uint16_t ofx = 0;
    uint16_t ofy = 0;
};

struct TexaReg
{
    uint8_t ta0 = 0;
    bool aem = false;
    uint8_t ta1 = 0;
};

struct TexClutReg
{
    uint8_t cbw = 0;
    uint8_t cou = 0;
    uint16_t cov = 0;
};

// One drawing context's registers (the game draws through context 1).
struct DrawContext
{
    FrameReg frame;
    ScissorReg scissor;
    Tex0Reg tex0;
    XYOffsetReg xyoffset;
    ZbufReg zbuf;
    uint64_t tex1 = 0;
    uint64_t miptbp1 = 0;
    uint64_t miptbp2 = 0;
    uint64_t clamp = 0;
    uint64_t alpha = 0;
    uint64_t test = 0;
    uint64_t fba = 0;
};

struct PrimReg
{
    PrimType type = kPrimPoint;
    bool iip = false;
    bool tme = false;
    bool fge = false;
    bool abe = false;
    bool aa1 = false;
    bool fst = false;
    bool ctxt = false;
    bool fix = false;
};

struct DrawState
{
    DrawContext context{};
    PrimReg prim{};
    TexaReg texa{};
    TexClutReg texclut{};
    bool pabe = false;
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint8_t fogR = 0;
    uint8_t fogG = 0;
    uint8_t fogB = 0;
    uint16_t textureWidth = 1;
    uint16_t textureHeight = 1;
    bool linearFilter = false;
};

// A mesh in the producer's hands: a VU1 chunk's header for the model vertex
// stage (its vertices made there), or a 2D piece's vertices in window
// coordinates.
struct NativeMesh
{
    struct Vertex
    {
        float x = 0.0f, y = 0.0f, z = 0.0f; // object space (a 2D piece's: window)
        float u = 0.0f, v = 0.0f;           // texture coordinates, before the divide by w
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f; // color before the clamp, 0-255
        float depth = -1.0f;                // a 2D piece's Z as the packet had it
    };
    // Trivially copyable: records carry it as bytes.
    struct Header
    {
        // To GS window coordinates (XYOFFSET included), row vectors:
        // clip = x * matrix[0] + y * matrix[1] + z * matrix[2] + matrix[3].
        float matrix[4][4] = {};
        // F = clamp(fog[0] + fog[3] * w, at most fog[1], at least fog[2]).
        float fog[4] = {};
        // The projected z, scaled, then capped, before its fixed point.
        float depthScale = 1.0f;
        float depthMax = std::numeric_limits<float>::max();
        float colorMax[4] = {};
        // A vertex is outside the guard band when p * bandScale + bandOffset *
        // bandScale passes 1 in x, y or z, p the projected point.
        float bandScale[4] = {};
        float bandOffset[4] = {};
        // The first triangle's winding sign; it flips from one to the next.
        float winding = 1.0f;
        // A triangle is skipped when a vertex is outside the guard band,
        // with guardBand; and when it faces away (cross * winding < 0),
        // with backface.
        uint32_t guardBand = 0u;
        uint32_t backface = 0u;
        uint32_t vertexCount = 0u;
        // A triangle list (PRIM 3) rather than a strip.
        uint32_t triangleList = 0u;
        // The program clips the chunk (slot E) at its near and far planes.
        uint32_t clips = 0u;
        float clipPlanes[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    };
    Header header{};
    std::vector<Vertex> vertices;
};

// The VU's arithmetic the native programs reproduce: no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif

// 1 / x as the VU1's DIV gives it: at 0 the most float, with x's sign.
inline float nativeReciprocal(float x)
{
    if (x != 0.0f)
        return 1.0f / x;
    return std::signbit(x) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
}

// FTOI0 and FTOI4: truncation toward zero, saturated.
inline int32_t nativeFtoi(float value, float scale)
{
    const double scaled = static_cast<double>(value) * scale;
    if (!(scaled > -2147483648.0))
        return std::numeric_limits<int32_t>::min();
    if (scaled >= 2147483647.0)
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(scaled);
}

#if defined(__clang__)
#pragma clang fp contract(on)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

} // namespace rg
