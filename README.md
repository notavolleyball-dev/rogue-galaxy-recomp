# Rogue Galaxy Recompiled

**Rogue Galaxy** (Level-5 / SCE, PlayStation 2) as a native Windows game: a
static recompilation of the USA release (SCUS-97490), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp). You build it from your own
disc image; nothing of the game is distributed.

- 60 fps, or the original 30
- Widescreen, up to 32:9
- Up to 8x internal resolution
- An in-game menu (F1) for graphics, sound and controls
- Game options: camera, field of view, draw distance, instant text,
  encounter rate, experience, money, item drops, shop prices and more
  ([all of them](docs/OPTIONS.md))

## Install (Windows)

You need Windows 10 or 11 (64-bit), about 16 GB free, an internet connection
and your own image of the disc: Rogue Galaxy, USA, SCUS-97490
([`docs/DISC.md`](docs/DISC.md)).

1. Download `RogueGalaxy-<version>.zip` from the [releases](../../releases)
   page (not "Source code").
2. Extract it to a short folder, such as `C:\RogueGalaxy`.
3. Double-click `Install.bat` and choose your disc image.
4. Wait 15 to 60 minutes: it downloads the build tools (about 300 MB) into
   the folder, extracts your disc and builds the game. Nothing is installed
   in Windows.
5. Play with `Rogue Galaxy.exe`.

If it stops, `install.log` says why; running `Install.bat` again continues
where it stopped. If an antivirus removes the compiler, restore it and run
it again. To uninstall, delete the folder (your saves are in `disc\mc0`).

## Controls

The game window takes a keyboard or a gamepad (PlayStation layout).

| Pad | Keyboard |
|---|---|
| Left stick | W A S D |
| D-pad | Arrow keys |
| ✕ / ○ / □ / △ | X or Space / C or Esc / Z / V |
| L1 / R1 / L2 / R2 | Q / E / Left Shift / Right Shift |
| Start / Select | Enter / Tab |
| Right stick | I J K L |

**F1** (or the Guide button, or Back+Start) opens the menu: display, sound,
controls, and the Game page with the options above. F11 or Alt+Enter
toggles fullscreen, F12 takes a screenshot. The options are also on the
game's own Settings screen (System > Settings, L1 and R1 to turn the page).
See [docs/OPTIONS.md](docs/OPTIONS.md) for each one.

## More

- [CHANGELOG.md](CHANGELOG.md): what each release brings, and its known issues
- [docs/OPTIONS.md](docs/OPTIONS.md): the menu and every game option
- [docs/BUILDING.md](docs/BUILDING.md): building by hand, and the project's layout
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): the debug launchers, profiling and
  the environment variables
- [docs/STATUS.md](docs/STATUS.md): how the work went, and what the game needs

## Legal

You must own the game. No game data, BIOS or disc-derived files are part of
this repository: `disc/` and disc images are gitignored and must never be
committed.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j, and the
  [fork](https://github.com/Sinan-Karakaya/PS2Recomp) by Sinan Karakaya.
- [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) by Sinan
  Karakaya, the reference for MWo3 overlays on PS2Recomp, and the source of
  the SDL3 GPU GS backend and the in-game menu in `thirdparty/dq8-gfx`.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT), which
  the menu is drawn with, and its Roboto Medium font (Apache 2.0).
- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0), the function maps
  and names for this build (`config/SCUS_974.90/functions_*.csv` derive from
  it).
- [Redump](http://redump.org/disc/6567/) and [PCSX2](https://github.com/PCSX2/pcsx2)
  (GameDB, patches) for disc and hardware notes.

## License

GPL-3.0, the same as PS2Recomp, which the built executable links. See
[`LICENSE`](LICENSE).
