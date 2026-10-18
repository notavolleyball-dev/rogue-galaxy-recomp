#pragma once

#include "ps2_runtime.h"

// The game's rand (0x133710) is bound to the runtime's libc stub
// (config/SCUS_974.90/recomp/main.toml), which returns std::rand() & 0x7FFF:
// 15 bits. The game's own is newlib's: a 64-bit linear congruential
// generator in its reentrancy struct whose high word's low 31 bits come
// back, 0..2^31-1, and every caller divides by 2^31 (RandFloat 0x13C010,
// RandRangeFloat 0x1CA5D0, the AI scripts' RandInt 0x2CBF80). With the
// stub every random number in the game was 0. The AI scripts' main loop
// takes a 10% branch on RandInt(100) < 10 that goes back to the top of the
// loop without yielding the frame, counting on the next pass to yield: taken
// every time, a party member's AI spun there forever and the game froze
// (docs/STATUS.md, "The freezes").
//
// This puts newlib's generator back, on the seed in the game's struct, so
// Srand (0x1336F8, recompiled) still seeds it.
namespace rg::libc
{

// After loadELF().
void install(PS2Runtime &runtime);

} // namespace rg::libc
