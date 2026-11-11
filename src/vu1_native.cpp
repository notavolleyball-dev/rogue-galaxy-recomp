#include "vu1_native.h"
#include "fminmax.h"

#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "gfx/native/native_records.h"
#include "gif_events.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

// The strip programs, piece by piece (byte addresses):
//   the MSCAL at 0x320 only ends; each mesh chunk is UNPACKed and then run by
//   an MSCNT from 0x328, through
//   A 0x328  the chunk header at TOP (PRIM, vertex count, vertex tag, the
//            sections' offsets), the GIF header of the output (template
//            quad 0, PRIM, PRMODE from quad 87.x, the vertex tag with quad
//            86), the guard band (quad 109 = 17 with xy times 16.xy, 110 = 18);
//   B 0x960  int16 positions, normals and UVs to floats: x / 32768 times the
//            mesh scales in quads 77, 78, 79 (the skinning B then sums the
//            position over the vertex's bones);
//   F 0x39d0 nothing unless quad 88.xy is set (scrolling UVs), or a
//            sphere map from the normals, or the wave (9df36d6a): the
//            positions turned in x and y about (0, quad 84.x), by an angle
//            growing with y, or a reflection map (ad209cd3);
//   C 0x10e0 the vertex colors: the int16 color / 32768 * 256 times
//            quad 82 * quad 42 + quad 83 * 128 (82.w scaled by 81.w), or
//            that color alone, or lights on the normal;
//   E 0x2620 with quad 81.x & 2 only: clipping, before D;
//   D 0x1900 per vertex: clip = position * quads 68-71 (the matrix lands in
//            screen space), Q = 1/w, XYZ in 12.4, F the fog clamp(40.x +
//            40.w * w, 40.y, 40.z) in XYZF2's w, ST = UV * Q, the color
//            clamped to quad 39; ADC as its Cull says; then FOGCOL (quad 41
//            behind tag quad 19, its low word zeroed with quad 87.y & 0x10)
//            is kicked, then the chunk.
// Every chunk of these is drawn by the renderer's model vertex stage
// (native_model.vert) from the inputs and constants made here; this file no
// longer makes a vertex. The other programs (Kind) are written out below,
// each by itself: the outlines, shadow volumes and particles for the same
// stage (outlineDraw, shadowMeshes, particleMesh), buildShadow as the
// shadows' packet fallback, runBase for what
// they leave in VU1 memory.
// The arithmetic runs in the VU's rounding (toward zero), set around each
// entry point (VuRounding): the compiler must not move float work across it.
#pragma STDC FENV_ACCESS ON

