#pragma once

class PS2Runtime;

// The opening movie at boot (the Sony and Level-5 logos, then the galaxy),
// which the title screen plays before its menu.
//
// The title overlay's TitleOpeningMovieTick (0x4EBAF0) lets a button
// (Triangle, Cross, Square, Circle or Start) fade it out once it has run six
// frames, but only after it has been seen once (0x481B90, set when it ends):
// at boot it always plays whole. The skip (the menu's game option
// `skip_opening`, or RG_SKIP_OPENING=1) marks it seen and answers the tick's
// first button check with a press, so the game's own fade takes it to the
// title screen.
namespace rg::opening
{

// After loadELF().
void install(PS2Runtime &runtime);

void setSkip(bool on);

} // namespace rg::opening
