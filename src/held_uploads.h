#pragma once

#include <memory>

class GSRasterBackend;

namespace rg::graphics
{

// Named texture uploads held before the GS worker's queue (native renderer
// with DQ8_GFX_NATIVE_BYPASS; docs/NATIVE-SUBMIT-PLAN.md, phase 1).
//
// The game sends most of its textures again every frame it loads them
// (TexGroupAllocVram_157e70 builds each group's uploads from the image nodes
// in RAM whenever it loads the group; RGDecomp), about 4 MB a frame. Each
// upload the host named (gfx/gs/gs_named_textures.h) is held here, on the
// thread feeding the GS, and hashed. Its bytes then go to the backend as one
// shared buffer beside the transfer, not through the queue, and the backend
// skips the upload whole when it still holds an image of that content there.
//
// RG_HELD_UPLOADS=0 turns it off; =measure only counts what could be skipped.
std::unique_ptr<GSRasterBackend> holdNamedUploads(std::unique_ptr<GSRasterBackend> backend);

} // namespace rg::graphics
