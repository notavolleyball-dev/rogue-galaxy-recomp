#pragma once

class PS2Runtime;

// The developers' tools left in the retail game. Fifteen places ask
// 0x207FE0, a function that returns 0, whether they are on (RGDecomp names
// it ReturnZero_207fe0); the menu's game option `dev_tools`, or
// RG_DEV_TOOLS=1, makes it answer 1. Then, with pad 1:
// - the main menu: R3 shows a grid of every item's icon and R1 + R3 a list
//   of them; in either, Cross gives one of the one picked
//   (MenuMainUpdate, 0x297180);
// - the help pages and the info list: R1 + Select unlocks every entry
//   (0x34F060, 0x399C10);
// - the bestiary: Select fills it (0x35C1E0);
// - the Revelation Flow: R2 + Triangle gives one of each item the picked
//   cell asks for
//   (0x34C290);
// - paused in a scene: Triangle + R1 + R2 ends it, also one that cannot be
//   skipped (PauseStepUpdate, 0x2F5FB0);
// - an enemy's preview in the bestiary: R1 + Triangle switches its camera's
//   framing (0x35FC00); the name entry: pad 2's L3 and R3 fill in a name.
// The other callers ignore the answer.
namespace rg::devtools
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);

} // namespace rg::devtools
