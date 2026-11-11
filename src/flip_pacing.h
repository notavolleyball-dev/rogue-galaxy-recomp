#pragma once

#include <cstdint>

class PS2Runtime;

// The game's flip (GfxFlipBuffers, 0x00143650): each one closes a frame of
// the native pipeline with a Present record of the scene target, which the
// presenting thread shows (graphics.cpp), one frame per flip, in order. It
// also counts the flips, the frames the game finished drawing.
//
// (Until M1 of docs/NATIVE-ONLY.md this also paced the GS path's
// presentations on the flips.)
namespace rg::graphics
{

// After loadELF(): hooks the game's flip function.
void installFlipHook(PS2Runtime &runtime);

// Flips the game has done so far, the frames it finished drawing: the menu's
// frame-rate counter. From any thread.
uint64_t guestFlips();

} // namespace rg::graphics
