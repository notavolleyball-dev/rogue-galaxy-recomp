#pragma once

#include "ps2_runtime.h"

// The systems that count frames instead of taking the scene's step, made to
// take it, for the 60 fps mode (RG_FPS=60; off with RG_STEP_FIXES=0):
//
// - NPCs walk a fixed distance and turn a fixed angle per update
//   (NpcObjUpdate, no step anywhere): at half steps they moved and turned
//   twice as fast. The walk factor they all share is scaled by the step for
//   the duration of their update, and the turn rates at its two calls.
// - Projectiles, thrown objects and attack shapes are stepped with a
//   constant 1.0 (TaskStepJudgeObject) though their integrator takes a step:
//   they flew at twice the speed, under half the gravity. They get the step.
// - Gravity is added once per frame to a velocity that was scaled by the
//   step (CharaMoveCollideMaybe): at half steps it accelerated everything
//   twice as fast, so jumps, players' and enemies' alike, reached half their
//   height. The gravity added there is scaled by the step squared, the
//   gravity the launch computations and the stepped integrators ask for is
//   a whole frame's, and the terminal velocity is clamped to the step's
//   share of it.
//
// Addresses, and why each is what it is: docs/sixty-fps-logic.md.
namespace rg::steps
{

// After rg::timing::install().
void install(PS2Runtime &runtime);

// With RG_TIME_TRACE: prints and resets the hooks' counts (the timing
// trace calls it every 300 updates).
void trace();

} // namespace rg::steps
