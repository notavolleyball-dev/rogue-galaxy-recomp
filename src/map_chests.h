#pragma once

class PS2Runtime;

// The chests not yet opened, on the map before the planet's map is complete.
//
// MinimapDrawEntry (0x325720) draws the corner map, the full map and the
// transporter menu's map. Its pass over the loaded zones' chest records
// (an 8x8 icon from "minimap_icon" at 0x34,0; a chest already opened, one
// whose conditions hide it, one on a moving node or in a cell not explored
// yet is left out) runs only when SavePointAreaCompleteTest (0x3EC7A0,
// called from 0x3267A8 alone) says the planet is complete: its flag (460 to
// 466 by planet) is set once every save point there has been activated,
// which also reveals the whole map. With the menu's game option
// `map_chests`, or RG_MAP_CHESTS=1, that test answers yes for the map's
// call, and nothing is written to the game: the save is unchanged and the
// planet's own completion still comes.
namespace rg::mapchests
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);

} // namespace rg::mapchests
