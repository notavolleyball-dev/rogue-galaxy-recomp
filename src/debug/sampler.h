#pragma once

namespace rg::debug
{

// RG_PROFILE=out.txt samples the instruction pointer of every thread of the
// process about once a millisecond, from RG_PROFILE_START seconds after start
// for RG_PROFILE_SECONDS (default 0 and 20), and writes "thread address count"
// lines plus the executable's load address. tools/profile/report.py turns that
// into a per-function report. Windows only; a no-op elsewhere.
// RG_PROFILE_ON_MARK=1 opens the window at each F9 instead (see
// requestProfileWindow), into numbered files. With RG_PROFILE_BEFORE=S too,
// sampling runs all the time and each window also holds the S seconds before
// its F9; RG_PROFILE_AUTO_MS=N then opens one by itself at a hitch (a frame
// N ms long, or the frame rate halved for a few frames; noteFrameTime).
void startSampler();

// At exit: ends a window in progress early and waits for it to be written.
void stopSampler();

// With RG_PROFILE and RG_PROFILE_ON_MARK=1, opens a sampling window of
// RG_PROFILE_SECONDS now (F9 in the game window); otherwise does nothing.
void requestProfileWindow();

// Each presented frame's time since the previous one, from the thread that
// presents: with RG_PROFILE_AUTO_MS, a hitch opens a window.
void noteFrameTime(double seconds);

} // namespace rg::debug
