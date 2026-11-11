#pragma once

#include <cstdint>
#include <vector>

#include "native_types.h"

class PS2Runtime;

// M4 of docs/NATIVE-ONLY.md: the game's 2D at its source. Every GsPkt block
// the game builds (GsPktBegin to GsPktEnd: DrawListBeginPrim's, and the
// functions that frame their blocks themselves) is made on the game thread
// from its shadow (src/native_gspkt.h), under the draw state the game
// thread follows (rg::drawstate::game(): the draw environments' packets
// and the blocks, in the game's order): its primitives become pieces there,
// each with its state and the running draw task's layer, kept under a unit
// whose marker goes in the draw list where the block's CALL would have
// been. The game writes none of the block. The list's run, reaching the
// marker, sends the pieces to the producer in the stream's order. A block
// inside a pass's call is not drawn (the pass is); a composite's head
// begins its object layer (rg::nativefx::compositeBegin).
namespace rg::nativeblocks
{

// After loadELF(), with the native effects (their markers are written
// alike).
void install(PS2Runtime &runtime);
bool enabled();

// The list's run, at a block's marker: its pieces to the producer. False:
// no pieces kept for `unit`.
bool emit(uint32_t unit);

// A block's drawn pieces as made (window pixels, the GS's XYOFFSET in), for
// what reshapes a block's 2D at its end instead of the game's vertices
// (hud_size, widescreen: M4's "transforms of the 2D layer"): the block's
// owner (DrawListBeginPrim's return address) and its meshes, each with
// whether it is textured.
struct BlockMesh
{
    rg::NativeMesh *mesh;
    bool textured;
};
// x' = sx x + tx, y' = sy y + ty on a piece's vertices (a fogged piece's
// x and y carry its w).
void transform(rg::NativeMesh &mesh, float sx, float sy, float tx, float ty);
// A vertex's x and y as drawn (a fogged piece's w divided out).
float vertexX(const rg::NativeMesh &mesh, const rg::NativeMesh::Vertex &v);
float vertexY(const rg::NativeMesh &mesh, const rg::NativeMesh::Vertex &v);


} // namespace rg::nativeblocks
