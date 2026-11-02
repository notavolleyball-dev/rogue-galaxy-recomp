#pragma once

#include <memory>

class GSRasterBackend;

namespace rg::graphics
{

// Serves GS local-to-host transfers of depth buffers (PSMZ32/24/16/16S) on
// the game's thread, straight from GS local memory, instead of draining the
// GS worker queue for them.
//
// Rogue Galaxy reads an 8x8 block of Z every frame, for its lens-flare
// occlusion test (GfxEndFrame, sceGsExecStoreImage with PSMZ32). Through the
// worker that is a full drain: the game thread waits for the whole frame's GS
// work, about a third of a frame each time.
//
// The CPU backend draws Z into local memory, so these read it there. The SDL
// GPU backend draws depth on the GPU only, and local memory holds none of
// it: there the read takes the block the backend last copied out of its
// depth target, a frame or two old (dq8-gfx's gfx/gs/gs_depth_probe.h;
// DQ8_GFX_DEPTH_PROBE=0 goes back to local memory's zeros, which left the
// lens flare never occluded).
//
// RG_GS_DEPTH_READS=queue turns it off.
std::unique_ptr<GSRasterBackend> serveDepthReadsLocally(std::unique_ptr<GSRasterBackend> backend);

} // namespace rg::graphics
