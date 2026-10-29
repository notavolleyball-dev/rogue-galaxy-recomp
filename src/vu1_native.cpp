#include "vu1_native.h"

#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "gif_events.h"

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
//   F 0x39d0 nothing unless quad 88.xy is set (scrolling UVs);
//   C 0x10e0 the vertex colors: the int16 color / 32768 * 256 times
//            quad 82 * quad 42 + quad 83 * 128 (82.w scaled by 81.w);
//   E 0x2620 with quad 81.x & 2 only: clipping, before D;
//   D 0x1900 per vertex: clip = position * quads 68-71 (the matrix lands in
//            screen space), Q = 1/w, XYZ in 12.4, F the fog clamp(40.x +
//            40.w * w, 40.y, 40.z) in XYZF2's w, ST = UV * Q, the color
//            clamped to quad 39; ADC as its Cull says; then FOGCOL (quad 41
//            behind tag quad 19, its low word zeroed with quad 87.y & 0x10)
//            is kicked, then the chunk.
// The arithmetic runs in the VU's rounding (toward zero), set around each
// entry point (VuRounding): the compiler must not move float work across it.
#pragma STDC FENV_ACCESS ON

namespace rg::vu1native
{
namespace
{

constexpr uint32_t kEntry = 0x320u;

// Round toward zero while it lives, as VU1Interpreter::run sets it for the
// VU1: the products, sums and the divide then come out as the VU1's, bit for
// bit, done in its order. (Round to nearest left Z a unit off at times, which
// a pass drawn at nearly the same depth turned into missing pixels.)
class VuRounding
{
public:
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    VuRounding() : m_saved(_mm_getcsr()) { _mm_setcsr((m_saved & ~0x6000u) | 0x6000u); }
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
    {0x39d0u, 0x3a68u, 0xd3f6f6312a13ff1cull},
};
constexpr const char *kCommonNames[] = {"A", "F"};

// The B pieces. A calls B at 0x960, or with quad 76.z set at 0x960 plus
// the chunk's TOP+1.w pairs: af62ff64 has the one entry; 26a933a5 has three,
// 0 one bone, 2 two bones, 4 four (skin()).
struct DecodePiece
{
    Piece b;
    Decode decode;
};
constexpr DecodePiece kDecodes[] = {
    {{0x0960u, 0x0a38u, 0xaf62ff64ac67b45full}, Decode::Plain},
    {{0x0960u, 0x1020u, 0x26a933a58f99a9daull}, Decode::Skinned},
};

// The C pieces: both build the material color, quad 82 * 42 + 83 * 128
// (82.w scaled by 81.w); 70362be4 multiplies it by each vertex's int16
// color / 32768 * 256 (and has paths of its own with 87.y & 0x100 or
// 0x200, not read yet), 780d503e gives it to every vertex as it is (with
// 0x200, 82 * 43 and no ambient).
struct ColorPiece
{
    Piece c;
    Color color;
};
constexpr ColorPiece kColors[] = {
    {{0x10e0u, 0x1328u, 0x70362be426b17f94ull}, Color::PerVertex},
    {{0x10e0u, 0x11b0u, 0x780d503e649c9116ull}, Color::Material},
};

// The D pieces. (a786efabfa469ada is also the name of its compiled routine,
// the VIF1 thread's busiest in docs/STATUS.md.)
struct Finish
{
    Piece d;
    Cull cull;
    bool depthBounds;
};
constexpr Finish kFinishes[] = {
    {{0x1900u, 0x20c8u, 0xa786efabfa469adaull}, Cull::GuardBandAndBackface, false},
    {{0x1900u, 0x1c38u, 0x126a58a39a8f944aull}, Cull::GuardBand, false},
    {{0x1900u, 0x1bc0u, 0x4b7b81f2fe1beeefull}, Cull::None, false},
    // No culling; z times 87.z, at most 1.y; the bounds into 98 and 99.
    {{0x1900u, 0x1b98u, 0xb8ea8ed53805b0c4ull}, Cull::None, true},
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
        if (program.cull == shape.cull && program.color == shape.color && program.depthBounds == shape.depthBounds &&
            program.decode == shape.decode)
        {
            return &program;
        }
    }
    std::string &name = names.emplace_back("strip");
    name += shape.cull == Cull::GuardBandAndBackface ? ", guard band and backfaces"
            : shape.cull == Cull::GuardBand          ? ", guard band"
                                                     : "";
    name += shape.depthBounds ? ", depth and bounds" : "";
    name += shape.color == Color::Material ? ", one color" : "";
    name += shape.decode == Decode::Skinned ? ", skinned" : "";
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

// How many bones the skinning B's entry sums (A enters B at 0x960 plus
// TOP+1.w pairs when quad 76.z is set): 1, 2 or 4, or 0 for another entry.
uint32_t bonesOf(const VuData &vu, uint32_t top)
{
    const uint32_t entry = vu.half(76, 2) != 0u ? vu.half(top + 1u, 3) : 0u;
    return entry == 0u ? 1u : entry == 2u ? 2u : entry == 4u ? 4u : 0u;
}

// The skinning B: the position (w 1) scaled by each bone's weight (an int
// / 255), times the bone's matrix (quads 900 + 2 * index, rows 0-3, xyz),
// summed in ACC bone by bone and row by row. The indices are the low halves
// of the quad at `weights`, the weights the words of the next.
void skin(const VuData &vu, uint32_t weights, uint32_t bones, float position[3])
{
    uint32_t bits = 0x3B808081u; // the program's I: 1/255
    float scale;
    std::memcpy(&scale, &bits, sizeof(scale));
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

void putFloats(std::vector<uint8_t> &packet, float x, float y, float z, float w)
{
    const float words[4] = {x, y, z, w};
    putQuad(packet, words);
}

// A chunk read out of VU1 data memory: the FOGCOL packet, the first quads of
// the program's output (GIF header, PRIM, PRMODE, the vertex tag) and the
// strip as a mesh.
struct Chunk
{
    std::vector<uint8_t> fog;
    uint8_t header[4][16] = {};
    GSNativeMesh mesh;
};

void readChunk(const Program &program, const VuData &vu, uint32_t top, Chunk &chunk)
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

    GSNativeMesh::Header &h = chunk.mesh.header;
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

    const Vec4 positionScale = vu.quad(77);
    const Vec4 uvScale = vu.quad(79);
    Vec4 material = vu.quad(82);
    material.w *= vu.quad(81).w;
    const Vec4 light = vu.quad(42);
    const Vec4 ambient = vu.quad(83);
    Vec4 colorBase{material.x * light.x + ambient.x * 128.0f, material.y * light.y + ambient.y * 128.0f,
                   material.z * light.z + ambient.z * 128.0f, material.w * light.w};
    // The one-color C with 87.y & 0x200: quad 82 times 43, no ambient.
    if (program.color == Color::Material && (vu.half(87, 1) & 0x200u))
    {
        const Vec4 other = vu.quad(43);
        colorBase = {material.x * other.x, material.y * other.y, material.z * other.z, material.w * other.w};
    }
    const bool perVertex = program.color == Color::PerVertex;
    const uint32_t weights = positions + vu.half(top + 1u, 2);
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(vu, top) : 0u;
    chunk.mesh.vertices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        GSNativeMesh::Vertex &v = chunk.mesh.vertices[k];
        float position[3] = {itof15(vu.word(positions + k, 0)) * positionScale.x,
                             itof15(vu.word(positions + k, 1)) * positionScale.y,
                             itof15(vu.word(positions + k, 2)) * positionScale.z};
        if (bones != 0u)
        {
            skin(vu, weights + k * 2u, bones, position);
        }
        v.x = position[0];
        v.y = position[1];
        v.z = position[2];
        v.u = itof15(vu.word(uvs + k, 0)) * uvScale.x;
        v.v = itof15(vu.word(uvs + k, 1)) * uvScale.y;
        v.r = perVertex ? colorBase.x * itof15(vu.word(colors + k, 0)) * 256.0f : colorBase.x;
        v.g = perVertex ? colorBase.y * itof15(vu.word(colors + k, 1)) * 256.0f : colorBase.y;
        v.b = perVertex ? colorBase.z * itof15(vu.word(colors + k, 2)) * 256.0f : colorBase.z;
        v.a = perVertex ? colorBase.w * itof15(vu.word(colors + k, 3)) * 256.0f : colorBase.w;
    }
}

} // namespace

