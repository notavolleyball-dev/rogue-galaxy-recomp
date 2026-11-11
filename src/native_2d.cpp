#include "native_2d.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "gs_writes.h"
#include "native_producer.h"

namespace rg::native2d
{
namespace
{
// The GS's vertex state as the shadow's writes leave it, and what each
// write does: a register written, or a vertex (XYZ2/XYZF2 kick, XYZ3/XYZF3
// not) with the vertex state at that moment.
struct VertexState
{
    uint16_t prim = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0;
    uint8_t f = 0;
};
struct Event
{
    enum class Kind : uint8_t
    {
        Register,
        Vertex,
    };
    Kind kind = Kind::Register;
    uint8_t reg = 0;
    uint64_t value = 0;
    bool drawing = false;
    bool fog = false;
    uint16_t prim = 0;
    uint16_t x = 0, y = 0; // 12.4 fixed point
    uint32_t z = 0;
    uint8_t f = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0; // 10.4 fixed point
};

float asFloat(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void vertex(VertexState &state, std::vector<Event> &out, bool drawing, bool fog, uint16_t x, uint16_t y, uint32_t z,
            uint8_t f)
{
    if (fog)
        state.f = f;
    Event event;
    event.kind = Event::Kind::Vertex;
    event.drawing = drawing;
    event.fog = fog;
    event.prim = state.prim;
    event.x = x;
    event.y = y;
    event.z = z;
    event.f = state.f;
    std::memcpy(event.rgba, state.rgba, sizeof(event.rgba));
    event.q = state.q;
    event.s = state.s;
    event.t = state.t;
    event.u = state.u;
    event.v = state.v;
    out.push_back(event);
}

// One write (src/gs_writes.h's, PACKED's own forms too) on the vertex state.
void apply(uint8_t reg, uint64_t value, VertexState &state, std::vector<Event> &out)
{
    namespace w = rg::gswrites;
    switch (reg)
    {
    case w::kPrim:
        state.prim = static_cast<uint16_t>(value & 0x7FFu);
        break;
    case w::kRgbaq:
        for (int i = 0; i < 4; ++i)
            state.rgba[i] = static_cast<uint8_t>(value >> (8 * i));
        state.q = asFloat(static_cast<uint32_t>(value >> 32));
        return;
    case w::kPackedRgba:
        for (int i = 0; i < 4; ++i)
            state.rgba[i] = static_cast<uint8_t>(value >> (8 * i));
        return;
    case w::kPackedQ:
        state.q = asFloat(static_cast<uint32_t>(value));
        return;
    case w::kSt:
        state.s = asFloat(static_cast<uint32_t>(value));
        state.t = asFloat(static_cast<uint32_t>(value >> 32));
        return;
    case w::kUv:
        state.u = static_cast<uint16_t>(value & 0x3FFFu);
        state.v = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        return;
    case w::kXyzf2:
    case w::kXyzf3:
        vertex(state, out, reg == w::kXyzf2, true, static_cast<uint16_t>(value), static_cast<uint16_t>(value >> 16),
               static_cast<uint32_t>((value >> 32) & 0xFFFFFFu), static_cast<uint8_t>(value >> 56));
        return;
    case w::kXyz2:
    case w::kXyz3:
        vertex(state, out, reg == w::kXyz2, false, static_cast<uint16_t>(value), static_cast<uint16_t>(value >> 16),
               static_cast<uint32_t>(value >> 32), 0u);
        return;
    case w::kFog:
        state.f = static_cast<uint8_t>(value >> 56);
        return;
    default:
        break;
    }
    Event event;
    event.reg = reg;
    event.value = value;
    out.push_back(event);
}

rg::NativeMesh::Header screenHeader()
{
    rg::NativeMesh::Header header{};
    header.matrix[0][0] = header.matrix[1][1] = header.matrix[2][2] = header.matrix[3][3] = 1.0f;
    header.fog[0] = header.fog[1] = 255.0f; // F = 255: no fog
    header.colorMax[0] = header.colorMax[1] = header.colorMax[2] = header.colorMax[3] = 255.0f;
    header.triangleList = 1u;
    return header;
}
// What the GS would carry from one block to the next (game thread): the GIF
// vertex state, a strip or fan under way, PRMODECONT and PRMODE.
struct Carried
{
    VertexState state;
    std::vector<Event> queue;
    uint16_t queuePrim = 0xFFFFu;
    bool attributesFromPrim = true; // PRMODECONT AC
    uint16_t prmode = 0u;
};
thread_local Carried t_carried;
} // namespace

bool build(const Span *packets, size_t count, std::vector<Piece> &out, std::string &why)
{
    // The last call's meshes' vertex storage, for this call's meshes: a
    // packet's meshes would otherwise allocate and free it each time.
    thread_local std::vector<std::vector<rg::NativeMesh::Vertex>> spare;
    thread_local std::vector<std::vector<uint32_t>> spareIndices;
    // And its register writes' (one vector a flush).
    thread_local std::vector<std::vector<std::pair<uint8_t, uint64_t>>> spareWrites;
    for (Piece &old : out)
    {
        if (old.mesh.vertices.capacity() != 0u && spare.size() < 64u)
        {
            old.mesh.vertices.clear();
            spare.push_back(std::move(old.mesh.vertices));
        }
        if (old.indices.capacity() != 0u && spareIndices.size() < 64u)
        {
            old.indices.clear();
            spareIndices.push_back(std::move(old.indices));
        }
        if (old.writes.capacity() != 0u && spareWrites.size() < 64u)
        {
            old.writes.clear();
            spareWrites.push_back(std::move(old.writes));
        }
    }
    out.clear();
    const auto freshWrites = [&](std::vector<std::pair<uint8_t, uint64_t>> &into) {
        if (!spareWrites.empty())
        {
            into.swap(spareWrites.back());
            spareWrites.pop_back();
        }
        into.clear();
    };
    Carried &carried = t_carried;
    VertexState &state = carried.state;
    std::vector<Event> &queue = carried.queue;
    uint16_t &queuePrim = carried.queuePrim;
    thread_local std::vector<Event> events;
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    freshWrites(writes);
    Piece mesh;
    uint16_t meshPrim = 0xFFFFu;

    const auto closeMesh = [&] {
        // Called at every register write: an empty mesh (none open, or one
        // just opened) keeps its storage and only forgets its PRIM.
        if (mesh.mesh.vertices.empty())
        {
            meshPrim = 0xFFFFu;
            return;
        }
        {
            if (meshPrim != 0xFFFFu && (meshPrim & 0x20u))
            {
                // Fogged: clip = (x w, y w, 0, w) with w = 1 + F/255, and
                // F = -255 + 255 w gives each vertex its own F back (the
                // shader's varyings are noperspective; Z is the vertex's own).
                rg::NativeMesh::Header &h = mesh.mesh.header;
                h.matrix[2][2] = 0.0f;
                h.matrix[2][3] = 1.0f;
                h.matrix[3][3] = 0.0f;
                h.fog[0] = -255.0f;
                h.fog[1] = 255.0f;
                h.fog[2] = 0.0f;
                h.fog[3] = 255.0f;
                // Texel UVs too: an FST draw reads gs_native.vert's
                // coordinate, (u, v) / w, as texels, without dividing by Q.
                const bool texels = (meshPrim & 0x110u) == 0x110u;
                for (rg::NativeMesh::Vertex &v : mesh.mesh.vertices)
                {
                    v.x *= v.z;
                    v.y *= v.z;
                    if (texels)
                    {
                        v.u *= v.z;
                        v.v *= v.z;
                    }
                }
            }
            mesh.mesh.header.vertexCount = static_cast<uint32_t>(mesh.mesh.vertices.size());
            out.push_back(std::move(mesh));
        }
        // Moved from: only the vertex storage needs replacing (openMesh sets
        // the header and texelUvs before any vertex).
        mesh.mesh.vertices = {};
        mesh.indices = {};
        if (!spare.empty())
        {
            mesh.mesh.vertices = std::move(spare.back());
            spare.pop_back();
        }
        if (!spareIndices.empty())
        {
            mesh.indices = std::move(spareIndices.back());
            spareIndices.pop_back();
        }
        meshPrim = 0xFFFFu;
    };
    const auto flushWrites = [&] {
        if (!writes.empty())
        {
            closeMesh();
            Piece piece;
            piece.writes.swap(writes);
            out.push_back(std::move(piece));
            freshWrites(writes);
        }
    };
    const auto vertexOf = [&](const Event &e, const Event &provoking, uint16_t prim, bool &ok) {
        rg::NativeMesh::Vertex v;
        v.x = static_cast<float>(e.x) / 16.0f;
        v.y = static_cast<float>(e.y) / 16.0f;
        v.z = static_cast<float>(e.z);
        v.depth = v.z; // the Z as the packet has it
        if (prim & 0x20u) // FGE: w carries F (closeMesh scales x and y by it)
            v.z = 1.0f + (static_cast<float>(e.f) + 0.25f) / 255.0f;
        const Event &c = (prim & 0x08u) ? e : provoking; // IIP
        v.r = c.rgba[0];
        v.g = c.rgba[1];
        v.b = c.rgba[2];
        v.a = c.rgba[3];
        if (prim & 0x10u) // TME
        {
            if (prim & 0x100u) // FST: texel UVs, 10.4 (Piece::texelUvs)
            {
                v.u = static_cast<float>(e.u) / 16.0f;
                v.v = static_cast<float>(e.v) / 16.0f;
            }
            else
            {
                const float q = e.q != 0.0f ? e.q : 1.0f;
                v.u = e.s / q;
                v.v = e.t / q;
            }
        }
        return v;
    };
    const auto openMesh = [&](uint16_t prim) {
        if (meshPrim == prim)
            return;
        closeMesh();
        // The draw state the mesh takes is the GS's PRIM: its attributes,
        // as a triangle, written just before it.
        writes.emplace_back(0x00u, static_cast<uint64_t>((prim & ~7u) | 3u));
        flushWrites();
        mesh.mesh.header = screenHeader();
        mesh.texelUvs = (prim & 0x110u) == 0x110u;
        meshPrim = prim;
    };
    const auto triangle = [&](const Event &a, const Event &b, const Event &c,
                              uint16_t prim, bool &ok) {
        openMesh(prim);
        const uint32_t base = static_cast<uint32_t>(mesh.mesh.vertices.size());
        mesh.mesh.vertices.push_back(vertexOf(a, c, prim, ok));
        mesh.mesh.vertices.push_back(vertexOf(b, c, prim, ok));
        mesh.mesh.vertices.push_back(vertexOf(c, c, prim, ok));
        mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u});
    };
    // Four corners, as the triangles (0, 1, 3) and (0, 3, 2).
    const auto quad = [&](const rg::NativeMesh::Vertex &v0, const rg::NativeMesh::Vertex &v1,
                          const rg::NativeMesh::Vertex &v2, const rg::NativeMesh::Vertex &v3) {
        const uint32_t base = static_cast<uint32_t>(mesh.mesh.vertices.size());
        mesh.mesh.vertices.insert(mesh.mesh.vertices.end(), {v0, v1, v2, v3});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 3u, base, base + 3u, base + 2u});
    };
    const auto sprite = [&](const Event &a, const Event &b, uint16_t prim, bool &ok) {
        openMesh(prim);
        // Corners from the two vertices; Z and colour are the second's.
        Event corner[4] = {a, b, a, b};
        corner[0].z = corner[2].z = b.z;
        corner[1].x = b.x; corner[1].y = a.y; corner[1].u = b.u; corner[1].v = a.v; corner[1].s = b.s; corner[1].t = a.t;
        corner[2].x = a.x; corner[2].y = b.y; corner[2].u = a.u; corner[2].v = b.v; corner[2].s = a.s; corner[2].t = b.t;
        corner[1].q = corner[2].q = b.q;
        corner[0].q = a.q;
        const rg::NativeMesh::Vertex v0 = vertexOf(corner[0], b, prim & ~0x08u, ok);
        const rg::NativeMesh::Vertex v1 = vertexOf(corner[1], b, prim & ~0x08u, ok);
        const rg::NativeMesh::Vertex v2 = vertexOf(corner[2], b, prim & ~0x08u, ok);
        const rg::NativeMesh::Vertex v3 = vertexOf(corner[3], b, prim & ~0x08u, ok);
        quad(v0, v1, v2, v3);
    };

    bool ok = true;
    bool anyVertex = false;
    for (size_t index = 0; index < count; ++index)
    {
        const Span &packet = packets[index];
        {
            const uint32_t at = 0u, size = packet.size;
            // Its writes (every GIF packet in the bytes: the GS reads on past
            // an EOP), and what they do to the vertex state. A tag cut at
            // the end is taken as far as it goes, as the GS would.
            thread_local std::vector<rg::gswrites::Write> regs;
            regs.clear();
            const rg::gswrites::Read read = rg::gswrites::read(packet.data, packet.size, regs);
            if (read.image)
            {
                // An upload: on as it is, in its place.
                flushWrites();
                closeMesh();
                queue.clear();
                Piece piece;
                piece.view = packet.data + at;
                piece.viewSize = size;
                out.push_back(std::move(piece));
                continue;
            }
            events.clear();
            for (const auto &[reg, value] : regs)
                apply(reg, value, state, events);
            for (const Event &e : events)
            {
                if (e.kind == Event::Kind::Register)
                {
                    if (e.reg == 0x1Au)
                        carried.attributesFromPrim = (e.value & 1u) != 0u;
                    if (e.reg == 0x1Bu)
                        carried.prmode = static_cast<uint16_t>(e.value & 0x7F8u);
                    if (e.reg == 0x00u)
                    {
                        queue.clear();
                        queuePrim = 0xFFFFu;
                    }
                    closeMesh();
                    writes.emplace_back(e.reg, e.value);
                    continue;
                }
                anyVertex = true;
                // With PRMODECONT's AC clear the attributes are PRMODE's.
                const uint16_t prim = carried.attributesFromPrim
                                          ? e.prim
                                          : static_cast<uint16_t>((e.prim & 7u) | carried.prmode);
                const uint32_t type = prim & 7u;
                if (type == 7u)
                {
                    why = "a reserved primitive type";
                    return false;
                }
                if (prim != queuePrim)
                {
                    queue.clear();
                    queuePrim = prim;
                }
                switch (type)
                {
                case 0u: // point: a pixel's square
                    if (e.drawing)
                    {
                        Event a = e, b = e;
                        a.x = static_cast<uint16_t>(e.x & ~15u);
                        a.y = static_cast<uint16_t>(e.y & ~15u);
                        b.x = static_cast<uint16_t>(a.x + 16u);
                        b.y = static_cast<uint16_t>(a.y + 16u);
                        sprite(a, b, static_cast<uint16_t>(prim & ~0x08u), ok);
                    }
                    break;
                case 1u: // line
                case 2u: // line strip
                    queue.push_back(e);
                    if (queue.size() > 2u)
                        queue.erase(queue.begin());
                    if (queue.size() == 2u && e.drawing)
                    {
                        // Half a pixel either side of the centre line, as the
                        // backend widens the GS's lines.
                        const Event &p0 = queue[0], &p1 = queue[1];
                        const float dx = (static_cast<float>(p1.x) - p0.x) / 16.0f;
                        const float dy = (static_cast<float>(p1.y) - p0.y) / 16.0f;
                        const float length = std::sqrt(dx * dx + dy * dy);
                        const float nx = length > 1e-6f ? -dy / length * 0.5f : 0.5f;
                        const float ny = length > 1e-6f ? dx / length * 0.5f : 0.0f;
                        openMesh(prim);
                        rg::NativeMesh::Vertex v[4] = {vertexOf(p0, p1, prim, ok), vertexOf(p0, p1, prim, ok),
                                                     vertexOf(p1, p1, prim, ok), vertexOf(p1, p1, prim, ok)};
                        v[0].x += nx; v[0].y += ny;
                        v[1].x -= nx; v[1].y -= ny;
                        v[2].x -= nx; v[2].y -= ny;
                        v[3].x += nx; v[3].y += ny;
                        quad(v[0], v[1], v[3], v[2]); // (0, 1, 2) and (0, 2, 3)
                        if (type == 1u)
                            queue.clear();
                    }
                    else if (type == 1u && queue.size() == 2u)
                    {
                        queue.clear();
                    }
                    break;
                case 6u: // sprite
                    queue.push_back(e);
                    if (queue.size() == 2u)
                    {
                        if (e.drawing)
                            sprite(queue[0], queue[1], prim, ok);
                        queue.clear();
                    }
                    break;
                case 3u: // triangle
                    queue.push_back(e);
                    if (queue.size() == 3u)
                    {
                        if (e.drawing)
                            triangle(queue[0], queue[1], queue[2], prim, ok);
                        queue.clear();
                    }
                    break;
                case 4u: // strip
                    queue.push_back(e);
                    if (queue.size() > 3u)
                        queue.erase(queue.begin());
                    if (queue.size() == 3u && e.drawing)
                        triangle(queue[0], queue[1], queue[2], prim, ok);
                    break;
                case 5u: // fan
                    if (queue.size() == 3u)
                    {
                        queue[1] = queue[2];
                        queue.pop_back();
                    }
                    queue.push_back(e);
                    if (queue.size() == 3u && e.drawing)
                        triangle(queue[0], queue[1], queue[2], prim, ok);
                    break;
                default:
                    break;
                }

            }
        }
    }
    // The vertex registers the meshes took in (RGBAQ, ST, UV, FOG) and PRIM
    // as the packets left them: the GS keeps them for what comes next, and a
    // packet after the block may draw with them (an ST and XYZ with the Q of
    // an earlier RGBAQ).
    if (anyVertex)
    {
        uint32_t q;
        std::memcpy(&q, &state.q, sizeof(q));
        uint32_t sBits, tBits;
        std::memcpy(&sBits, &state.s, sizeof(sBits));
        std::memcpy(&tBits, &state.t, sizeof(tBits));
        closeMesh();
        const auto put = [&](uint8_t reg, uint64_t value) { writes.emplace_back(reg, value); };
        put(0x00u, static_cast<uint64_t>(state.prim));
        put(0x01u, static_cast<uint64_t>(state.rgba[0]) | (static_cast<uint64_t>(state.rgba[1]) << 8) |
                                       (static_cast<uint64_t>(state.rgba[2]) << 16) |
                                       (static_cast<uint64_t>(state.rgba[3]) << 24) | (static_cast<uint64_t>(q) << 32));
        put(0x02u, static_cast<uint64_t>(sBits) | (static_cast<uint64_t>(tBits) << 32));
        put(0x03u, static_cast<uint64_t>(state.u & 0x3FFFu) | (static_cast<uint64_t>(state.v & 0x3FFFu) << 16));
        put(0x0Au, static_cast<uint64_t>(state.f) << 56);
    }
    flushWrites();
    closeMesh();
    return true;
}

} // namespace rg::native2d
