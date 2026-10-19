# Status

**The game runs.** `roguegalaxy` boots to the title screen, starts a new
game, plays the opening movie and the real-time cutscenes in Salgin, and
fights the first battles (HUD, damage, allies, results screen) at the game's
own 30 frames per second, every frame presented once, with the GS on the
GPU, VU1 compiled ahead of time and on a thread of its own, music and sound
effects, keyboard or gamepad. The first battle, the heaviest scene so far,
dips to 15 to 22 and then runs in slow motion.
Known gaps: a rare hang after the Story loading screen (below), and the
remaining phase 5 work (widescreen, 60 fps logic, self-contained
packaging).

| Phase | Goal | State |
|---|---|---|
| 0 | Repo, framework pin, disc verify/extract, overlay table | done |
| 1 | Function maps for the ELF and the 5 real overlays | done |
| 2 | Build ELF + overlays into one executable; overlay-aware dispatch | done |
| 3 | Boot to the title screen | done |
| 4 | Field, battle, movies, sound | done |
| 5 | Performance, widescreen, 60 fps, launcher | 30 fps nearly everywhere, launcher, in-game menu; rest next |

## Phase 1 in short

Details in [MAPS.md](MAPS.md).

- One map per image (`config/SCUS_974.90/functions_<image>.csv`), from
  RGDecomp's symbol files with 62 overlay constructors, 4 main constructors
  and 17 other functions added, 13 removed and two functions split, each
  justified in [`funcmap.toml`](../config/SCUS_974.90/funcmap.toml). The
  big one: every overlay's static constructors live in its .data, where no
  map had them, so the first overlay load would have failed. (The main ELF's
  constructor fixes came from the first boot, in phase 2.)
- One ps2_recomp config per image (`config/SCUS_974.90/recomp/`). The main
  one binds 208 SDK functions (205 since phase 4) to runtime handlers, seeded from
  DQ8-Recompiled's list for the same framework. (It also carried
  ps2_analyzer's MMIO table until phase 3 showed that table was wrong.)
- `python setup.py check` proves coverage, bounds, cross-image control flow,
  constructors, names and bindings against the retail bytes; it passes.
- Smoke test: the pinned `ps2_recomp` recompiles all 20,072 functions with no
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

## Phase 2: one executable

- `python setup.py recompile` runs `check`, wraps the overlays as ELFs and
  runs ps2_recomp on all six images into clean `build/generated/<image>/`
  trees, refusing any skipped function, decode failure or unhandled
  instruction. For each overlay it also writes `overlay_info.h`, the MWo3
  header that tree was generated from.
- The top-level `CMakeLists.txt` builds the framework runtime only, and
  `src/CMakeLists.txt` one static library per generated tree (unity batches
  of 64, a shared PCH). The main tree's `register_functions.cpp` defines the
  runtime's own function table; each overlay's is compiled through
  `src/overlay/overlay_table.cpp.in`, which renames those globals and exposes
  the table as a `FunctionRegion`.
- `src/overlay/overlay_manager.cpp` replaces the `jr ra` stub at 0x00100400.
  On every load it reads the header at a0, refuses it unless it matches the
  header its tree was generated from field by field, and points the
  runtime's region resolver for the arena at that overlay's table (VIEWER and
  LAUNCHER, which hold no code, leave the arena without one). Arena calls
  with nothing resident fail loudly through the runtime's missing-function
  report.
- `src/main.cpp` loads the ELF from the extracted disc, sets the guest memory
  layout (below), installs the manager and runs.
- Build: clang 22 (LLVM MinGW) with Ninja; the generated code compiles in
  about 4 minutes on 16 threads into a 113 MB executable. The runtime's
  Release LTO is switched off: with clang on MinGW it drops the TLS
  initializer of an `inline thread_local` with a destructor and the link
  fails.

**Memory layout.** The game sizes its main memory stack (mgCMemory at
0x4A6FA0, one 24.7 MB memalign) to nearly all the heap, which on hardware
runs up to the main thread's stack at 0x01FD0000. The runtime caps the guest
heap at 0x01F00000, so that memalign failed, the game went on with a null
base, and its first clear wiped RAM from address 0 (the overlay path table
went with it). `main.cpp` now gives the game the heap up to 0x01F80000, keeps
the runtime's callback stacks between that and the main stack, and moves the
runtime's own small heap to 0x00080000..0x00100000 (EE kernel space, unused
here). If more than 20 callback stacks are ever needed they would grow into
the game heap; the executable reports it on exit.

**First boot.** CRT and all 233 constructors run; the IOP is rebooted and
the game's IRX modules load (CDVDSTM, the sk sound kit, PCMPLAY, ...); the CD
code finds all seven overlays; TITLE.BIN loads and the manager makes it
resident; the title code opens `cddat:menu/rc/menu_static_res_EN.pac` from
the DATA archives (via the HD6 index) and reads it with raw sector reads
(`sceCdRead` at the file's sector plus an offset, which the runtime's
pseudo-LBN mapping serves). Then nothing more is read and every frame is
black, at about 58 frames per second, with no error logged.

**Debugging aids.** `RG_TRACE=0xADDR,...` logs calls of main-ELF functions
(arguments, strings they point at, return value; `RG_TRACE_LIMIT`,
`RG_TRACE_PEEK=0xADDR,...` to print guest words at each call).
`DQ8_GFX_SCREENSHOT_DIR` + `DQ8_GFX_SCREENSHOT_EVERY=N` (framework) write
every Nth frame as PPM.

## Phase 3: to the title screen and into the game

The black screen after TITLE.BIN loaded was the main thread asleep in
`DelayThread`, waiting for a libkernl timer alarm that never fired. Three
fixes got the game from there to the title screen, NEW GAME and the opening:

- **No `[mmio]` table.** ps2_analyzer's MMIO table recorded only the page
  base, 0x10000000, for 137 instruction sites, and an `[mmio]` entry
  overrides the address ps2_recomp resolves itself. Every listed timer access
  went to Timer 0's count: `StartTimerSystemTime` read Timer 2's mode from
  there, got 0, and wrote back a mode with no clock source, so Timer 2 never
  reached its compare value. ps2_recomp needs no table (it routes statically
  resolved hardware addresses through the runtime, and READ/WRITE check at
  run time), so `recomp/main.toml` now has none.
- **Callbacks the main ELF points into overlays.** Three `return 1` menu
  callbacks in TITLE.BIN sit right after the `jr ra` of the function
  RGDecomp merged them into, and only the main ELF's widget tables point at
  them. Split out in `funcmap.toml`.
- **Vtable getters.** `rg_entry_002d94b0` was eight 8-byte getters a vtable
  points at one by one; `funcmap.toml` now supports `split = [...]`.
- `setup.py check` now also flags any code address taken as data (in any
  image's data, or built with lui/addiu) that lands after a `jr ra` and its
  padding inside a mapped function, the shape all of these had, across
  images (overlays are only matched against themselves and the main ELF).

**Result.** Memory card check, Japan Studio and Level-5 logos, title screen
(NEW GAME / CONTINUE / OPTIONS), NEW GAME, the "Story" text with its loading
bar, NORMAL.BIN loaded and made resident, then the opening in the desert of
Rosa and the town of Salgin, with subtitles. No error in the log.

**Test aids.** `RG_INPUT="SECONDS:BUTTON[+BUTTON]:MS,..."` presses pad
buttons on a schedule (unattended runs); `RG_WATCH=0xADDR,...` prints guest
words, I/O registers included, every `RG_WATCH_MS`. `setup.py recompile`
now rewrites only the generated files that changed, so a rebuild after a
map or config change takes seconds instead of minutes.

Still open from the first boot: `sceCdReadDvdDualInfo` (0x12ACD0) runs as
guest code and its S-command RPC (server 0x80000593, rpc 0x27) is unhandled,
so the game takes the disc for single-layer. That works with the extracted
tree, which holds both layers' files in one folder.

## Phase 4: sound and speed

**Sound.** The disc's sound drivers (LIBSD, SDRDRV, the Sound Kit and
PCMPLAY, the same set DQ8-Recompiled runs) now load onto the framework's
emulated IOP with an SPU2 (`ps2_native_iop::setModules` in `main.cpp`), so the
game's libsdr and sound RPCs run their real code. Music and sound effects
play (`PS2X_AUDIO_DUMP` confirms a clean music signal on the title screen).
Movie audio did not, until phase 5 (**Movie audio**, below): during the
opening movie the output was noise (a sign change on nearly every sample).
`RG_MUTE=1` silences the output.

