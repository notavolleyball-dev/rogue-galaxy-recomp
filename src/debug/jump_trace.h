#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_JUMP_TRACE=1: logs the party leader's movement updates while the
// character is off the ground (a jump, a fall, a motion that carries it):
// host time, position, the step-scaled velocity, the motion's displacement,
// the action mode and the gravity flags, one line per CharaMoveCollideMaybe.
// For comparing a jump's arc and timing between the 30 and 60 fps modes.
// RG_JUMP_TRACE=3 logs every character's, each line with the character's
// address and id, and for enemies ([enemy] lines) every move that changes
// the position, what drives it (velocity, the per-frame moves, the move
// request) and how far something else moved it since the last one, and
// every move request started, with its caller (a boss's leap across its
// arena).
// Hooks the function table; call after the other hooks are in.
void installJumpTrace(PS2Runtime &runtime);

} // namespace rg::debug
