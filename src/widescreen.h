#pragma once

class PS2Runtime;

// The game drawn for a 16:9 screen.
//
// The game keeps a wide mode of its own that nothing in this release turns
// on: an aspect mode at 0x481638, set to 1 by the system config's "TVMODE
// WIDE". In it the scene camera takes 9/16 for 3/4 (SceneCameraApplyAspect)
// and the HUD drawn over the scene (gauges, target slots, minimap entries,
// the attack chain, the sprite helpers) narrows its sprites by 3/4, so they
// keep their shape once the picture is widened. Its two readers,
// GetAspectMode and IsWidescreen, answer 1 here while the option is on.
//
// The rest is nemesis2000's PCSX2 patch for SCUS-97490: the compass and the
// insect menus' model previews, which set 3/4 themselves, take 9/16 too; a movie
// made for 16:9, drawn between bars on 4:3, fills the frame from top to
// bottom; an event's letterbox stays open. Beyond the patch, a 4:3 movie
// (512x448) narrows to keep its shape, with bars at its sides.
//
// The presenter shows 16:9 when the menu's aspect ratio is Auto (the
// backend's game wide-screen query). 2D menus and text are drawn for 4:3
// and come out stretched; the screens before the game, which have nothing
// else (the title overlay's: opening, title, load menu, and the loading
// screen's recap), stay 4:3, and the game draws for 4:3 there.
//
// The menu's game option `widescreen`, or RG_WIDESCREEN=1.
namespace rg::widescreen
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool enable);

// The option is on and the game is past the screens before it: the frame
// is drawn for 16:9.
bool wide();

} // namespace rg::widescreen
