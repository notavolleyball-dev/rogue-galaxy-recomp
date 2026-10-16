#pragma once

class PS2Runtime;

// Keeps the game's scene logic at one step per frame.
//
// Rogue Galaxy times each frame (GfxEndFrame, with EE timer 0) and, when one
// ran long, scales the next frame's logic up to match: up to twice the step
// (GameSceneTimeScaleUpdate, 0x001CEBE0). On the console only an overloaded
// frame triggers that. Here timer 0 follows the host clock, so frames the
// host is too slow to finish count as long ones: in the first battle about
// half the frames took three vblanks, the logic ran 1.5 to 2 times per
// frame, and characters' turning overshot back and forth. An emulator's
// guest clock slows down with the host instead, so the game never sees
// slow frames and runs in slow motion; this does the same, by leaving the
// scene's step and time scale at 1 after the game's update.
//
// Only the scene: music still advances by the measured frame time, so it
// keeps its tempo. RG_TIME_SCALE=game leaves the game's own compensation on.
namespace rg::timing
{

// After loadELF().
void install(PS2Runtime &runtime);

} // namespace rg::timing
