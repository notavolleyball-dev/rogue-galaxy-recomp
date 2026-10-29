// The field's strip program, natively: what the VU1 program
// 9f727028f73861c3:320 draws, computed from the inputs the game gave it.
//
// Read from the program's code (tools/vu1/vudis.py over a recording's
// coverage), slot by slot. The MSCAL at 0x320 only ends; each mesh chunk is
// UNPACKed and then run by an MSCNT from 0x328:
//   A 0x328  reads the chunk header at TOP, writes the GIF header of the
//            output (template quad 0, PRIM from the header, PRMODE from
//            quad 87.x, the vertex tag from the header and quad 86) and the
//            section pointers (quads 103-104), the guard band (109-110);
//   B 0x960  int16 positions, normals and UVs to floats: x / 32768 times the
//            mesh scales in quads 77, 78, 79;
//   F 0x39d0 nothing unless quad 88.xy is set (scrolling UVs, not done here);
//   C 0x10e0 the vertex colours: the int16 colour / 32768 * 256 times
//            quad 82 * quad 42 + quad 83 * 128 (quad 82's w scaled by 81.w);
//   E 0x2620 the clipping path, with quad 81.x & 2 (not done here);
//   D 0x1900 per vertex: clip = position * quads 68-71, Q = 1/w, XYZ the
//            projected position in 12.4 (the matrix lands in screen space),
//            F the fog clamp(40.x + 40.w * w, 40.y, 40.z), ST = UV * Q, the
//            colour clamped to quad 39; a triangle that leaves the guard
//            band, |(xyz + quad 110) * quad 109| > 1, or faces away (the
//            strip's alternating winding times quad 111.z, negated by quad
//            87.y & 0x1000) sets ADC on its last vertex; then FOGCOL from
//            quad 41 behind tag quad 19 is kicked, then the chunk.
// It builds the same packets from those inputs with plain float arithmetic,
// as a GPU would, and the oracle decodes them like the recorded ones.
#include <cmath>
#include <cstring>

#include "oracle.h"

namespace rg::oracle
{
namespace
{

constexpr uint64_t kProgramHash = 0x9f727028f73861c3ull;
constexpr uint32_t kProgramPC = 0x320u;
// The code it was read from, in bytes: slots A, B, C, D and F as far as the
// field's runs reached them. Another image with the same bytes there runs
// the same program (slot E is only reached with quad 81.x & 2).
constexpr uint32_t kSlotRanges[][2] = {
    {0x320, 0x5d8}, {0x960, 0xa38}, {0x10e0, 0x1328}, {0x1900, 0x20c8}, {0x39d0, 0x3a68}};

struct Vec4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

class VuData
{
public:
    explicit VuData(const std::vector<uint8_t> &data) : m_data(data.data()) {}

