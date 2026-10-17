#pragma once

#include <memory>

#include "runtime/gs/gs_backend.h"

namespace rg::debug
{

// RG_GS_CENSUS=1 prints, every two seconds, the draw states the game used
// that the GPU backend cannot reproduce without framebuffer fetch: the
// destination alpha test (TEST.DATE), alpha-test fail modes other than KEEP,
// blending by destination alpha and partial FBMSK channels. Each line is one
// distinct state with its batch count and screen bounds. It also prints,
// once each, draws whose scissor is wider than their buffer.
//
// RG_GS_SKIP=MASK:VALUE (hex, on the TEST register) drops every batch whose
// TEST & MASK == VALUE, to see on screen which draws a state accounts for,
// e.g. RG_GS_SKIP=4000:4000 drops the draws that test destination alpha.
//
// RG_GS_FRAMELOG=file@N writes every batch (state and vertices), transfer
// and clear of one frame to file: the first whole frame after the game's
// pad read count reaches N (see RG_INPUT_CLOCK=pad in input_script.h).
// GS memory goes beside it as file.vram at the end of that frame, and with
// file@N#K also as file.K.vram just before batch K (textures it samples).
// Returns the backend unchanged when none is set.
std::unique_ptr<GSRasterBackend> censusGsBackend(std::unique_ptr<GSRasterBackend> backend);

} // namespace rg::debug
