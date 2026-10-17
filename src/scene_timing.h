#pragma once

#include "ps2_runtime.h"

// Fixed time step, and the 60 fps mode.
//
// The game measures each frame's time in vblanks (GfxEndFrame, clamped to
// between one and two vsync intervals) and, when a frame ran long, scales
// the next frame's logic up to match: up to twice the step
// (GameSceneTimeScaleUpdate, 0x001CEBE0). On the console only an overloaded
// frame triggers that. Here timer 0 follows the host clock, so frames the
// host is too slow to finish count as long ones too: in the first battle
// about half the frames took three vblanks or more. An emulator's guest
// clock slows down with the host instead, so the game never sees slow frames
// and runs in slow motion; this does the same, by leaving the scene's step
// and time scale at 1 after the game's update. It was first blamed for
// characters' turning swinging back and forth, which was the host's rounding
// (patches/ps2recomp/0003). The first play with the bigger steps back on
// lost the leader in battle, cause not yet pinned down, so it stays.
//
// RG_FPS=60: the flip waits one vblank instead of two (the vsync interval the
// game sets at boot and keeps outside its own 60 Hz menus), and the scene's
// time scale follows the measured frame time: 0.5 at full rate, 1.0 when a
// frame took two vblanks, so the logic advances at the same rate over
// however many frames the host manages. RG_TIME_SCALE=fixed makes it 0.5
// always (slow motion below 60). Movies keep two vblanks. The game's own
// overlay mode (menus at 60 Hz with the scale locked) is left alone.
//
// Only the scene: music still advances by the measured frame time, so it
// keeps its tempo. RG_TIME_SCALE=game at 30 fps leaves the game's own
// compensation on.
namespace rg::timing
{

// After loadELF(): hooks GameSceneTimeScaleUpdate.
void install(PS2Runtime &runtime);

// From the flip hook, before the flip runs: sets the vsync interval the flip
// waits for this frame (RG_FPS=60).
void beforeFlip(uint8_t *rdram, R5900Context *ctx);

} // namespace rg::timing
