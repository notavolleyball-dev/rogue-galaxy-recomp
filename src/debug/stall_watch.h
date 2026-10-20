#pragma once

class PS2Runtime;

namespace rg::debug
{

// A thread that watches the game's flips. When none has happened for
// RG_STALL_SECONDS (15) while the host is not paused, it writes to stderr
// what every guest thread is doing (status, what it waits on, pc), whether
// VIF1 lists are in flight and which return hooks are pending, and again
// every minute while the stall lasts. The game's log then says what a
// freeze was waiting for. RG_STALL_WATCH=0 turns it off.
void startStallWatch(PS2Runtime &runtime);

// After PS2Runtime::run() returns, before the runtime goes.
void stopStallWatch();
// The same report now, from any thread: F10 in the game window. For a
// freeze the flip count does not show (a loading screen that keeps
// flipping while the main thread waits forever).
void requestStallReport();


} // namespace rg::debug
