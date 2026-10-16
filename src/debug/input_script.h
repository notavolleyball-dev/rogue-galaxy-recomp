#pragma once

namespace rg::debug
{

// RG_INPUT="SECONDS:BUTTON[+BUTTON...]:MS[,...]" holds those pad buttons for
// MS milliseconds, SECONDS after start, in place of the real pad: a scripted
// player for unattended test runs, e.g. RG_INPUT="30:START:150,33:CROSS:150".
// Buttons: SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRIANGLE CIRCLE
// CROSS SQUARE, and the left stick pushed fully: LUP LDOWN LLEFT LRIGHT.
void startInputScript();

} // namespace rg::debug