namespace rg::vu1native
{
namespace
{

constexpr uint32_t kEntry = 0x320u;
constexpr uint32_t kOverlayEntry = 0x960u;
constexpr uint32_t kBaseEntry = 0x000u;

// Round toward zero while it lives, as VU1Interpreter::run sets it for the
// VU1: the products, sums and the divide then come out as the VU1's, bit for
// bit, done in its order. (Round to nearest left Z a unit off at times, which
// a pass drawn at nearly the same depth turned into missing pixels.) And
// denormals as zero, in and out (DAZ, FTZ), as the VU has them: the overlay
// sprites add a denormal to -2^24, which must stay -2^24.
class VuRounding
{
public:
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    VuRounding() : m_saved(_mm_getcsr()) { _mm_setcsr((m_saved & ~0x6000u) | 0x6000u | 0x8040u); }
    ~VuRounding() { _mm_setcsr(m_saved); }

private:
    uint32_t m_saved;
#else
    VuRounding() : m_saved(std::fegetround()) { std::fesetround(FE_TOWARDZERO); }
    ~VuRounding() { std::fesetround(m_saved); }

private:
    int m_saved;
#endif
};

struct Piece
{
    uint32_t start;
    uint32_t end;
    uint64_t hash;
};

// Slots A and F, the same in every strip program met so far.
constexpr Piece kCommon[] = {
    {0x0320u, 0x05d8u, 0x07151c2597283286ull},
};
constexpr const char *kCommonNames[] = {"A"};

// The transporter beam's strips: its own A (c8143ab4: B, F, E, C and D in
// that order, and no guard band setup for clipping), E a stub that returns
// (2aa11602: nothing is clipped), F b1d29245 (the UV is a projection of the
// position) and D 1bd1b5c0 (below).
constexpr Piece kBeamA = {0x0320u, 0x0530u, 0xc8143ab4bcadef13ull};
constexpr Piece kBeamE = {0x2620u, 0x2630u, 0x2aa116029d0c7d60ull};

// The F pieces (A calls F after B, before C): d3f6f631 scrolls the UVs by
// quad 88.xy (declined when set), 150031ee replaces them by a sphere map,
// 9df36d6a bends the positions (below, the wave), ad209cd3 replaces the UVs
// by a reflection map.
struct UvPiece
{
    Piece f;
    Uv uv;
};
constexpr UvPiece kUvs[] = {
    {{0x39d0u, 0x3a68u, 0xd3f6f6312a13ff1cull}, Uv::Plain},
    {{0x39d0u, 0x3b70u, 0x150031ee1534d185ull}, Uv::SphereMap},
    {{0x39d0u, 0x3b98u, 0x9df36d6a55fdb496ull}, Uv::Wave},
    {{0x39d0u, 0x3d10u, 0xad209cd3feea3c5dull}, Uv::Reflect},
    // UNVERIFIED: no recording holds this piece; read from the executable's code only.
    {{0x39d0u, 0x3ab0u, 0x0594a06392e0fbb3ull}, Uv::Ramp},
    // The beam's: recorded (a780f947d3b7787a, 31 vertices).
    {{0x39d0u, 0x3c08u, 0xb1d2924548c74d41ull}, Uv::Beam},
    // UNVERIFIED (nothing in the game was seen to run it): the positions pushed out along the normals.
    {{0x39d0u, 0x3c48u, 0xa04c11ce76242bb6ull}, Uv::Swell},
};

// The B pieces. A calls B at 0x960, or with quad 76.z set at 0x960 plus
// the chunk's TOP+1.w pairs: af62ff64 has the one entry; 26a933a5 has four,
// 0 one bone, 2 two bones, 4 and 6 four (skin()).
// 542b235c (the outlines' and the shadow volumes') has the same four, its
// one-bone path positions only; its four-bone path (0x9b0, the skinned
// shadow volumes' 4 and 6) skins as 26a933a5's and only scales the normals.
struct DecodePiece
{
    Piece b;
    Decode decode;
    uint8_t maxBones;
    bool raw = false;         // the positions, normals and UVs arrive as floats and are only skinned
    bool skinNormals = true;  // false: the normals are left as they are
};
constexpr DecodePiece kDecodes[] = {
    {{0x0960u, 0x0a38u, 0xaf62ff64ac67b45full}, Decode::Plain, 0},
    {{0x0960u, 0x1020u, 0x26a933a58f99a9daull}, Decode::Skinned, 4},
    {{0x0960u, 0x0f30u, 0x542b235c4775a710ull}, Decode::Skinned, 4},
    // Positions only, one entry (a shadow volume B).
    {{0x0960u, 0x09e0u, 0xa293001f25f94532ull}, Decode::Plain, 0},
    // UNVERIFIED (nothing was seen to run them): skinning of positions (and
    // for 36d686cd the normals) the EE uploaded as floats, in place; the
    // entries as 26a933a5's (1, 2, 4 bones).
    {{0x0960u, 0x0e58u, 0x8912329060d1df1eull}, Decode::Skinned, 4, true, false},
    {{0x0960u, 0x0f98u, 0x36d686cd420c9469ull}, Decode::Skinned, 4, true, true},
    // The unskinned float meshes' B (the table's B[0] for the strip types,
    // NATIVE-ONLY-NOTES.md "The program a group draws with"): a bare JR,
    // the EE's floats taken as they are.
    {{0x0960u, 0x0978u, 0x857fbbc2794f6128ull}, Decode::Plain, 0, true, false},
};

// The outline program's A (header with PRIM 1, lines; calls B, then C) and
// C (3689bac9, which also fills slot D's space).
constexpr Piece kOutlineA = {0x0320u, 0x04e8u, 0x3f6df595be6e31b7ull};
constexpr Piece kOutlineC = {0x10e0u, 0x1e70u, 0x3689bac97cae3c73ull};

// The base piece: the setup each object's draw starts with (runBase()).
constexpr Piece kBase = {0x0000u, 0x02c0u, 0x12ad73d26e1badecull};

// The overlay sprites: B 6e42dac6 entered by an MSCAL at its start.
constexpr Piece kOverlayB = {0x0960u, 0x0b38u, 0x6e42dac662887054ull};

// The particle program's A (header with PRIM 4; calls D only) and D
// (8ecadb3a).
constexpr Piece kParticleA = {0x0320u, 0x04a8u, 0x0b1031aeb003ccb0ull};
constexpr Piece kParticleD = {0x1900u, 0x2890u, 0x8ecadb3a83d5e584ull};

// The projected texture program: A 4beba151 (calls B, F, E with 81.x & 2,
// then C), F 15be21a5 (the UVs and color) and C 3b890d00 (the rest).
constexpr Piece kProjectorA = {0x0320u, 0x05c0u, 0x4beba151fbf23f60ull};
constexpr Piece kProjectorF = {0x39d0u, 0x3af8u, 0x15be21a50caaa048ull};
constexpr Piece kProjectorC = {0x10e0u, 0x1b00u, 0x3b890d00703404b6ull};

// The shadow volume program's A (header with PRIM 5, fans; calls B, then C)
// and C (dc8dbfe4).
constexpr Piece kShadowA = {0x0320u, 0x0500u, 0x821b638f42fcaa6eull};
constexpr Piece kShadowC = {0x10e0u, 0x1938u, 0xdc8dbfe4905cb635ull};

// The C pieces: both build the material color, quad 82 * 42 + 83 * 128
// (82.w scaled by 81.w), or with 87.y & 0x200 82 * 43 and no ambient;
// 70362be4 multiplies it by each vertex's int16 color / 32768 * 256 (and
// has a loop of its own with 87.y & 0x100, not read yet), 780d503e gives
// it to every vertex as it is.
struct ColorPiece
{
    Piece c;
    Color color;
};
constexpr ColorPiece kColors[] = {
    {{0x10e0u, 0x1328u, 0x70362be426b17f94ull}, Color::PerVertex},
    {{0x10e0u, 0x11b0u, 0x780d503e649c9116ull}, Color::Material},
    {{0x10e0u, 0x1240u, 0x91f542d069307d6cull}, Color::Lit},
    // 91f542d0's lighting times the vertex colors (87.y & 0x100: a loop of
    // its own, not read).
    {{0x10e0u, 0x14a0u, 0x2460e5281034f152ull}, Color::LitVertex},
    // UNVERIFIED (no recording holds it; the oracle's synthetic check agrees
    // with the interpreter): 91f542d0 and four point lights.
    {{0x10e0u, 0x1720u, 0x127ca0b4834b2363ull}, Color::LitPoint},
    // UNVERIFIED likewise: that, with each vertex's int16 color c (/ 32768 *
    // 256): max(lit * c, -(lit + c * 128)), or with 87.y & 0x100 the signs
    // swapped: max(-(lit * c), lit + c * 128).
    {{0x10e0u, 0x1848u, 0xe3bca52a27b10195ull}, Color::LitPointVertex},
};

// The D pieces. (a786efabfa469ada is also the name of its compiled routine,
// the VIF1 thread's busiest in docs/STATUS.md.)
struct Finish
{
    Piece d;
    Cull cull;
    bool depthBounds;
    bool bounds = true; // false: the depth is scaled but 98 and 99 are left alone
    bool affine = false; // ST kept as it is, Q written as 1
    bool rawColor = false; // the colors are floats read from the C section, no ST written
    bool white = false;  // the color is (128, 128, 128) and the vertex's alpha
};
constexpr Finish kFinishes[] = {
    {{0x1900u, 0x20c8u, 0xa786efabfa469adaull}, Cull::GuardBandAndBackface, false},
    {{0x1900u, 0x1c38u, 0x126a58a39a8f944aull}, Cull::GuardBand, false},
    {{0x1900u, 0x1bc0u, 0x4b7b81f2fe1beeefull}, Cull::None, false},
    // No culling; z times 87.z, at most 1.y; the bounds into 98 and 99.
    {{0x1900u, 0x1b98u, 0xb8ea8ed53805b0c4ull}, Cull::None, true},
    // The guard band (its z before the scale), and that depth and bounds.
    {{0x1900u, 0x1cb8u, 0x634107b0f8a88c88ull}, Cull::GuardBand, true},
    // The guard band and backfaces, and that depth and bounds; triangle
    // lists have a path of their own (declined).
    {{0x1900u, 0x21e0u, 0x93cfd2f5a1056991ull}, Cull::GuardBandAndBackface, true},
    // UNVERIFIED (no recording holds it; read from the code only): the guard
    // band and the scaled depth as 634107b0, without the bounds, and ST not
    // multiplied by Q (the GS sees Q = 1: the texture is not perspective-correct).
    {{0x1900u, 0x1be0u, 0x2feaa9a15b81d8baull}, Cull::GuardBand, true, false, true},
    // UNVERIFIED: a786efab's guard band and backfaces, but the colors are floats
    // read from the colors section (103.w) and no ST is written (an untextured
    // draw); strips (lists are declined as they are there).
    {{0x1900u, 0x2250u, 0x312d076bfbf37fb2ull}, Cull::GuardBandAndBackface, false, true, false, true},
    // The beam's D, recorded: that, with the color (128, 128, 128) and the
    // vertex's alpha, clamped to quad 39.
    {{0x1900u, 0x1be0u, 0x1bd1b5c0cd88b246ull}, Cull::GuardBand, true, false, true, false, true},
};

// The program of these pieces, made once and kept: its name says what each
// piece does ("strip, guard band, one color").
const Program *intern(const Program &shape)
{
    static std::mutex mutex;
    static std::deque<Program> programs;
    static std::deque<std::string> names;
    const std::lock_guard<std::mutex> lock(mutex);
    for (const Program &program : programs)
    {
        if (program.cull == shape.cull && program.color == shape.color && program.depthBounds == shape.depthBounds && program.bounds == shape.bounds && program.affine == shape.affine && program.rawColor == shape.rawColor && program.white == shape.white && program.rawFloats == shape.rawFloats &&
            program.skinNormals == shape.skinNormals &&
            program.decode == shape.decode && program.maxBones == shape.maxBones && program.kind == shape.kind &&
            program.uv == shape.uv)
        {
            return &program;
        }
    }
    std::string &name = names.emplace_back(shape.kind == Kind::Outline  ? "outline"
                                           : shape.kind == Kind::Shadow ? "shadow volume"
                                           : shape.kind == Kind::Overlay ? "overlay sprites"
                                           : shape.kind == Kind::Base    ? "object setup"
                                           : shape.kind == Kind::Particles ? "particles"
                                           : shape.kind == Kind::Projector ? "projected texture"
                                                                        : "strip");
    name += shape.kind != Kind::Strip                  ? ""
            : shape.cull == Cull::GuardBandAndBackface ? ", guard band and backfaces"
            : shape.cull == Cull::GuardBand            ? ", guard band"
                                                       : "";
    name += shape.depthBounds ? (shape.bounds ? ", depth and bounds" : shape.white ? ", depth, affine texture, white" : ", depth, affine texture (unverified)") : "";
    name += shape.color == Color::Material && shape.kind == Kind::Strip ? ", one color"
            : shape.color == Color::Lit                                  ? ", lit"
            : shape.color == Color::LitVertex                            ? ", lit vertex colors"
            : shape.color == Color::LitPoint                             ? ", lit with point lights (unverified)"
            : shape.color == Color::LitPointVertex                       ? ", lit with point lights, vertex colors (unverified)"
                                                                         : "";
    name += shape.uv == Uv::SphereMap ? ", sphere map" : shape.uv == Uv::Wave ? ", wave" : shape.uv == Uv::Reflect ? ", reflection map" : shape.uv == Uv::Ramp ? ", ramp (unverified)" : shape.uv == Uv::Beam ? ", projected UVs (beam)" : shape.uv == Uv::Swell ? ", swell (unverified)" : "";
    name += shape.decode == Decode::Skinned ? ", skinned" : "";
    name += shape.rawColor ? ", float colors, untextured (unverified)" : "";
    name += shape.rawFloats ? " floats (unverified)" : "";
    Program &program = programs.emplace_back(shape);
    program.name = name.c_str();
    return &program;
}

uint64_t fnv(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

bool matches(const uint8_t *code, const Piece &piece)
{
    return fnv(code + piece.start, piece.end - piece.start) == piece.hash;
}

// The entry of `table` whose piece (the member `piece`) is in `code`, or null.
template <typename Entry, size_t N>
const Entry *present(const uint8_t *code, const Entry (&table)[N], Piece Entry::*piece)
{
    for (const Entry &entry : table)
    {
        if (matches(code, entry.*piece))
        {
            return &entry;
        }
    }
    return nullptr;
}

struct Vec4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

class VuData
{
public:
    explicit VuData(const uint8_t *data) : m_data(data) {}

    Vec4 quad(uint32_t q) const
    {
        Vec4 v;
        std::memcpy(&v, bytes(q), sizeof(v));
        return v;
    }
    int32_t word(uint32_t q, uint32_t lane) const
    {
        int32_t v;
        std::memcpy(&v, bytes(q) + lane * 4u, sizeof(v));
        return v;
    }
    // ILW reads the low 16 bits of a word.
    uint32_t half(uint32_t q, uint32_t lane) const { return static_cast<uint32_t>(word(q, lane)) & 0xFFFFu; }
    const uint8_t *bytes(uint32_t q) const { return m_data + (q & 1023u) * 16u; }

private:
    const uint8_t *m_data;
};

float itof15(int32_t v)
{
    return static_cast<float>(v) / 32768.0f;
}

float fromBits(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// How many bones the skinning B's entry sums (A enters B at 0x960 plus
// TOP+1.w pairs when quad 76.z is set): 1, 2 or 4 (entries 4 and 6 branch
// to the same code), or 0 for another entry.
uint32_t bonesOf(const Program &program, const VuData &vu, uint32_t top)
{
    // The skinning Bs' entry table (26a933a5, 542b235c, 89123290, 36d686cd
    // alike): pairs 0, 2, 4, 6 branch to the 1-, 2-, 4- and 4-bone loops; an
    // odd entry runs its pair's NOP and falls into the next branch (1: 2
    // bones, 3 and 5: 4); 7 lands on a JR (nothing decoded, not drawn).
    const uint32_t entry = vu.half(76, 2) != 0u ? vu.half(top + 1u, 3) : 0u;
    const uint32_t bones = entry == 0u ? 1u : entry <= 2u ? 2u : entry <= 6u ? 4u : 0u;
    return bones <= program.maxBones ? bones : 0u;
}

// The skinning B: the position (w 1) scaled by each bone's weight (an int
// / 255), times the bone's matrix (quads 900 + 2 * index, rows 0-3, xyz),
// summed in ACC bone by bone and row by row. The indices are the low halves
// of the quad at `weights`, the weights the words of the next.
void skin(const VuData &vu, uint32_t weights, uint32_t bones, float position[3])
{
    const float scale = fromBits(0x3b808081u); // the program's I: 1/255
    float acc[3] = {};
    bool first = true;
    for (uint32_t b = 0; b < bones; ++b)
    {
        const float w = static_cast<float>(vu.word(weights + 1u, b)) * scale;
        const float scaled[4] = {position[0] * w, position[1] * w, position[2] * w, w};
        const uint32_t matrix = 900u + ((vu.half(weights, b) * 2u) & 0xFFFFu);
        for (uint32_t row = 0; row < 4u; ++row)
        {
            const Vec4 m = vu.quad(matrix + row);
            const float lanes[3] = {m.x, m.y, m.z};
            for (int i = 0; i < 3; ++i)
            {
                const float product = lanes[i] * scaled[row];
                acc[i] = first ? product : acc[i] + product;
            }
            first = false;
        }
    }
    std::memcpy(position, acc, sizeof(acc));
}

void putQuad(std::vector<uint8_t> &packet, const void *bytes)
{
    const auto *b = static_cast<const uint8_t *>(bytes);
    packet.insert(packet.end(), b, b + 16);
}

void putWords(std::vector<uint8_t> &packet, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    const uint32_t words[4] = {x, y, z, w};
    putQuad(packet, words);
}

// Vertex k's position as B leaves it: the int16 / 32768 times quad 77, then
// skinned over `bones` bones (0: not skinned).
void decodePosition(const VuData &vu, uint32_t top, uint32_t k, uint32_t bones, float position[3], bool raw = false)
{
    const uint32_t positions = top + 2u;
    const Vec4 scale = vu.quad(77);
    if (raw)
    {
        for (uint32_t i = 0; i < 3u; ++i)
            position[i] = fromBits(static_cast<uint32_t>(vu.word(positions + k, i)));
    }
    else
    {
        position[0] = itof15(vu.word(positions + k, 0)) * scale.x;
        position[1] = itof15(vu.word(positions + k, 1)) * scale.y;
        position[2] = itof15(vu.word(positions + k, 2)) * scale.z;
    }
    if (bones != 0u)
    {
        skin(vu, positions + vu.half(top + 1u, 2) + k * 2u, bones, position);
    }
}

// The lit C (91f542d0): four directional lights, their directions the lanes
// of quads 100-102, their colors quads 33-36, an ambient 37, all times the
// material (quad 82, its w by 81.w). Each light's term is clamped at 0; the
// sum goes into the vertex's RGBA slot, as floats, for D to clamp.
struct Lights
{
    Vec4 direction[3];
    Vec4 color[4];
    Vec4 ambient;

    explicit Lights(const VuData &vu)
    {
        const Vec4 material = vu.quad(82);
        const float alpha = material.w * vu.quad(81).w;
        for (uint32_t i = 0; i < 3u; ++i)
        {
            direction[i] = vu.quad(100u + i);
        }
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const Vec4 c = vu.quad(33u + i);
            color[i] = {c.x * material.x, c.y * material.y, c.z * material.z, 0.0f};
        }
        const Vec4 a = vu.quad(37);
        ambient = {a.x * material.x, a.y * material.y, a.z * material.z, a.w * alpha};
    }

    Vec4 shade(const float n[3]) const
    {
        const float *d0 = &direction[0].x, *d1 = &direction[1].x, *d2 = &direction[2].x;
        float d[4];
        for (int i = 0; i < 4; ++i)
        {
            d[i] = rg::fmax(d0[i] * n[0] + d1[i] * n[1] + d2[i] * n[2], 0.0f);
        }
        const float *c0 = &color[0].x, *c1 = &color[1].x, *c2 = &color[2].x, *c3 = &color[3].x;
        const float *base = &ambient.x;
        float out[3];
        for (int i = 0; i < 3; ++i)
        {
            out[i] = base[i] * 1.0f + c0[i] * d[0] + c1[i] * d[1] + c2[i] * d[2] + c3[i] * d[3];
        }
        return {out[0], out[1], out[2], ambient.w};
    }
};

// The sphere map F (150031ee): the normal through the view's rotation
// (quads 24-26, xy, times 64-66), then (uv + 1) / 2.
struct SphereMap
{
    float rows[3][2];

    explicit SphereMap(const VuData &vu)
    {
        const Vec4 r24 = vu.quad(24), r25 = vu.quad(25), r26 = vu.quad(26);
        for (uint32_t i = 0; i < 3u; ++i)
        {
            const Vec4 m = vu.quad(64u + i);
            rows[i][0] = r24.x * m.x + r25.x * m.y + r26.x * m.z;
            rows[i][1] = r24.y * m.x + r25.y * m.y + r26.y * m.z;
        }
    }

    void uv(const float n[3], float &u, float &v) const
    {
        u = (rows[0][0] * n[0] + rows[1][0] * n[1] + rows[2][0] * n[2] + 1.0f) * 0.5f;
        v = (rows[0][1] * n[0] + rows[1][1] * n[1] + rows[2][1] * n[2] + 1.0f) * 0.5f;
    }
};

// The VU's ESIN: a polynomial in x^2, as the interpreter has it.
float vuSin(float x)
{
    constexpr float c[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f,
                           0.000002601886990f};
    const float squared = x * x;
    float polynomial = c[4];
    for (int i = 3; i >= 0; --i)
        polynomial = c[i] + squared * polynomial;
    return x * polynomial;
}

// The beam's F (b1d29245; recorded): each position through quads 72-75 (the x,
// y and w lanes, a column each), divided by w, y negated, taken from the
// point quad 75 projects (pc, y negated) and scaled by quad 82.z, then moved
// by pc + (quad 82.xy - 1) * 0.05; the UV is (that + 1) / 2.
struct Beam
{
    Vec4 col[4];
    float centerX, centerY, shiftX, shiftY, scale;

    explicit Beam(const VuData &vu)
    {
        for (uint32_t i = 0; i < 4u; ++i)
            col[i] = vu.quad(72u + i);
        const float inverse = rg::nativeReciprocal(col[3].w);
        centerX = col[3].x * inverse;
        centerY = 0.0f - col[3].y * inverse;
        const Vec4 material = vu.quad(82);
        shiftX = centerX + (material.x - 1.0f) * 0.05f;
        shiftY = centerY + (material.y - 1.0f) * 0.05f;
        scale = material.z;
    }

    void uv(const float p[3], float &u, float &v) const
    {
        const float x = ((col[0].x * p[0] + col[1].x * p[1]) + col[2].x * p[2]) + col[3].x * 1.0f;
        const float y = ((col[0].y * p[0] + col[1].y * p[1]) + col[2].y * p[2]) + col[3].y * 1.0f;
        const float w = ((col[0].w * p[0] + col[1].w * p[1]) + col[2].w * p[2]) + col[3].w * 1.0f;
        const float inverse = rg::nativeReciprocal(w);
        const float dx = x * inverse - centerX;
        const float dy = (0.0f - y * inverse) - centerY;
        u = ((dx * scale + shiftX) + 1.0f) * 0.5f;
        v = ((dy * scale + shiftY) + 1.0f) * 0.5f;
    }
};

// The swell F (a04c11ce, UNVERIFIED: nothing was seen to run it): each position
// pushed out along its normal, by (normal * q1) * w * s, w the vertex's clip
// w (quads 68-71, w lanes), q1 = quad 84.x / D, D the sum of quad 12-15's x
// lanes times quad 8, and s = clamp((quad 84.y - w) / quad 84.y, 0.2, 1). It
// also writes colors (max(82.rgb * 128, 1), 82.w * 81.w * 128 * s); the C that
// follows (one color) writes over them.
struct Swell
{
    float q, far;
    float lane[4];

    explicit Swell(const VuData &vu)
    {
        for (uint32_t i = 0; i < 4u; ++i)
            lane[i] = vu.quad(68u + i).w;
        const Vec4 r8 = vu.quad(8);
        const float d = ((vu.quad(12).x * r8.x + vu.quad(13).x * r8.y) + vu.quad(14).x * r8.z) + vu.quad(15).x * r8.w;
        const Vec4 range = vu.quad(84);
        q = divide(range.x, d);
        far = range.y;
        inverseFar = divide(1.0f, range.y);
    }
    float inverseFar;

    static float divide(float a, float b)
    {
        if (b != 0.0f)
            return a / b;
        return std::signbit(a) != std::signbit(b) ? -std::numeric_limits<float>::max()
                                                  : std::numeric_limits<float>::max();
    }

    void push(float p[3], const float n[3]) const
    {
        const float depth = ((lane[0] * p[0] + lane[1] * p[1]) + lane[2] * p[2]) + lane[3] * 1.0f;
        const float scale = rg::fmin(rg::fmax((far - depth) * inverseFar, 0.2f), 1.0f);
        for (int i = 0; i < 3; ++i)
            p[i] = p[i] + ((n[i] * q) * depth) * scale;
    }
};

// The wave F (9df36d6a): B's decoded positions (quad 103.x, a vertex each,
// in place) turned in x and y about (0, a), a = quad 84.x, by t = q *
// max(y - a, 0), q = quad 900.z / (quad 84.y - a): with c = 1 - t * t / 2 and
// s = sin t (ESIN), x' = c x - s (y - a) and y' = c (y - a) + s x + a. The
// normals stay. The products and sums keep the VU's order.
struct Wave
{
    float a, q;

    explicit Wave(const VuData &vu)
    {
        const Vec4 range = vu.quad(84);
        a = range.x;
        q = vu.quad(900).z / (range.y - range.x);
    }

    void xy(float &x, float &y) const
    {
        const float d = y - a;
        const float t = (0.0f + q) * (d > 0.0f ? d : 0.0f);
        const float s = vuSin(t);
        const float c = 1.0f - (t * t) * 0.5f;
        const float nx = (0.0f - s) * d + c * x;
        const float ny = ((0.0f + c) * d + s * x) + a;
        x = nx;
        y = ny;
    }
};

// A chunk read out of VU1 data memory: the FOGCOL packet, the first quads of
// the program's output (GIF header, PRIM, PRMODE, the vertex tag) and the
// strip as a mesh.
// The point lights of the lit-with-points C (127ca0b4), added to the
// directional sum: four lights, their positions quads 44, 46, 48, 50 (w: the
// falloff), their colors quads 45, 47, 49, 51 times the material (quad 82).
// For each: d = light - W, W the vertex through quads 60-63; the normal
// through quads 64-66; f = max(max(d.N, 0) / |d| - max(d.N, 0) * w, 0), the
// colors summed in light order, then added to the directional sum. (The code
// reads each light's 1 / |d| from Q or P as it completes, so the light's own;
// the interpreter gives the first two vertices of a chunk, whose code runs
// ahead of the loop, the next light's in two lanes: a difference of a few
// levels on those two vertices, not reproduced here. Checked against the
// interpreter with the oracle's synthetic mode, all other vertices.)
struct PointLights
{
    Vec4 col[4], rot[3], position[4];
    float color[4][3];

    explicit PointLights(const VuData &vu)
    {
        for (uint32_t i = 0; i < 4u; ++i)
            col[i] = vu.quad(60u + i);
        for (uint32_t i = 0; i < 3u; ++i)
            rot[i] = vu.quad(64u + i);
        const Vec4 material = vu.quad(82);
        for (uint32_t i = 0; i < 4u; ++i)
        {
            position[i] = vu.quad(44u + i * 2u);
            const Vec4 c = vu.quad(45u + i * 2u);
            color[i][0] = c.x * material.x;
            color[i][1] = c.y * material.y;
            color[i][2] = c.z * material.z;
        }
    }

    void add(const float p[3], const float n[3], float rgb[3]) const
    {
        const float w[3] = {((col[0].x * p[0] + col[1].x * p[1]) + col[2].x * p[2]) + col[3].x * 1.0f,
                            ((col[0].y * p[0] + col[1].y * p[1]) + col[2].y * p[2]) + col[3].y * 1.0f,
                            ((col[0].z * p[0] + col[1].z * p[1]) + col[2].z * p[2]) + col[3].z * 1.0f};
        const float nx = (rot[0].x * n[0] + rot[1].x * n[1]) + rot[2].x * n[2];
        const float ny = (rot[0].y * n[0] + rot[1].y * n[1]) + rot[2].y * n[2];
        const float nz = (rot[0].z * n[0] + rot[1].z * n[1]) + rot[2].z * n[2];
        float sum[3] = {};
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const float dx = position[i].x - w[0], dy = position[i].y - w[1], dz = position[i].z - w[2];
            const float dot = rg::fmax((dx * nx + dy * ny) + dz * nz, 0.0f);
            const float root = std::sqrt(std::fabs((dx * dx + dy * dy) + (dz * dz) * 1.0f));
            const float inverse = root != 0.0f ? 1.0f / root : std::numeric_limits<float>::max();
            const float f = rg::fmax(dot * inverse - dot * position[i].w, 0.0f);
            for (int k = 0; k < 3; ++k)
                sum[k] = i == 0u ? color[i][k] * f : sum[k] + color[i][k] * f;
        }
        for (int k = 0; k < 3; ++k)
            rgb[k] = rgb[k] + sum[k];
    }
};

struct Chunk
{
    std::vector<uint8_t> fog;
    uint8_t header[4][16] = {};
    rg::NativeMesh mesh;
};

// The chunk's header (readChunk's first part): the FOGCOL packet, the GIF
// header, PRIM and PRMODE, and the mesh header (matrices, fog, cull, depth,
// clip), without its vertices.
void chunkHeader(const Program &program, const VuData &vu, uint32_t top, Chunk &chunk)
{
    // FOGCOL (quad 41) behind the tag in quad 19, NLOOP 1 and EOP.
    uint32_t fogTag[4];
    std::memcpy(fogTag, vu.bytes(19), sizeof(fogTag));
    putWords(chunk.fog, 0x8001u, fogTag[1], fogTag[2], fogTag[3]);
    putQuad(chunk.fog, vu.bytes(41));
    if (vu.half(87, 1) & 0x10u)
    {
        std::memset(chunk.fog.data() + 16u, 0, 4u);
    }

    // The chunk: its header at TOP, its sections after it.
    const uint32_t prim = vu.half(top, 0);
    const uint32_t count = vu.half(top, 1);
    const uint32_t positions = top + 2u;
    const uint32_t uvs = positions + vu.half(top + 1u, 0);
    const uint32_t colors = positions + vu.half(top + 1u, 1);
    const uint32_t primWords[4] = {prim, 0u, 0u, 0u};
    const uint32_t prmodeWords[4] = {vu.half(87, 0), 0u, 27u, 0u};
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = vu.half(top, 2);
    std::memcpy(chunk.header[0], vu.bytes(0), 16u);
    std::memcpy(chunk.header[1], primWords, 16u);
    std::memcpy(chunk.header[2], prmodeWords, 16u);
    std::memcpy(chunk.header[3], tag, 16u);

    rg::NativeMesh::Header &h = chunk.mesh.header;
    for (uint32_t row = 0; row < 4u; ++row)
    {
        const Vec4 m = vu.quad(68u + row);
        h.matrix[row][0] = m.x;
        h.matrix[row][1] = m.y;
        h.matrix[row][2] = m.z;
        h.matrix[row][3] = m.w;
    }
    const Vec4 fog = vu.quad(40), colorMax = vu.quad(39);
    const Vec4 viewport = vu.quad(16), band = vu.quad(17), bandOffset = vu.quad(18);
    std::memcpy(h.fog, &fog, sizeof(h.fog));
    std::memcpy(h.colorMax, &colorMax, sizeof(h.colorMax));
    h.bandScale[0] = band.x * viewport.x;
    h.bandScale[1] = band.y * viewport.y;
    h.bandScale[2] = band.z;
    h.bandScale[3] = band.w;
    std::memcpy(h.bandOffset, &bandOffset, sizeof(h.bandOffset));
    h.winding = vu.quad(111).z * ((vu.half(87, 1) & 0x1000u) ? 1.0f : -1.0f);
    h.guardBand = program.cull != Cull::None ? 1u : 0u;
    h.backface = program.cull == Cull::GuardBandAndBackface ? 1u : 0u;
    h.vertexCount = count;
    if (program.depthBounds)
    {
        h.depthScale = vu.quad(87).z;
        h.depthMax = vu.quad(1).y;
    }
    h.triangleList = (prim & 7u) == 3u ? 1u : 0u;
    // A chunk slot E clips (the beam's E is a stub): E's clip space is
    // quads 72-75 (the base program's view times its clip matrix, rows as
    // 68-71's), and the renderer clips the strip at its z = -w and z = w.
    if ((vu.half(81, 0) & 2u) && program.uv != Uv::Beam)
    {
        h.clips = 1u;
        for (uint32_t row = 0; row < 4u; ++row)
        {
            const Vec4 m = vu.quad(72u + row);
            h.clipPlanes[0][row] = m.z + m.w;
            h.clipPlanes[1][row] = m.w - m.z;
        }
    }

}

// The outline C. With TOP.x 19 the chunk's points come in fours: an edge
// (the second and third) and the far vertices of its two triangles (the
// first and fourth); otherwise in twos, the edge alone. Each is transformed
// (quads 68-71) and divided. The edge's vertices carry ADC unless both ends
// are inside the guard band and, in fours, the two triangles face the same
// way on screen (the z of their cross products agree in sign: a
// silhouette). It is drawn as four lines, the edge moved by +-h in x, then
// in y: h = alpha / 2, alpha = clamp(10/9 - w / 360, 0, 1) of its first
// end, which is also its RGBA's (16, 16, 16, alpha * 64). The vertex tag's
// NLOOP is the edge count, also written to quad 1023.x.
uint32_t outlinePoints(const VuData &vu, uint32_t top)
{
    return vu.half(top, 0) == 19u ? 4u : 2u;
}

uint32_t outlineLoopWord(const VuData &vu, uint32_t top)
{
    const float share = outlinePoints(vu, top) == 4u ? 0.25f : 0.5f;
    return static_cast<uint32_t>(rg::nativeFtoi(static_cast<float>(vu.half(top, 1)) * share + 32768.0f, 1.0f));
}

// The GIF packet at quad `q` as XGKICK sends it: tags and their data up to
// the one with EOP.
std::vector<uint8_t> gifPacket(const VuData &vu, uint32_t q)
{
    std::vector<uint8_t> packet;
    for (uint32_t tags = 0; tags < 64u; ++tags)
    {
        const uint32_t lo = static_cast<uint32_t>(vu.word(q, 0)), hi = static_cast<uint32_t>(vu.word(q, 1));
        const uint32_t loops = lo & 0x7FFFu, flag = (hi >> 26) & 3u;
        const uint32_t regs = ((hi >> 28) & 0xFu) == 0u ? 16u : (hi >> 28) & 0xFu;
        const uint32_t quads = flag == 0u ? loops * regs : flag == 1u ? (loops * regs + 1u) / 2u : loops;
        for (uint32_t i = 0; i <= quads; ++i)
        {
            putQuad(packet, vu.bytes(q + i));
        }
        q += quads + 1u;
        if (lo & 0x8000u)
        {
            break;
        }
    }
    return packet;
}

// out = rows * v as the VU's ACC sums it: ((r0 v.x + r1 v.y) + r2 v.z) + r3 v.w.
Vec4 transform(const Vec4 rows[4], const Vec4 &v)
{
    const float *r0 = &rows[0].x, *r1 = &rows[1].x, *r2 = &rows[2].x, *r3 = &rows[3].x;
    float out[4];
    for (int i = 0; i < 4; ++i)
    {
        out[i] = r0[i] * v.x + r1[i] * v.y + r2[i] * v.z + r3[i] * v.w;
    }
    return {out[0], out[1], out[2], out[3]};
}

bool clipsOwnW(const Vec4 &v)
{
    const float w = std::fabs(v.w);
    return v.x > w || v.x < -w || v.y > w || v.y < -w || v.z > w || v.z < -w;
}

// The z of OPMULA a, b then OPMSUB b, a: a.x b.y - b.x a.y.
float crossZ(const Vec4 &a, const Vec4 &b)
{
    return a.x * b.y - b.x * a.y;
}

Vec4 sub(const Vec4 &a, const Vec4 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z, 0.0f};
}

// The shadow volume C. The chunk's points come in threes, a triangle of
// the caster. Its world normal (quads 60-63 the model matrix) against the
// light (quad 34) must exceed 1e-4, else nothing is drawn. Its corners
// (front) and the same pushed away from the light (back: the model matrix
// times quads 30-33) go through the view (quads 8-11 times 24-27, x and y
// scaled by 16) and, unless any falls outside its own w, through the
// projection (quads 12-15, 12.x by 16.z and 13.y by 16.w) and the divide.
// It draws the volume as five fans of four: three sides, the front cap and
// the back cap, each after the A+D packet at quad 38 if it faces the screen
// (its z cross product is positive), at quad 35 if not (the blend that adds
// or takes away).
void buildShadow(const Program &program, const VuData &vu, uint32_t top,
                 std::vector<std::vector<uint8_t>> &packets)
{
    packets.clear();
    const uint32_t count = vu.half(top, 1);
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    Vec4 model[4], light[4], view[4], projection[4], away[4], camera[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        away[i] = vu.quad(30u + i);
        camera[i] = vu.quad(8u + i);
    }
    const Vec4 viewport = vu.quad(16);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        light[i] = transform(away, model[i]);
        view[i] = transform(camera, vu.quad(24u + i));
        view[i].x *= viewport.x;
        view[i].y *= viewport.y;
        projection[i] = vu.quad(12u + i);
    }
    projection[0].x *= viewport.z;
    projection[1].y *= viewport.w;
    const Vec4 toLight = vu.quad(34);
    const float lit = fromBits(0x38d1b717u); // 1e-4

