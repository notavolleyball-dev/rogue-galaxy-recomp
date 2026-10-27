# Changelog

## 0.1.0 (pre-release)

The first release: the source and an installer that builds the game on
your PC from your own disc image (Rogue Galaxy, USA, SCUS-97490). Nothing
of the game is distributed. Windows 10 or 11, 64-bit; see "Install
(Windows)" in the [README](README.md).

### What it does

- A native Windows build of the game, translated from your disc's code,
  with the GS on the GPU (internal resolution up to 8x), the VU programs
  compiled ahead of time, music, sound and the movies.
- 60 frames per second by default, the game's own 30 as an option: the
  game's systems that count frames (cutscenes, battle results, the jump,
  enemy AI, menus, texture animations, the camera) keep the console's pace.
- Widescreen of the window's shape (4:3 to 32:9), with the HUD, compass
  and movies fitted; beside a 4:3 picture, black bars, the picture blurred
  or an image of your own.
- An in-game menu (F1, or the Guide button): window and fullscreen, vsync,
  resolution, scaling, volume, controller and keyboard remapping, pause,
  fast-forward, a frame-rate counter, screenshots. The window opens as it
  was left.
- Game options, in the F1 menu and on the game's own Settings screen:
  - Camera and view: field of view, camera distance, faster camera
    rotation, draw distance, the developers' free camera.
  - Convenience: skip the opening logos, the loading recap at once,
    instant text, unopened chests on the map, faster transporters and
    Revelation Flow.
  - Battles and rewards: encounter rate, experience, money, item drops,
    shop prices, action gauge speed.
  - Developers: the tools left in the game (item lists, unlocks).
- Modern controllers: the analog sticks reshaped to the DualShock 2's
  square range, so the party runs on diagonals.
- Fixes for the game on PC: the party falling through floors (VU0 flags),
  freezes (the random number stub, return handling), the picture's edges,
  captured frames in events, the Monography Shot's transparency.

### Known issues

- Only the USA release (SCUS-97490) is supported.
- A fast CPU is needed for a steady 60: the heaviest scenes (a boss gate,
  big battles) run 50 to 60. The first time a scene is seen it can
  stutter while the GPU prepares its shaders.
- With widescreen on, menus and text are drawn for 4:3 and come out
  stretched; the title screen, the load menu and the loading screen stay
  4:3.
- In the 60 fps mode something that counts frames may still run at the
  wrong speed somewhere; the 30 fps mode (F1 > Game) is the console's.
- An antivirus may quarantine a compiler during the install: restore it,
  or allow the `toolchain` folder, and run `Install.bat` again.
- If the game freezes, press F10 before closing it: the report goes to
  `build\last-run.log`, which helps find the cause.
