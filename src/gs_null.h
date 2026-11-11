#pragma once

#include <memory>

class GSRasterBackend;

namespace rg::graphics
{

// The runtime's GS backend: none (docs/NATIVE-ONLY.md, M1). The game is drawn
// by the native pipeline from records; nothing is meant to reach the GS
// backend, so every call into this one is a crossing the pipeline missed.
// The first of each kind is printed with a backtrace, and the counts at exit.
// Draws, transfers, flushes and syncs go nowhere; reads answer zero, and a
// presentation shows nothing.
//
// RG_NATIVE_GS=abort stops the run at the first crossing, with its trace.
std::unique_ptr<GSRasterBackend> nullGsBackend();

} // namespace rg::graphics