    uint8_t header[4][16];
    const uint32_t prim[4] = {5u, 0u, 0u, 0u};
    const uint32_t prmode[4] = {vu.half(87, 0), 0u, 27u, 0u};
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = 0x8004u;
    std::memcpy(header[0], vu.bytes(0), 16u);
    std::memcpy(header[1], prim, 16u);
    std::memcpy(header[2], prmode, 16u);
    std::memcpy(header[3], tag, 16u);
    const std::vector<uint8_t> facing = gifPacket(vu, 38), backing = gifPacket(vu, 35);

    for (uint32_t first = 0; first < count; first += 3u)
    {
        Vec4 world[3], pushed[3];
        for (uint32_t j = 0; j < 3u; ++j)
        {
            float p[3];
            decodePosition(vu, top, first + j, bones, p);
            const Vec4 point{p[0], p[1], p[2], 1.0f};
            world[j] = transform(model, point);
            pushed[j] = transform(light, point);
        }
        const Vec4 a = sub(world[1], world[0]), b = sub(world[2], world[0]);
        const float nx = a.y * b.z - b.y * a.z, ny = a.z * b.x - b.z * a.x, nz = a.x * b.y - b.x * a.y;
        if (!(toLight.x * nx + toLight.y * ny + toLight.z * nz > lit))
        {
            continue;
        }
        Vec4 corners[6];
        bool outside = false;
        for (uint32_t j = 0; j < 3u; ++j)
        {
            corners[j] = transform(view, world[j]);
            corners[3u + j] = transform(view, pushed[j]);
        }
        for (const Vec4 &corner : corners)
        {
            outside = outside || clipsOwnW(corner);
        }
        if (outside)
        {
            continue;
        }
        Vec4 screen[6];
        uint32_t fixed[6][4];
        for (uint32_t j = 0; j < 6u; ++j)
        {
            Vec4 &s = screen[j] = transform(projection, corners[j]);
            const float q = rg::nativeReciprocal(s.w);
            s.x *= q;
            s.y *= q;
            s.z *= q;
            const float lanes[4] = {s.x, s.y, s.z, s.w};
            for (int i = 0; i < 4; ++i)
            {
                fixed[j][i] = static_cast<uint32_t>(rg::nativeFtoi(lanes[i], 16.0f));
            }
        }
        // The fans: front 0-2, back 3-5, and the corners each takes, and
        // the two edges whose cross product says which way it faces.
        constexpr uint8_t kFans[5][4] = {{0, 3, 4, 1}, {1, 4, 5, 2}, {2, 5, 3, 0}, {0, 1, 2, 2}, {5, 4, 3, 3}};
        constexpr uint8_t kFacing[5][3] = {{0, 3, 4}, {1, 4, 5}, {2, 5, 3}, {0, 1, 2}, {5, 4, 3}};
        for (uint32_t f = 0; f < 5u; ++f)
        {
            const Vec4 &origin = screen[kFacing[f][0]];
            const float z = crossZ(sub(screen[kFacing[f][1]], origin), sub(screen[kFacing[f][2]], origin)) * 1.0f;
            packets.push_back(z > 0.0f ? facing : backing);
            std::vector<uint8_t> &fan = packets.emplace_back();
            fan.reserve(128u);
            for (const auto &quad : header)
            {
                putQuad(fan, quad);
            }
            for (uint32_t v = 0; v < 4u; ++v)
            {
                putQuad(fan, fixed[kFans[f][v]]);
            }
        }
    }
}