**Speed.** In the first 3D scene (Salgin, the soldiers' cutscene) the game
ran at 2.7 frames per second. Profiling (`RG_PROFILE`, below) showed why, in
three steps:

1. The software GS rasterizer ran on the game thread: now it runs on a
   worker thread behind the framework's ordered queue (`GSThreadedBackend`).
2. That left the worker saturated by full-screen effect passes, which a
   per-pixel CPU rasterizer cannot keep up with. The GS now renders on the
   GPU with the SDL3 GPU backend from DQ8-Recompiled (vendored in
   `thirdparty/dq8-gfx/`, GPL-3.0), Vulkan on Windows, 2x internal
   resolution, in its own window: 2.7 -> 20 frames per second.
3. That left the game thread 85% inside the VU1 interpreter. VU1
   microprograms are now compiled to C++ ahead of time from a recording of
   the ones a run executes (`PS2_VU_PROGRAM_PROFILE=build/vu1-profile`,
   picked up by CMake): 20 -> about 60 presents per second. (Presents, not
   game frames: the game draws at most 30, and phase 5 below found that in
   heavy scenes it drew far fewer.)

The recorded microcode is game code, so it is not committed: a manifest,
`config/SCUS_974.90/vu1-programs.txt`, names where each program sits in the
game's files (48 footprints in the ELF and overlays, with a checksum and no
data), and `setup.py recompile` rebuilds them from the user's disc into
`build/vu1-manifest`. Programs it lacks run on the interpreter until a local
recording (`build/vu1-profile`) adds them.

**Field and battle.** With scripted input (`RG_INPUT`) the opening plays
through to Jaster's first fights in Salgin: the battle tutorial (START pages
through it), real-time combat against the Baphu with Steve and the Hooded
Man, the results screen (EXP, skills, items), then the story carries on into
the next fight and its tutorial. No error in the log, about 57 frames per
second in battle.

Bindings: `fflush`, `fseek` and `ftell` are no longer bound to the runtime.
Its versions only know handles from its own `fopen`, and this game opens its
files with its own newlib, so every call failed ("Invalid file handle").

`RG_GS=sw` returns to the software rasterizer, `RG_GS_SCALE=N` sets the GPU
internal resolution, `RG_GS_WORKER=0` puts the rasterizer back on the game
thread.

**Profiling.** `RG_PROFILE=out.txt` (with `RG_PROFILE_START`,
`RG_PROFILE_SECONDS`) samples every thread's instruction pointer about every
millisecond; `tools/profile/report.py out.txt build/game/src/roguegalaxy.exe`
prints where each thread spends its time, by function.

## Phase 5: a steady 30

Played by hand, the real-time cutscenes looked choppy: characters visibly
skipped animation frames. The game runs its logic at 30 frames per second
(GfxFlipBuffers waits two vblanks), but a recording of the window showed the
picture changing only 15 to 20 times a second in 3D scenes, and the time
went in four places.

**EE float conversion.** The runtime's `FPU_CVT_W_S` rounded to nearest;
CVT.W.S on the R5900 truncates and saturates. 563 recompiled functions use
it, among them the animation clock (`AnimKeyFindSpan`, the cvt.w.s at
0x0015600C, the defect RGDecomp's notes name as the character-animation
jerk). `src/ee_fpu.h` redefines it for the generated code, through its
precompiled header.

**Presenting.** The runtime's presenter samples the display at each host
vblank and waits for the GS worker to reach that sample; with the worker
busy those samples fell unevenly against the game's flips, so frames showed
twice or never. `src/flip_pacing.cpp` hooks GfxFlipBuffers and prepares the
presentation on the game's thread, in the GS stream right behind the flip's
copy; the window shows exactly those frames, one each (`RG_PRESENT=vblank`
for the old way). Mailbox replaces VSync as the present mode.

**The lens-flare probe.** GfxEndFrame reads an 8x8 block of Z every frame
(sceGsExecStoreImage, PSMZ32) for flare occlusion, and through the worker
that drained the whole queue: a third of a frame waiting. The SDL GPU
backend never reads depth back from the GPU, so it answered from local
memory anyway; `src/depth_reads.cpp` answers depth reads from that same
memory without the drain (`RG_GS_DEPTH_READS=queue` for the old way).