const Program *identify(const uint8_t *code, uint32_t startPC)
{
    if (startPC != kEntry)
    {
        return nullptr;
    }
    for (const Piece &piece : kCommon)
    {
        if (!matches(code, piece))
        {
            return nullptr;
        }
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    const ColorPiece *color = present(code, kColors, &ColorPiece::c);
    const Finish *finish = present(code, kFinishes, &Finish::d);
    if (!decode || !color || !finish)
    {
        return nullptr;
    }
    Program shape{nullptr, finish->cull, color->color, finish->depthBounds, decode->decode};
    return intern(shape);
}

std::string describePieces(const uint8_t *code, uint32_t startPC)
{
    if (startPC != kEntry)
    {
        return "-";
    }
    std::string out;
    for (size_t i = 0; i < std::size(kCommon); ++i)
    {
        out += matches(code, kCommon[i]) ? kCommonNames[i] : "-";
        out += ' ';
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    out += !decode                             ? "B:? "
           : decode->decode == Decode::Skinned ? "B:skinned "
                                               : "B:plain ";
    const char *color = "C:?";
    for (const ColorPiece &candidate : kColors)
    {
        if (matches(code, candidate.c))
        {
            color = candidate.color == Color::PerVertex ? "C:vertex colors" : "C:one color";
        }
    }
    const char *finish = "D:?";
    for (const Finish &candidate : kFinishes)
    {
        if (matches(code, candidate.d))
        {
            finish = candidate.depthBounds                        ? "D:depth and bounds"
                     : candidate.cull == Cull::GuardBandAndBackface ? "D:guard band and backfaces"
                     : candidate.cull == Cull::GuardBand            ? "D:guard band"
                                                                    : "D:no culling";
        }
    }
    return out + color + " " + finish;
}

const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips)
{
    const VuData vu(data);
    if (program.decode == Decode::Plain && vu.half(76, 2) != 0u)
        return "quad 76.z set (an entry into B other than its first)";
    if (program.decode == Decode::Skinned && bonesOf(vu, top) == 0u)
        return "an entry into the skinning B other than 0, 2 or 4";
    if ((vu.half(81, 0) & 2u) && !clips)
        return "clipping (slot E)";
    // PRIM from the chunk header. Only the backface piece reads it, with a
    // path of its own for triangle lists; the others run lists through
    // their strip code, whose ADC window of three vertices lands on each
    // triangle.
    const uint32_t primType = vu.half(top, 0) & 7u;
    if (primType == 3u && program.cull == Cull::GuardBandAndBackface)
        return "triangle list (D's own path)";
    if (primType != 3u && primType != 4u)
        return "neither strip nor triangle list";
    const uint32_t flags = vu.half(87, 1);
    if ((flags & 0x200u) && program.color == Color::PerVertex)
        return "quad 87.y & 0x200 (C's other color)";
    if ((flags & 0x100u) && program.color == Color::PerVertex)
        return "quad 87.y & 0x100 (C's other loop)";
    if (vu.quad(88).x != 0.0f || vu.quad(88).y != 0.0f)
        return "scrolling UVs (slot F)";
    return nullptr;
}

namespace
{

// The bounds a depth-and-bounds chunk leaves: quads 98 and 99 as they were,
// widened lane by lane by each vertex's projected x, y, z and fog.
void boundsAfter(const Program &program, const uint8_t *data, uint32_t top, Vec4 &least, Vec4 &most)
{
    const VuData vu(data);
    least = vu.quad(98);
    most = vu.quad(99);
    Chunk chunk;
    readChunk(program, vu, top, chunk);
    for (const GSNativeMesh::Vertex &v : chunk.mesh.vertices)
    {
        const GSNativeMeshPoint p = gsNativeMeshPoint(chunk.mesh.header, v);
        const float lanes[4] = {p.px, p.py, p.pz, p.pf};
        float *lo = &least.x, *hi = &most.x;
        for (int i = 0; i < 4; ++i)
        {
            lo[i] = std::fmin(lo[i], lanes[i]);
            hi[i] = std::fmax(hi[i], lanes[i]);
        }
    }
}

} // namespace

void applyMemoryEffects(const Program &program, uint8_t *data, uint32_t top)
{
    if (!program.depthBounds)
    {
        return;
    }
    const VuRounding rounding;
    Vec4 least, most;
    boundsAfter(program, data, top, least, most);
    std::memcpy(data + 98u * 16u, &least, sizeof(least));
    std::memcpy(data + 99u * 16u, &most, sizeof(most));
}

bool memoryEffectsMatch(const Program &program, const uint8_t *before, uint32_t top, const uint8_t *after)
{
    if (!program.depthBounds)
    {
        return true;
    }
    const VuRounding rounding;
    Vec4 least, most;
    boundsAfter(program, before, top, least, most);
    const VuData vu(after);
    const Vec4 vuLeast = vu.quad(98), vuMost = vu.quad(99);
    const float *a = &least.x, *b = &vuLeast.x, *c = &most.x, *d = &vuMost.x;
    for (int i = 0; i < 4; ++i)
    {
        const float scale = std::fmax(1.0f, std::fmax(std::fabs(a[i]), std::fabs(c[i])));
        if (std::fabs(a[i] - b[i]) > 1e-4f * scale || std::fabs(c[i] - d[i]) > 1e-4f * scale)
        {
            return false;
        }
    }
    return true;
}

void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets)
{
    const VuRounding rounding;
    Chunk chunk;
    readChunk(program, VuData(data), top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));

    // The output as D writes it: the header, then ST, RGBAQ and XYZF2 per
    // vertex, ADC as the program's culling says (ISW keeps only the low half
    // of the word it sets).
    const GSNativeMesh &mesh = chunk.mesh;
    const uint32_t count = static_cast<uint32_t>(mesh.vertices.size());
    std::vector<uint8_t> &packet = packets.emplace_back();
    packet.reserve(64u + count * 48u);
    for (const auto &quad : chunk.header)
    {
        putQuad(packet, quad);
    }
    std::vector<GSNativeMeshPoint> points(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        const GSNativeMeshPoint &p = points[k] = gsNativeMeshPoint(mesh.header, mesh.vertices[k]);
        putFloats(packet, p.s, p.t, p.q, 0.0f);
        putWords(packet, static_cast<uint32_t>(p.rgba[0]), static_cast<uint32_t>(p.rgba[1]),
                 static_cast<uint32_t>(p.rgba[2]), static_cast<uint32_t>(p.rgba[3]));
        putWords(packet, static_cast<uint32_t>(p.x16), static_cast<uint32_t>(p.y16), static_cast<uint32_t>(p.z16),
                 static_cast<uint32_t>(p.f16));
    }
    if (program.cull != Cull::None)
    {
        // The guard band piece clears the clip flags, then sets each
        // vertex's ADC from the last three judgments: the first vertex's
        // from its own. (It never draws; it is in the packet.)
        if (program.cull == Cull::GuardBand && count != 0u && points[0].outside)
        {
            const uint32_t w = (static_cast<uint32_t>(points[0].f16) & 0xFFFFu) | 0x8000u;
            std::memcpy(packet.data() + 64u + 2u * 16u + 12u, &w, 4u);
        }
        float winding = mesh.header.winding;
        for (uint32_t k = 1; k < count; ++k)
        {
            const bool skip = gsNativeMeshSkips(mesh.header, points.data(), k, winding);
            winding = -winding;
            const uint32_t w = (static_cast<uint32_t>(points[k].f16) & 0xFFFFu) | (skip ? 0x8000u : 0u);
            std::memcpy(packet.data() + 64u + (k * 3u + 2u) * 16u + 12u, &w, 4u);
        }
    }
}

