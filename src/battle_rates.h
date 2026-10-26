#pragma once

class PS2Runtime;

// How often random battles come, and what they pay.
//
// Encounters come by distance: each frame TaskAddEncountDist (0x1F6D90)
// hands the distance the party leader walked to CharaSysEncounterDistTick
// (0x3A5890, f12), which takes it off a countdown (a double at +0x20 of the
// party's system) and queues a battle at 0; the next countdown is drawn from
// the area's encounter record. The menu's game number `encounter_rate`, a
// percentage, scales that distance: 0 for no random battles, 200 for twice
// as many.
//
// BattleCalcResults (0x3A2100) totals the defeated enemies' experience into
// its results block (+0x248 of its argument, +0x0), capped at 9999 (0x465C90),
// then adds each enemy's money, drawn between its minimum and maximum, with
// PartyAddMoney (0x3C8E90, the call returning to 0x3A2944), and last gives
// every member the total (CharaAddExp, from 0x3A2D38), halved in reserve, the
// cap applied again. `battle_experience` scales the total at the first of
// those money calls, before anyone gets it, so the results screen shows what
// was given, and `battle_money` each enemy's money. Shops, chests and the
// story's rewards stay as they are.
//
// A character's action gauge refills in CharaActionGaugeRegen (0x3B6490, a
// virtual UnitBaseUpdate calls each frame with the time step, f12): a rate
// for what the character is doing times the step. `action_gauge` scales the
// step for the members of the party fighting (in the battle bench, the only
// ones it ran for).
namespace rg::battlerates
{

// After loadELF().
void install(PS2Runtime &runtime);

// Percentages of the game's.
void setEncounterRate(int percent);
void setExperience(int percent);
void setMoney(int percent);
void setActionGauge(int percent);

} // namespace rg::battlerates