**Throughput.** With every frame presented, heavy scenes still ran at 15
to 25 frames per second: the GS worker and the game thread were both
saturated. On the GS side (changes marked "Rogue Galaxy" in
`thirdparty/dq8-gfx`): 4- and 8-bit texture uploads write runs instead of a
dispatched call per pixel, 4-bit textures expand through the resolved
palette (the general path re-read each texel's CLUT entry), and resolves
reuse one download buffer and staging texture instead of creating them each
time. `RG_SELFTEST=gs` checks the new writers and reader against GSMem.
On the game thread two thirds of the time was the VIF1 list (VU1 and its GS
packets), which the runtime runs inside sceDmaSend. The game starts each
frame's list at the end of GfxEndFrame and waits for it at the start of the
next, as the console runs VU1 beside the EE; `src/vif1_thread.cpp` now does
the same, on a thread of its own (`RG_VIF1_THREAD=0` for the old way). The
list's closing VIFcode interrupt still reaches the game's handler, which
times it, so the frame-time compensation sees real GS time. GS packets go
straight to the GS rather than through the framework's unlocked
GifArbiter, and a framework patch (`patches/ps2recomp/`) lets DMA started
through the registers wait for the lists in flight.

Over the scripted run to the first battle, frames per second for each ten
seconds went from dips of 12 to 24 in the cutscenes before the first battle
to 25 at worst, 28 to 30 elsewhere. Tools: `RG_GS_TIMING=1` times every GS
backend call on both sides of the worker queue; `RG_PROFILE_STACKS=1` adds
call stacks to `RG_PROFILE`, so samples in drivers or waits name their
caller.

At 144 Hz a 30 fps game shows each frame for 4 or 5 refreshes, which reads
as judder; 120 Hz (4 each) or variable refresh avoids it.

**Frame-time compensation.** Played by hand, the first battle then went
wrong: characters' facing flipped from frame to frame. GfxEndFrame times
each frame with EE timer 0, and from the VIF1 list's completion interrupt
(D_004810B4), and GameSceneTimeScaleUpdate scales the next frame's logic by
it, up to two steps. With the list on its own thread its real duration
became visible: in the battle about half the frames took three vblanks, and
the logic ran 1.5 to 2 times per frame, so turning overshot. Timer 0 follows
the host clock, so host slowness reads as slow frames, which an emulator's
guest clock never shows. `src/scene_timing.cpp` leaves the scene's step and
time scale at 1 after the update (slow motion, as an emulator gives, when
the host is slow); music still follows the measured time
(`RG_TIME_SCALE=game` restores the game's scaling). The flips themselves
turned out to have another cause, rounding (below), and came back by hand
with the clamp in place. With rounding fixed, the game's own scaling turns
cleanly in a scripted battle (4 single snaps in 556 updates of the leader,
none swinging back), but the first play with it lost Jaster in battle, so
the clamp stays the default until that is explained.

**Rounding.** The EE's FPU and VU0 round every result toward zero. The
host rounds to nearest, and code that rounds with the 1.5 * 2^23 trick
(add and subtract 12582912) then gets the next integer up. AngleWrap
(0x1CBF30) and VecWrapAngle (0x1CBE90) wrap an angle that way, so on the
host a small angle such as -0.23 came back as 6.05, a full turn added.
Turning code that takes the difference of two wrapped angles then turned
the leader the wrong way at full rate, and in battle, whenever the stick
was released, the leader swung 60 degrees each way around its target every
frame: 497 jumps of over a radian in 966 movement updates. Guest code now
runs toward zero, with denormals as zero, like PCSX2's default
(`patches/ps2recomp/0003`, `PS2X_EE_ROUNDING=nearest` to compare): 2 in
669, all angles within plus or minus pi. The mode belongs to the guest
fibers: the executor's own stack keeps the host's, but the host code that
guest code calls on a fiber (syscalls, stubs, the GS frontend) runs under
it too, and its float work tolerates that (exact divisions by 16, no
rounding to integers).

**Story-transition hangs.** After the Story loading bar reached 100% the
game could stop for good. One cause was the GS worker: it held every
prepared presentation until the end of its batch, each holding one of the
SDL backend's three slots, so a batch with four waited on itself
(`patches/ps2recomp/0002`; a forced worker stall reproduced it). Another,
in about one scripted run in ten, remains: the saved return address of
CfgVmRun (0x42FA20) at 0x01FFEF30 is overwritten by CfgVmFindOp and
DataTableFindByKey working through a stale VM pointer in the global data
table (gp-0x2078, its +4), left from an earlier DataTableRunCfg frame; the
main thread then returns into the stack and is put to sleep while the
loading thread keeps drawing the full bar. `RG_HWWATCH` (an x86 debug
register on a guest word) found the writers; what leaves the pointer stale
is not known yet.

**Hooks and safe points.** A hook that replaces a function's table entry
and does its work after calling the original was wrong whenever a safe
point inside the original unwound to the run loop: the original finished
later, after the hook's work. `src/return_hook.cpp` runs that work when
the call really returns, by taking over the return address's table entry
until then; flip pacing, the scene timing and the movie audio use it. A
hook that returns without calling the original has to leave `pc = ra`
itself, as `jr $ra` does: a call through dispatchGuestBranch forgives a pc
left at the entry, but a thread that a safe point unwound at that call is
resumed by the run loop at the entry, and a hook that leaves pc there is
dispatched again forever. The VIF1 hooks for sceDmaSend and sceDmaSync
did that, and the 60 fps mode's event cadence made the loading thread's
`Vif1DmaHandlerShutdown` hit it every time at 1x scale: the game froze at
the Story text with the executor spinning (`RG_PROFILE` with
`RG_PROFILE_STACKS=1` showed the game thread cycling through the hook,
`PS2X_KERNEL_TRACE_MS` the thread stuck at the hook's entry).

**Movie audio.** The opening movie's sound turned, part way through, into
full-scale noise or a garbled version of itself. Movie files (.MVI) are
program streams with every 2048-byte sector XORed by one keystream (seed
times 0x02E90EDD per word, the seed from a key string the game decodes),
which a sector's pack header gives away, so the disc's PCM could be
compared with what the game sent to the IOP (`RG_DUMP` on
SifDmaSendSync, 0x1D2C20) and with the output (`PS2X_AUDIO_DUMP`). The
game's PCM callback (0x1D4CA0) copies each packet, after its four-byte
sub-stream header, into a 48 KB ring that the read thread drains to
pcmplay's 24 KB on the IOP, and drops what does not fit: the runtime's
demux runs ahead of the IOP by wall-clock time, so whole packets went
missing, 4059, then 8132, 12191 bytes into the stream. The "SShd"/"SSbd"
stream is 16-bit stereo in 512-byte blocks per channel: an odd shift gave
noise, an even one swapped channels block by block. `src/movie_audio.cpp`
queues every packet and tops the ring up whenever the read thread has
sent some of it (`RG_MOVIE_AUDIO=game` for the game's callback); the
whole movie now matches the disc sample for sample, bar a click in its
first 0.2 s. The SPU2 plays it at full scale, as the game sets it (BVOL
0x7FFF, MVOL 0x3FFF on both cores).

**A VIF1-thread race.** The runtime's MSCAL/MSCNT callbacks read FBRST's
D/T enables from, and wrote VPU_STAT into, the context of whatever guest
thread the scheduler was running; from the VIF1 thread that context could be
an interrupt invocation being popped. `vif1_thread.cpp` installs its own,
which take the enables captured when the list was started and leave the stop
bits for the game thread. One device loss (`VK_ERROR_DEVICE_LOST`) at the
Story-to-cutscene transition remains unexplained (once in about fifty
runs); `src/debug/crash_report.cpp` now prints the exception and the call
stack of any crash into the log.

**Seeing through walls.** After some menus (the transporter's, and
something after the first battle) the canal water, characters and props
drew over walls: a glassy band across the screen by the canals, dark boxes
in mid-air. The header strip of the transporter menu is drawn with a
scissor of 0 to 512 on a 512-pixel buffer (FBW 8); the GPU backend sized
its surfaces by the scissor, so the depth buffer at page 0xE0 grew to 513
pixels and its page set took in page 0x150, one past its end. The game
uploads textures there for every object, and each upload made the backend
discard the depth buffer (7905 times by the frame examined), so whatever
came after drew on cleared depth. The surfaces now grow past FBW only for
geometry that reaches past it, and host scissors are clipped to the
surface (`thirdparty/dq8-gfx`, see its README). Found with a frame log
(`RG_GS_FRAMELOG`, every batch of one frame and GS memory beside it),
rasterizing it offline with the right depth, skipping batch ranges of that
frame (`RG_GS_FRAMESKIP`) and `DQ8_GFX_TRACE_TARGETS`; scripted runs
replay alike with `RG_INPUT_CLOCK=pad`.

**In-game menu.** DQ8-Recompiled's menu (`thirdparty/dq8-gfx/src/ui`, Dear
ImGui over the SDL GPU window) is in, on F1, the Guide button or Back+Start:
window and fullscreen, vsync, internal resolution changed live, aspect and
scaling filter, volume, controller and keyboard remapping, pause,
fast-forward, a frame-rate counter and screenshots, saved to `settings.ini`
under `%APPDATA%\RogueGalaxy\Recompiled`. Every hook it needs was already in
the vendored backend (identical headers), and the runtime had host pause and
speed, so the port is the copy with the name taken off, defaults for this
game (2x, mailbox, no line-blend change) and about sixty lines of host glue
in `src/graphics.cpp`. The counter counts the game's flips (the hook in
`src/flip_pacing.cpp` now installs even without flip pacing), not presents,
which go on while the game is paused. Widescreen and 60 fps are separate
work: the game has no 16:9 option of its own; nemesis2000's PCSX2 patch (five
words in the ELF, verified) would give it, and no speed-correct 60 fps code
exists for the NTSC-U release.

**60 fps (`RG_FPS=60`, experimental).** The game paces itself with a vsync
interval the flip waits for (2 at boot, `DrawEnvSetVsyncInterval`; its own
menus switch to 1 with the scene's time scale locked) and scales the
scene's logic by the measured frame time: `GfxEndFrame` clamps the frame's
CPU/GS time to between one and two intervals, and `GameSceneTimeScaleUpdate`
divides it by two, never below 1.0, so at interval 1 the game would run
twice as fast (the PAL 60 fps patch rewrites those constants).
`src/scene_timing.cpp` writes interval 1 before each flip (2 while a movie
plays, between `MoviePlayerStart` and `MoviePlayerShutdown`; the game's own
`MovieIsStarted` flag stays set after a movie) and, after the update, a
scale of frame time over two with 0.5 as the floor, so the logic advances
at the same rate over however many frames the host manages
(`RG_TIME_SCALE=fixed` holds 0.5). The game's locked overlays are left to
themselves. It runs, at the right speed, and this machine then shows about
33 frames a second in Salgin's streets at 1x as at 2x: the GS worker is
the limit, not the GPU's resolution. Its first profile (`RG_PROFILE`) was
a quarter CPU-side texture writes (`GsVram::writeRunT4` 23%, the 4-bit
textures the game re-uploads every frame, plus 6% in 32-bit writes), a
quarter the NVIDIA driver, and the rest texture-cache and render-target
bookkeeping, including fence waits for render targets read back to the CPU
(`downloadScaled`); the game thread was idle 40% of the time, the VIF1
thread about 15%. Six changes since, all in `thirdparty/dq8-gfx` and the
framework (see the dq8-gfx README's list and `patches/README.md`): texture
writes in closed form with SSE kernels (PSMT4 uploads 40x faster; the
upload path went from 30% of the worker to 6%), host-to-local transfers
that flush the queued draws only on a real conflict, a whole-surface sprite
taking its surface over instead of importing aliased pages, texture reads
counting only the pages they sample, a passive depth attachment so a
clear and the object after it share a pass, the rounding mode read from
MXCSR instead of the CRT per command, and a 16 MiB worker queue. Every one
left the regression frame byte-identical. Salgin went from 33 to about 35
frames a second, at 2x as at 3x, and the worker is still saturated, now
almost entirely in draw submission (SubmitMany 65%): 900 draw calls, 220
render passes and 14 render-target readbacks a frame, each readback a
fence wait that drains the GPU. The CPU savings bought little because the
frame is bound by that serialisation of the GPU, not by CPU work beside
it. Reaching 60 means changing how the backend schedules the GPU: ending
the readbacks the game's VRAM reuse provokes (the character target at
page 0x150 shares pages with the bloom targets and with every object's
texture upload), and far fewer passes and command buffers per frame. The
VIF1 thread (85% busy, a sixth of it waiting on the worker before the
queue grew) and the game thread (75%) come next. For the VIF1 thread the
profile says, in order of gain per effort: batch primitives into one
SubmitMany per run instead of one queue command (with its lock, copies and
variant) per primitive (about 6%); finish an XGKICK in bulk once the
program has ended instead of one quadword per two interpreted cycles
(4-5%); UNPACK fast paths for the plain modes and one copy fewer per IMAGE
chunk (4%); and, bigger, having the VU1 compiler emit flag bookkeeping
only where a flag is read (5%). About 45% of its time is the compiled VU1
programs themselves.

**Readbacks (the first of those fronts).** The backend's stats now count
resolves by reason (`DQ8_GFX_STATS_EVERY`) and its target trace can be
switched on for a window of frames (`RG_GS_TRACE_BEFORE`). In Salgin's
streets the game provoked 7.8 readbacks a frame, 9 to 13 ms of fence
waits: three quarters of them host writes of 4-bit textures into pages a
live target owned in part (the uploads land in the character buffer's and
the bloom buffers' pages, which only CT32 writes could patch), the rest
T8 textures read from pages the bloom target still held, the padding row
of the bloom blur's feedback snapshot, and page marking that took the
page after each 16-colour palette. All in `thirdparty/dq8-gfx` (its
README lists them): exact page marking for a block base inside a page;
host writes in CT16, T8 and T4 patching a native CT32 target word by word
like CT32 ones, across payloads; texture reads consulting those words, and
leaving a target's pages that the draw reaches no texel of; the snapshot
padding copied from the owning target's texture (through the reinterpret
shader for a CT16 view). The readbacks are gone (0.01 ms a frame) and the
walk stays byte-identical to its references at 1x and at 3x. Under the
test harness (stats and census on) at the game's 30 fps the walk at 3x
went from 22.8 frames a second with the worker at 98% to a steady 29.9 at
75%, and at 1x the worker went from 74% to 70%. In the 60 fps mode at 3x
it changed nothing: 32 to 36 frames a second in the streets with the new
paths as without them (there the waits were only 2 ms a frame, and the
CPU patches the uploads now become cost about as much). What the worker
does now, from its profile: about 45% in the driver and the kernel, where
every patch application, page import, reinterpret and clear is a command
buffer submission of its own on top of the ten flushes a frame; the rest
in draw submission, the texture cache and the patches (every patch is
expanded scale-squared on the CPU and uploaded, 40,000 Kpx a minute at
3x). Next: patch uploads and page imports recorded into the flush's
command buffer instead of their own, patches uploaded at native size and
scaled on the GPU, then the pass count.

**Submissions (the second front).** Every texture upload, target patch,
page import, 8-bit expansion, reinterpret, local copy and clear was a
command buffer submission of its own, and so was each flush of the queued
draws, of which the game makes about 70 a frame (feedback snapshots,
transfers into pages the queued draws touch, aliased views), not the ten
the transfer counter showed: about 150 submissions a frame. The backend
now records all of it into one pending command buffer, in order, and
submits it once 96 draws are in it, or when a readback, the present or
the budget for resources SDL cycles per use asks (`thirdparty/dq8-gfx`,
its README: `SdlGpuDevice::pendingCommands`, `DQ8_GFX_SUBMIT_DRAWS`). The
field's frames go out in 4 to 5 submissions. Under the harness at the
game's 30 fps the worker went from 70% to 51% at 1x and from 75% to 59%
at 3x, with the logged frame byte-identical to its references at both
scales; in the 60 fps mode at 3x the streets went from 32-36 to 40-43
frames a second (38.4 to 46.4 over the six intervals before the logged
frame), the worker from 97% to 92%. Next on this front: the render pass
count (about 200 a frame, half of them the breaks around feedback
snapshots, which copy the whole 512x448 surface for draws that sample a
strip of it) and the patch uploads expanded scale-squared on the CPU.

**60 fps logic: what the PAL patch maps to.** Red-tv's PCSX2 patch for
the PAL release (SCES-54552, February 2026; the only 60 fps patch there
is, NTSC-U has none) was mapped onto our ELF by code shape
(`docs/sixty-fps-logic.md`, with the game's time-scale anatomy). Its
vsync byte and two `lui 0x3f00` are what our hook does
(`src/scene_timing.cpp`: the interval field is 0x491B54, the scale is
read only through `GameSceneGetTimeStep`, and the step count at
scene+0x5E04 we also write has no reader). The rest of his patch
compensates, through PAL-only globals, for systems that count frames
rather than the step, and those are wrong in our mode too: the play clock
and the per-second event triggers run at half speed (`PlayTimeStep`
multiplies by the vsync interval); the field minimap halves its step
itself when the interval is 1; NPCs walk and turn at twice the speed
(`NpcObjUpdate` takes no step); thrown objects and projectiles fly at
twice the speed with half the gravity (`TaskStepJudgeObject` passes a
constant 1.0); character gravity, players' and enemies' alike,
accelerates twice as fast (the velocity is scaled by the step, gravity
added per frame, the velocity unscaled: jumps reach half their height,
bosses' jumps too, which his per-boss code cave patched); and the scale
stays 0.5 inside movies (their modal loop never runs the update), so
audio fades there run at half speed. Covered already: base speed, the
title screen (its own overlay mode), FMV pacing and subtitles
(frame-counted), menu timers (overlay mode, scale locked).

**60 fps logic: the frame-counted systems hooked.** All of the above now
take the step (`RG_STEP_FIXES=0` for the previous behaviour). The
interval field itself stays 1 (the flip's wait, `GfxEndFrame`'s clamp,
the quit-to-title prompt and the insect cage want the wait's value); the
three readers that take it for the vblanks a logic frame spans
(`PlayTimeStep`, `MinimapUpdate`, `TextEntrySetupState`, told apart by
the return address of their `DrawEnvGetVsyncInterval` call) get 2 while
the scale is ours, so the clock ticks 2 x 0.5 a frame. `MinimapDrawEntry`
also calls it but overwrites the result before using it. `MoviePlayModal`
is hooked (it reads the step once on entry for its sound fades): scale
1.0 on entry, the movie flag for its whole duration, cleared when it
returns. `src/step_fixes.cpp`, hooks told apart by return address where
a function serves several callers: NPCs run every frame as before, with
the walk factor they all share (0x47D278) scaled by the step for the
duration of `TaskStepNpc` (hooked there, not at the `NpcObjTableStepMaybe`
it jumps to: the recompiler makes a `j` to a known function a direct host
call, past the table) and the two `AngleStepToward` rates
of `NpcObjUpdate` scaled at the call (so they move smoothly at 60
instead of stepping every other frame; the waits they count in frames
are now half as long, which nobody will notice; the 1.0 chase speed
inside 70 units of the player stays per frame); `HandleRegistryRefStepAll`
gets the step in f12 instead of 1.0; `GameSceneGetTimeStep` returns step
squared to the gravity site in `CharaMoveCollideMaybe` (0x234FD8) and
`GetThrowGravity` the same to the held-character site (0x234FC0), while
`GetScaledGravity` returns a whole frame's gravity to everyone (the
launch computations of throws, gun shots, cartridges and script attack
objects, whose integrators now take the step); and after
`CharaMoveCollideMaybe` returns the scaled vertical velocity is clamped
to 10 x step, the terminal velocity's share of the frame, which the
game's own clamp at 10 let reach twice (long falls). Verified in a
scripted run only (the hooks fire, the play clock advances 60 vblanks a
second of game time, no regression in the frame); jumps, throws, bosses'
leaps, NPC walks and the clock want a play-test.

**The freezes: the libc `rand` stub.** Three times in the first hour of
play in the 60 fps mode the picture froze with the music going on. A
watchdog (`src/debug/stall_watch.cpp`: no flip for `RG_STALL_SECONDS`,
15, while the window is active) logs every guest thread's state, the
running one's registers and the addresses it passes over a second: the
game thread was running, inside `ScriptVmExec` (0x173B00, the script
interpreter: 12-byte commands, a slot stack, opcode 21 calls a command
table, 23 yields the frame, 16-18 jump) and the AI commands it calls
(`AiCmdIsInBattleRadius`, `AiCmdRandInt`), the other threads waiting
normally. Reading the frozen process's memory (`tools/hang/dumpmem.ps1`
finds the guest RAM and the thread contexts; `tools/hang/aiscript.py`
decodes a VM's bytecode with the command names from the table at
0x4511E0) gave the loop: a party member's main AI routine, whose top is
reached again without a yield from a branch taken when `RandInt(100) <
10`, expecting the next pass to yield; the state it was in (linked to
itself, nothing to do) made every other branch skip to that one. It was
taken every time because the game's `rand` (0x133710) is bound to the
runtime's libc stub (`main.toml`), `std::rand() & 0x7FFF`, while the
game's own is newlib's 64-bit generator returning 31 bits, and every
caller divides by 2^31 (`RandFloat`, `RandRangeFloat`, `RandInt`): every
random number in the game had been 0 since the start of the port (damage
variance, particles, AI choices, loco selection). `src/libc_rand.cpp`
puts the game's generator back, on the seed in its reentrancy struct
(1, as on the console: the game only calls `srand` in a debug mode).
Nothing to do with the 60 fps hooks; the `RG_STEP_FIXES` toggles stay for
the play-test. The scripted canal walk boots and runs with the fix, but
its logged frame could not be compared: another game was running on the
machine at the time (GPU at 53%, 8 GB of its memory), the GS worker took
83% of wall instead of 40%, and the logged frames came out partial (HUD
and parts of the player missing, fewer draws per frame) with the fix on
or off and with the committed build too. To redo on an idle machine; the
references in `build/run` were also made with every random number 0, so
scenes with random effects may differ legitimately.

**A fourth freeze, from the fix itself.** The first session on the fix
froze again after an hour, differently: the watchdog showed the game
thread "running" at pc 0x133710 (`rand`) with ra inside
`ParticleSwirlUpdate` for 15 minutes, `v0` changing between reports and
nothing else. The scheduler's run loop dispatches a thread parked at a
safe point straight at its pc, and relies on the function leaving the pc
at its return address when it finishes (`jr ra` in generated code; the
generated stub wrappers write `ctx->pc = ra` before calling the stub). A
host function hooked into the table that returns without touching the pc
is dispatched again, forever, the moment a safe point lands on a call to
it; `src/libc_rand.cpp` did that, the other replacements in `src/`
(`movie_audio.cpp`, `overlay_manager.cpp`, `vif1_thread.cpp`) already set
the pc. Fixed by setting it. The watchdog now also prints every host
thread's stack (`src/debug/host_stacks.cpp`, module+offset, executable
frames as link-time addresses for `llvm-symbolizer`), since a guest
thread "running" with a pc that never moves is parked in host code. The
toolchain's `lldb` crashes on start here, so there is no attaching to a
frozen process from outside; the log has to carry everything.

**The party falling out of Salgin's chest house: the VU0 flags.** Four
times on 2026-10-18 the characters fell through the floor on the way to
a treasure chest inside a house in Salgin. `RG_POS_TRACE=1`
(`src/debug/pos_trace.cpp`: every `CharaSetPos`/`CharaSetPosXYZ`/
`CharaPlaceAt` with its caller, the frame the floor clamp first finds
nothing with the collision lists dumped to `build/run/fall_N.bin`, and
the sweeps of the next moves) showed nobody teleporting them: at
(-1235, 29.6, -877) the upper floor ends, a short ramp leads down to the
chest's level, and the lower floor was not in the collision list. The
list (`ActionMgrMoveAll`'s 256-record work buffer, filled once a move by
`MapEvCollectCollTrisNearMaybe`: event objects and treasure boxes first,
then every active map block whose bounds hold the character) was full,
256 of 256, with 60 of the 113 terrain triangles more than 100 units
away; the house block came last and lost its tail. Each block's model
culls by `AabbOverlap` (0x13E0E0), which, like `AabbContains` (0x13E090)
and `AabbContainsPoint` (0x13E130), is VU0 macro code: `ctc2 zero,
Status`, two `vsub.xyz`, `cfc2` of the status register, return
`(status & 0x80) == 0` (the sticky sign bit). The recompiler emits the
subtractions without any flag update, so the read is always zero and
every box test says yes: nothing is culled, the list overflows wherever
the active blocks hold more than 256 triangles, and whichever block is
late in the area's table loses its floor. The three are the only readers
of the VU0 control registers in the main ELF (the `sceVu0Clip*` readers
have no callers), and a scan of the seven overlay images (VIEWER, TITLE,
INSMENU, LAUNCHER, NORMAL, FACTORY, INSECT) for `cfc2` of registers
16-18 finds none, so the host versions cover every reader the game has. `src/vu0_flags.cpp` replaces them with host versions
with the same register side effects (`RG_VU0_FLAGS=0` keeps the
generated ones); the proper fix, status and MAC flags for macro-mode
FMAC instructions in the recompiler, is pending and would need a full
regeneration. Nothing to do with the 60 fps mode or the random numbers;
with every `AabbContainsPoint` true, `CollShapeContainsPoint` was also
always true, so trigger shapes may have fired early before this.

**Walking instead of running on diagonals: the stick's shape.** Reported
2026-10-18 with the chest fix, but older: the character drops from a run
to a walk depending on the direction the stick points. The game reads
each axis as `(byte - 128)`, ignores the first 50 of its 128 steps and
scales the rest to 0..1 (`PadGetAnalogLXNorm`, 0x183AF0), and the player
control (0x1FC4A0, constants at 0x47DC3C/0x47DC40) stands still under a
squared length of 0.0256, walks under 0.81 and runs from there, so a run
needs 0.9 of the stick. A DualShock 2 fills its square range and a full
diagonal reads near (1, 1); the Xbox 360 controller in use (and the
DualShock 4 and DualSense) is round, so a full diagonal is (0.7, 0.7),
which the game's deadzone turns into (0.5, 0.5), length 0.74: the party
ran only within about 20 degrees of each of the four directions. PCSX2's
"analog sensitivity" of 1.33 exists for the same reason. The pad bytes
come from dq8-gfx's SDL sampler through `src/graphics.cpp` (`pump`), so
`src/stick_response.cpp` reshapes them there: the direction and the
pushed distance stay, the distance is measured as the longer axis
(`RG_STICK_SQUARE=0` passes the round range through). Both sticks get it,
as on the console.

**In-game cutscenes at double speed (60 fps mode).** Reported 2026-10-18.
The event system counts frames: `SystemVmUpdate` (0x2509F0, from
`TaskStepEvent` and the battle sub-modes) resumes the system VM and runs
`EventSysUpdate` (0x26AC10: the event camera, the event player with its
cast, voices, sound fades and queued scripts; `EventPlayerUpdate` adds one
to its +0x468/+0x470 counters per call and the scripts' waits count
calls), and `TaskStepMessage` runs `MesMgrUpdateAll` (0x277AC0), whose
windows count their character and page waits in frames. At 60 updates a
second all of it ran twice as fast. `src/step_fixes.cpp` (`event`) now
runs both on the frames that complete a whole step (`rg::timing::
logicFrame`: the host's scales accumulated, capped at two; a button going
down at this frame's `PadUpdate` runs them at once, so no press is lost),
with the scene's scale at 1.0 for the call so the step takers inside (the
event camera, script commands) take a whole one. Two more things fell out
of the same reading. The field modes that lock the scale for a scripted
sequence (`TreasureBoxModeUpdate`, `CharaInteractModeUpdateMaybe`,
`TransporterWarpModeUpdate`) were left to the game's 1.0 while the flip
waited one vblank: chests, talks and warps ran at double speed too. The
lock now only protects the game's own 60 Hz overlays, told apart by the
interval the game itself last set (`DrawEnvSetVsyncInterval`, 0x144140:
1 from `SceneOverlayModeOn`, 2 from `Off` and at boot; a direct write by
an overlay's own copy shows at the flip) and, for a lock at interval 2,
by `GetGameMode` being a field mode (5-7, 9-13). And `PlayTimeStep` gets
its interval of 2 in those locked field modes now, so the clock no longer
runs double there. The frame-time trace (`RG_TIME_TRACE=1`) prints the
game's interval and the logic frames run, skipped and run early. The same
day's 40 s profile of real play (`build/run/profile-play.txt`, 60 fps
mode, 3x, Salgin): the VIF1 thread 90% busy (compiled VU1 programs 30%,
their flag bookkeeping 7%, XGKICK and GIF 7%, UNPACK 8%, one queue
command per primitive 5%), the GS worker 88% (driver 18%, draw
submission 12%, page imports 8%, CPU patches 7%, page and texture-cache
bookkeeping 12%, memcpy 6%), the game thread 54% (46% waiting for the
other two). The frame rate the player sees follows what is in view, 30
through a door to the street, 60 facing a wall, 40-45 in the streets:
both emulation threads need to lose about half their work per frame for
a steady 60 in the heavy views, so the next cuts go to both.

**Battle results at double speed, and the jump checked (60 fps mode).**
Reported 2026-10-19 morning. The battle results window is a task:
`TaskTableRun` (0x1F5340) calls the step of every enabled entry of the
table at 0x44ECF4 once a frame and ignores its return, and `TaskStepMenuWin`
(0x204350) jumps into NORMAL.BIN's `MenuBattleEndStep` (0x4EB6C0), which
counts frames: a wait counter at +0x102, widgets moved 0x14 a call,
`UiFadeWidgetAlpha` over 0x48 frames, and none of it reads the step. The
same holds for the other menu tasks that run in the field and in battle
(`TaskStepMenuBtlChg`, `BtlEsc`, `BattleMenuStep` with the HUD's party
panels, `Shop`, `Think`, `Help`, `EasyChange`, `CompleteCheck`,
`7thMystery`, the insect menus, `MenuWarp`, `Caption`; the NORMAL.BIN
steps call `GameSceneGetTimeStep` only in two Think and transporter
states). `LiveTalk` was in the list for a day: besides the party's remarks
it sequences the skill actions in step with the battle, and on logic
frames using a skill froze the game (2026-10-19, the screen still drawn,
the watchdog silent), so it runs every frame again. `src/step_fixes.cpp` (`menu`) now runs those steps
through the same `logicFrame` gate as the event system: on the frames
that complete a whole step, at once on a button press, with the scale at
1.0 for the call. The main, transporter and space menus run under the
game's own overlay mode (interval 1), where `logicFrame` leaves everything
as it is. `TaskStepStatus` (HP gauges and the screen markers that follow
the characters) stays at every frame on purpose. The player confirmed
the results window and the running jump the same morning.
The jump was reported as feeling fast, with a float at the top in the
original. Measured with the new `RG_JUMP_TRACE=1` (one line per
`CharaMoveCollideMaybe` of the party leader while off the ground;
`tools/bench/jumps.py` sums the arcs) on the scripted load, Circle at pad
read 2277 (Circle jumps in the field; Cross does nothing there): the jump
is a move motion (`CharaActMoveMotion`, 0x21B180, move ids 0xC8..0xCB
from `CharaActLoco`): a rise of 55 units driven by the motion with the
gravity flags set (bits 46 and 37 of chara+0x3D0), a hold at the top
under the same flags, then the fall under gravity (move id 1) until
`CharaHasLandedMaybe` (id 4). Its state counter at chara+0xAD0 advances by
the step the action manager passes in its context (+0x4 =
`GameSceneGetTimeStep`, 0x232220), so it keeps wall time. 30 fps mode:
rise 0.268 s (8 moves), hold 0.235 s (7), fall 0.50 s (15 moves,
-0.4356 a move), peak 55.00. 60 fps mode: rise 0.267 s (16 moves), hold
0.220 s (13), fall 0.50 s (30 moves, -0.1089 a move), peak 55.00. The
standing jump's arc, float and landing are the same to the frame. The
running jump was not: 192 units of ground against 101 in the 30 fps mode
(Circle while holding the stick; `RG_JUMP_TRACE=2` logs the moves on the
ground too). In the air CharaUpdateMovement takes another path: a loop of
whole frames (`for (i = 0; i < step; i += 1)`, 0x20B3A8..0x20B4A0) steers
and clamps the velocity, then sets the step register f23 to 1.0 (0x20B4AC)
and scales the velocity by it before the move, so the move covers one
frame's distance whatever the step (on the console a slow frame made the
jump go half as far a second; nobody noticed). The `CharaMoveCollideMaybe`
hook (`gravity`) now sees f23 == 1.0 with a step below 1 (f23 is
callee-saved and holds the step on the walking path), scales the velocity
by the step for the move and divides it back after, since the game's own
unscale divides by 1.0. With fixed half steps the running jump then lasts
24.5 logic frames against 25, falls 1.80 a move against 3.60, lands with
the same velocity, moves 1.80 a move against 3.60 in every phase, and
covers 77 units against 83 (the end points of the measurement differ by a
move and the landing is on rising ground).

**Throughput, round three (2026-10-19 afternoon): the GS worker.** A
profile of the player's own session (`RogueGalaxy-perfil.bat`, fifteen
minutes of fighting from the File 01 save, `build/run/play-perfil.prof`)
put the worker at 93% busy, the VIF1 thread waiting on its queue a fifth
of the time and the game thread idle half the time. Half of the worker
was the texture stream again: uploads into the pages the scene buffer
held a frame earlier (recording the patches 13%, applying them 7%, the
page imports that follow 4%), and three pieces of bookkeeping: the
texture cache's invalidation walking all 2048 entries for every upload
(7%), the patch finding a page's owners by a linear search per 8x8
cell, and `regionForPages` testing every cell of a target's page grid
for each batch drawn (4%). Done, in `thirdparty/dq8-gfx`: the texture
cache keeps its entries by page (`m_entriesByPage`), so an invalidation
visits only the entries over the written pages (1.3% of the worker
now); `regionForPages` and `markDrawn` walk the set bits
(`gsForEachPage`, reading the bitset's words directly: libc++'s bitset
shift and mask go through bit iterators and cost as much as the tests
they replaced, which the first profile showed as 5% in bitset
internals); `patchHostWrite` indexes its owners table by page. Then
the morning's experiment, releasing the pages an upload covers whole
instead of patching them (`releaseCoveredPages`), is on by default:
the 915 VRAM bytes it left different are the patched path's own. With
`DQ8_GFX_PATCH_CT32_ONLY=1` (a target read back before any non-CT32
write instead of patched) the VRAM is byte-identical to the released
path's, so the block patch of a T4/T8 write leaves byte 2 of some words
of a page's first block row stale (blocks 0, 2, 8 and 10 of pages
0x150-0x151, where a 128x128 CT32 upload is read as a 256x256 T8
texture); that is still to be found, and now only matters for a page a
write covers in part. The display buffer is identical in every
comparison. Numbers at 3x in the 60 fps mode: the street walk's worker
share of the wall 86% to 57-59% at the 60 cap (two runs); the long
walk's limited stretch from 55.0 (profiled) and 59.3 to 59.9 with the
worker at 61% instead of 85-90%; page refreshes 2800 to 1350 per 60
frames (40 to 16 Mpx). References remade as `build/run/ref1x30c` and
`ref3x30c` (the b ones differ only in those 915 bytes).
`DQ8_GFX_NO_COVERED_RELEASE=1` restores the patching. Play-tested the
same afternoon: the battles are much better by the player's feel, with
room left. The profile
launcher now also prints a `[gfx]` line a second into
`build/last-run.log`, so a drop in a session can be put to its minute.
A street profile of this build (`build/run/wl3p.prof`) has the three
threads within reach of each other at the 60 cap: the worker about 70%
busy (texture decode from local memory `GsTextureCache::expand` 6%,
`appendPrimitive` 4%, CT32 writes 3.5%, the CT32 patch 3%, the driver
17%), the VIF1 thread about 80% (the VU1 routines 40%, the image
continuation's copy into the worker's queue 5%, `fmacFlagsWanted`
rescanning images 2%), the game thread 70% (VU0 micro programs through
the interpreter 12%, `dispatchGuestBranch` plus `lookupFunction` 8%, the
VIF1 send 6%). Next, in that order of value for cost: a few-entry cache
for `fmacFlagsWanted`; handing the image continuation to the worker
without the copy; compiling the VU0 micro programs the way VU1's are;
a direct-mapped cache in front of `lookupFunction`; and the block patch's
stale byte 2 above, which `DQ8_GFX_CHECK_GPU_TEXTURES=1` should catch.
The first of those is done (patch 0006): the memory keeps a bit per pair
of VU1 code for the status readers as the VIF's MPG writes the code
(`PS2Memory::markVU1CodeWritten`), so `fmacFlagsWanted` looks at 32
words instead of rescanning the image after each code upload; it is
gone from the VIF1 profile, frames byte-identical. For the player's
sessions, F9 now writes a `[mark]` line to the log; for measuring
battles, `walk.sh` takes `MC0=` (another card: `build/run/mc0-battle`
holds the player's File 03 alone, two characters on a field where
walking meets a battle) and `FRAME=` (the pad read of the logged frame),
and `build/run/input_battle1.txt` loads and walks up and down.

