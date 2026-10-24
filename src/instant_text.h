#pragma once

class PS2Runtime;

// Text in the message windows at once, a page at a time, instead of letter by
// letter. A window types its text out in MesWinAdvanceTypeout (0x275F00),
// which reads the window's instant flag (+0xB4) through MesWinGetInstant
// (0x277600) and nowhere else; scripts set it with a text speed of 0
// (MesWinSetTextSpeed, 0x277580). With the menu's game option
// `instant_text`, or RG_INSTANT_TEXT=1, the flag reads 1: the game's own
// instant mode, which still stops at the pauses and waits written into the
// text and at each page.
namespace rg::instanttext
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);

} // namespace rg::instanttext
