#pragma once

#include <cstdint>
#include <vector>

#include "native_types.h"

class PS2Runtime;

// M4 of docs/NATIVE-ONLY.md: the game's 2D at its source. Every GsPkt block
// the game writes (DrawListBeginPrim to DrawListEndPrim, 126 call sites) is
// read on the game thread when it ends, from the bytes it wrote, under the
// draw state the game thread follows itself (rg::drawstate::Tracker: the
// draw environments' packets and the blocks, in the game's order): its
// primitives become the 2D converter's pieces there, each with its state
// and the running draw task's layer, kept by the unit of the block's packet
// range. The VIF1 thread, reaching the range, sends the pieces to the
// producer in the stream's order (as it does a pass) and the packets on
// with their kicks withheld: the VIF1 thread's converter no longer sees
// the blocks. RG_NATIVE_BLOCKS=0: the VIF1 thread converts them, as before.
namespace rg::nativeblocks
{

// After loadELF(), with the native effects (their ranges carry the units).
void install(PS2Runtime &runtime);
bool enabled();

// The VIF1 thread (or the draw stage), at a block's range: its pieces to the
// producer. False: no pieces kept for `unit` (the converter takes the
// packets).
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

// After the block's packets went on: the VIF1 thread's mirror compared
// with what the game thread's said at the block's end (counted).
void compare(uint32_t unit);

} // namespace rg::nativeblocks
