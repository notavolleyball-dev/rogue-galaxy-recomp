# 60 fps logic: the game's time scale, and Red-tv's PAL patch mapped onto NTSC-U

Static analysis of October 2026 for the `RG_FPS=60` mode (`src/scene_timing.cpp`):
what the game scales by its time step, what it counts in frames, and what the
one existing 60 fps patch for the game (Red-tv's PCSX2 pnach for the PAL
release, SCES-54552, February 2026) changes, line by line, in NTSC-U terms.
Section 4 is the list of what our mode still gets wrong and the hook each
needs. The findings were made with `tools/mipsdis.py` (a small R5900
disassembler with the function names of `config/SCUS_974.90/functions_*.csv`);
the PAL ELF was not available, so PAL addresses are matched by code shape and
meaning, with the confidence stated for each.

Static analysis of `disc/SCUS_974.90` (and the overlay BINs) with RGDecomp's
names from `config/SCUS_974.90/functions_*.csv`. No RGDecomp source checkout
exists on this machine and no MIPS disassembler was available (the LLVM
toolchain lacks the Mips target, no capstone), so the work used a small R5900
disassembler written for this task:
`tools/mipsdis.py`
(`dis START END`, `refs ADDR..`, `calls ADDR`, `scan WORD MASK`, `find HEX`;
gp = 0x485170 from `.reginfo`). The PAL ELF is not available, so every PAL
address below is mapped by code shape and meaning, not by diffing binaries.
PAL/NTSC-U code deltas are not constant (about +0xB9C..+0xBCC around the
time-scale code, +0x8A0 at the gravity site), and the data/bss deltas differ
again (+0xEB80 for the vsync field), so PAL data addresses could only be
mapped semantically.

## 1. NTSC-U anatomy the patch touches

### 1.1 GameScene time scale (0x1CEB60..0x1CED1C)

Scene object: static, pointer at gp-0x37D8 = 0x481998 (`GetCurrentScene_1f5be0`);
the run logs show it at 0x004A7000. Fields:

| off     | type | meaning (from the code)                                                   |
|---------|------|---------------------------------------------------------------------------|
| +0x5E00 | u8   | "double step" mode: a slow frame takes step 2 instead of scale 2 (never set in the main ELF) |
| +0x5E04 | s32  | step count this frame (1 or 2). **No reader anywhere in the main ELF** (scanned every instruction with imm 0x5E04) |
| +0x5E08 | f32  | base scale, the game's slow-motion knob (`GameSceneSetTimeScale_1ceb70`: value + frame countdown) |
| +0x5E0C | f32  | per-frame scale from the measured frame time                              |
| +0x5E10 | s32  | countdown (frames) for the base scale, -1 = none                          |
| +0x5E14 | u8   | locked: the update leaves +0x5E0C alone (overlay/menu mode)               |
| +0x5E15 | u8   | suspended (set once by `GameSessionInit_1f5d80+0x5f8`)                    |

`GameSceneGetTimeStep_1ceba0` returns `+0x5E0C * +0x5E08`; this is the only
way any code reads the scale (no direct loads of +0x5E08/+0x5E0C/+0x5E04
outside the 0x1CEBxx getters/setters).

`GameSceneTimeScaleUpdate_1cebe0` (called once per frame from
`GameFrameUpdate_27c4c0+0x8` via `GameSceneTimeScaleUpdateCur_2f76f0`):

```
if (countdown > 0 && --countdown == 0) { base = 1.0f (lui v1,0x3f80 @0x1CEC08); countdown = -1; }
scale = 1.0f (lui v1,0x3f80 @0x1CEC18); step = 1;
if (locked) return;                     // 0x1CEC28
if (suspended) return;                  // 0x1CEC38
f = DrawCtxGetFrameTime(GetDrawCtx()) / 2.0f;     // ctx+0xF10, in vblanks
if (f <= 1.0f (lui v1,0x3f80 @0x1CEC64)) return;  // on-time frame
if (doubleStepMode) { if (f <= 1.1f [0x47D464]) return; step = 2; return; }
scale = f;                              // (the "scale <= 2.0" test is always true here)
```

So with the flip waiting one vblank and the clamp below, f is 0.5..1.0 and the
game runs at double speed unless the scale is forced to 0.5, which is what our
hook (`src/scene_timing.cpp`) and Red-tv's two `3c033f00` writes both do.

