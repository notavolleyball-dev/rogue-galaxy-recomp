#pragma once

class PS2Runtime;

// The field's minimap and compass made smaller.
//
// The game's 2D goes in groups, each opened by DrawListBeginPrim and closed
// by DrawListEndPrim, and its vertices through a few setters (GsPktSetXYZ*);
// the group's owner, the function that opened it, tells what it is. When the
// minimap's view or its arrow (MinimapDrawEntry's groups) closes, still in
// guest memory, its vertices move in toward the map's bottom right corner;
// the compass's dial is drawn smaller by its own screen matrix
// (ViewSetScreenMatrix from CompassHudRender), its centre in toward its
// bottom left, and its letters each about their own centre. Wide or not.
// The transporter menu's map and compass, inside its panel, stay as they are.
//
// The menu's game numbers `minimap_size` and `compass_size`, percentages of
// the game's size; RG_MINIMAP_SIZE=N and RG_COMPASS_SIZE=N.
namespace rg::hudsize
{

// After loadELF().
void install(PS2Runtime &runtime);

// 50 to 100.
void setMinimapSize(int percent);
void setCompassSize(int percent);

} // namespace rg::hudsize
