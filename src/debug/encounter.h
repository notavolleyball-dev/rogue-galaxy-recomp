#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_FORCE_ENCOUNTER=1: a battle at the first steps in a field with
// enemies, for the battle bench. CharaSysEncounterDistTick (0x3A5890) takes
// the distance the leader walked off the encounter's countdown (a double at
// +0x20) and queues the battle when it reaches 0, in field exploration
// with an encounter table only; the countdown is cut to almost nothing
// before each tick, so the game's own checks still decide.
void installForcedEncounter(PS2Runtime &runtime);

} // namespace rg::debug
