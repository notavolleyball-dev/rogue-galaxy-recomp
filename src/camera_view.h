#pragma once

class PS2Runtime;

// The field and battle camera's field of view and distance.
//
// The scene's projection is set again every frame by ViewSetProjection
// (0x143000, f12 the vertical field of view in radians), from ViewSetAspect,
// ViewSetDepthRange and ViewReapplyCameraRec. The field camera's is fixed at
// 52 degrees across a 4:3 picture (CameraObjInit, 0x309AB0:
// FovHorizToVert(52), 40.2 degrees high), in the field, in battle and in the
// transporter's menu alike. With the menu's game number `field_of_view` at
// another value, that projection takes it instead while exploring (game
// mode 6) or fighting (7); an event's camera, which sets its own angle
// (CameraTrackUpdate), keeps it. A wide picture widens it further, as before.
//
// The camera keeps between a nearest and a farthest distance from the leader,
// pushed back or pulled in past them (CameraObjOrbitControl): 65 to 120 in
// the field, written by CameraSetDefaultParams (0x309260) into the camera's
// block at +0x15C, and in battle 150 to 190 up or 120 to 160 down, the two
// settings the camera button switches between, by CameraSetPresetParams
// (0x309100) into the same block. `camera_distance`, a percentage, scales
// both as they are written, and when it moves the cameras (the game keeps
// two, 0x4B5950 and 0x4B6220) take it at the next projection, each whose
// block still holds what was last written there.
namespace rg::cameraview
{

// After loadELF().
void install(PS2Runtime &runtime);

// Degrees across a 4:3 picture; 52 is the game's.
void setFieldOfView(int degrees);
// Percent of the game's distances.
void setDistance(int percent);

} // namespace rg::cameraview
