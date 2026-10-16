# Status

**Phase 1 (function maps) is done.** All six code images recompile to C++
cleanly; nothing is built into an executable yet.

| Phase | Goal | State |
|---|---|---|
| 0 | Repo, framework pin, disc verify/extract, overlay table | done |
| 1 | Function maps for the ELF and the 5 real overlays | done |
| 2 | Build ELF + overlays into one executable; overlay-aware dispatch | next |
| 3 | Boot to the title screen | |
| 4 | Field, battle, movies, sound | |
| 5 | Performance, widescreen, 60 fps, launcher | |

## Phase 1 in short

Details in [MAPS.md](MAPS.md).

- One map per image (`config/SCUS_974.90/functions_<image>.csv`), from
  RGDecomp's symbol files with 62 constructors and 17 other functions added,
  13 removed and one split in two, each justified in
  [`funcmap.toml`](../config/SCUS_974.90/funcmap.toml). The
  big one: every overlay's static constructors live in its .data, where no
  map had them, so the first overlay load would have failed.
- One ps2_recomp config per image (`config/SCUS_974.90/recomp/`). The main
  one binds 208 SDK functions to runtime handlers, seeded from
  DQ8-Recompiled's list for the same framework, and routes 186 hardware
  register accesses through the runtime.
- `python setup.py check` proves coverage, bounds, cross-image control flow,
  constructors, names and bindings against the retail bytes; it passes.
- Smoke test: the pinned `ps2_recomp` recompiles all 20,067 functions with no
  decode failure and no unhandled instruction. 186 functions (183 in the main
  ELF) have computed jumps it could not resolve and fall back to making every
  instruction an entry point.
- `$gp` is settled: 0x00485170, from `.reginfo` and the entry code.

## Framework

`thirdparty/PS2Recomp` is [Sinan-Karakaya/PS2Recomp](https://github.com/Sinan-Karakaya/PS2Recomp)
at `379b9ca` (2026-10-11), the fork that Dragon Quest VIII Recompiled builds
on (DQ8 pins `888ec47`, the parent commit). Against upstream
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) `eb8688b` it is 139
commits ahead and 7 behind. What we need from it:

- dispatch of overlaid code (`1d4c4dc`) through a `FunctionRegion` resolver:
  upstream has one dense function table per ELF and no way to hold several
  overlays at the same addresses;
- the game's own IRX modules on an emulated IOP with an SPU2 path (`f16758d`,
  `2d03b8c`): upstream has no SPU2, so the game would be silent;
- whole VU1 microprograms compiled ahead of time (`97d1214`, `7d4779e`);
- PSS movie fixes (`96c2ecf`, `e3f7eb2`, `dc4bbd8`), a raster backend that
  can own the window (`438d05e`).