Lock/overlay users: `SceneOverlayModeOn_288340` = interval 1 + lock,
`SceneOverlayModeOff_288300` = unlock + interval 2; called by
`MainMenuModeUpdate_27d460`, `SpaceMenuModeUpdate_27d130`,
`TransporterMenuModeUpdate_27cc20`, `EvCmdSceneOverlayModeSet_25bc80`,
`LoadingModeUpdate_285450` (off), INSMENU.BIN (0x4E9F5C/0x4E9FA4),
NORMAL.BIN (0x51663C/0x51A084). TITLE.BIN has its own copy
(`title_TitleSceneOverlayModeOff_4eaa00`: interval 1 + lock; the function
before it, 0x4EA9E0, writes interval 2). Lock only (no interval change):
`TreasureBoxModeUpdate`, `CharaInteractModeUpdateMaybe`,
`TransporterWarpModeUpdate` (1 then 0); `BattleModeUpdate_280220+0x102c` and
`FieldExploreModeUpdate_2833a0+0x194` pass 0 (unlock).

### 1.2 Frame time and the draw context

`GfxSysInit_15dd20`: DrawCtx is the static object **0x4918B0** (pointer at
gp-0x40CC = 0x4810A4). `DrawCtxInit_145a10+0x50` sets ctx+0x270 = ctx+0x280,
so the DrawEnv is **0x491B30** and the vsync interval field env+0x24 is
**0x491B54** (env+0x28 = 0x491B58 is the second argument of
`DrawEnvSetVsyncInterval_144140`, always 0). Our runtime log confirms it:
`[timing] flip: ... drawctx 0x004918B0 env 0x00491B30`.

`GfxEndFrame_141c20`: f20 = T0 count / env+0x2C, f21 = (vblank-handler
timestamp 0x4810B4, `Vif1IntcHandler_15e170`) / env+0x2C (CPU and GS side of
the frame, in vblanks); 100*f/interval feed the perf meter (ctx+0x3D24/0x3D28);
then `GfxFlipBuffersMaybe_143650` (which calls `WaitVsyncCount_15de90` with
env+0x24 against the vblank counter 0x4810C4); then at 0x142078..0x1420C8:
`frameTime = max(f20, f21); if (frameTime <= interval) frameTime = interval;
if (frameTime > 2*interval) frameTime = 2*interval; ctx+0xF10 = frameTime`.
The lower clamp exists because a frame cannot have taken less than the vsync
interval the flip just waited for (the logic step is never shorter than the
display period); the upper clamp bounds catch-up to one extra period (beyond
that the game slows down instead of stepping further). Note the clamp runs
after the flip, so a flip-time override of env+0x24 that is restored right
after the flip would clamp against the restored value.

### 1.3 Per-frame driver

`MainLoop_1c2c10` -> mode function -> `GameFrameUpdate_27c4c0`:
TimeScaleUpdate, pad bindings, `PadUpdate_1843a0` (once per frame),
pause requests, `PauseStepUpdate_2f5fb0`, `GameModeDispatch_27c670` (mode
table at 0x460D00, index = `GetGameMode` = u32 0x4B51A0: 1 GameStart,
3 Loading, 4 DiscChange, 5 MapWarp, 6 FieldExplore, 7 Battle, 9 AttackChain,
10 Ability, 11 TransporterWarp, 12 CharaInteract, 13 TreasureBox,
14 InsectMenu, 15 InsectBattle, 16 Factory, 17 MainMenu, 19 SpaceMenu,
20 TransporterMenu, 21 DebugAction), then `GfxEndFrame`.
`MoviePlayModal_267c60` is a separate modal loop (its own DrawListFrameBegin /
GfxEndFrame / PadUpdate, never calls TimeScaleUpdate), entered from
`EvCmdPlayMovie_2455a0` / `EvCmdPlayMovie_268680`.

## 2. Red-tv's lines, one by one