// The shadow volumes for the model vertex stage (M5, flag 64 of the row):
// the chunk's triangles as inputs (a list, skinned there), drawn twice:
// after the A+D packet at quad 38 with the facing triangles of each
// triangle's volume kept, and after quad 35's with the others, the
// renderer's backface rule choosing from the same projected corners. The
// stage makes buildShadow's volume: the corners and the same pushed from the
// light (rows 38-41: the model times quads 30-33), in world space, none
// when the triangle is not lit (row 42: the light's direction, its
// threshold) or a corner is outside its own w (rows 44-47: the camera,
// scaled by quad 16); five fans of two triangles. Each draw's color (row
// 43) is the RGBAQ its packet sets; false when one sets none (the caller
// keeps the packets).
bool shadowMeshFrom(const Program &program, const uint8_t *data, uint32_t top,
                    std::vector<std::vector<uint8_t>> (&packets)[2], rg::NativeMesh (&meshes)[2],
                    std::vector<dq8::gfx::native::ModelInput> &inputs, dq8::gfx::native::ModelRow (&rows)[2],
                    std::vector<float> &palette)
{
    const VuData vu(data);
    // The colors the two packets set (decoded twice from two different
    // colors: a packet that sets RGBAQ ends on the same one).
    const std::vector<uint8_t> state[2] = {gifPacket(vu, 38), gifPacket(vu, 35)};
    float rgba[2][4];
    for (int i = 0; i < 2; ++i)
    {
        rg::gif::GifState a, b;
        std::memset(a.rgba, 0x00, sizeof(a.rgba));
        std::memset(b.rgba, 0xFF, sizeof(b.rgba));
        std::vector<rg::gif::GifEvent> events;
        rg::gif::decodeGifPacket(state[i].data(), static_cast<uint32_t>(state[i].size()), a, events);
        rg::gif::decodeGifPacket(state[i].data(), static_cast<uint32_t>(state[i].size()), b, events);
        if (std::memcmp(a.rgba, b.rgba, sizeof(a.rgba)) != 0)
            return false;
        for (int l = 0; l < 4; ++l)
            rgba[i][l] = float(a.rgba[l]);
    }
    const uint32_t count = vu.half(top, 1);
    Vec4 model[4], away[4], camera[4], window[4], light[4], view[4], world[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        away[i] = vu.quad(30u + i);
        camera[i] = vu.quad(8u + i);
        window[i] = vu.quad(12u + i);
    }
    const Vec4 viewport = vu.quad(16);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        light[i] = transform(away, model[i]);
        view[i] = transform(camera, vu.quad(24u + i));
        view[i].x *= viewport.x;
        view[i].y *= viewport.y;
        world[i] = transform(window, transform(camera, vu.quad(24u + i)));
    }
    bool list = false;
    modelChunk(program, data, top, inputs, list);
    inputs.resize(std::min<size_t>(inputs.size(), count / 3u * 3u));
    modelRow(program, data, top, rows[0], palette);
    dq8::gfx::native::ModelRow &row = rows[0];
    const auto put = [&](uint32_t at, const Vec4 &v) {
        row.q[at][0] = v.x;
        row.q[at][1] = v.y;
        row.q[at][2] = v.z;
        row.q[at][3] = v.w;
    };
    row.q[0][3] = 0.0f;
    row.q[1][3] = 0.0f;
    row.q[2][3] = 64.0f;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        put(38u + i, light[i]);
        put(44u + i, view[i]);
    }
    const Vec4 toLight = vu.quad(34);
    put(42u, {toLight.x, toLight.y, toLight.z, fromBits(0x38d1b717u)});
    rows[1] = rows[0];
    for (int i = 0; i < 2; ++i)
    {
        for (int l = 0; l < 4; ++l)
            rows[i].q[43][l] = rgba[i][l];
        packets[i].clear();
        packets[i].push_back(state[i]);
        std::vector<uint8_t> &prim = packets[i].emplace_back();
        uint32_t tag[4];
        std::memcpy(tag, vu.bytes(0), sizeof(tag));
        tag[0] |= 0x8000u;
        putQuad(prim, tag);
        putWords(prim, 3u, 0u, 0u, 0u); // a triangle list
        putWords(prim, vu.half(87, 0), 0u, 27u, 0u);
        rg::NativeMesh::Header &h = meshes[i].header;
        h = rg::NativeMesh::Header{};
        for (uint32_t r = 0; r < 4u; ++r)
        {
            h.matrix[r][0] = world[r].x;
            h.matrix[r][1] = world[r].y;
            h.matrix[r][2] = world[r].z;
            h.matrix[r][3] = world[r].w;
        }
        const Vec4 fogQuad = vu.quad(40);
        std::memcpy(h.fog, &fogQuad, sizeof(h.fog));
        for (float &c : h.colorMax)
            c = 255.0f;
        h.triangleList = 1u;
        h.backface = 1u;
        // A list's triangles are judged with -winding: +1 keeps those whose
        // projected cross product (buildShadow's) is positive, the facing.
        h.winding = i == 0 ? 1.0f : -1.0f;
        meshes[i].vertices.clear();
    }
    return !inputs.empty();
}

