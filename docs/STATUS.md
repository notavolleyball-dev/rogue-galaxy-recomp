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