| PAL patch | NTSC-U equivalent | what it does | confidence |
|---|---|---|---|
| `004A06D4 byte 01` | **0x491B54** (DrawEnv+0x24, DrawEnv 0x491B30 = DrawCtx 0x4918B0+0x280) | vsync interval the flip waits for; PCSX2 re-applies it every vsync, so it overrides `SystemHeapsInit_2020d0+0x8c` (2 at boot), `SceneOverlayModeOff` (2), the title overlay's 2 at 0x4EA9E8. Same field our `beforeFlip` writes (1, or 2 while a movie plays). The PAL bss is shifted by +0xEB80 here. | high |
| `001ce06c word 0`, `001ce088 3c033f00`, `001ce098 3c033f00` | `GameSceneTimeScaleUpdate_1cebe0`: `lui v1,0x3f80` at 0x1CEC08 (+0x28, base restore), 0x1CEC18 (+0x38, default per-frame scale), 0x1CEC64 (+0x84, on-time threshold) | By meaning: default scale 0.5 and threshold 0.5 give `scale = 0.5 on time, frameTime/2 when slower` (exactly our RG_TIME_SCALE=game behaviour). The PAL spacing (+0x1C, +0x10) does not match NTSC-U (+0x10, +0x4C), so the PAL function is laid out differently; the nop'd one is most plausibly the base-restore `lui` (that path only runs after `SetTimeScale(x, n>0)`, which nothing in the main ELF issues: `SceneTimeScaleRestore_2882d0` passes -1, so a nop there is harmless). The instruction-level match cannot be settled without the PAL ELF. | medium (semantics), low (which word is which) |
| `0020b6d4 3c023f80 -> 3c023f76` "player gravity" | `CharaUpdateMovement_20a7d0+0x17A4` = **0x20BF74**: `lui v0,0x3f80; mtc1; div.s f12 = 1.0/dt; VecScale(vel, vel, f12)` | the velocity un-scale after `CharaMoveCollideMaybe`; 0.96/dt damps the velocity (and the gravity it just accumulated) 4 % per frame. It is the only `lui v0,0x3f80` in the function on the velocity path (the other candidate, +0x1150 at 0x20B920, multiplies the slide deceleration). Delta +0x8A0. Why it is needed: section 3.4. | medium-high |
| `0048cb84 41f00000 -> 42700000` (30 -> 60) | the frame-rate constant 30.0f is **rodata 0x45B078** (block after the "Scene Main" string: 0x45B070 0x3343, 0x45B078 30.0, 0x45B080 20.0 units/metre, 0x45B088 9.8) | seconds<->frames everywhere: `GetScaledGravity_23b170` (-2*9.8*20/30^2*dt), `CharaMoveCollideMaybe_234dc0` (+0x20c gravity, +0x914/+0x9c8 15-frame timers), `MoviePlayModal+0x324` (30-frame read-stall timer), `StatusEntryGetTime_3a60f0`, `CharaStatusFramesLeft_3b6c90`, `ScriptFnEventTimerSet_3d6ae0`, `EventFireIntervalTriggers_3e0220` (play time % 30), `MinimapUpdate_3295a0`, `LoadingScreenFrame_3435d0`, `HatenaMarkStep`, `InsectItemTickSatiety`, `UnitUpdate`, `PartyUpdate`, six DataTable param loaders, ... In NTSC-U no `30.0, 1.0, 1.0` run exists anywhere in the image and no code initialises such a trio in bss (searched `41f00000 3f800000 3f800000`, all 96 `lui 0x41f0` sites and their stores). Best hypothesis: the PAL trio is PAL-conversion state (fps + two 50/60 Hz scale factors) that NTSC-U does not have. Changing 30 -> 60 is not something our approach needs. | medium |
| `0048cb8c 3f800000 -> 40000000` (3.0 in menus, 1.0 in FMV) and `0048cb88 3f800000 -> 3fe40000` | no NTSC-U counterpart found | the only global multipliers on the affected systems are 0x47D26C = 1.6 (throw gravity scale, `GetThrowGravity_3647b0`), 0x47D278 = 0.8 (NPC walk speed, below), 0x47DB34 = 0.98 (per-frame velocity friction), 0x47D464 = 1.1 (double-step threshold). Red-tv's descriptions ("timer, menu, object distance, speed", "gravity and object distance") match the frame-stepped systems in section 3.3/3.4, i.e. he used PAL-only globals as knobs for bugs we must fix per system. | unresolved |
| `0048da18 3f800000 -> 3F000000` "NPC distance" | `NpcObjUpdate_2fc450`: step per update = **0x47D278 (0.8f, gp-0x7EF8)** x record speed (+0x10) at 0x2FC7AC..0x2FC7BC; direction x step added to the position (sceVu0ScaleVector/AddVector 0x2FC90C/0x2FC91C); turning by `AngleStepToward` with fixed per-call rates (15.0 or 0x47D3FC = 6 deg) | no time step anywhere in NpcObjUpdate (its call list has no GetTimeStep); it runs every frame via `TaskStepNpc_3127d0` -> `NpcObjTableStepMaybe_2fae50`. 0x47D278 is the NTSC-U "NPC distance modifier"; PAL's value was 1.0, so either PAL retuned it or Red-tv's global is a different factor. | medium |
| `E0010001 00729EE6` (menu open) | heap address in PAL, no static counterpart | native signals: scene+0x5E14 lock set by `SceneOverlayModeOn`, or `GetGameMode()` in {17, 19, 20} | n/a |
| `E0020002 004C1E50` (== 2, movie) | not found (value 2 matched nothing; `EvCmdPlayMovie` only returns 2 for "aborted") | native: our `MoviePlayerStart_1d5b50` / `MoviePlayerShutdown_1d56c0` hooks, or the byte `MovieIsStarted` 0x4817BC (gp-0x39B4; set by `MovieReadThread_1d4f50+0x24c`, cleared by `MovieOpen_1d5cf0`) | n/a |
| `E0020001 004B2660` (== 1, subtitles) | int16 at `*(u32*)0x4B8B3C + 0x1C` (GameData 0x4B8B30 -> +0xC options block, allocated at run time, null after `GameDataInit_3ee5b0`) | read by `MoviePlayModal+0x338` (`lh v0,0x1c(v0)`) and `EventCueStep_2491b0+0x68`; only dynamically resolvable in NTSC-U | medium |
| code cave: `0x4F4708 == 2` (in battle) | `IsInBattle_288420` byte **0x4821B8** (value 1; `SetInBattle_288410`/`ClearInBattle_288400`) or `GetGameMode()==7` | the PAL value 2 does not match either, so it may be a battle-phase variable we did not find | low |
| code cave: `0x49110C == 0x01A473C0`, hook `FUN_001C6870` halving a 4-float delta | unresolved; with the local delta NTSC-U ~0x1C73F0..0x1C7410 = `SrtSetScale_1c73f0` / `SrtSetPosFromVec_1c7410` (jumps to `VecCopyAsPoint_1cbe10`), `SrtSetPos_1c73b0` nearby, `CollSweepCollectHits_1c7550` after | a per-boss symptom patch for the jump integration bug in section 3.4; fixing that bug removes the need | low |