// The base piece (12ad73d2), run by an MSCAL at 0x000 before each object's
// chunks: it kicks the object's state packet at quad 19, then leaves in VU1
// memory what the strip programs read. With M the model rows (quads 60-63)
// and W = quads 24-27 times each: 68-71 = quads 4-7 times W (to the GS
// window), 72-75 = quads 8-11 times W (to clip space); 64-66 = the model
// rows 60-62 normalized (ERLENG: 1 / sqrt(x x + y y + z z), as the
// runtime's EFU has it), their w kept; 100-102 = the lights (quads 30-32)
// times those; 111.z = 1, or -1 when the model rows' determinant is
// negative (the winding of the strips' backface test).
void runBase(uint8_t *data, std::vector<std::vector<uint8_t>> *packets)
{
    const VuData vu(data);
    if (packets)
    {
        packets->push_back(gifPacket(vu, 19));
    }
    const auto store = [data](uint32_t q, const Vec4 &v) { std::memcpy(data + q * 16u, &v, 16u); };
    Vec4 model[4], view[4], window[4], clip[4], lights[3];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        view[i] = vu.quad(24u + i);
        window[i] = vu.quad(4u + i);
        clip[i] = vu.quad(8u + i);
    }
    for (uint32_t i = 0; i < 3u; ++i)
    {
        lights[i] = vu.quad(30u + i);
    }
    const Vec4 &r0 = model[0], &r1 = model[1], &r2 = model[2];
    const float cx = r0.y * r1.z - r1.y * r0.z, cy = r0.z * r1.x - r1.z * r0.x, cz = r0.x * r1.y - r1.x * r0.y;
    const float determinant = cx * r2.x + cy * r2.y + cz * r2.z;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const Vec4 w = transform(view, model[i]);
        store(68u + i, transform(window, w));
        store(72u + i, transform(clip, w));
    }
    for (uint32_t i = 0; i < 3u; ++i)
    {
        const Vec4 &r = model[i];
        const float length = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        const float scale = length != 0.0f ? 1.0f / length : length;
        const Vec4 unit{r.x * scale, r.y * scale, r.z * scale, r.w};
        store(64u + i, unit);
        const Vec4 v{unit.x, unit.y, unit.z, 0.0f};
        Vec4 light;
        float *out = &light.x;
        const float *l0 = &lights[0].x, *l1 = &lights[1].x, *l2 = &lights[2].x;
        for (int lane = 0; lane < 4; ++lane)
        {
            out[lane] = l0[lane] * v.x + l1[lane] * v.y + l2[lane] * v.z;
        }
        store(100u + i, light);
    }
    const float sign = determinant < 0.0f ? -1.0f : 1.0f;
    std::memcpy(data + 111u * 16u + 8u, &sign, 4u);
}

