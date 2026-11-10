#pragma once

struct SDL_Window;

namespace rg::debug
{

// A bug mark: one key when something looks wrong, one folder with what is
// needed to find out why, in place of a screenshot, a touched file and a
// description. F8 in the game window with the mouse over the fault (or
// `touch build/run/mark-now`, or RG_BUG_MARK_AT=PADREAD[,PADREAD...] in a
// scripted run) writes build/run/marks/<date>-<time>/:
//   info.txt      the commit, the flip and pad read, the window's size, the
//                 mouse in window pixels, every RG_ and DQ8_ variable
//   settings.ini  a copy of RG_SETTINGS_FILE
//   frame_*.rdc   RenderDoc's capture of the next three flips, when the game
//                 was started under it (ENABLE_VULKAN_RENDERDOC_CAPTURE=1;
//                 on Wayland its layer has to be one built with Wayland
//                 support, or the GPU device is refused: Arch's is not):
//                 every pass and draw, replayable; tools/bugmark reads it
//   depthlog-*.txt, log.txt
//                 under the native pipeline, the depth log and the 2D piece
//                 dump of that frame (as depth-log-now and dump-now ask for
//                 them); log.txt is the part of RG_LOG_FILE around the mark
// "[bugmark] done DIR" on stderr about two seconds later says it is whole.
void requestBugMark();

// Once per pump, on the thread that owns the window.
void pollBugMarks(SDL_Window *window);

} // namespace rg::debug
