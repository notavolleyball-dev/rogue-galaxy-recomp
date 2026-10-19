#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_JUMP_TRACE=1: logs the party leader's movement updates while the
// character is off the ground (a jump, a fall, a motion that carries it):
// host time, position, the step-scaled velocity, the motion's displacement,
// the action mode and the gravity flags, one line per CharaMoveCollideMaybe.
// For comparing a jump's arc and timing between the 30 and 60 fps modes.
// Hooks the function table; call after the other hooks are in.
void installJumpTrace(PS2Runtime &runtime);

} // namespace rg::debug