// The particles for the model vertex stage (M5, flag 32 of the row): each
// particle six inputs (its strip's triangles 0 1 2 and 1 2 3 as a list),
// each with the particle's position, its shape (bones: x, y and the angle),
// the corner's offsets (normal) and texel (uv), and its color quad; the
// stage makes buildParticles' billboard (the eye from quad 28, the model
// from 60-63) in world space. The header's matrix world to window (quads
// 8-11, 24-27, 12-15), no cull. The state packets are buildParticles'
// (FOGCOL, then the GIF header, PRIM and PRMODE).
void particleMesh(const VuData &vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets, rg::NativeMesh &mesh,
                  std::vector<dq8::gfx::native::ModelInput> &inputs, dq8::gfx::native::ModelRow &row)
{
    packets.clear();
    std::vector<uint8_t> &fog = packets.emplace_back();
    uint32_t fogTag[4];
    std::memcpy(fogTag, vu.bytes(19), sizeof(fogTag));
    putWords(fog, 0x8001u, fogTag[1], fogTag[2], fogTag[3]);
    putQuad(fog, vu.bytes(41));
    if (vu.half(87, 1) & 0x10u)
        std::memset(fog.data() + 16u, 0, 4u);
    std::vector<uint8_t> &state = packets.emplace_back();
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(0), sizeof(tag));
    tag[0] |= 0x8000u;
    putQuad(state, tag);
    putWords(state, 3u, 0u, 0u, 0u); // a triangle list
    putWords(state, vu.half(87, 0), 0u, 27u, 0u);

    const uint32_t count = std::min<uint32_t>(vu.half(top, 1), 256u);
    const uint32_t positions = top + 2u;
    const uint32_t offsets = positions + vu.half(top, 3);
    const uint32_t uvs = positions + vu.half(top + 1u, 0);
    const uint32_t colors = positions + vu.half(top + 1u, 1);
    Vec4 camera[4], window[4], rows[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        camera[i] = vu.quad(8u + i);
        window[i] = vu.quad(12u + i);
    }
    for (uint32_t i = 0; i < 4u; ++i)
        rows[i] = transform(window, transform(camera, vu.quad(24u + i)));
    rg::NativeMesh::Header &h = mesh.header;
    h = rg::NativeMesh::Header{};
    for (uint32_t r = 0; r < 4u; ++r)
    {
        h.matrix[r][0] = rows[r].x;
        h.matrix[r][1] = rows[r].y;
        h.matrix[r][2] = rows[r].z;
        h.matrix[r][3] = rows[r].w;
    }
    const Vec4 fogQuad = vu.quad(40);
    std::memcpy(h.fog, &fogQuad, sizeof(h.fog));
    for (float &c : h.colorMax)
        c = 255.0f;
    h.triangleList = 1u;
    mesh.vertices.clear();

    row = {};
    const auto put = [&](uint32_t at, const Vec4 &v) {
        row.q[at][0] = v.x;
        row.q[at][1] = v.y;
        row.q[at][2] = v.z;
        row.q[at][3] = v.w;
    };
    for (uint32_t i = 0; i < 4u; ++i)
        put(20u + i, vu.quad(60u + i));
    put(27u, vu.quad(28));
    row.q[2][3] = 32.0f;

    inputs.clear();
    inputs.reserve(size_t(count) * 6u);
    for (uint32_t i = 0; i < count; ++i)
    {
        const Vec4 p = vu.quad(positions + 2u * i), shape = vu.quad(positions + 2u * i + 1u);
        const Vec4 o0 = vu.quad(offsets + 2u * i), o1 = vu.quad(offsets + 2u * i + 1u);
        const Vec4 uv0 = vu.quad(uvs + 2u * i), uv1 = vu.quad(uvs + 2u * i + 1u);
        const float corners[4][2] = {{o0.x, o0.y}, {o1.x, o0.y}, {o0.x, o1.y}, {o1.x, o1.y}};
        const float texels[4][2] = {{uv0.x, uv0.y}, {uv1.x, uv0.y}, {uv0.x, uv1.y}, {uv1.x, uv1.y}};
        const Vec4 color = vu.quad(colors + i);
        for (const uint32_t k : {0u, 1u, 2u, 1u, 2u, 3u})
        {
            dq8::gfx::native::ModelInput &v = inputs.emplace_back();
            v = {};
            v.position[0] = p.x;
            v.position[1] = p.y;
            v.position[2] = p.z;
            v.bones[0] = shape.x;
            v.bones[1] = shape.y;
            v.bones[2] = shape.z;
            v.normal[0] = corners[k][0];
            v.normal[1] = corners[k][1];
            v.uv[0] = texels[k][0];
            v.uv[1] = texels[k][1];
            v.color[0] = color.x;
            v.color[1] = color.y;
            v.color[2] = color.z;
            v.color[3] = color.w;
        }
    }
}

} // namespace

bool shadowMeshes(const Program &program, const uint8_t *data, uint32_t top,
                  std::vector<std::vector<uint8_t>> (&packets)[2], rg::NativeMesh (&meshes)[2],
                  std::vector<dq8::gfx::native::ModelInput> &inputs, dq8::gfx::native::ModelRow (&rows)[2],
                  std::vector<float> &palette)
{
    const VuRounding rounding;
    return shadowMeshFrom(program, data, top, packets, meshes, inputs, rows, palette);
}

const Program *identify(const uint8_t *code, uint32_t startPC)
{
    if (startPC == kBaseEntry && matches(code, kBase))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Base};
        return intern(shape);
    }
    if (startPC == kOverlayEntry && matches(code, kOverlayB))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Overlay};
        return intern(shape);
    }
    if (startPC != kEntry)
    {
        return nullptr;
    }
    if (matches(code, kProjectorA) && matches(code, kProjectorF) && matches(code, kProjectorC))
    {
        const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
        if (!decode)
        {
            return nullptr;
        }
        Program shape{nullptr, Cull::GuardBandAndBackface, Color::Material, false, decode->decode,
                      decode->maxBones, Kind::Projector};
        return intern(shape);
    }
    if (matches(code, kParticleA) && matches(code, kParticleD))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Particles};
        return intern(shape);
    }
    // The programs whose C draws by itself, by their A and C.
    constexpr struct
    {
        const Piece &a;
        const Piece &c;
        Kind kind;
    } kOwnDrawing[] = {{kOutlineA, kOutlineC, Kind::Outline}, {kShadowA, kShadowC, Kind::Shadow}};
    for (const auto &own : kOwnDrawing)
    {
        if (matches(code, own.a))
        {
            const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
            if (!decode || decode->raw || !matches(code, own.c))
            {
                return nullptr;
            }
            Program shape{nullptr, Cull::None, Color::Material, false, decode->decode, decode->maxBones, own.kind};
            return intern(shape);
        }
    }
    const bool beam = matches(code, kBeamA);
    if (beam)
    {
        if (!matches(code, kBeamE))
        {
            return nullptr;
        }
    }
    else
    {
        for (const Piece &piece : kCommon)
        {
            if (!matches(code, piece))
            {
                return nullptr;
            }
        }
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    const ColorPiece *color = present(code, kColors, &ColorPiece::c);
    const Finish *finish = present(code, kFinishes, &Finish::d);
    const UvPiece *uv = present(code, kUvs, &UvPiece::f);
    if (!decode || !color || !finish || !uv)
    {
        return nullptr;
    }
    if (uv->uv == Uv::Swell && color->color != Color::Material)
        return nullptr;
    // The beam's pieces go together (its F and D read and write what the
    // others do not).
    if (beam != (uv->uv == Uv::Beam) || beam != finish->white)
    {
        return nullptr;
    }
    Program shape{nullptr,          finish->cull,     color->color, finish->depthBounds,
                  decode->decode,   decode->maxBones, Kind::Strip,  uv->uv, finish->bounds, finish->affine, finish->white, decode->raw, decode->skinNormals,
                  finish->rawColor};
    return intern(shape);
}