**The battle bench, and the VU0 micro programs compiled (2026-10-19,
late afternoon).** With File 03's three characters the game thread is
the limiter, not the worker: in the field 97% busy at 50-55 frames a
second, in the battle 94% (the worker 70-85%), with the VU0 micro
programs interpreted on it (`VU1Interpreter::execUpper` and the pair
scheduler, 18%), `dispatchGuestBranch` plus `lookupFunction` (9%), the
VIF1 `send` (6%). The whole-program compile covers VU1 only (16 KB data
memory, `drained()`), but the framework's block compiler
(`compile_vu_blocks.py`, `PS2X_VU_AOT_IMAGES`) takes either unit and was
simply not wired in: a battle run with `PS2_VU_AOT_PROFILE` recorded 109
VU0 blocks (and 533 VU1 blocks the interpreter still ran, left out for
now), CMake compiles every `build/vu-blocks/*.txt` (one shard at a time:
the first build, with 32 shards two at a time and the rest of the build
in parallel, was killed for memory), and the interpreter is gone from
the game thread's profile (86% busy from 94%, `execUpper` off the list).
The link needed patch 0007: `ps2_vu_detail::regionCounters`, an inline
thread_local with a destructor, got a TLS initialization wrapper per
shard and lld on COFF refused the duplicates; it is `extern` now and
defined once. Frames byte-identical at 1x and 3x; the battle at 53-57
frames a second (62-67k draws a second) against 48-57 before, in runs
that vary. What remains on the game thread: the indirect-branch dispatch
(a direct-mapped cache in front of `lookupFunction`), the VIF1 send's
gather copy, `checkpointDue` (4%), the EE timers (2%). Two of those are
done since: the dispatcher looks the target up once (patch 0008; the
table slot is a direct index, so there was no cache to add, only the
second look in `lookupFunction` to drop: the dispatcher and the lookup
went from 10% of the game thread to 8%), and the VIF1 send walks the
chain's tags once, keeping the spans aside, instead of twice (once for
the size). Both byte-identical at 1x. The battle bench's number to
compare is now `tools/bench/battlefps.py`, the frame rate over the
intervals with 55k draws a second or more, since each run meets a
different fight: medians 57.4 before the VU0 blocks, 56.1 and 59.8 with
them, 59.3 and 57.4 with these two, in fights of different weight (a
profiled run of a heavy one sat at 41-47 with no thread above 77% busy:
the three wait on each other's queues, which is the next thing to look
at, together with the flip's wait when a frame runs late).

**Throughput, round two (2026-10-19 morning): the emulation threads.**
Profiles of the street view at the player's scale (3, from the menu's
settings.ini) with the new `INPUT=`/`PROFILE_START=` overrides of
`tools/bench/walk.sh`: standing in the street at 3x the GS worker is the
limiter (88-91% of wall, 58.7 fps), with the driver 24%, the CPU patches of
uploads into pages that live targets own 14% (`patchHostWrite`,
`ReadRowCT32`, `writeRunCT32`), draw submission 7%, the caches 5%. At 1x
the same view ran 36-41 fps with the worker at 56%: there the VIF1 thread
(compiled VU1 programs 35%, their flag bookkeeping 7%, the GS queue 5%,
UNPACK 4%, 26% idle) and the game thread (85% busy: the copy of each DMA
chain in `rg::vif1::send` 14%, VU0 microprograms through the interpreter
12%, `advanceEeTimers` at every safe point 5.6%, the dispatch of every call
through `checkpointDue`/`lookupFunction` 11%) set the pace. Three cuts,
rendering byte-identical between the three builds at 1x in the 30 fps
mode (`fl1`..`fl3`):
- patch 0006, the VU1 flags: Rogue Galaxy's microcode images read MAC
  flags in a few places (FMAND 29, FMEQ 13 across all images) and status
  flags nowhere, and nothing outside VU1 can see them; a compiled run of
  an image without such readers (`VU1Interpreter::fmacFlagsWanted`, one
  scan per microcode generation) no longer queues its FMAC results, which
  is most of `commitFlags`. `PS2X_VU_FMAC_FLAGS=1` keeps the old behaviour.