## 3. Who consumes the scale

### 3.1 Through `GameSceneGetTimeStep` (scale x base) -- these honour 0.5

49 call sites in the main ELF (`calls 1ceba0`), plus `GetSceneTimeStep_293350`
(13 menu sites), `ScriptCmdGetSceneTimeStep_3325d0`, `AiCmdSceneGetTimeStep_2c2d10`,
INSECT.BIN x2, NORMAL.BIN x6, FACTORY.BIN x1. By system:

- scene/map: `SceneObjsStep_1f97a0`, `TaskStepMap_200d70`, `MapTickAnims_1e1ac0`,
  `MapMgrAuxAreaTexAnimUpdate`, `GameSceneMapEffectDraw`, `TaskStepMapEffect`,
  `StreamStep_206f20`, `GameSceneSoundUpdate_1d0860`, `GameDataTick_1febd0`
- characters/actions: `CharaUpdateMovement_20a7d0` (+0xac, into f23),
  `CharaMoveCollideMaybe_234dc0` (+0x210, gravity), `ActionMgrMoveAll`,
  `ActionMgrRunActions`, `ActionMgrPostUpdateAll`, `CharaCalcBoundsRangeMaybe`,
  `GetScaledGravity_23b170`, `AttackChainStep_382270`, `TaskStepObject2_2cf580`,
  `PartyTendencyUpdateMaybe`
- camera: `CameraCharaStep_1f9bb0`, `CameraModeUpdate_309770`
- battle/UI: `BattleModeUpdate_280220` (8 sites, e.g. the 0x482218 countdown),
  `TargetMngAimTimerTick`, `BattleEffectBurstUpdate`, `EffModelObjDrawModelMaybe`,
  `MinimapUpdate_3295a0`, `IconStealthStep`, `TaskStepInsectCage`,
  `MenuObjsAnimStepMaybe`, `MainMenuModeUpdate+0x564`, menu model/preview updates
