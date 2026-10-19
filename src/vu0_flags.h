#pragma once

#include "ps2_runtime.h"

// The game's bounding-box tests (AabbOverlap, AabbContains,
// AabbContainsPoint) are written in VU0 macro mode: clear the status
// register, two vsub.xyz, then read the status register's sticky sign bit
// to learn whether any component went negative. The generated code does
// not update the VU0 status or MAC flags for macro-mode arithmetic, so the
// read always sees zero and every test says yes. The collision collection
// then takes every triangle of every active map block, overflows its
// 256-entry list, and the floor under the player is missing wherever its
// block comes late: the party fell out of Salgin's chest house that way.
// These are the host versions, in place of the three until the recompiler
// models the flags (RG_VU0_FLAGS=0 keeps the generated code).
namespace rg::vu0
{
void install(PS2Runtime &runtime);
}