void buildMesh(const Program &program, const uint8_t *data, uint32_t top,
               std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh)
{
    const VuRounding rounding;
    Chunk chunk;
    readChunk(program, VuData(data), top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));
    // PRIM and PRMODE behind the program's GIF tag, made the packet's end.
    std::vector<uint8_t> &state = packets.emplace_back();
    uint32_t tag[4];
    std::memcpy(tag, chunk.header[0], sizeof(tag));
    tag[0] |= 0x8000u;
    putQuad(state, tag);
    putQuad(state, chunk.header[1]);
    putQuad(state, chunk.header[2]);
    // The Z exactly as the program has it: the GPU rounds to nearest.
    for (GSNativeMesh::Vertex &v : chunk.mesh.vertices)
    {
        v.depth = gsNativeMeshDepth(chunk.mesh.header, v);
    }
    mesh = std::move(chunk.mesh);
}

Mode modeFromEnvironment()
{
    const char *value = std::getenv("RG_VU1_NATIVE");
    if (!value || std::strcmp(value, "0") == 0)
        return Mode::Off;
    if (std::strcmp(value, "check") == 0)
        return Mode::Check;
    if (std::strcmp(value, "skip") == 0)
        return Mode::Skip;
    if (std::strcmp(value, "gpu") == 0)
        return Mode::Gpu;
    return Mode::Replace;
}

