// The host's round analog sticks, reshaped onto the DualShock 2's square
// range before the game reads them.
#pragma once

#include <cstdint>

namespace rg::pad
{

// Takes the packed stick bytes the SDL backend sampled (rx, ry, lx, ly from
// the top byte down, 128 at rest) and returns them with each stick's travel
// measured the console's way. RG_STICK_SQUARE=0 passes them through.
uint32_t shapeSticks(uint32_t sticks);

} // namespace rg::pad
