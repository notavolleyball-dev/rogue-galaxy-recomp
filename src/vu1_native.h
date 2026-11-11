#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

namespace dq8::gfx::native
{
struct ModelInput;
struct ModelRow;
} // namespace dq8::gfx::native

// VU1 programs done natively: what a program would kick for a mesh chunk,
// computed from the same VU1 data memory, for docs/NATIVE-RENDERER.md's phase 1.
//
// The game's model programs are composed per draw from pieces in six code
// slots (A at pair 0x64, B 0x12C, C 0x21C, D 0x320, E 0x4C4, F 0x73A). A
// program is recognized by the FNV-1a hash of each piece's code, so no game
// code is kept here. Every piece was read from the code with
// tools/vu1/vudis.py over a recording's coverage and is checked against the
// VU1 with tools/vu1/oracle (removed in M5 of docs/NATIVE-ONLY.md; the GPU
// runs the strip programs now).
//
// Done: every program the recordings hold (docs/VU1-NATIVE.md, "Status"):
// the strip programs, composed from a piece per slot (A; B plain or skinned;
// C vertex colors, one color, lit, lit vertex colors, those with four point
// lights; D: none, guard band, guard band and backfaces, each with or
// without the scaled depth and bounds, and the beam's; F scroll, sphere map,
// wave, reflection map, ramp, the beam's), and the programs whose pieces
// draw by themselves (Kind): the outlines, the shadow volumes, the overlay
// sprites, the object setup, the particles and the projected texture. The
// pieces no recording holds (the ramp and swell Fs, the point-light Cs, the
// depth-only affine D, the float-skinning Bs, the untextured float-color D) are written from the code and checked against the interpreter
// only (docs/VU1-NATIVE.md, "Pieces written from the code alone"); their
// program names say "unverified". So are the triangle lists of the backface
// D pieces and of the projected texture, and the shadow skinning's four
// bones. Not done, so left to the VU1 chunk by chunk: clipping (slot E)
// where the GPU does not clip, 87.y & 0x100 in the vertex-color Cs, lists
// not of whole triangles, and the pieces nothing reached.
//
// The arithmetic per vertex is the runtime's (gsNativeMeshPoint in
// runtime/gs/gs_types.h), whichever way a chunk goes out.
namespace rg::vu1native
{

// How the D piece sets ADC on a strip's vertices.
enum class Cull : uint8_t
{
    None,
    GuardBand,
    GuardBandAndBackface,
};

// What the C piece gives each vertex.
enum class Color : uint8_t
{
    PerVertex, // the vertex's int16 color times the material color
    Material,  // the material color alone
    Lit,       // four directional lights on the normal, and an ambient
    LitVertex, // that, times the vertex's int16 color / 32768 * 256
    LitPointVertex, // LitPoint times the vertex's int16 color (e3bca52a; from the code only)
    LitPoint,  // Lit, and four point lights from quads 44-51 (127ca0b4; from the code only)
};

// What the F piece does to the UVs.
enum class Uv : uint8_t
{
    Plain,     // nothing, unless quad 88.xy scrolls them (declined)
    SphereMap, // replaced by the normal in view space, as (n.xy + 1) / 2
    Reflect,   // replaced by the view vector reflected in the normal, as (r.x + 1) / 2 and (r.z + 1) / 2 (ad209cd3)
    Swell,     // the positions pushed out along the normals, fading with depth (a04c11ce; from the code only)
    Beam,      // the UVs a projection of the position through quads 72-75 (b1d29245)
    Ramp,      // the UVs replaced by ((normal . quads 100-102.x) + 1) / 2, 0 (0594a063; from the code only)
    Wave,      // the UVs stay; each position turned in x and y about (0, quad 84.x) by an angle growing with y (9df36d6a)
};

// What the program draws: the A piece decides (it calls the others).
enum class Kind : uint8_t
{
    Strip,   // triangle strips and lists: A, B, C, D, F (E clips)
    Outline, // the toon outlines: A, B, then C draws the silhouette edges as lines
    Shadow,  // the shadow volumes: A, B, then C extrudes the lit triangles as fans
    Overlay, // the screen sprites over the last object's bounds: B's own entry, its MSCAL
    Base,    // the per-object setup at 0x000: the state packet, matrices and lights into VU1 memory
    Particles, // camera-facing quads, rotated and scaled: A, then D draws them
    Projector, // strips with a projected texture: A, B, F (UVs, color), C transforms and kicks
};

// How the B piece decodes a vertex's position.
enum class Decode : uint8_t
{
    Plain,   // int16 / 32768 times quad 77
    Skinned, // that, then summed over 1, 2 or 4 bones (A's entry into B, 76.z)
};

// A program: the behavior of each of its pieces. identify() composes it from
// the pieces present, so each combination is one Program, alive for good.
struct Program
{
    const char *name;
    Cull cull;
    Color color;
    // The projected z scaled by quad 87.z and capped by quad 1.y, and the
    // vertices' bounds kept in quads 98 (least) and 99 (most).
    bool depthBounds = false;
    Decode decode = Decode::Plain;
    // The most bones the B piece has an entry for (Skinned).
    uint8_t maxBones = 0;
    Kind kind = Kind::Strip;
    Uv uv = Uv::Plain;
    // With depthBounds: the D piece keeps the bounds in quads 98 and 99 (false: only the depth).
    bool bounds = true;
    // The D piece leaves ST as the UV and writes Q = 1 (2feaa9a1); the GPU mesh cannot say so.
    bool affine = false;
    // The color is (128, 128, 128) with the vertex's alpha (the beam's D, 1bd1b5c0).
    bool white = false;
    // B only skins floats the EE uploaded (positions, normals and UVs are not int16); normals too when skinNormals.
    bool rawFloats = false;
    bool skinNormals = true;
    // D reads the colors as floats from the colors section and writes no ST (312d076b).
    bool rawColor = false;
};

// Whether buildMesh can make the program's chunks a GSNativeMesh; if not,
// the GPU path sends buildChunk's packets.
inline bool hasMesh(const Program &program)
{
    return (program.kind == Kind::Strip && !program.affine && !program.rawColor) || program.kind == Kind::Projector;
}

// Whether the program draws in its MSCAL (the overlay sprites) rather than
// in the MSCNTs after it (every other: the MSCAL only ends). Its chunk is
// then the MSCAL's run.
inline bool drawsAtCall(const Program &program)
{
    return program.kind == Kind::Overlay || program.kind == Kind::Base;
}

// The program whose MSCAL enters at `startPC` (in bytes) with `code` (all of
// micro memory) in place, or null. Hashes about 6 KiB: callers cache it per
// code image.
const Program *identify(const uint8_t *code, uint32_t startPC);

// Why the chunk an MSCNT runs (its header at `top`, in quadwords) is left to
// the VU1, or null when it is done natively. `clips`: what draws it clips
// triangles itself (the GPU), so slot E's chunks are done too, for the
// programs with a mesh (hasMesh) only.
const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips = false);

