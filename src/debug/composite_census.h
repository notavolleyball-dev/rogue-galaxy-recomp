#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_COMPOSITE_CENSUS=1: counts the calls of ModelDrawOffscreenComposite
// (docs/NATIVE-RENDERER.md, stage E) by what decides how it draws: its
// settings (a0: +0x20 the offscreen buffer, +0x24 the glow's strength,
// +0x28, +0x50, +0x5C, +0x60), f12 (the glow's alpha, squared into its
// radius) and f13 (the alpha the object is laid over the frame with; the
// glow is drawn only at 1). A line per kind every 10 s.
void installCompositeCensus(PS2Runtime &runtime);

} // namespace rg::debug
