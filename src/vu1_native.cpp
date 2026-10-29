#include "vu1_native.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gif_events.h"

// The strip programs, piece by piece (byte addresses):
//   the MSCAL at 0x320 only ends; each mesh chunk is UNPACKed and then run by
//   an MSCNT from 0x328, through
//   A 0x328  the chunk header at TOP (PRIM, vertex count, vertex tag, the
//            sections' offsets), the GIF header of the output (template
//            quad 0, PRIM, PRMODE from quad 87.x, the vertex tag with quad
//            86), the guard band (quad 109 = 17 with xy times 16.xy, 110 = 18);
//   B 0x960  int16 positions, normals and UVs to floats: x / 32768 times the
//            mesh scales in quads 77, 78, 79;
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
namespace rg::vu1native
{
namespace
{

constexpr uint32_t kEntry = 0x320u;

struct Piece
{
    uint32_t start;
    uint32_t end;
    uint64_t hash;
};

// Slots A, B, C and F, the same in every strip program met so far.
constexpr Piece kCommon[] = {
    {0x0320u, 0x05d8u, 0x07151c2597283286ull},
    {0x0960u, 0x0a38u, 0xaf62ff64ac67b45full},
    {0x10e0u, 0x1328u, 0x70362be426b17f94ull},
    {0x39d0u, 0x3a68u, 0xd3f6f6312a13ff1cull},
};

struct Finish
{
    Piece d;
    Program program;
};

// The D pieces. (a786efabfa469ada is also the name of its compiled routine,
// the VIF1 thread's busiest in docs/STATUS.md.)
constexpr Finish kFinishes[] = {
    {{0x1900u, 0x20c8u, 0xa786efabfa469adaull}, {"strip, guard band and backfaces", Cull::GuardBandAndBackface}},
    {{0x1900u, 0x1c38u, 0x126a58a39a8f944aull}, {"strip, guard band", Cull::GuardBand}},
    {{0x1900u, 0x1bc0u, 0x4b7b81f2fe1beeefull}, {"strip", Cull::None}},
};

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
    h.triangleList = (prim & 7u) == 3u ? 1u : 0u;

    const Vec4 positionScale = vu.quad(77);
    const Vec4 uvScale = vu.quad(79);
    Vec4 material = vu.quad(82);
    material.w *= vu.quad(81).w;
    const Vec4 light = vu.quad(42);
    const Vec4 ambient = vu.quad(83);
    const Vec4 colorBase{material.x * light.x + ambient.x * 128.0f, material.y * light.y + ambient.y * 128.0f,
                         material.z * light.z + ambient.z * 128.0f, material.w * light.w};
    chunk.mesh.vertices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        GSNativeMesh::Vertex &v = chunk.mesh.vertices[k];
        v.x = itof15(vu.word(positions + k, 0)) * positionScale.x;
        v.y = itof15(vu.word(positions + k, 1)) * positionScale.y;
        v.z = itof15(vu.word(positions + k, 2)) * positionScale.z;
        v.u = itof15(vu.word(uvs + k, 0)) * uvScale.x;
        v.v = itof15(vu.word(uvs + k, 1)) * uvScale.y;
        v.r = colorBase.x * itof15(vu.word(colors + k, 0)) * 256.0f;
        v.g = colorBase.y * itof15(vu.word(colors + k, 1)) * 256.0f;
        v.b = colorBase.z * itof15(vu.word(colors + k, 2)) * 256.0f;
        v.a = colorBase.w * itof15(vu.word(colors + k, 3)) * 256.0f;
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
    for (const Finish &finish : kFinishes)
    {
        if (matches(code, finish.d))
        {
            return &finish.program;
        }
    }
    return nullptr;
}

const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips)
{
    const VuData vu(data);
    if (vu.half(76, 2) != 0u)
        return "quad 76.z set (A's other branch)";
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
    if (flags & 0x200u)
        return "quad 87.y & 0x200 (C's other color)";
    if (flags & 0x100u)
        return "quad 87.y & 0x100 (C's other loop)";
    if (vu.quad(88).x != 0.0f || vu.quad(88).y != 0.0f)
        return "scrolling UVs (slot F)";
    return nullptr;
}

void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets)
{
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

void Monitor::check(const Program &program, const std::vector<std::vector<uint8_t>> &vu,
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
        return;
    }
    ++m_differ;
    if (m_shown++ < 5u)
    {
        const size_t at = static_cast<size_t>(result.firstMismatch);
        std::fprintf(stderr, "[vu1native] %s differs at event %zu of %zu (%+lld events):\n  vu1    %s\n  native %s\n",
                     program.name, at, reference.size(), static_cast<long long>(result.countDifference),
                     at < reference.size() ? rg::gif::describe(reference[at]).c_str() : "-",
                     at < candidate.size() ? rg::gif::describe(candidate[at]).c_str() : "-");
    }
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
    }
    std::fprintf(stderr, "\n");
}

} // namespace rg::vu1native