- `src/vif1_thread.cpp`: the chain's tags are walked once for the size
  and the copy happens once into a reserved buffer, which comes back from
  the VIF1 thread with its capacity (a pool of eight) instead of being
  allocated and freed across threads for every list.
- patch 0006, the EE timers: `advanceEeTimers` batches its cycles to 2048
  (7 us) before walking the timers; register reads and writes settle the
  batch first, so the count they see is exact.
At 3x in that street the frame rate did not move (58.7 to 58.5, the
worker being the limiter), but the game thread's profile did: `send` 6.1
to 3.9% with its memcpy from 13% to under 1%, `advanceEeTimers` 5.6 to
1.3%. The view that matters is the player's own: standing in the street
by the save point of File 01, 46 fps at 3x, and there the VIF1 thread is
the limiter (90% busy: the compiled VU1 programs 45%, `commitFlags` still
6.8% because the map's image reads MAC flags and keeps its results
queued, the copy of textures split over DIRECT packets 5%, the GS
frontend 5%; the worker waits 22% of the time, the game thread 35%). A
texture split over DIRECTs now continues in place
(`GS::processImageContinuation`, patch 0006) instead of being copied
behind a synthesized IMAGE tag: that view went from 46 fps (44-47 over
20 intervals, three runs) to 53-58 (run to run; the street at 3x varies
by 3 fps between runs now, so a change needs three runs each way). An
experiment on the worker's side, releasing the pages an upload covers
whole instead of patching them (`DQ8_GFX_COVERED_RELEASE=1`, see the
dq8-gfx README), took the worker from 79% to 63% of the wall without
moving the frame rate and left 915 VRAM bytes different; it stays off. Then the generated VU1 code itself:
`compile_vu_programs.py` now decides pair by pair whether a MAC flag
reader can still see an FMAC's result (backwards over the routine's
blocks: a later FMAC that lands first kills it, a call, register jump or
pair left to the interpreter keeps it, the E bit ends it) and the compiled
pair queues the result only then; 138 of the game's 1775 FMAC pairs keep
their flags. The map's routine went from 14% to 9% of the VIF1 thread,
`commitFlags` from 7% to 3%, and the player's street view from 53-56 to
the 60 cap (three runs), byte-identical frames at 1x and 3x. What is left
on that thread: the pairs' cycle bookkeeping itself (the routines are
still 35% of it), the GS queue (one command per primitive, 3-4%), the
UNPACK shapes outside the plain path. A trap found on the way: the scripted
load picks a save by its position in the menu, and the player's File 02,
saved again that morning, changed what the walk loaded (3155 batches at
the logged frame against the reference's 5820, the scene drawn into
frame buffer 0 instead of the 0x150 pass). `RG_MC0_DIR` (new) points the
memory card elsewhere; walk.sh runs on `build/run/mc0-bench`, a copy
holding only File 01, so the player's card is never read or written by a
run, and the references are remade on it.

