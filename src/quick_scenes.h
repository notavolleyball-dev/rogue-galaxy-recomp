#pragma once

#include <cstdint>

class PS2Runtime;

// Scenes that only make the player wait, played faster: the whole game at up
// to three times the speed while one is on screen (the host's pace, as the
// menu's fast-forward; the sound driver keeps the audio device's, so music
// and sounds do not change).
//
// What the game is doing is its mode (u32 at 0x4B51A0, the index into the
// mode table GameModeDispatch runs) and the mode's phase (s32 at 0x4B51E0):
// - a chest opening: mode 13 (TreasureBoxModeUpdate runs the chest's script:
//   the lid, the items flying out, the message);
// - a transporter's warp: mode 11 (TransporterWarpModeUpdate, which runs the
//   area script a warp request names, s32 at 0x4821E0) on the transporter's
//   scripts, 0x320 leaving and 0x321 arriving; the other warps' scripts
//   (doors, lifts, story scenes) keep their pace;
// - items going into a Revelation Flow cell: the board's step of placing
//   them (+0x76E of the board RevelationBoardUpdate, 0x34C290, takes each
//   frame), the zoom, the items flying in, the glow and the ability won,
//   but not while it waits for the player.
// RG_SCENE_TRACE=1 prints each change of mode or phase, and of the main
// menu's state (the id of the entry its machine at 0x48235C runs).
namespace rg::quickscenes
{

enum class Scene
{
    Chests,
    Transporter,
    Revelation,
};

// After loadELF().
void install(PS2Runtime &runtime);
void setEnabled(Scene scene, bool on);

// The speed for what the game shows now (1 when nothing is to be hurried),
// from guest RAM; the main thread's.
double speed(const uint8_t *rdram);

} // namespace rg::quickscenes