- events: `EventCueStep_2491b0` (only for audio fades), `EventPlayerAbort`,
  `EventObjMgrUpdateAll`, `EventObjMgrPreUpdateAllMaybe`, `EvDoorObjSet*ByName`
- misc: `PadIdleTimerUpdate_2f5a60`, `PlayTimeStep_2f7570` (see 3.3),
  `MoviePlayModal_267c60+0x50`

### 3.2 Step count (+0x5E04)

Written by the update and `GameSceneTimeScaleReset_1cecf0`, read by nothing.
The "double step" mode (+0x5E00) is never enabled in the main ELF. Our
`step = 1` write is therefore inert; everything rides on the scale.

### 3.3 Frame- or interval-counted systems (do NOT scale)

- `PlayTimeStep_2f7570`: `tick = DrawEnvGetVsyncInterval() * GetTimeStep()` ->
  `PlayTimeTick_3e6360` -> `PlayTimeAdvance_3d61a0` (double accumulator in
  vblank units) and `EventFireIntervalTriggers_3e0220` (fires per 30 units).
  Original: 2 x 1.0 per 30 fps frame = 60/s. Ours: 1 x 0.5 per 60 fps frame =
  30/s. **Play clock at half speed, and the per-second interval triggers with it.**
- `MinimapUpdate_3295a0+0x94`: `step = GetTimeStep(); if (interval == 1) step /= 2`
  (the game's own 60 Hz menu mode halves the step itself). Field minimap
  (`MapMgrMinimapUpdate_1e2860` -> `MinimapMgrUpdate_322030`): ours gives 0.25.
  Same pattern in `InsectCageMenuUpdate_38a230+0x30`, `InsectCageSceneDraw_389ed0+0x70`
  (menu, scale locked, so fine), `MinimapDrawEntry_325720+0x78`,
  `TextEntrySetupState_4359f0+0x30` (name entry timings).
- `PauseStepUpdate_2f5fb0+0x4c4`: runs `GoTitleMenuUpdateThunk` `interval`
  times per frame (a 60 Hz menu stepped twice per 30 fps frame). At interval 1
  it runs once per 60 Hz frame: correct in our mode, 2x if the env kept 2.
- Judge objects (projectiles, thrown map objects, attack shapes):
  `TaskStepJudgeObject_3135c0+0x138` passes **f12 = 1.0 constant**
  (`lui v0,0x3f80` at 0x3136F8) to `HandleRegistryRefStepAll_3147d0`;
  `ProjectileMoveStep_317db0` is dt-correct inside (`vel.y += gravity*dt`,
  `pos += vel*dt` at 0x317E08/0x317E14, `JudgeObjectStep_314360` timers -= dt)
  but always receives 1.0. The launch gravity, however, comes from
  `GetThrowGravity_3647b0` = 1.6 x `GetScaledGravity` ∝ GetTimeStep (0.5).
  **Projectiles move at 2x per second with half gravity**: direction+speed
  throws (`EvMapObjThrowStart_3661f0+0x2a0`) fly about twice as far; target
  throws (`CharaCmdHoldThrow_22a160+0x1b0`, velocity = distance/T, T in frames)
  reach the target in half the time.
- NPCs: `NpcObjUpdate_2fc450` per-frame step (section 2) -> **2x walk and
  turn speed**.
- `MoviePlayModal`: subtitle cues are counted in movie frames
  (`EventCueStep`: `+0x49C++` per call, entries matched by frame number),
  so FMV subtitles are independent of the scale; the 30-frame stall timer at
  +0x324 is per loop iteration (paced by the movie).
- `PadSetRepeat_183d50` / `PadIsRepeatTriggered_183dd0`: auto-repeat
  delay/interval in frames (min 2); `PadUpdate_1843a0` runs once per
  `GameFrameUpdate` (+0x4C), so at 60 fps input is sampled every frame
  (lower latency than the original), and field-side repeat runs 2x faster.
- `FrameCounterInc_1c6b00` (0x481630) has no readers besides reset.
- `CharaMoveCollideMaybe+0x914/+0x9c8`: 15-frame ("0.5 * 30") timers stored
  as integers, decremented per frame.

### 3.4 dt-inconsistent integrators (take 0.5 but integrate wrongly)

Character gravity (player, party, enemies, including enemy jumps):
`CharaUpdateMovement_20a7d0`: `VecScale(vel, vel, dt)` at 0x20BF38 ->
`CharaMoveCollideMaybe_234dc0` (0x234FC8..0x235010: `gravity = -2*9.8*20/30^2 * GetTimeStep()`,
`vel += gravity` (vel is already dt-scaled), `pos += vel`, then the terminal
clamp `|vel.y| <= 0.5*20` at 0x235064 on the scaled velocity) ->
`VecScale(vel, vel, 1/dt)` at 0x20BF90. Net per frame: `v += g` (per frame,
not per dt) while `pos += v*dt`. At dt = 0.5 the vertical acceleration is 2x
in wall time: jumps reach about half the height in half the time, falls
accelerate twice as fast, and the terminal velocity is 20 instead of 10
units/frame. The game's own dt = 2 handling has the mirror image of the same
flaw, which is why nobody noticed. This is Red-tv's "Breaks Gravity" and the
reason for his 0.96 damping, the 1.78 "gravity modifier", the "backwards jump
slightly higher/longer" note and the per-boss code cave (enemy jumps use the
same path: `CharaJumpToTarget_211070` / `CharaLaunchTowardTarget_2114e0`
compute a launch velocity for a gravity that then acts twice as fast).
The horizontal velocity: walk speed is right (velocity x dt), but friction
(x0.98 per frame, 0x47DB34 at 0x20B7E0) and the accel/decel ramps near
0x20B908 are per frame, so stops are snappier at 60 fps.

