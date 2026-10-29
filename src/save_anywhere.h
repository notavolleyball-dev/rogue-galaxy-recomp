#pragma once

class PS2Runtime;

// The transporter's menu away from a transporter.
//
// In the field, FieldExploreModeUpdate (0x2833A0) opens the main menu for
// Triangle (PadIsTriggered(pad 1, 0x10), the call returning to 0x283840)
// when the leader is free (LeaderGetControlStateMaybe 0; for 2 and 3 it
// says why not), with GameReqQueuePushBack(17, -1), the call returning to
// 0x283880. At a transporter, Cross asks for mode 20 instead,
// TransporterMenuModeUpdate (0x27CC20): the panel with Save, Teleport,
// Exchange Points and Item Storage, which holds the field and gives it back
// when it closes. With the menu's game option `save_anywhere`
// (RG_SAVE_ANYWHERE=1), Triangle with R1 held (the field has no use for R1
// alone) asks for mode 20 where the main menu would open.
namespace rg::saveanywhere
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);

} // namespace rg::saveanywhere
