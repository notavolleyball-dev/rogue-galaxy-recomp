#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "native_types.h"

// The 2D of a GsPkt block's shadow (src/native_gspkt.h, the GIF stream the
// block would have carried, made on the game thread): its primitives
// (sprites, triangles, strips, fans; points and lines as thin quads) become
// meshes in the GS's window coordinates, a triangle list each, for
// rg::nativeproducer::piece, in order with its register writes. M6: no
// packet the game sends goes through it any more (src/gs_writes.h reads
// those as register state).
namespace rg::native2d
{

struct Piece
{
    // Register writes the converter made (an A+D packet's worth), when not
    // empty: the sender applies them without the packet (vif1's sendWrites).
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    std::vector<uint8_t> packet; // a packet the converter made, when not empty
    // Or a packet of the input as it is (an upload), in place: valid while
    // the input is.
    const uint8_t *view = nullptr;
    uint32_t viewSize = 0u;
    rg::NativeMesh mesh; // otherwise
    // The mesh's triangles, three indices into mesh.vertices each: a sprite
    // or a line's quad is four vertices, not six. (mesh.vertices alone is
    // not a triangle list: expand through these for one.)
    std::vector<uint32_t> indices;
    // The mesh's UVs are texels (FST), not normalised coordinates.
    bool texelUvs = false;
    bool isWrites() const { return !writes.empty(); }
    bool isPacket() const { return view != nullptr || !packet.empty(); }
    const uint8_t *bytes() const { return view ? view : packet.data(); }
    uint32_t size() const { return view ? viewSize : static_cast<uint32_t>(packet.size()); }
};

struct Span
{
    const uint8_t *data;
    uint32_t size;
};

// The packets, in order, as pieces: register-only packets and uploads, and
// meshes. False, with `why`, when they hold something the converter cannot
// express (a reserved primitive type): then they go on as they are.
bool build(const Span *packets, size_t count, std::vector<Piece> &out, std::string &why);

} // namespace rg::native2d
