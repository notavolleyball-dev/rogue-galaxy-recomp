#pragma once

class PS2Runtime;

// The developers' free camera, on a switch.
//
// The scene's camera runs, each frame, CameraModeUpdate (0x309770), which
// runs the update of the camera's mode, its override (+0x26C, a byte) when
// that is not -1. Modes 9 to 13 are the developers' free camera
// (CameraDebugFreeUpdate, 0x305C60), driven by pad 1 (the pad object at
// 0x4B2570): the left stick moves the point it looks at across the ground,
// 8 units a frame (40 with Square held; with L2 held the stick's up and down
// lift and lower it instead), the right stick turns the camera around it,
// left and right, and tilts it, up and down (with R2 held it comes nearer
// and goes farther instead; with R1 held, neither). 13 is the free one;
// the game itself sets it with CameraSetModeOverride (0x309300) to hold the
// camera still for a warp.
//
// With the switch on, exploring or fighting, the camera's override becomes
// 13 before its update, written as CameraSetModeOverride writes it but
// without the mode's setup (which only snaps the camera where it is; a call
// into the guest from a hook would come back early at the first virtual
// call, which the generated code leaves to the run loop), and -1 again when
// the switch goes off, the follow camera catching up with the party, or
// when the game makes another camera the live one (the first-person view's,
// and back), which then takes the 13. While
// it is 13 the game finds pad 1 untouched (scePadRead for port 0 answers no
// buttons and the sticks centred), so the party stands, and the free camera
// reads the controller: the runtime gives port 1 the same pad as port 0,
// and pad 2's object (0x4B27E0) is copied over pad 1's for the camera's
// update alone. Start ends it, and pad 1 stays untouched until every button
// is let go. It moves a step a frame, so in the 60 fps mode the sticks count
// the frame's share of a step past their dead zone (half at 60 frames a
// second; step fix "camera").
namespace rg::freecamera
{

// After loadELF().
void install(PS2Runtime &runtime);

void setEnabled(bool on);
bool enabled();

} // namespace rg::freecamera
