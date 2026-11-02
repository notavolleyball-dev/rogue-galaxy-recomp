#pragma once

#include <cstdint>

// Render targets the game names itself (Rogue Galaxy: added).
//
// The game sets most of its offscreen render targets from a texture
// descriptor (DrawEnvSetFrameFromDesc_144fe0 in RGDecomp: the descriptor's
// TEX0 at +0x38 and its height at +4 become the draw environment's FRAME).
// The host hooks that function and declares each target here, so the backend
// knows from the game which buffers are offscreen work rather than inferring
// it from a range of GS addresses.
//
// Bases are in GS blocks (FBP * 32), the unit GsSurface::base uses.
namespace dq8::gfx::namedtargets {

struct Target {
    uint32_t base = 0;
    uint32_t bufferWidth = 0; // in 64-pixel units
    uint32_t width = 0;       // pixels
    uint32_t height = 0;      // pixels
    uint32_t psm = 0;
};

// Any thread. Re-declaring the same target is cheap.
void declare(const Target &target);

// True once the host declares targets: the backend's rules switch from their
// fixed GS address ranges to what was declared.
bool active();

// A declared target at this base and format that is smaller than the screen
// (the frame, the display buffer and the full-screen work buffers are
// excluded: they keep what earlier passes left in them).
bool offscreen(uint32_t base, uint32_t psm);

// The declared target at this base, if any.
bool find(uint32_t base, Target &out);

void reset();

} // namespace dq8::gfx::namedtargets