Upstream commits the fork lacks and we will want: `eb8688b` PS2 float
semantics for EE FPU results and DIV.S/RSQRT.S (#272), see the float patch
below; `a5d3049` resume points for standalone `entry_*` functions (#271);
`2c5fbb9` syscalls 0x79/0x7A (#274). Upstream's own IOP emulator (`75d729c`,
#244) diverges from the fork's and is not needed.

## What the game needs (known so far)

**Overlays.** Main ELF code is 0x00100000-0x00436958 plus static constructors
at 0x00469300-0x0046F9BC (layout in [MAPS.md](MAPS.md)). Five real overlays
(TITLE, INSMENU, NORMAL, FACTORY, INSECT) share the arena
0x004E9E80-0x00528200, each with its own map and generated code. Dispatch
must follow whichever one is resident: the main ELF calls into the arena
directly at 263 sites, and every overlay calls back into the main ELF. The
hook point is the `jr ra` stub at 0x00100400, called with a0 = header after
every load and before the constructors run. Details in
[`config/SCUS_974.90/overlays.toml`](../config/SCUS_974.90/overlays.toml).
This is the same Metrowerks scheme as DQ8 (its stub is at 0x00100320).

**IOP.** Per RGDecomp, the ELF reboots the IOP with `MODULES/IOPRP300.IMG`,
then loads 15 IRX modules from `cdrom0:\MODULES\` with `sceSifLoadModule`,
retrying until each succeeds. Sound goes through SDRDRV (`sceSdRemote*`) and the SCE "sk"
sound kit (SKSOUND/SKHSYNTH/SKMIDI/SKMSIN, MODHSYN/MODMIDI/MODMSIN).

**CD/DVD.** Data archives are opened per layer with `sceCdLayerSearchFile`
(0x001297E0) and `sceCdReadDvdDualInfo` (0x0012ACD0). Neither upstream's
runtime nor the fork has a handler for them; they need one (in a host-folder
layout both can resolve by name, see [DISC.md](DISC.md)). Streaming uses
CDVDSTM.

**Movies.** Sony libmpeg + IPU (`PsIIlibipu 3000`), PSS demuxed by the game
(`MpegDemuxPss` in RGDecomp). MVI files carry an XOR layer; DQ8 has a
converter (`tools/mvi/mvi_decrypt.py`) for the same container.

**FPU.** PCSX2 patches `0x00124898` to `3442fffe` (clamp to 0x7FFFFFFE
instead of 0x7FFFFFFF) for the out-of-bounds glitch in the Vedan/Myna area
([pcsx2#3544](https://github.com/PCSX2/pcsx2/issues/3544)). The runtime must
reproduce PS2 float saturation or apply the same patch.

**GS.** The PCSX2 GameDB carries only hardware-renderer fixes for this game,
which name the features it leans on: accurate blending (text/menu colour),
auto flush (lamp glow), round sprite (minimap/menu), preloaded frame data
(water), partial target invalidation (UI, subtitles), half-pixel offset
(bloom), native scaling (post-processing / DOF). No EE/VU game fixes.

**Timing.** Game logic runs at 30 fps; the PAL 60 fps pnach has to patch
gravity and timers too.

## Phase 2 plan

1. Build the six generated trees and the fork's runtime into one executable
   (CMake), with the generated function names kept apart per image.
2. Overlay manager: hook 0x00100400, read the MWo3 id from the header at a0,
   switch the arena's `FunctionRegion` to that overlay's table; nothing
   resident means arena calls fail loudly.
3. Keep each overlay's bootstrap entry (0x004E9EC0) out of the boot path.
4. Feed real jump tables (`[jump_tables]`) for the 186 functions that fall
   back, starting with the hottest.

## Open questions

- Whether the game ever reads raw sectors computed from search results
  (the extracted tree keeps the original sector numbers in `disc/manifest.json`
  in case it does).
- Where the MVI XOR key comes from on this disc.
- The two `sceCdGetDiskType` entries (0x12A9D8, called by the game; 0x12AA70,
  the wrapper ps2_analyzer names) are both bound; phase 3 confirms which is
  the public one.

## References

- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0): matching
  decompilation of this exact build. `config/*.symbols.txt` gives names,
  addresses and sizes for the main ELF and each overlay, from a Ghidra export
  of 14,620 functions: the seed for the function maps.
- [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) (GPL-3.0):
  same overlay format, compiler and SDK generation. Overlay manager,
  `mwo3_to_elf.py`, HD6 and MVI tools, SDL GPU backend, launcher; its SDK
  binding list seeded ours.
- [DC2-PS2RECOMP](https://github.com/Red-tv141/DC2-PS2RECOMP): Dark Cloud 2
  (Level-5, same compiler family) brought to a playable state on PS2Recomp.
- PCSX2 [GameIndex.yaml](https://github.com/PCSX2/pcsx2/blob/master/bin/resources/GameIndex.yaml)
  and [pcsx2_patches](https://github.com/PCSX2/pcsx2_patches)
  (`SCUS-97490_0643F90C.pnach`).