The air path (found 2026-10-19 by measuring a running jump: 192 units at
60 fps against 101 at 30). The sub-step loop at 0x20B3A8..0x20B4A0 is not
dead: 0x20B35C..0x20B388 enter it for move ids 0 and 1 of
`CharaActMoveMotion` (a jump's flight and its fall). It runs
`for (i = 0; i < dt; i += 1)` whole-frame iterations of air steering and
speed clamping, then sets the step register f23 to 1.0 (0x20B4AC) and
jumps to the `VecScale(vel, vel, f23)` before `CharaMoveCollideMaybe`: the
move covers one frame's distance whatever dt is (at dt = 2 on the console
the jump went half as far a second, unnoticed). At dt = 0.5 that is twice
the horizontal distance a second, with the vertical right only by accident
(the step² gravity on an unscaled velocity integrates the position
correctly, with half the velocity). Fix (`src/step_fixes.cpp`, `gravity`):
the `CharaMoveCollideMaybe` hook sees f23 == 1.0 with a step below 1 (f23
is callee-saved and holds the step on the walking path), scales the
velocity by the step before the move and divides it back after, since
CharaUpdateMovement's own unscale divides by 1.0.

## 4. Assessment against our mode (RG_FPS=60: env+0x24 = 1 before each flip, scale 0.5 / step 1 after each update, interval 2 during movies)

*Done since (2026-10-18, `src/scene_timing.cpp` and `src/step_fixes.cpp`,
off with `RG_STEP_FIXES=0`): neither strategy below as such. The interval
field stays 1 and `DrawEnvGetVsyncInterval` returns 2 to the three readers
that take it for a logic frame's vblanks (`PlayTimeStep` ra 0x2F75BC,
`MinimapUpdate` ra 0x32963C, `TextEntrySetupState` ra 0x435A28;
`MinimapDrawEntry` overwrites the result before using it, `lui v0,0x3f80`
at 0x3257A0); `MoviePlayModal` is hooked for scale 1.0 on entry;
NPCs: 0x47D278 scaled by the step around `TaskStepNpc` (the recompiler
turns its `j NpcObjTableStepMaybe` into a direct host call, so a hook on
the jumped-to function is never reached from there; any function reached
only by `j` has to be hooked at the jumper) and the `AngleStepToward` rates
scaled at ra 0x2FC8A4/0x2FC8BC (smooth at 60, waits halved, the 1.0 chase
speed at 0x2FC83C not covered); judge objects:
`HandleRegistryRefStepAll` f12 = step; gravity: `GetTimeStep` step² at ra
0x234FD8, `GetThrowGravity` step² at ra 0x234FC0, `GetScaledGravity` ÷ step
for everyone, and a post-return clamp of vel.y to 10 x step after
`CharaMoveCollideMaybe` (its own clamp at 0x235064 is on the scaled velocity
and zeroes s3+0x18 on both paths, so nothing else depends on it). Since
2026-10-19: the frame counters of the event system (`SystemVmUpdate`,
`MesMgrUpdateAll`) and of the field's menu and HUD tasks (`TaskStepMenuWin`,
the battle results, and the other `TaskStepMenu*` thunks into NORMAL.BIN,
`TaskStepCaption`; not `TaskStepLiveTalk`, which sequences the skill
actions and froze a skill on logic frames; `TaskTableRun` ignores a step's
return) run on the frames that complete a whole step (`logicFrame`, a
button press runs them at once), and the field modes that lock the scale
(chests, talks, warps) take the half steps.*

