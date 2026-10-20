# Rogue Galaxy Recompiled

Static recompilation of **Rogue Galaxy** (Level-5 / SCE, PlayStation 2) to a
native PC executable, built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).

| | |
|---|---|
| Release | NTSC-U, **SCUS-97490** v1.01 (one DVD9) |
| Framework | [Sinan-Karakaya/PS2Recomp](https://github.com/Sinan-Karakaya/PS2Recomp) fork, pinned in `thirdparty/PS2Recomp` |
| State | **Runs**: title screen, opening, field and the first battles at the game's 30 fps, dipping to about 15 to 22 in the first battle, where it then runs in slow motion rather than skipping (GS on the GPU, VU1 compiled ahead of time and on its own thread), with music and sound. See [`docs/STATUS.md`](docs/STATUS.md). |

## Setup

Requirements: Git, Python 3.11+, CMake 3.24+ with Ninja, clang with C++20,
and your own dump of the game ([`docs/DISC.md`](docs/DISC.md) lists the one
that works).

```bash
git clone --recurse-submodules <this repo>
cd rogue-galaxy-recomp

# Read-only check of the dump (add --full to compare the whole image with Redump)
python setup.py verify --iso "/path/to/Rogue Galaxy (USA).iso"

# Unpack both DVD layers into disc/ (about 6.3 GiB), hashing every file
python setup.py extract --iso "/path/to/Rogue Galaxy (USA).iso"

# SYSTEM.CNF and the MWo3 overlay table, cross-checked against the ELF
python setup.py info

# Check the function maps and recompiler configs against your disc
python setup.py check
```

## Build

From the repository root. The generated code is built with clang (the flags
are GNU-style; on Windows an LLVM MinGW clang works). Configuring the game
downloads raylib, SDL3 and glslang (for the GPU backend's shaders) and, on
Windows, a prebuilt LGPL FFmpeg whose hash the framework pins.

```bash
# 1. The recompiler, from the pinned framework (once)
cmake -S thirdparty/PS2Recomp -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build build/tools --target ps2_recomp ps2_analyzer

# 2. Recompile your disc's code to C++ in build/generated/<image>/ (about a
#    minute): runs `check`, wraps the overlays, runs ps2_recomp on all six images
python setup.py recompile

# 3. Build the game against the framework runtime
cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/game

# 4. Run it on your extracted disc
build/game/src/roguegalaxy disc/SCUS_974.90
```

Run `setup.py recompile` again after changing a map or config, then rebuild.

**VU1 programs.** 3D scenes need the game's VU1 microprograms compiled ahead
of time to run at full speed. `setup.py recompile` rebuilds them from
[`config/SCUS_974.90/vu1-programs.txt`](config/SCUS_974.90/vu1-programs.txt)
(where each one sits in the game's files, plus a checksum: no game data) and
your disc into `build/vu1-manifest`, which CMake compiles into the runtime.
Programs the manifest lacks run on the interpreter; record them with
`PS2_VU_PROGRAM_PROFILE=build/vu1-profile` (CMake compiles that folder too),
then configure and build again (maintainers: `python setup.py vu1-manifest`
folds a recording into the manifest). The VU0 micro programs, which run on
the game thread, are compiled as blocks instead: a run with
`PS2_VU_AOT_PROFILE=build/vu-blocks/NAME.txt` records the blocks the
interpreter executes (both units; keep the lines starting with `0` for
VU0 alone), and CMake compiles every `build/vu-blocks/*.txt` it finds
(one shard at a time: each takes gigabytes to compile).

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

**F1** (or the Guide button, or Back+Start) opens the in-game menu, from
DQ8-Recompiled: window and fullscreen, vertical sync, internal resolution,
aspect and scaling filter, volume, controller and keyboard remapping with
dead zones, pause (F2), fast-forward (F4), a frame-rate counter (F3),
screenshots (F12) and renderer statistics. F11 or Alt+Enter toggles
fullscreen. The settings live in `%APPDATA%\RogueGalaxy\Recompiled\settings.ini`
(`RG_SETTINGS_FILE` names another file).

The game runs at 30 frames per second; on a 144 Hz monitor each frame lasts
4 or 5 refreshes, which reads as judder, so 120 Hz or variable refresh looks
smoother.

## Running

On Windows, double-click `RogueGalaxy.bat` (or run it from a terminal): it
starts the built game on `disc/SCUS_974.90` and writes the game's log to
`build/last-run.log`. The build copies the FFmpeg and clang runtime DLLs
next to `roguegalaxy.exe`, so it needs nothing on PATH. To set any of the
variables below, add a `set` line to the batch file (it has examples) or set
them in the terminal before running it. The batch file also sets
`VK_LOADER_LAYERS_DISABLE=~implicit~`, which keeps other programs' Vulkan
layers (OBS's capture hook, ReShade, the Steam, GOG and Epic overlays) out
of the game's process; set it yourself when running from a terminal.
`RogueGalaxy-perfil.bat` runs the same game with the profiler on
(`RG_PROFILE`, fifteen minutes from one minute after boot, into
`build/run/play-perfil.prof`) for a session of real play; report it with
`tools/profile/report.py` against `build/run/exe-perfil.exe`, the copy of
the build it was recorded with. It also prints a `[gfx]` line a second
into `build/last-run.log` (`DQ8_GFX_STATS_EVERY=60`), and F9 writes a
`[mark]` line with the seconds since launch (any launcher), so pressing
it when a drop is felt puts the drop next to the `[gfx]` lines of that
moment. F10 writes the stall watchdog's report at once (the guest
threads and what each waits on, the semaphores' counts, the host
stacks), for a freeze the watchdog does not see by itself because the
screen keeps flipping; the watchdog also reports on its own when the
main thread has not moved for 15 s, and the runtime logs a `[ee]` line
when a guest thread ends with pc 0 or the main thread exits. A frozen
process can be dumped with `tools\hang\dumpmem.ps1` (its guest RAM and
thread contexts) and a thread's call chain read from the dump with
`python tools/hang/stackwalk.py DUMPDIR SP RA`.

| Variable | Effect |
|---|---|
| `RG_GS=sw` | Software GS rasterizer instead of the SDL3 GPU backend |
| `RG_GS_SCALE=N` | GPU internal resolution multiplier for this run (the menu's saved setting otherwise, default 2) |
| `RG_GS_NO_WINDOW=1` | Present through the runtime's window instead of SDL's |
| `RG_GS_WORKER=0` | Run the GS backend on the game's thread instead of its own worker thread |
| `RG_SHOW_FPS=1` | The menu's frame-rate counter on from the start (in the window title when the runtime presents) |
| `RG_PRESENT_MODE=vsync\|immediate` | Present mode for this run (the menu's setting otherwise, default mailbox) |
| `RG_SETTINGS_FILE=path` | The menu's settings file (default `settings.ini` in `%APPDATA%\RogueGalaxy\Recompiled`) |
| `RG_UI_SCRIPT=file\|text` | Drive the menu for tests and screenshots: `<guest frame> <command> [args]` entries, `;`-separated (see `thirdparty/dq8-gfx/src/ui/ui_script.cpp`) |
| `RG_PRESENT=vblank` | Present at host vblanks instead of on the game's flips |
| `RG_VIF1_THREAD=0` | Run VIF1 lists (VU1) on the game's thread instead of their own |
| `RG_FPS=60` | Experimental: the game flips every vblank and the scene's step follows the measured frame time (half at 60, whole at 30), so it plays at its normal speed at whatever rate the host reaches (movies keep 30); `RG_TIME_SCALE=fixed` keeps half steps regardless, `RG_TIME_TRACE=1` logs the frame times and the play clock, `RG_STEP_FIXES=0` leaves the systems that count frames (play clock, minimap, NPCs, projectiles, gravity and the jump's flight, movie fades) as the game has them, or a comma list of `clock`, `movie`, `npc`, `judge`, `gravity`, `event`, `menu` picks which take the step (`event`: the scripts, the event player and the message windows count frames, so they run on the frames that complete a whole step, and at once when a button is pressed; the field modes that hold the scale for a scripted sequence, chests, talks and warps, take the half steps too; `menu`: the field's menus and HUD windows, the battle results among them, count frames as well and run on the same frames) |
| `RG_TIME_SCALE=game` | Let the game take bigger logic steps after frames the host was too slow for, as on the console (off by default: slow frames play in slow motion, as in an emulator) |
| `RG_EE_CVT=round` | Diagnostic: float-to-int conversions round, as the runtime used to, instead of truncating as the EE does |
| `RG_MOVIE_AUDIO=game` | The game's own movie PCM callback, which drops packets the runtime's demux delivers too early (noise) |
| `PS2X_EE_ROUNDING=nearest` | Diagnostic: guest float math rounds to nearest, the host default, instead of toward zero as the EE does (patch 0003) |
| `RG_GS_DEPTH_READS=queue` | Serve the game's depth readbacks through the GS worker queue |
| `RG_MUTE=1` | No sound output (the IOP still runs) |
| `RG_INPUT="30:CROSS:150,..."` | Scripted pad input: seconds, buttons (`+`-joined; `LUP` etc. for the left stick), milliseconds; with `RG_INPUT_CLOCK=pad` both numbers count the game's pad reads, so a script replays alike at any host speed |
| `RG_TRACE=0xADDR,...` | Log calls of main-ELF functions with guest thread, sp, ra and a0-a3 (`RG_TRACE_PEEK` words, `RG_TRACE_ARG0` offsets from a0, `RG_TRACE_LIMIT` calls each, default 16) |
| `RG_DUMP=0xFUNC:file` | Append each call of a `FUNC(dst, src, size)` to file: dst, size and the bytes at src |
| `RG_WATCH=0xADDR,...` | Print guest words, I/O registers included, every `RG_WATCH_MS` (default 1000) |
| `RG_HWWATCH=0xADDR` | x86 debug register on a guest word: writes leaving a value in `RG_HWWATCH_RANGE=lo-hi` (default the main stack) print the writer's stack (`RG_HWWATCH_READS=1`, `RG_HWWATCH_READER=0xFUNC`, `RG_HWWATCH_DELAY=s`); Windows only |
| `RG_PROFILE=out.txt` | Sample all threads for `RG_PROFILE_SECONDS` (default 20) from `RG_PROFILE_START` seconds in (default 0); report with `tools/profile/report.py` (`RG_PROFILE_STACKS=1` also records the callers of samples inside the executable, which the report skips) |
| `RG_STALL_SECONDS=15` | When no frame has been flipped for that long while the window is active, the log gets every guest thread's state (what it waits on; the running one's registers and the addresses it passes over the next second) and every host thread's stack (`exe!0x...` frames resolve with `llvm-symbolizer --obj=build/game/src/roguegalaxy.exe`, from the toolchain's `bin`), again every minute; `RG_STALL_WATCH=0` turns the watchdog off |
| `RG_VU0_FLAGS=0` | Keep the generated code for the game's three VU0 macro-mode bounding-box tests (`AabbOverlap`, `AabbContains`, `AabbContainsPoint`), which always answer yes because the recompiler does not model the VU0 status flags; by default `src/vu0_flags.cpp` replaces them with host versions, which is what keeps the collision list from overflowing and the floor under the player |
| `RG_POS_TRACE=1` | Log every call that puts a character somewhere it did not walk to (`CharaSetPos`, `CharaSetPosXYZ`, `CharaPlaceAt`: character, id, old and new position, caller), and any character still falling after 90 movement updates, with where it fell from |
| `RG_MC0_DIR=path` | Memory card folder instead of `disc/mc0` (`mc1` is its sibling). The benchmarks point it at `build/run/mc0-bench`, a copy holding only the save they load, since a second save changes what the scripted load menu picks |
| `RG_JUMP_TRACE=1` | Log the party leader's movement updates while off the ground (host time, position, step-scaled velocity, the motion's displacement, the action mode and the gravity flags, one line per `CharaMoveCollideMaybe`; `=2` logs the moves on the ground too, for the speed ramps): compares a jump's arc and timing between the 30 and 60 fps modes |
| `RG_STICK_SQUARE=0` | Hand the game the host's round analog stick range as it is. By default `src/stick_response.cpp` reshapes both sticks onto the DualShock 2's square range (the direction and the pushed distance stay, the distance counts as the longer axis, so a full diagonal reads full on both), which the game's run threshold was tuned for: with a round stick the party only runs within about 20 degrees of each of the four directions and walks in between |
| `RG_GS_TIMING=1` | Time every GS backend call on both sides of the worker queue, every 2 s |
| `RG_GS_CENSUS=1` | Every 2 s, the draw states the GPU backend only approximates (destination alpha test and the like), and draws whose scissor is wider than their buffer; `RG_GS_SKIP=MASK:VALUE` drops draws by TEST register |
| `RG_GS_FRAMELOG=file@N` | Every batch, transfer and clear of one frame, once the game's pad read count reaches N, with GS memory as `file.vram` (`@N#K` also before batch K); `RG_GS_FRAMESKIP=A-B,...` leaves batches of that frame out (`A-` to the end); `RG_GS_TRACE_BEFORE=M` turns the backend's target trace (`DQ8_GFX_TRACE_TARGETS`, a line per transfer and ownership move) on from M pad reads before that frame to its end |
| `RG_SELFTEST=gs` | Check the code changed in vendored dependencies, then exit |
| `DQ8_GFX_SCREENSHOT_DIR`, `DQ8_GFX_SCREENSHOT_EVERY` | Framework: write every Nth frame as PPM |
| `PS2X_AUDIO_DUMP=file` | Framework: copy of the audio output, raw 48 kHz s16 stereo |

## Layout

| Path | What |
|---|---|
| `setup.py` | `verify`, `extract`, `info`, `overlays`, `check`, `recompile` (and `make-hashes`, `maps` for maintainers) |
| `CMakeLists.txt`, `src/` | The game executable: build of the generated trees, overlay manager (`src/overlay/`), call tracing (`src/debug/`), `main.cpp` |
| `config/SCUS_974.90/` | `hashes.json` (known-good dump), `overlays.toml` (MWo3 table, loader hook), `funcmap.toml` (map sources and corrections), `functions_<image>.csv` (function maps), `recomp/<image>.toml` (ps2_recomp configs, SDK bindings), `vu1-programs.txt` (VU1 microcode locations) |
| `tools/disc/iso9660.py` | Read-only ISO9660 reader for dual-layer PS2 DVDs |
| `tools/mwo3/mwo3.py` | MWo3 overlay header parser, ELF cross-check, overlay-to-ELF wrapper |
| `tools/funcmap/` | Function-map builder and the checks behind `setup.py check` |
| `tools/bench/` | `walk.sh NAME [SCALE] [FPS]`: the scripted Salgin walk on the pad clock, for speed (3x, 60 fps mode, the `[gfx]` intervals before the logged frame) and for regressions (1x, 30 fps mode, `cmpvram.py` against a reference frame made with the same save); `PROFILE=1` samples the threads as well |
| `thirdparty/PS2Recomp` | Framework submodule; `patches/ps2recomp/` holds our changes to it, applied on configure ([`patches/README.md`](patches/README.md)) |
| `thirdparty/dq8-gfx` | SDL3 GPU GS backend and in-game menu from DQ8-Recompiled (GPL-3.0), see its README |
| `thirdparty/imgui` | Dear ImGui submodule (MIT), which the menu draws with |
| `docs/` | [`STATUS.md`](docs/STATUS.md) (state, plan, what the game needs), [`DISC.md`](docs/DISC.md), [`MAPS.md`](docs/MAPS.md) (function maps, SDK bindings) |
| `disc/` | Your extracted disc (gitignored) |

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
