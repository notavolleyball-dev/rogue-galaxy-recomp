#pragma once

namespace rg::debug
{

// RG_PROFILE=out.txt samples the instruction pointer of every thread of the
// process about once a millisecond, from RG_PROFILE_START seconds after start
// for RG_PROFILE_SECONDS (default 0 and 20), and writes "thread address count"
// lines plus the executable's load address. tools/profile/report.py turns that
// into a per-function report. Windows only; a no-op elsewhere.
// RG_PROFILE_ON_MARK=1 opens the window at each F9 instead (see
// requestProfileWindow), into numbered files.
void startSampler();

// With RG_PROFILE and RG_PROFILE_ON_MARK=1, opens a sampling window of
// RG_PROFILE_SECONDS now (F9 in the game window); otherwise does nothing.
void requestProfileWindow();

} // namespace rg::debug
