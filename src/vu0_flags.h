#pragma once

#include "ps2_runtime.h"

// The game's bounding-box tests (AabbOverlap, AabbContains,
// AabbContainsPoint) are written in VU0 macro mode: clear the status
// register, two vsub.xyz, then read the status register's sticky sign bit
// to learn whether any component went negative. The generated code did not
// update the VU0 status or MAC flags for macro-mode arithmetic, so the read
// always saw zero and every test said yes. The collision collection then
// took every triangle of every active map block, overflowed its 256-entry
// list, and the floor under the player went missing wherever its block came
// late: the party fell out of Salgin's chest house that way. The
// recompiler now updates the flags in the functions that read them
// (patches/ps2recomp/0017); these host versions stay in place of the three,
// as the shorter path. RG_VU0_FLAGS=0 keeps the generated code, and
// RG_VU0_FLAGS=check runs it and counts where it disagrees with these.
namespace rg::vu0
{
void install(PS2Runtime &runtime);
}
