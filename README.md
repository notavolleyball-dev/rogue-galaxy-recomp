# Rogue Galaxy Recompiled

Static recompilation of **Rogue Galaxy** (Level-5 / SCE, PlayStation 2) to a
native PC executable, built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).

| | |
|---|---|
| Release | NTSC-U, **SCUS-97490** v1.01 (one DVD9) |
| Framework | [Sinan-Karakaya/PS2Recomp](https://github.com/Sinan-Karakaya/PS2Recomp) fork, pinned in `thirdparty/PS2Recomp` |
| State | **Phase 0 done**: disc tooling and overlay table. Not playable, nothing recompiled yet. See [`docs/STATUS.md`](docs/STATUS.md). |

## Setup

Requirements: Git, Python 3.10+, CMake 3.20+ with Ninja and a C++20 compiler
(clang or MSVC), and your own dump of the game
([`docs/DISC.md`](docs/DISC.md) lists the one that works).

```bash
git clone --recurse-submodules <this repo>
cd rogue-galaxy-recomp

# Read-only check of the dump (add --full to compare the whole image with Redump)
python setup.py verify --iso "/path/to/Rogue Galaxy (USA).iso"

# Unpack both DVD layers into disc/ (about 6.3 GiB), hashing every file
python setup.py extract --iso "/path/to/Rogue Galaxy (USA).iso"

# SYSTEM.CNF and the MWo3 overlay table, cross-checked against the ELF
python setup.py info
```

Build the recompiler tools from the pinned framework:

```bash
cmake -S thirdparty/PS2Recomp -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build build/tools --target ps2_recomp ps2_analyzer
```

## Layout

| Path | What |
|---|---|
| `setup.py` | `verify`, `extract`, `info` (and `make-hashes` for maintainers) |
| `config/SCUS_974.90/` | `hashes.json` (known-good dump), `overlays.toml` (MWo3 table, loader hook) |
| `tools/disc/iso9660.py` | Read-only ISO9660 reader for dual-layer PS2 DVDs |
| `tools/mwo3/mwo3.py` | MWo3 overlay header parser and ELF cross-check |
| `thirdparty/PS2Recomp` | Framework submodule |
| `docs/` | [`STATUS.md`](docs/STATUS.md) (state, plan, what the game needs), [`DISC.md`](docs/DISC.md) |
| `disc/` | Your extracted disc (gitignored) |

## Legal

You must own the game. No game data, BIOS or disc-derived files are part of
this repository: `disc/` and disc images are gitignored and must never be
committed.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j, and the
  [fork](https://github.com/Sinan-Karakaya/PS2Recomp) by Sinan Karakaya.
- [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled), the
  reference for MWo3 overlays on PS2Recomp.
- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0), the function map
  and names for this build.
- [Redump](http://redump.org/disc/6567/) and [PCSX2](https://github.com/PCSX2/pcsx2)
  (GameDB, patches) for disc and hardware notes.

## License

GPL-3.0, the same as PS2Recomp, which the built executable links. See
[`LICENSE`](LICENSE).
