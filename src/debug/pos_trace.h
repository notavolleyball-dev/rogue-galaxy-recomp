#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_POS_TRACE=1: logs every call that puts a character somewhere
// (CharaSetPos, CharaSetPosXYZ, CharaPlaceAt) with the character, its id,
// the old and new positions and the caller's address, and reports a
// character that keeps falling for longer than any jump lasts, with the
// position it fell from. For characters that fall out of the scenery: the
// log then tells whether something teleported them or the floor let them
// through. Hooks the function table; call after the other hooks are in.
void installPosTrace(PS2Runtime &runtime);

} // namespace rg::debug
