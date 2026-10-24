#pragma once

class PS2Runtime;

// How far away the scenery and the objects are still drawn.
//
// Each map's configuration gives its nodes, models and the map as a whole a
// far clip: a far distance past which they are hidden, and a near one inside
// which they are, 0 or -1 for none. FarClipRecParseArgs (0x18C850) writes
// them into a record at +0xC and +0x10, for MapCfgFarClip and
// MapCfgMapFarClip; MapModelCfgFarClip (0x195DC0) into the current model's
// configuration (0x481328) at +0x14 and +0x18. NodeDistanceFadeUpdate,
// ModelObjDistanceFadeUpdate and ModelObjIsVisible compare the camera's
// distance with them. The map's fog starts and ends at +0x1A4 and +0x1A8 of
// its scene configuration (0x481304, MapSceneCfgFog, 0x192BB0). Salgin's
// streets have far clips of 4000, near ones of 38, 300 and 360 with no far
// one to pair with (things that hide as the camera comes close), and fog
// from 5 to 10000 or 4500.
//
// The menu's game number `draw_distance`, a percentage, scales the far
// distances and the fog as they are read; the near ones stay. The fog thins
// in step, so what comes into view has the haze the game gave the same share
// of the distance. From the next map loaded.
namespace rg::drawdistance
{

// After loadELF().
void install(PS2Runtime &runtime);

void setPercent(int percent);

} // namespace rg::drawdistance