std::string describePieces(const uint8_t *code, uint32_t startPC)
{
    if (const Program *program = identify(code, startPC); program && program->kind != Kind::Strip)
    {
        return std::string("all: ") + program->name;
    }
    if (startPC != kEntry)
    {
        return "-";
    }
    std::string out;
    for (size_t i = 0; i < std::size(kCommon); ++i)
    {
        out += matches(code, kCommon[i]) ? kCommonNames[i] : matches(code, kBeamA) ? "A:beam" : "-";
        out += ' ';
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    out += !decode                             ? "B:? "
           : decode->decode == Decode::Skinned ? "B:skinned "
                                               : "B:plain ";
    const UvPiece *uv = present(code, kUvs, &UvPiece::f);
    out += !uv ? "F:? " : uv->uv == Uv::SphereMap ? "F:sphere map " : uv->uv == Uv::Wave ? "F:wave " : uv->uv == Uv::Reflect ? "F:reflection " : uv->uv == Uv::Ramp ? "F:ramp " : uv->uv == Uv::Beam ? "F:beam " : uv->uv == Uv::Swell ? "F:swell " : "F:scroll ";
    const char *color = "C:?";
    for (const ColorPiece &candidate : kColors)
    {
        if (matches(code, candidate.c))
        {
            color = candidate.color == Color::PerVertex   ? "C:vertex colors"
                    : candidate.color == Color::Lit       ? "C:lit"
                    : candidate.color == Color::LitVertex ? "C:lit vertex colors"
                    : candidate.color == Color::LitPoint ? "C:lit, point lights"
                    : candidate.color == Color::LitPointVertex ? "C:lit, point lights, vertex colors"
                                                          : "C:one color";
        }
    }
    const char *finish = "D:?";
    for (const Finish &candidate : kFinishes)
    {
        if (matches(code, candidate.d))
        {
            finish = candidate.rawColor ? "D:guard band and backfaces, float colors"
                     : candidate.white ? "D:guard band, depth, white"
                     : !candidate.bounds ? "D:guard band, depth"
                     : candidate.depthBounds && candidate.cull == Cull::GuardBandAndBackface
                         ? "D:guard band and backfaces, depth and bounds"
                     : candidate.depthBounds && candidate.cull == Cull::GuardBand ? "D:guard band, depth and bounds"
                     : candidate.depthBounds                        ? "D:depth and bounds"
                     : candidate.cull == Cull::GuardBandAndBackface ? "D:guard band and backfaces"
                     : candidate.cull == Cull::GuardBand            ? "D:guard band"
                                                                    : "D:no culling";
        }
    }
    return out + color + " " + finish;
}

const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top)
{
    const VuData vu(data);
    const bool clips = hasMesh(program);
    if (program.kind == Kind::Overlay || program.kind == Kind::Base)
        return nullptr;
    // A enters B at pair 300 + (76.z ? TOP+1.w : 0) (0x450-0x480): with a
    // plain B only the first entry is known.
    if (program.decode == Decode::Plain && vu.half(76, 2) != 0u && vu.half(top + 1u, 3) != 0u)
        return "quad 76.z and TOP+1.w set (an entry into a plain B other than its first)";
    if (program.decode == Decode::Skinned && bonesOf(program, vu, top) == 0u)
        return "an entry into the skinning B not read";
    // A chunk of no whole edge, triangle or particle, or of fewer vertices
    // than a pipelined F piece reads before it tests the count (the VU would
    // run past the chunk's end there): the model stage draws the whole ones
    // it has, and nothing for none.
    if (program.kind == Kind::Outline || program.kind == Kind::Shadow || program.kind == Kind::Particles)
        return nullptr;
    if (program.kind == Kind::Projector)
        return (vu.half(81, 0) & 2u) && !clips ? "clipping (slot E)" : nullptr;
    if ((vu.half(81, 0) & 2u) && !clips && program.uv != Uv::Beam)
        return "clipping (slot E)";
    // PRIM from the chunk header: the model stage draws strips and lists (a
    // list's whole triangles; the backface pieces' own list path would run
    // past the end of another count, the model stage draws what it has).
    const uint32_t primType = vu.half(top, 0) & 7u;
    if (primType != 3u && primType != 4u)
        return "neither strip nor triangle list";
    return nullptr;
}

namespace
{

} // namespace

void applyMemoryEffects(const Program &program, uint8_t *data, uint32_t top)
{
    if (program.kind == Kind::Base)
    {
        const VuRounding rounding;
        runBase(data, nullptr);
        return;
    }
    if (program.kind == Kind::Outline)
    {
        const VuRounding rounding;
        const uint32_t word = outlineLoopWord(VuData(data), top);
        std::memcpy(data + 1023u * 16u, &word, sizeof(word));
        return;
    }
    // The bounds the depth-and-bounds D pieces keep in quads 98 and 99 are
    // read only by the overlay B, inside a composite tail whose overlay is
    // M3's ObjectLayer pass (docs/NATIVE-ONLY-NOTES.md, "M5"): not kept.
}

void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets)
{
    const VuRounding rounding;
    if (program.kind == Kind::Shadow)
    {
        buildShadow(program, VuData(data), top, packets);
        return;
    }
    if (program.kind == Kind::Overlay)
    {
        // The overlay B's sprites are drawn only inside a composite's tail,
        // by M3's ObjectLayer pass from the overlay calls' arguments (the
        // tail's packets withheld): nothing to send, nothing it leaves is
        // read.
        packets.clear();
        return;
    }
    if (program.kind == Kind::Base)
    {
        packets.clear();
        packets.push_back(gifPacket(VuData(data), 19));
        return;
    }
    packets.clear(); // (every other program is the model vertex stage's)
}

// --- M5: the strip programs on the GPU (native_model.vert) ---------------

void particleMesh(const uint8_t *data, uint32_t top, std::vector<std::vector<uint8_t>> &packets, rg::NativeMesh &mesh,
                  std::vector<dq8::gfx::native::ModelInput> &inputs, dq8::gfx::native::ModelRow &row)
{
    particleMesh(VuData(data), top, packets, mesh, inputs, row);
}

bool modelOnGpu(const Program &program)
{
    return program.kind == Kind::Strip || program.kind == Kind::Projector;
}

namespace
{
// The chunk's sections: its count, where each lies, and its bones (0: not
// skinned).
struct Sections
{
    uint32_t count, positions, normals, uvs, colors, skin, bones;
};
Sections sectionsOf(const Program &program, const VuData &vu, uint32_t top)
{
    Sections s{};
    s.count = std::min<uint32_t>(vu.half(top, 1), 256u);
    s.positions = top + 2u;
    s.normals = s.positions + vu.half(top, 3);
    s.uvs = s.positions + vu.half(top + 1u, 0);
    s.colors = s.positions + vu.half(top + 1u, 1);
    s.skin = s.positions + vu.half(top + 1u, 2);
    s.bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    return s;
}

// A bone's palette slot from its index half (as skin() addresses its
// matrix, 900 + 2 * index, wrapped): quad 896 - 4 x is slot x.
float slotOf(uint32_t half)
{
    const uint32_t quad = (900u + ((half * 2u) & 0xFFFFu)) & 1023u;
    if (quad > 896u || (896u - quad) % 4u != 0u || (896u - quad) / 4u > 35u)
        return 0.0f;
    return float((896u - quad) / 4u);
}
} // namespace

uint64_t modelChunkHash(const Program &program, const uint8_t *data, uint32_t top)
{
    const VuData vu(data);
    const Sections s = sectionsOf(program, vu, top);
    thread_local std::vector<uint8_t> bytes;
    bytes.clear();
    const auto add = [&](const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    add(vu.bytes(top), 16u);
    add(vu.bytes(top + 1u), 16u);
    // (The inputs' reading depends on the float D and B too.)
    const uint32_t fields[3] = {s.bones, static_cast<uint32_t>(program.uv),
                                (program.rawColor ? 1u : 0u) | (program.rawFloats ? 2u : 0u)};
    add(fields, sizeof(fields));
    const auto section = [&](uint32_t first, uint32_t quads) {
        for (uint32_t q = 0; q < quads; ++q)
            add(vu.bytes(first + q), 16u);
    };
    section(s.positions, s.count);
    section(s.normals, s.count);
    section(s.uvs, s.count);
    section(s.colors, s.count);
    if (s.bones)
        section(s.skin, s.count * 2u);
    return XXH3_64bits(bytes.data(), bytes.size());
}

void modelChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<dq8::gfx::native::ModelInput> &out, bool &list)
{
    const VuData vu(data);
    const Sections s = sectionsOf(program, vu, top);
    list = (vu.half(top, 0) & 7u) == 3u;
    out.resize(s.count);
    for (uint32_t k = 0; k < s.count; ++k)
    {
        dq8::gfx::native::ModelInput &v = out[k];
        // The float-skinning Bs take the EE's floats as they are.
        const auto value = [&](uint32_t q, uint32_t lane) {
            const int32_t word = vu.word(q, lane);
            return program.rawFloats ? fromBits(static_cast<uint32_t>(word)) : float(word);
        };
        for (uint32_t i = 0; i < 3u; ++i)
        {
            v.position[i] = value(s.positions + k, i);
            v.normal[i] = value(s.normals + k, i);
        }
        v.uv[0] = value(s.uvs + k, 0);
        v.uv[1] = value(s.uvs + k, 1);
        // The float-color D's colors are floats (fromBits), the rest ints.
        for (uint32_t i = 0; i < 4u; ++i)
            v.color[i] = program.rawColor ? fromBits(static_cast<uint32_t>(vu.word(s.colors + k, i)))
                                          : float(vu.word(s.colors + k, i));
        for (uint32_t b = 0; b < 4u; ++b)
        {
            const bool used = b < s.bones;
            v.bones[b] = used ? slotOf(vu.half(s.skin + k * 2u, b)) : 0.0f;
            v.weights[b] = used ? float(vu.word(s.skin + k * 2u + 1u, b)) : 0.0f;
        }
    }
}