**Throughput, round one (2026-10-18 night).** Measured with
`tools/bench/walk.sh`, the scripted Salgin walk on the pad clock: speed
as the six `[gfx]` intervals before the logged frame at 3x in the 60 fps
mode, correctness as the frame at pad read 2660 at 1x in the 30 fps mode
against a reference made with the same build and save
(`build/run/ref1x30`; the save changed at 21:23, so the 10-17 references
are dead). Baseline 50.5 frames a second. Three cuts, each byte-identical
at 1x: the VIF1 thread's XGKICK finishing in bulk once the program has
ended or stalls on its next kick, plus a plain-shape UNPACK path
(`patches/ps2recomp/0005`): 54.5; the GS worker's same-state fast path
for primitives whose draw state repeats the previous one, 78% of the
streets' primitives (`thirdparty/dq8-gfx`, `Impl::submitSameState`):
56.2; page imports between targets copying runs of pages as one copy:
57.9; a second pass over the fast path (no cache call for texels already
acquired, pages by the batch's bounding box) took the worker from 96% to
90% of wall at the same 57 to 58, the walk's own noise being about one
frame a second between runs; an early-out in the texture cache's
invalidation (the union of every entry's pages, so a flush or host
write into pages no texture uses skips the walk over up to 2048 entries)
measured 58.2 in the same noise. The worker is still the limit. The
flushes of the queued draws, now counted by reason in the stats line,
are about 83 a frame in the streets: 46 for texture reads of pages
queued draws write, 17 after recorded feedback draws, 9 for transfers
into queued pages, 8 for views aliasing queued pages; each ends a render
pass, which is where the 230 passes a frame come from. A street-only
profile (`PROFILE=1 LONG=1`, pacing one stretch of street for 40 s)
puts the worker at 86% busy with a quarter of it in host writes landing
in pages live targets own (the CT32 patches: `applyCpuPatches` 11%,
`patchHostWrite` 5%, the VRAM row reads 5%), the driver at 19%, memcpy
at 10%; the VIF1 thread at 78% (compiled VU1 programs 35%, `commitFlags`
7%, the queue to the worker 5%); and the game thread at 78%, a quarter
of that waiting to hand lists to the VIF1 thread. The large CPU patches
now go to the GPU at native size and are blitted scaled (`patchNative`;
the expansion was 24 MB a frame at 3x): the worker's share of the walk
went from 93% to 84% of wall, 58.9 frames a second against a cap of 60,
and the frame at 3x in the 30 fps mode is byte-identical to a reference
made just before (`build/run/ref3x30`; 3x holds 29.9 there now, so
scale-dependent changes can be checked too). The walk's six intervals
sit at the cap now; the long walk's pacing window (`LONG=1`, printed as
intervals 12 to 36 after the marker, 44 to 60 frames a second) is the
speed gauge from here. A trace of three frames (`RG_GS_TRACE_BEFORE=3`)
put 41 of the 46 texture-read flushes a frame on one read: the 512x512
CT32 frame buffer at page 0, sampled by draws into the CT24 buffer at
page 112, with a live target for it that the draw could not bind. A
target behind only on patched host writes now gets them landed and is
sampled live (`settlePatches`): pacing window 54.6 to 56.5 frames a
second, frame identical at 3x, but the flush count did not change, so
the frame buffer is held back by something else (needsUpload, aliasing
queued draws, or the sampled rectangle not fitting); the trace line
prints those now. It was the aliasing, by half a texel: the sampled
rectangle carries bilinear's half texel past the last row, which for a
512x448 frame buffer read in 41 strips lands in the back buffer's first
page row, where that frame's draws are queued, and likewise for an object
buffer read while the next one is drawn. A draw that samples within a
live target now counts only that target's pages against queued draws:
texture-read flushes 47 to 3 a frame, all flushes 83 to 40, render passes
230 to 180, pacing window 56.5 to 58.3 frames a second, both reference
frames byte-identical. What remains of the flushes: 17 a frame after
recorded feedback draws (one snapshot texture, so each such draw lands
before the next snapshot), 10 for transfers into queued pages, 8 for
views aliasing queued pages; and the CT16 buffer at 0x3800 reading
itself in 16 strips reaches two rows past its end, one more than the
snapshot padding covers. Left on
the list: the GS worker's draw submission and texture-cache bookkeeping,
the render pass count, the VIF1 queue's per-primitive command and
copies, and the compiled VU1 programs themselves. A profile taken by the
walk (`PROFILE=1`) is diluted by the loading and menu stretches around
it; the user's play profile is the better read.

## Open questions

- What leaves the data table's VM pointer stale before the Story hang.
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