void Monitor::declined(const Program &program, const char *reason)
{
    ++m_declined[std::string(program.name) + ": " + reason];
}

const char *Monitor::check(const Program &program, const std::vector<std::vector<uint8_t>> &vu,
                           const std::vector<std::vector<uint8_t>> &native)
{
    const auto events = [](const std::vector<std::vector<uint8_t>> &packets) {
        std::vector<rg::gif::GifEvent> out;
        rg::gif::GifState state;
        for (const auto &packet : packets)
        {
            rg::gif::decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, out);
        }
        return out;
    };
    rg::gif::Tolerance tolerance;
    tolerance.xy = 1u;
    tolerance.z = 16u;
    tolerance.rgba = 1u;
    tolerance.stq = 1e-5f;
    const auto reference = events(vu);
    const auto candidate = events(native);
    const rg::gif::Comparison result = rg::gif::compare(reference, candidate, tolerance);
    ++m_checked;
    if (result.mismatched == 0u && result.countDifference == 0)
    {
        return nullptr;
    }
    ++m_differ;
    const char *kind = result.countDifference != 0     ? "vertex count"
                       : result.kindMismatches != 0u   ? "ADC or register"
                       : result.maxXy > tolerance.xy   ? "position"
                       : result.maxZ > tolerance.z     ? "Z"
                       : result.maxRgba > tolerance.rgba ? "color"
                       : result.maxUv > tolerance.uv   ? "UV"
                       : result.maxStq > tolerance.stq ? "STQ"
                                                       : "fog";
    ++m_kinds[std::string(program.name) + ": " + kind];
    if (m_shown++ < 5u)
    {
        const size_t at = static_cast<size_t>(result.firstMismatch);
        std::fprintf(stderr, "[vu1native] %s differs at event %zu of %zu (%+lld events):\n  vu1    %s\n  native %s\n",
                     program.name, at, reference.size(), static_cast<long long>(result.countDifference),
                     at < reference.size() ? rg::gif::describe(reference[at]).c_str() : "-",
                     at < candidate.size() ? rg::gif::describe(candidate[at]).c_str() : "-");
    }
    return kind;
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
    std::fprintf(stderr, "[vu1native] %s: native chunks%s; left to the VU1%s", 
                 m_mode == Mode::Check  ? "check"
                 : m_mode == Mode::Skip ? "skip"
                 : m_mode == Mode::Gpu  ? "gpu"
                                        : "replace",
                 line.empty() ? " none" : line.c_str(), left.empty() ? " none" : left.c_str());
    if (m_mode == Mode::Check)
    {
        std::fprintf(stderr, "; checked %llu, differ %llu", static_cast<unsigned long long>(m_checked),
                     static_cast<unsigned long long>(m_differ));
        for (const auto &[kind, count] : m_kinds)
        {
            std::fprintf(stderr, " [%s]=%llu", kind.c_str(), static_cast<unsigned long long>(count));
        }
    }
    std::fprintf(stderr, "\n");
}

} // namespace rg::vu1native