// Which of the pieces this file knows the code at `startPC` has, slot by
// slot, for rg_vu1_oracle --inventory: "A F B:plain C:vertex colors D:guard
// band" when all of a program, "A F B:? C:one color D:?" when B and D are
// other pieces.
std::string describePieces(const uint8_t *code, uint32_t startPC);

// What the program leaves in VU1 data memory that a later run, or the EE,
// may read: the bounds in quads 98 and 99 for the programs with
// `depthBounds`. `apply` writes it where the VU1 did not run (skip, gpu);
// `matches` says whether the VU1 left the same (check), from the data
// memory before the run and after it. `built`, when given: the chunk as
// buildMesh made it from this same data memory, not read again.
// `bounds`, when buildMesh made them: written as they are.
struct Bounds
{
    bool made = false;
    float least[4] = {}, most[4] = {};
};
void applyMemoryEffects(const Program &program, uint8_t *data, uint32_t top, const GSNativeMesh *built = nullptr,
                        const Bounds *bounds = nullptr);

// The GS packets the program kicks for that chunk, in order.
void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets);

// The chunk as a mesh: the packets that set the GS state it draws under
// (FOGCOL, PRIM and PRMODE), to go first, and the strip itself, for
// GS::submitNativeMesh. With `bounds`, and a program that keeps them
// (applyMemoryEffects), they are made in the pass that gives each vertex its
// depth: the same projection of the same vertex, done once instead of twice.
void buildMesh(const Program &program, const uint8_t *data, uint32_t top,
               std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh, Bounds *bounds = nullptr);

// M5 of docs/NATIVE-ONLY.md: a strip program's chunks drawn by the
// renderer's model vertex stage (native_model.vert), which runs B, F and C
// on the GPU. modelOnGpu: whether it can (int16 inputs, a mesh).
// modelChunkHash: an identity of the chunk's inputs (its header and
// sections). modelChunk: its vertices as the program reads them (a strip,
// or a list). modelHeader: buildMesh's packets and header with no vertices.
// modelRow: the constants the stage reads for this draw (its layout is
// native_model.vert's), and the bones' matrices the chunk uses.
bool modelOnGpu(const Program &program);
// The particles program's chunk as a mesh in world space (M5): drawn with
// the model matrix the identity (the caller's VU1 quads 60-63 made so).
void particleMesh(const uint8_t *data, uint32_t top, std::vector<std::vector<uint8_t>> &packets,
                  GSNativeMesh &mesh);
uint64_t modelChunkHash(const Program &program, const uint8_t *data, uint32_t top);
void modelChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<dq8::gfx::native::ModelInput> &out, bool &list);
void modelHeader(const Program &program, const uint8_t *data, uint32_t top,
                 std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh);
// `knownSlots`: the palette's size (modelSlots) when the chunk's sections
// are not in memory (a chunk the list no longer carries); ~0u: read them.
void modelRow(const Program &program, const uint8_t *data, uint32_t top, dq8::gfx::native::ModelRow &row,
              std::vector<float> &palette, uint32_t knownSlots = ~0u);
uint32_t modelSlots(const Program &program, const uint8_t *data, uint32_t top);

// The chunks done natively are sent as meshes the renderer draws, without
// running the VU1. (The check mode that compared them with the VU1's
// packets, the RG_VU1_REPLAY recordings and the oracle went in M5 of
// docs/NATIVE-ONLY.md: the GPU runs the strip programs now.)
enum class Mode : uint8_t
{
    Gpu,
};
Mode modeFromEnvironment();

// What the VIF1 thread did with each strip chunk, and in check mode how the
// VU1's packets compare with the native ones (within 1/16 pixel, 16 Z units,
// one color level and 1e-5 of STQ); a line every two seconds, and the first
// few differences in full.
class Monitor
{
public:
    explicit Monitor(Mode mode) : m_mode(mode) {}

    void native(const Program &program) { ++m_native[program.name]; }
    void declined(const Program &program, const char *reason);
    // Prints the line when it is due.
    void tick();

private:
    Mode m_mode;
    std::map<std::string, uint64_t> m_native;
    std::map<std::string, uint64_t> m_declined;
    int64_t m_next = 0;
};

} // namespace rg::vu1native
