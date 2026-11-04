#pragma once

#include <memory>

class GSRasterBackend;

namespace rg::graphics
{

// Step 6's structural check (docs/NATIVE-PIPELINE.md): under the native
// pipeline the GS backend is not there for the game. Every call into it is
// a crossing the counters missed; the first of each kind is printed with
// where it came from and a backtrace.
//
// RG_NATIVE_GS selects what a crossing does:
//   drop  (the default) draws, transfers, flushes and syncs go nowhere;
//         reads and presentation still reach the backend, logged
//   abort the first crossing of any kind stops the run with its trace
//   pass  everything reaches the backend as before, logged
//
// With the pipeline off it is a plain pass-through.
//
// `bare` (step 7's structural check, the pipeline on): the backend is only
// the window and the native renderer's host, with no GS of its own: its GS
// local memory, transfer engine and target and texture caches are never
// made (Initialize is not passed on). RG_NATIVE_GS_INIT=1 makes them as
// before.
std::unique_ptr<GSRasterBackend> tripOnGs(std::unique_ptr<GSRasterBackend> backend, bool bare = false);

} // namespace rg::graphics
