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
// work, about a third of a frame each time. The SDL GPU backend never reads
// depth back from the GPU, so it answers these from the very same local
// memory this reads, which is shared with it: the bytes come out identical.
//
// RG_GS_DEPTH_READS=queue turns it off.
std::unique_ptr<GSRasterBackend> serveDepthReadsLocally(std::unique_ptr<GSRasterBackend> backend);

} // namespace rg::graphics