| Red-tv item | our mode | status | native equivalent |
|---|---|---|---|
| base speed (his vsync byte + two 0x3f00) | scale 0.5 written after every update; RG_TIME_SCALE=game reproduces his threshold change | covered | done (`scene_timing.cpp`) |
| title screen | TITLE.BIN runs its own overlay mode (interval 1 + lock, 0x4EAA00); `afterUpdate` honours the lock | covered; his fix was about PCSX2 re-applying the byte over the title's own writes | none |
| NPC walking too far | not handled: `NpcObjUpdate` steps per frame | **broken** (2x walk/turn) | hook `NpcObjTableStepMaybe_2fae50` (or `TaskStepNpc_3127d0`) to run on every other 60 Hz frame while unlocked (accumulate the scene step and call when >= 1), or hook `NpcObjUpdate` and scale the 0x47D278 factor; the game has no dt here, so skipping frames keeps its exact semantics |
| objects thrown too far | not handled: judge objects stepped with dt = 1.0 while launch gravity uses 0.5 | **broken** (2x speed, ~2x range) | hook `HandleRegistryRefStepAll_3147d0` from `TaskStepJudgeObject` and set FPR f12 = `GetTimeStep()` (scale x base) before calling the original; `ProjectileMoveStep` is already dt-correct, and the launch gravity then matches |
| player gravity (0x3f76, 1.78) | not handled | **broken** (jumps about half height/time, falls 2x acceleration) | make the gravity added inside `CharaMoveCollideMaybe` be g*dt^2 so the un-scale yields `v += g*dt`: e.g. hook `GameSceneGetTimeStep` and return step^2 when `ra` == 0x234FD8 (the call at 0x234FD0) and, for held/thrown characters, the `GetThrowGravity` call at 0x234FB8 (ra 0x234FC0); plus the terminal clamp (0x235064 constant 0.5 -> 0.5*dt) if long falls matter. Red-tv's 0x3f76/0x20BF74 is an approximation of this |
| timer at half speed | not handled: `PlayTimeStep` = interval(1) x 0.5 | **broken** (play clock and per-second event triggers at half speed) | hook `PlayTimeTick_3e6360` (a0 = GameData, f12 = tick) and double f12 while our 0.5 is in force (unlocked, not in a movie); or strategy B below |
| input latency | pad sampled once per frame -> every 16 ms at 60 fps | better than original; field UI auto-repeat is 2x faster (frames) | optional: hook `PadIsRepeatTriggered` to run on alternate frames; low priority |
| FMV (interval 2, multiplier 1.0) | movie hooks set interval 2 | covered for pacing; but `MoviePlayModal` never calls TimeScaleUpdate, so the scale stays at the last field value (0.5): audio fades (`StreamMgrUpdate`, `GameSceneSoundUpdate` via `EventCueStep`) run at half speed inside movies | write scene+0x5E0C = 1.0 in the `movieStart` hook (the next field update restores 0.5) |
| FMV subtitles | cue timing is per movie frame | covered (nothing to do) | none; his fix is PAL-specific |
| menu timers (3.0 in menus) | menus are overlay mode (interval 1, scale locked 1.0): `PlayTimeStep` ticks 1 x 1 = correct, `PauseStepUpdate` loops once, insect cage halves itself | covered | none |
| field minimap | `MinimapUpdate` halves GetTimeStep at interval 1 -> 0.25 | **likely broken** (minimap scroll/fade at half speed); `TextEntrySetupState` similar | hook `DrawEnvGetVsyncInterval` is wrong (the game's own menus rely on 1); either hook `MinimapUpdate` to compensate or use strategy B |
| giant bosses leave small arenas / enemy jumps too far (code cave) | not handled | **broken**, and not the gravity (2026-10-20, the gate fight's boss): the AI counts frames. `AiUpdate_2c05b0` runs once a frame per character, counts its timers in frames, and its script moves the character a step a call (`AiCmdCharaSetPosById_2c9ee0`: position + 29.8 units; `AiCmdMoveByLocalOffset_2c90f0`: a requested move CharaMoveCollideMaybe applies whole), while what ends the charge took as long: 54 steps at 60 fps against 27, out of the arena, held in the air | `ai` in `src/step_fixes.cpp`: AiUpdate on the logic frames as a whole step, the move it made shared half and half with the next frame |
| backwards jump higher/longer | not handled | same | same |
| loading screen's story so far at 2x (lines and Press X) | the screen's own thread (`LoadingScreenThread_343510`: `LoadingScreenFrame`, then GfxEndFrame's flip) fades each line by a constant a frame (0x3445B4..0x344628) and its timer by 1/30 a frame | **broken** until 2026-10-20 night (every flip waited one vblank) | `src/loading_screen.cpp`: between `LoadingScreenStart_343240` and `LoadingScreenStop_3431e0` the flip waits two vblanks, as for the movies |

Two consistent ways to treat the interval field:

- A (current): write env+0x24 = 1 before the flip and leave it. Consistent
  with `PauseStepUpdate`, GfxEndFrame's clamp and the game's own 60 Hz menus;
  needs per-system fixes for `PlayTimeStep` and the minimap/name-entry
  halving.
- B: in `beforeFlip` save env+0x24, write 1 (2 in movies), and in the
  after-flip hook (`flip_pacing.cpp` already has `afterFlip`) restore the
  saved value. The game's logic then sees 2 in the field (PlayTimeStep
  2 x 0.5 = 1, `MinimapUpdate` no halving, overlay menus keep their own 1),
  only the wait is shortened. Costs: `PauseStepUpdate` would run the
  quit-to-title prompt twice per frame (hook it or accept it), the GfxEndFrame
  clamp runs after the restore so ctx+0xF10 is >= 2 (the adaptive
  RG_TIME_SCALE=game would need host wall-time instead of ctx+0xF10), and
  `GfxFlipBuffersMaybe` reads env+0x24 again after the wait (0x1436F8) for its
  bookkeeping.

Either way the gravity, projectile and NPC items are independent of the
interval and need their own hooks.

## 5. Parts of Red-tv's patch that are PCSX2/PAL workarounds rather than game fixes

- The byte poke each vsync (0x4A06D4) and all `E0..` conditionals exist
  because pnach cannot hook functions; we have hooks.
- The `0x48cb8c` juggling (2.0 normally, 3.0 in menus, 1.0 in FMV and with
  subtitles) compensates for his own global multiplier; NTSC-U has no such
  global and we scale per frame instead.
- The 30 -> 60 change of the frame-rate constant would alter gravity
  (∝ 1/fps^2) and every seconds-to-frames conversion; in our design frames
  stay "30 fps frames" with step 0.5.
- The per-boss code cave (gated on a heap pointer value 0x01A473C0) patches a
  symptom of the gravity integration bug.
- "Fix Subtitles in FMV" (restore one `lui`, 0x48cb8c = 1.0 while subtitles
  are on) is not conditional on a movie playing at all, so in PAL it probably
  cancels part of his own speed change whenever subtitles are enabled; NTSC-U
  subtitles are frame-counted and need nothing.

## 6. Not determined

- Instruction-level identity of the three PAL `lui` words (PAL layout differs
  from NTSC-U; only the semantic mapping is solid).
- NTSC-U counterparts of PAL 0x48cb84/0x48cb88/0x48cb8c and 0x48da18 beyond
  the semantic candidates (0x45B078 rodata 30.0; 0x47D278 NPC speed 0.8);
  the PAL trio most likely does not exist in NTSC-U.
- PAL 0x4C1E50 (== 2 movie), 0x4F4708 (== 2 battle), 0x49110C (pointer),
  0x729EE6 (menu, heap), FUN_001C6870.
- Judge-object subclasses other than `Projectile` (`AttackObjBase`,
  `NodeRefObj`) and the horizontal accel ramps in `CharaUpdateMovement` were
  not traced in detail; NORMAL.BIN's six GetTimeStep sites (battle overlay)
  were not analysed; the event message/voice auto-advance timers
  (`MesWin*`, `EventVoiceTick`) were not checked for frame counting.
- Whether `TextEntrySetupState` and `MinimapDrawEntry` produce visible
  problems (they read the interval, but what they scale was not followed).
