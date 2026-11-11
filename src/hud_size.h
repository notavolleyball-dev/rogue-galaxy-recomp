#pragma once

#include <cstdint>
#include <vector>

#include "native_blocks.h"

class PS2Runtime;

// The field's minimap and compass made smaller.
//
// The game's 2D goes in blocks, each opened by DrawListBeginPrim and closed
// by DrawListEndPrim; the block's owner, the function that opened it, tells
// what it is. Each block's 2D records are made at its end
// (src/native_blocks.h), and reshape() scales them there (M4: the 2D's
// records, not the game's vertices): the minimap's view and its arrow
// (MinimapDrawEntry's blocks) toward the map's bottom right corner;
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

// A block's 2D, as made at its end (game thread): scaled when it is the
// minimap's or the compass's letters.
void reshape(uint32_t owner, std::vector<rg::nativeblocks::BlockMesh> &meshes);

// 50 to 100.
void setMinimapSize(int percent);
void setCompassSize(int percent);

} // namespace rg::hudsize