    Vec4 quad(uint32_t q) const
    {
        Vec4 v;
        std::memcpy(&v, m_data + (q & 1023u) * 16u, sizeof(v));
        return v;
    }
    int32_t word(uint32_t q, uint32_t lane) const
    {
        int32_t v;
        std::memcpy(&v, m_data + (q & 1023u) * 16u + lane * 4u, sizeof(v));
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

// FTOI0 and FTOI4: truncation toward zero, saturated.
int32_t ftoi(float v, float scale)
{
    const double scaled = static_cast<double>(v) * scale;
    if (!(scaled > -2147483648.0))
        return INT32_MIN;
    if (scaled >= 2147483647.0)
        return INT32_MAX;
    return static_cast<int32_t>(scaled);
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

class NativeStrip final : public Candidate
{
public:
    const char *name() const override { return "native strip (the code of 9f727028f73861c3:320)"; }

    bool accepts(const Run &run) const override
    {
        if (!sameProgram(run))
        {
            return false;
        }
        const char *reason = decline(run);
        if (reason)
        {
            ++m_declined[reason];
        }
        return reason == nullptr;
    }

    std::map<std::string, uint32_t> declined() const override { return m_declined; }

    void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) override
    {
        events.clear();
        learn(recording, run);
        // The MSCAL only ends; the MSCNTs draw.
        if (!(run.header.flags & rg::vu1replay::kRunResume) || !sameProgram(run) || decline(run))
        {
            return;
        }
        const VuData vu(run.data);
        GifState state;
        std::vector<uint8_t> packet;

        // FOGCOL (quad 41) behind the tag in quad 19, NLOOP 1 and EOP.
        const Vec4 fogTag = vu.quad(19);
        uint32_t fogTagWords[4];
        std::memcpy(fogTagWords, &fogTag, sizeof(fogTagWords));
        putWords(packet, 0x8001u, fogTagWords[1], fogTagWords[2], fogTagWords[3]);
        putQuad(packet, vu.bytes(41));
        decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);

        // The chunk: its header at TOP, its sections after it.
        const uint32_t top = run.header.top;
        const uint32_t prim = vu.half(top, 0);
        const uint32_t count = vu.half(top, 1);
        const uint32_t vertexTag = vu.half(top, 2);
        const uint32_t positions = top + 2u;
        const uint32_t uvs = positions + vu.half(top + 1u, 0);
        const uint32_t colours = positions + vu.half(top + 1u, 1);

        packet.clear();
        putQuad(packet, vu.bytes(0));
        putWords(packet, prim, 0u, 0u, 0u);
        putWords(packet, vu.half(87, 0), 0u, 27u, 0u);
        uint32_t tagWords[4];
        std::memcpy(tagWords, vu.bytes(86), sizeof(tagWords));
        putWords(packet, vertexTag, tagWords[1], tagWords[2], tagWords[3]);

        const Vec4 positionScale = vu.quad(77);
        const Vec4 uvScale = vu.quad(79);
        Vec4 material = vu.quad(82);
        material.w *= vu.quad(81).w;
        const Vec4 lightColour = vu.quad(42);
        const Vec4 ambient = vu.quad(83);
        const Vec4 colourBase{material.x * lightColour.x + ambient.x * 128.0f,
                              material.y * lightColour.y + ambient.y * 128.0f,
                              material.z * lightColour.z + ambient.z * 128.0f, material.w * lightColour.w};
        const Vec4 colourMax = vu.quad(39);
        const Vec4 fog = vu.quad(40);
        const Vec4 m0 = vu.quad(68), m1 = vu.quad(69), m2 = vu.quad(70), m3 = vu.quad(71);
        const Vec4 viewport = vu.quad(16), band = vu.quad(17), bandOffset = vu.quad(18);
        const Vec4 bandScale{band.x * viewport.x, band.y * viewport.y, band.z, band.w};
        float winding = vu.quad(111).z * ((vu.half(87, 1) & 0x1000u) ? -1.0f : 1.0f);

        std::vector<Vec4> screen(count);
        std::vector<bool> outside(count);
        std::vector<uint32_t> xyzf(count * 4u);
        for (uint32_t k = 0; k < count; ++k)
        {
            const Vec4 p{itof15(vu.word(positions + k, 0)) * positionScale.x,
                         itof15(vu.word(positions + k, 1)) * positionScale.y,
                         itof15(vu.word(positions + k, 2)) * positionScale.z, 1.0f};
            const Vec4 clip{p.x * m0.x + p.y * m1.x + p.z * m2.x + m3.x, p.x * m0.y + p.y * m1.y + p.z * m2.y + m3.y,
                            p.x * m0.z + p.y * m1.z + p.z * m2.z + m3.z, p.x * m0.w + p.y * m1.w + p.z * m2.w + m3.w};
            const float q = 1.0f / clip.w;
            const Vec4 s{clip.x * q, clip.y * q, clip.z * q, 0.0f};
            screen[k] = s;
            const float f = std::fmax(std::fmin(fog.x + fog.w * clip.w, fog.y), fog.z);
            const float gx = (s.x + bandOffset.x) * bandScale.x;
            const float gy = (s.y + bandOffset.y) * bandScale.y;
            const float gz = (s.z + bandOffset.z) * bandScale.z;
            outside[k] = std::fabs(gx) > 1.0f || std::fabs(gy) > 1.0f || std::fabs(gz) > 1.0f;
            xyzf[k * 4u + 0u] = static_cast<uint32_t>(ftoi(s.x, 16.0f));
            xyzf[k * 4u + 1u] = static_cast<uint32_t>(ftoi(s.y, 16.0f));
            xyzf[k * 4u + 2u] = static_cast<uint32_t>(ftoi(s.z, 16.0f));
            xyzf[k * 4u + 3u] = static_cast<uint32_t>(ftoi(f, 16.0f));

            const float u = itof15(vu.word(uvs + k, 0)) * uvScale.x;
            const float v = itof15(vu.word(uvs + k, 1)) * uvScale.y;
            putFloats(packet, u * q, v * q, q, 0.0f);
            const Vec4 c{std::fmin(colourBase.x * itof15(vu.word(colours + k, 0)) * 256.0f, colourMax.x),
                         std::fmin(colourBase.y * itof15(vu.word(colours + k, 1)) * 256.0f, colourMax.y),
                         std::fmin(colourBase.z * itof15(vu.word(colours + k, 2)) * 256.0f, colourMax.z),
                         std::fmin(colourBase.w * itof15(vu.word(colours + k, 3)) * 256.0f, colourMax.w)};
            putWords(packet, ftoi(c.x, 1.0f), ftoi(c.y, 1.0f), ftoi(c.z, 1.0f), ftoi(c.w, 1.0f));
            putWords(packet, 0u, 0u, 0u, 0u); // XYZF2, filled in below
        }
        // ADC on a triangle's last vertex: outside the band, or facing away.
        // The loop's prologue tests the second vertex too, against a
        // "previous" vertex at the origin (the registers start at zero):
        // it never draws, but its ADC is part of the packet.
        winding = -winding;
        const Vec4 origin;
        for (uint32_t k = 1; k < count; ++k)
        {
            const Vec4 &a = k >= 2 ? screen[k - 2] : origin, &b = screen[k - 1], &c = screen[k];
            const float e1x = b.x - a.x, e1y = b.y - a.y;
            const float e2x = c.x - b.x, e2y = c.y - b.y;
            const float facing = (e2x * e1y - e1x * e2y) * winding;
            winding = -winding;
            const bool skip = (k >= 2 && outside[k - 2]) || outside[k - 1] || outside[k] || facing < 0.0f;
            xyzf[k * 4u + 3u] = (xyzf[k * 4u + 3u] & 0xFFFFu) | (skip ? 0x8000u : 0u);
        }
        for (uint32_t k = 0; k < count; ++k)
        {
            std::memcpy(packet.data() + 64u + (k * 3u + 2u) * 16u, &xyzf[k * 4u], 16u);
        }
        decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
    }

private:
    // Whether the run's program has this one's code in the slots it runs.
    void learn(const Recording &recording, const Run &run)
    {
        if (run.programPC != kProgramPC || m_same.count(run.programHash))
        {
            return;
        }
        const auto reference = recording.codes.find(kProgramHash);
        const auto image = recording.codes.find(run.programHash);
        bool same = reference != recording.codes.end() && image != recording.codes.end();
        for (const auto &range : kSlotRanges)
        {
            same = same && std::memcmp(reference->second.data() + range[0], image->second.data() + range[0],
                                       range[1] - range[0]) == 0;
        }
        m_same[run.programHash] = same;
    }
    bool sameProgram(const Run &run) const
    {
        const auto found = m_same.find(run.programHash);
        return run.programPC == kProgramPC && found != m_same.end() && found->second;
    }

    std::map<uint64_t, bool> m_same;

    // Why a run of the program is left to the VU1, or null.
    static const char *decline(const Run &run)
    {
        if (!(run.header.flags & rg::vu1replay::kRunResume))
        {
            return nullptr;
        }
        const VuData vu(run.data);
        if (vu.half(76, 2) != 0u)
            return "quad 76.z set (A's other branch)";
        if (vu.half(81, 0) & 2u)
            return "clipping (slot E)";
        if (vu.half(run.header.top, 0) == 3u)
            return "triangle list";
        const uint32_t flags = vu.half(87, 1);
        if (flags & 0x200u)
            return "quad 87.y & 0x200 (C's other colour)";
        if (flags & 0x100u)
            return "quad 87.y & 0x100 (C's other loop)";
        if (flags & 0x10u)
            return "quad 87.y & 0x10 (D's extra register)";
        if (vu.quad(88).x != 0.0f || vu.quad(88).y != 0.0f)
            return "scrolling UVs (slot F)";
        return nullptr;
    }

    mutable std::map<std::string, uint32_t> m_declined;
};

} // namespace

std::unique_ptr<Candidate> makeNativeStrip()
{
    return std::make_unique<NativeStrip>();
}

} // namespace rg::oracle
