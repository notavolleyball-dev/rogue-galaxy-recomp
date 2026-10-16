#pragma once

#include <memory>

#include "runtime/gs/gs_backend.h"

namespace rg::debug
{

// RG_GS_TIMING=1 wraps GS backends in a decorator that times every call and
// prints, every two seconds, the calls, total and longest time per method:
// where the GS side of a frame goes, and on which side of the worker queue.
// Returns the backend unchanged when the variable is unset.
std::unique_ptr<GSRasterBackend> timeGsBackend(std::unique_ptr<GSRasterBackend> backend, const char *label);

// With RG_GS_TIMING, called once per presenter loop: reports how long the
// presenter thread went between loops.
void notePresenterLoop();

} // namespace rg::debug
