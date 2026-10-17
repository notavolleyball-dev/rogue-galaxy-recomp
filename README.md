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
folds a recording into the manifest).

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

F11 toggles fullscreen. The game runs at 30 frames per second; on a 144 Hz
monitor each frame lasts 4 or 5 refreshes, which reads as judder, so 120 Hz
or variable refresh looks smoother.

## Running

On Windows, double-click `RogueGalaxy.bat` (or run it from a terminal): it
starts the built game on `disc/SCUS_974.90` and writes the game's log to
`build/last-run.log`. The build copies the FFmpeg and clang runtime DLLs
next to `roguegalaxy.exe`, so it needs nothing on PATH. To set any of the
variables below, add a `set` line to the batch file (it has examples) or set
them in the terminal before running it.

| Variable | Effect |
|---|---|
| `RG_GS=sw` | Software GS rasterizer instead of the SDL3 GPU backend |
| `RG_GS_SCALE=N` | GPU internal resolution multiplier (default 2) |
| `RG_GS_NO_WINDOW=1` | Present through the runtime's window instead of SDL's |
| `RG_GS_WORKER=0` | Run the GS backend on the game's thread instead of its own worker thread |
| `RG_SHOW_FPS=1` | Frames per second in the window title |
| `RG_PRESENT_MODE=vsync\|immediate` | Present mode (default mailbox) |
| `RG_PRESENT=vblank` | Present at host vblanks instead of on the game's flips |
| `RG_VIF1_THREAD=0` | Run VIF1 lists (VU1) on the game's thread instead of their own |
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
| `RG_GS_TIMING=1` | Time every GS backend call on both sides of the worker queue, every 2 s |
| `RG_GS_CENSUS=1` | Every 2 s, the draw states the GPU backend only approximates (destination alpha test and the like), and draws whose scissor is wider than their buffer; `RG_GS_SKIP=MASK:VALUE` drops draws by TEST register |
| `RG_GS_FRAMELOG=file@N` | Every batch, transfer and clear of one frame, once the game's pad read count reaches N, with GS memory as `file.vram` (`@N#K` also before batch K); `RG_GS_FRAMESKIP=A-B,...` leaves batches of that frame out (`A-` to the end) |
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
| `thirdparty/PS2Recomp` | Framework submodule; `patches/ps2recomp/` holds our changes to it, applied on configure ([`patches/README.md`](patches/README.md)) |
| `thirdparty/dq8-gfx` | SDL3 GPU GS backend from DQ8-Recompiled (GPL-3.0), see its README |
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
  the SDL3 GPU GS backend in `thirdparty/dq8-gfx`.
- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0), the function maps
  and names for this build (`config/SCUS_974.90/functions_*.csv` derive from
  it).
- [Redump](http://redump.org/disc/6567/) and [PCSX2](https://github.com/PCSX2/pcsx2)
  (GameDB, patches) for disc and hardware notes.

## License

GPL-3.0, the same as PS2Recomp, which the built executable links. See
[`LICENSE`](LICENSE).