bool outlineDraw(const Program &program, const uint8_t *data, uint32_t top,
                 std::vector<std::vector<uint8_t>> &packets, rg::NativeMesh &mesh,
                 std::vector<dq8::gfx::native::ModelInput> &inputs, uint32_t &edge,
                 dq8::gfx::native::ModelRow &row, std::vector<float> &palette)
{
    const VuData vu(data);
    edge = outlinePoints(vu, top);
    const uint32_t count = vu.half(top, 1);
    if (count < edge)
        return false;
    // Its points as modelChunk reads a strip's (the positions and the
    // skinning; the rest unused).
    bool list = false;
    modelChunk(program, data, top, inputs, list);
    inputs.resize(std::min<size_t>(inputs.size(), count / edge * edge));
    if (inputs.empty())
        return false;
    // buildOutline's packets with no vertices: PRIM (lines) and PRMODE,
    // then the vertex tag, its loop none (its PRIM, when it has one).
    packets.clear();
    std::vector<uint8_t> &state = packets.emplace_back();
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(0), sizeof(tag));
    tag[0] |= 0x8000u;
    putQuad(state, tag);
    putWords(state, 1u, 0u, 0u, 0u);
    putWords(state, vu.half(87, 0), 0u, 27u, 0u);
    std::vector<uint8_t> &vertexTag = packets.emplace_back();
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = 0x8000u;
    putQuad(vertexTag, tag);
    // The header: 68-71 to the window, the guard band as buildOutline tests
    // it; no fog (the program writes 0), no backface rule (the stage's own
    // test), its z as it is.
    rg::NativeMesh::Header &h = mesh.header;
    h = rg::NativeMesh::Header{};
    for (uint32_t r = 0; r < 4u; ++r)
    {
        const Vec4 q = vu.quad(68u + r);
        h.matrix[r][0] = q.x;
        h.matrix[r][1] = q.y;
        h.matrix[r][2] = q.z;
        h.matrix[r][3] = q.w;
    }
    const Vec4 viewport = vu.quad(16), band = vu.quad(17), offset = vu.quad(18);
    h.bandScale[0] = band.x * viewport.x;
    h.bandScale[1] = band.y * viewport.y;
    h.bandScale[2] = band.z;
    h.bandOffset[0] = offset.x;
    h.bandOffset[1] = offset.y;
    h.bandOffset[2] = offset.z;
    h.guardBand = 1u;
    h.triangleList = 1u;
    for (float &c : h.colorMax)
        c = 255.0f;
    mesh.vertices.clear();
    // The row: the skinning's constants; no color or UV piece.
    modelRow(program, data, top, row, palette);
    row.q[0][3] = 0.0f;
    row.q[1][3] = 0.0f;
    row.q[2][3] = 16.0f;
    return true;
}

void modelHeader(const Program &program, const uint8_t *data, uint32_t top,
                 std::vector<std::vector<uint8_t>> &packets, rg::NativeMesh &mesh)
{
    Chunk chunk;
    chunkHeader(program, VuData(data), top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));
    std::vector<uint8_t> &state = packets.emplace_back();
    uint32_t tag[4];
    std::memcpy(tag, chunk.header[0], sizeof(tag));
    tag[0] |= 0x8000u;
    putQuad(state, tag);
    putQuad(state, chunk.header[1]);
    putQuad(state, chunk.header[2]);
    mesh.header = chunk.mesh.header;
    mesh.vertices.clear();
    if (program.kind == Kind::Projector)
    {
        // projectorMesh's: its C culls and caps as the strip D pieces do.
        const VuData vu(data);
        rg::NativeMesh::Header &h = mesh.header;
        h.guardBand = 1u;
        h.backface = 1u;
        h.winding = vu.quad(111).z * ((vu.half(87, 1) & 0x1000u) ? 1.0f : -1.0f);
        h.depthScale = vu.quad(87).z;
        h.depthMax = vu.quad(1).y;
        for (float &c : h.colorMax)
            c = 255.0f;
    }
}

uint32_t modelSlots(const Program &program, const uint8_t *data, uint32_t top)
{
    const VuData vu(data);
    const Sections s = sectionsOf(program, vu, top);
    uint32_t slots = 0u;
    for (uint32_t k = 0; k < s.count; ++k)
        for (uint32_t b = 0; b < s.bones; ++b)
            slots = std::max(slots, uint32_t(slotOf(vu.half(s.skin + k * 2u, b))) + 1u);
    return slots;
}

void modelRow(const Program &program, const uint8_t *data, uint32_t top, dq8::gfx::native::ModelRow &row,
              std::vector<float> &palette, uint32_t knownSlots)
{
    const VuData vu(data);
    const Sections s = sectionsOf(program, vu, top);
    row = {};
    const auto put = [&](uint32_t at, float x, float y, float z, float w) {
        row.q[at][0] = x;
        row.q[at][1] = y;
        row.q[at][2] = z;
        row.q[at][3] = w;
    };
    const auto quad = [&](uint32_t at, uint32_t q) {
        const Vec4 v = vu.quad(q);
        put(at, v.x, v.y, v.z, v.w);
    };
    const Vec4 q77 = vu.quad(77), q78 = vu.quad(78), q79 = vu.quad(79);
    const uint32_t flags87 = vu.half(87, 1);
    put(0, q77.x, q77.y, q77.z, float(static_cast<uint32_t>(program.color)));
    put(1, q78.x, q78.y, q78.z, float(static_cast<uint32_t>(program.uv)));
    put(2, q79.x, q79.y, float(s.bones),
        float((program.white ? 1u : 0u) | ((flags87 & 0x100u) ? 2u : 0u) | (program.skinNormals ? 4u : 0u) |
              (program.rawFloats ? 8u : 0u) | (program.affine ? 128u : 0u) | (program.rawColor ? 256u : 0u)));
    // The base color (readChunk's): the material times 42 and the ambient
    // 83, or with 87.y & 0x200 the material times 43.
    Vec4 material = vu.quad(82);
    material.w *= vu.quad(81).w;
    const Vec4 light = vu.quad(42), ambient = vu.quad(83);
    Vec4 base{material.x * light.x + ambient.x * 128.0f, material.y * light.y + ambient.y * 128.0f,
              material.z * light.z + ambient.z * 128.0f, material.w * light.w};
    if ((program.color == Color::PerVertex || program.color == Color::Material) && (flags87 & 0x200u))
    {
        const Vec4 other = vu.quad(43);
        base = {material.x * other.x, material.y * other.y, material.z * other.z, material.w * other.w};
    }
    put(3, base.x, base.y, base.z, base.w);
    const Lights lights(vu);
    for (uint32_t i = 0; i < 3u; ++i)
        put(4u + i, lights.direction[i].x, lights.direction[i].y, lights.direction[i].z, lights.direction[i].w);
    for (uint32_t i = 0; i < 4u; ++i)
        put(7u + i, lights.color[i].x, lights.color[i].y, lights.color[i].z, 0.0f);
    put(11, lights.ambient.x, lights.ambient.y, lights.ambient.z, lights.ambient.w);
    const PointLights points(vu);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        put(12u + i, points.position[i].x, points.position[i].y, points.position[i].z, points.position[i].w);
        put(16u + i, points.color[i][0], points.color[i][1], points.color[i][2], 0.0f);
    }
    for (uint32_t i = 0; i < 4u; ++i)
        quad(20u + i, 60u + i);
    for (uint32_t i = 0; i < 3u; ++i)
        quad(24u + i, 64u + i);
    quad(27, 28);
    const SphereMap sphere(vu);
    put(28, sphere.rows[0][0], sphere.rows[0][1], sphere.rows[1][0], sphere.rows[1][1]);
    const Wave wave(vu);
    put(29, sphere.rows[2][0], sphere.rows[2][1], wave.a, wave.q);
    const Beam beam(vu);
    put(30, beam.shiftX, beam.shiftY, beam.centerX, beam.centerY);
    const Swell swell(vu);
    put(31, beam.scale, swell.q, swell.far, swell.inverseFar);
    for (uint32_t i = 0; i < 4u; ++i)
        put(32u + i, beam.col[i].x, beam.col[i].y, beam.col[i].z, beam.col[i].w);
    put(36, swell.lane[0], swell.lane[1], swell.lane[2], swell.lane[3]);
    // The scroll F (d3f6f631): the UVs moved by quad 88.xy (0 but for the
    // effect models).
    if (program.uv == Uv::Plain)
    {
        const Vec4 scroll = vu.quad(88);
        put(42, scroll.x, scroll.y, 0.0f, 0.0f);
    }
    if (program.kind == Kind::Projector)
    {
        // projectorMesh's: the texture matrix (the object's through the
        // projector's, quads 30-33), and the color (128, 128, 128, alpha).
        Vec4 projector[4];
        for (uint32_t i = 0; i < 4u; ++i)
            projector[i] = vu.quad(30u + i);
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const Vec4 t = transform(projector, vu.quad(60u + i));
            put(38u + i, t.x, t.y, t.z, t.w);
        }
        const float alpha = static_cast<float>(rg::nativeFtoi(vu.quad(82).w * vu.quad(81).w * 128.0f, 1.0f));
        put(3, 128.0f, 128.0f, 128.0f, alpha);
        row.q[0][3] = 6.0f; // C: the projector's color
        row.q[1][3] = 7.0f; // F: the projected UVs
    }
    // The bones the chunk uses: slot x is quads 896 - 4x to 899 - 4x.
    palette.clear();
    if (s.bones)
    {
        const uint32_t slots = knownSlots != ~0u ? knownSlots : modelSlots(program, data, top);
        palette.resize(size_t(slots) * 16u);
        for (uint32_t x = 0; x < slots; ++x)
            for (uint32_t r = 0; r < 4u; ++r)
            {
                const Vec4 m = vu.quad(896u - 4u * x + r);
                float *at = palette.data() + (size_t(x) * 4u + r) * 4u;
                at[0] = m.x;
                at[1] = m.y;
                at[2] = m.z;
                at[3] = m.w;
            }
    }
}

void Monitor::declined(const Program &program, const char *reason)
{
    ++m_declined[std::string(program.name) + ": " + reason];
}

void Monitor::tick()
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < m_next)
    {
        return;
    }
    if (m_next == 0)
    {
        m_next = now + 2;
        return;
    }
    m_next = now + 2;
    std::string line;
    for (const auto &[name, count] : m_native)
    {
        line += " " + name + "=" + std::to_string(count);
    }
    std::string left;
    for (const auto &[reason, count] : m_declined)
    {
        left += " [" + reason + "]=" + std::to_string(count);
    }
    std::fprintf(stderr, "[vu1native] gpu: native chunks%s; left to the VU1%s",
                 line.empty() ? " none" : line.c_str(), left.empty() ? " none" : left.c_str());
    std::fprintf(stderr, "\n");
}

} // namespace rg::vu1native
