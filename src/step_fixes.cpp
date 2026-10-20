#include "step_fixes.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::steps
{
namespace
{

// NPCs. TaskStepNpc (0x3127D0) jumps to NpcObjTableStepMaybe (0x2FAE50),
// which runs NpcObjUpdate (0x2FC450) on every NPC of the map once a frame:
// each walks 0x47D278 (0.8) x its record's speed along its heading, and
// turns by AngleStepToward (0x13C150) at 15 degrees a call (0x2FC89C) or
// 0x47D3FC degrees (0x2FC8B4). No step anywhere in it; the waits it counts
// in frames are left to it. The hook goes on TaskStepNpc, which the task
// runner reaches through the table: the recompiler turns its `j` into a
// direct host call, past any entry for NpcObjTableStepMaybe.
constexpr uint32_t kTaskStepNpc = 0x003127D0u;
constexpr uint32_t kNpcWalkFactor = 0x0047D278u;
constexpr uint32_t kAngleStepToward = 0x0013C150u;
constexpr uint32_t kNpcTurnReturns[] = {0x002FC8A4u, 0x002FC8BCu};
// Judge objects (projectiles, thrown map objects, attack shapes):
// TaskStepJudgeObject (0x3135C0) steps them all through
// HandleRegistryRefStepAll (0x3147D0, its only caller) with a constant 1.0
// in f12 (0x3136F8), though ProjectileMoveStep (0x317DB0) integrates by the
// step it is given.
constexpr uint32_t kJudgeObjectsStep = 0x003147D0u;
// Gravity. GetScaledGravity (0x23B170) is -2 x 9.8 x 20 / 30^2 x
// GameSceneGetTimeStep, and GetThrowGravity (0x3647B0) 1.6 times that.
// CharaMoveCollideMaybe (0x234DC0) adds one of them (its own copy of the
// formula, GetTimeStep at 0x234FD0; GetThrowGravity at 0x234FB8 for a held
// character) to the velocity CharaUpdateMovement scaled by the step before
// the call (0x20BF38) and unscales after (0x20BF90): the velocity gains g a
// frame whatever the step while the position moves v x step a frame. Their
// other callers (CharaCmdHoldThrow, EvMapObjThrowStart, CharaCmdGunShoot,
// CharaCmdJoCartridgeEject, ScriptCmdAttackObjSetGravity,
// ObjTypeMonographUpdate) set up velocities for integrators that take the
// step, which want a whole frame's gravity.
constexpr uint32_t kGetTimeStep = 0x001CEBA0u;
constexpr uint32_t kGravityStepReturn = 0x00234FD8u;
constexpr uint32_t kGetScaledGravity = 0x0023B170u;
constexpr uint32_t kGetThrowGravity = 0x003647B0u;
constexpr uint32_t kHeldGravityReturn = 0x00234FC0u;
// After the move, CharaMoveCollideMaybe clamps the scaled vertical velocity
// to 0.5 x 20 units (0x235064): the terminal velocity, per frame.
constexpr uint32_t kCharaMoveCollide = 0x00234DC0u;
constexpr uint32_t kVelocity = 0x80u;
constexpr uint32_t kVelocityY = 0x84u;
constexpr float kTerminalVelocity = 10.0f;
// In the air (a jump's flight and its fall, move ids 0 and 1 of
// CharaActMoveMotion) CharaUpdateMovement takes another path: a loop of
// whole frames, `for (i = 0; i < step; i += 1)` (0x20B3A8..0x20B4A0),
// steers and clamps the velocity, then sets its step register f23 to 1.0
// (0x20B4AC) and scales the velocity by that before the move, so the move
// covers one frame's distance whatever the step. At half steps that is
// twice the horizontal distance a second: a running jump measured 192
// units against 101 in the 30 fps mode. f23 is callee-saved and still 1.0
// at the call; on the walking path it holds the step.
constexpr unsigned kStepRegister = 23u;
// Scripts and events. SystemVmUpdate (0x2509F0, from TaskStepEvent and the
// battle sub-modes) resumes the system VM and runs EventSysUpdate (0x26AC10:
// the event camera, the event player with its cast, voices, sound fades and
// queued scripts); TaskStepMessage runs MesMgrUpdateAll (0x277AC0), the
// message windows. They count frames, not steps: the player's +0x468/+0x470
// counters, the scripts' waits, the windows' character and page waits. At
// 60 fps they run on the frames rg::timing::logicFrame marks, with the
// scene's scale at 1.0 for the call so that what does take the step inside
// them (the event camera, script commands) takes a whole one.
constexpr uint32_t kSystemVmUpdate = 0x002509F0u;
// Callers that run a script to its end inside one frame, `do {} while
// (SystemVmUpdate() != 1)` with no vsync wait in the loop: AbilityModeRun
// (0x2790E0, a skill or power in battle) after SystemVmCall 0xBB8, 0x7D0
// and 0xFA0, and AttackChainModeUpdate (0x27F840). Their return addresses.
// The frame never ends inside such a loop, so a frame that skips the
// script skipped every call of it: using a power hung the game at 0 fps
// whenever it began on a skipped frame (2026-10-20). They run every call
// as a whole step instead.
constexpr uint32_t kSystemVmLoopReturns[] = {
    0x0027982Cu, // AbilityModeRun, script 0xBB8
    0x00279874u, // AbilityModeRun, script 0x7D0
    0x00279980u, // AbilityModeRun, script 0xFA0
    0x0027FDF0u, // AttackChainModeUpdate
};
constexpr uint32_t kMesMgrUpdateAll = 0x00277AC0u;
// The field's menus and HUD windows. Each is a task: TaskTableRun
// (0x1F5340) calls the step of every enabled entry once a frame and ignores
// what it returns, and these steps count frames. The battle results window
// (TaskStepMenuWin -> NORMAL.BIN MenuBattleEndStep: a wait counter at
// +0x102, widgets moved 0x14 a call, fades of 0x48 frames) is the one the
// player sees pass at double speed; the others are the same kind of code.
// Under the game's own overlay mode (the main, transporter and space menus,
// interval 1) the scale is the game's and logicFrame leaves them as they
// are. TaskStepStatus (the HP gauges and the screen markers) is left out:
// its markers follow the characters, which move every frame.
constexpr uint32_t kMenuTaskSteps[] = {
    0x00204350u, // TaskStepMenuWin: the battle results
    0x00204AF0u, // TaskStepMenuBtlChg: the battle change menu
    0x00203D10u, // TaskStepMenuBtlEsc: the escape prompt
    0x00203EA0u, // BattleMenuStep: the field HUD and its party panels
    0x00203750u, // TaskStepMenuShop
    0x00203640u, // TaskStepMenuThink
    0x002032A0u, // TaskStepMenuHelp
    0x002030C0u, // TaskStepMenuEasyChange: the leader switch
    0x00203460u, // TaskStepMenuCompleteCheck
    0x00203540u, // TaskStepMenu7thMystery
    0x00203380u, // TaskStepMenuInsectEntry
    0x002031A0u, // TaskStepMenuInsectCage
    0x00202EA0u, // TaskStepMenuInsectVs
    0x00202DE0u, // TaskStepMenuInsectResultThunk
    0x001FFB90u, // TaskStepMenuWarp: the warp menu's fade
    0x001FFB30u, // TaskStepCaption: the area name
};
// Not in the list: TaskStepLiveTalk (0x321210), which besides the party's
// remarks sequences the skill actions (SkillActionFindPending,
// SkillTimerStartMaybe, SkillActionEnd, an immediate script) in step with
// the battle; on logic frames a skill froze the game (2026-10-19).

PS2Runtime::RecompiledFunction g_taskStepNpc = nullptr;
PS2Runtime::RecompiledFunction g_angleStepToward = nullptr;
PS2Runtime::RecompiledFunction g_judgeObjectsStep = nullptr;
PS2Runtime::RecompiledFunction g_getTimeStep = nullptr;
PS2Runtime::RecompiledFunction g_getScaledGravity = nullptr;
PS2Runtime::RecompiledFunction g_getThrowGravity = nullptr;
PS2Runtime::RecompiledFunction g_systemVmUpdate = nullptr;
PS2Runtime::RecompiledFunction g_mesMgrUpdateAll = nullptr;
PS2Runtime::RecompiledFunction g_charaMoveCollide = nullptr;
bool g_trace = false;

struct Counts
{
    uint32_t npc = 0u;
    uint32_t turns = 0u;
    uint32_t judge = 0u;
    uint32_t gravity = 0u;
    uint32_t held = 0u;
    uint32_t launches = 0u;
    uint32_t clamps = 0u;
    uint32_t air = 0u;
};
Counts g_counts;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + physical, sizeof(T));
    return true;
}

template <typename T>
void writeGuest(uint8_t *rdram, uint32_t address, T value)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) <= PS2_RAM_SIZE)
    {
        std::memcpy(rdram + physical, &value, sizeof(T));
    }
}

// --- NPCs ---------------------------------------------------------------

void restoreWalkFactor(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t bits)
{
    float factor = 0.0f;
    std::memcpy(&factor, &bits, sizeof(factor));
    writeGuest(rdram, kNpcWalkFactor, factor);
}

void taskStepNpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const float step = rg::timing::sceneStep(rdram, ctx);
    ++g_counts.npc;
    float factor = 0.0f;
    if (step == 1.0f || !readGuest(rdram, kNpcWalkFactor, factor))
    {
        g_taskStepNpc(rdram, ctx, runtime);
        return;
    }
    // For the update's duration only: restored when it has returned, also
    // when a safe point inside it unwinds it first.
    writeGuest(rdram, kNpcWalkFactor, factor * step);
    uint32_t bits = 0u;
    std::memcpy(&bits, &factor, sizeof(bits));
    rg::hooks::callThen(g_taskStepNpc, rdram, ctx, runtime, &restoreWalkFactor, bits);
}

void angleStepToward(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    for (uint32_t site : kNpcTurnReturns)
    {
        if (returnPc == site)
        {
            // f14: the degrees this call may turn by.
            ctx->f[14] *= rg::timing::sceneStep(rdram, ctx);
            ++g_counts.turns;
            break;
        }
    }
    g_angleStepToward(rdram, ctx, runtime);
}

// --- Judge objects --------------------------------------------------------

void judgeObjectsStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->f[12] == 1.0f)
    {
        ctx->f[12] = rg::timing::sceneStep(rdram, ctx);
        ++g_counts.judge;
    }
    g_judgeObjectsStep(rdram, ctx, runtime);
}

// --- Gravity --------------------------------------------------------------

// A leaf: it returns to the hook.
void getTimeStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_getTimeStep(rdram, ctx, runtime);
    if (returnPc == kGravityStepReturn)
    {
        // Added to a velocity scaled by the step, unscaled after: step
        // squared gives it g x step a frame.
        ctx->f[0] *= ctx->f[0];
        ++g_counts.gravity;
    }
}

void unscaleGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    const float step = rg::timing::sceneStep(rdram, ctx);
    if (step != 1.0f)
    {
        // A whole frame's gravity, whatever this frame's step.
        ctx->f[0] /= step;
        ++g_counts.launches;
    }
}

void getScaledGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_getScaledGravity, rdram, ctx, runtime, &unscaleGravity, 0u);
}

void heldGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    // Unscaled by the hook on GetScaledGravity it calls; the integration
    // site wants step squared, like the one above.
    const float step = rg::timing::sceneStep(rdram, ctx);
    ctx->f[0] *= step * step;
    ++g_counts.held;
}

void getThrowGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kHeldGravityReturn)
    {
        rg::hooks::callThen(g_getThrowGravity, rdram, ctx, runtime, &heldGravity, 0u);
        return;
    }
    g_getThrowGravity(rdram, ctx, runtime);
}

void clampTerminal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t chara)
{
    const float step = rg::timing::sceneStep(rdram, ctx);
    if (!(step < 1.0f))
    {
        // The game's own clamp is the tighter one.
        return;
    }
    float velocityY = 0.0f;
    if (!readGuest(rdram, chara + kVelocityY, velocityY))
    {
        return;
    }
    const float limit = kTerminalVelocity * step;
    if (velocityY > limit)
    {
        velocityY = limit;
    }
    else if (velocityY < -limit)
    {
        velocityY = -limit;
    }
    else
    {
        return;
    }
    writeGuest(rdram, chara + kVelocityY, velocityY);
    ++g_counts.clamps;
}

// The step the air path's velocity was scaled by for this move, 0 outside
// one (the move is not reentrant).
float g_airStep = 0.0f;

bool scaleVelocity(uint8_t *rdram, uint32_t chara, float factor)
{
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    for (unsigned i = 0u; i < 3u; ++i)
    {
        if (!readGuest(rdram, chara + kVelocity + 4u * i, velocity[i]))
        {
            return false;
        }
    }
    for (unsigned i = 0u; i < 3u; ++i)
    {
        writeGuest(rdram, chara + kVelocity + 4u * i, velocity[i] * factor);
    }
    return true;
}

void afterMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t chara)
{
    clampTerminal(rdram, ctx, runtime, chara);
    if (g_airStep != 0.0f)
    {
        // CharaUpdateMovement divides by its f23 of 1.0 afterwards, so the
        // velocity is unscaled here instead.
        scaleVelocity(rdram, chara, 1.0f / g_airStep);
        g_airStep = 0.0f;
    }
}

void charaMoveCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // a0: the character's movement state; its velocity is at +0x80, still
    // scaled by the step when the function returns.
    const uint32_t chara = getRegU32(ctx, 4);
    const float step = rg::timing::sceneStep(rdram, ctx);
    g_airStep = 0.0f;
    if (step < 1.0f && ctx->f[kStepRegister] == 1.0f && scaleVelocity(rdram, chara, step))
    {
        g_airStep = step;
        ++g_counts.air;
    }
    rg::hooks::callThen(g_charaMoveCollide, rdram, ctx, runtime, &afterMove, chara);
}

void restoreScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t bits)
{
    float scale = 0.0f;
    std::memcpy(&scale, &bits, sizeof(scale));
    rg::timing::setSceneScale(rdram, ctx, scale);
}

// One call as a whole logic frame: the scene's scale at 1.0 for it.
void wholeStep(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const float scale = rg::timing::sceneScale(rdram, ctx);
    rg::timing::setSceneScale(rdram, ctx, 1.0f);
    uint32_t bits = 0u;
    std::memcpy(&bits, &scale, sizeof(bits));
    rg::hooks::callThen(original, rdram, ctx, runtime, &restoreScale, bits);
}

// A frame-counted system's update: skipped, run as the game would, or run
// as one whole logic frame.
void logicStep(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    switch (rg::timing::logicFrame(rdram, ctx))
    {
    case rg::timing::LogicFrame::AsIs:
        original(rdram, ctx, runtime);
        return;
    case rg::timing::LogicFrame::Skip:
        // Nothing to report this frame (SystemVmUpdate's "reset the event
        // scene" comes back on the next run); the host stands in for `jr ra`.
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    case rg::timing::LogicFrame::WholeStep:
        break;
    }
    wholeStep(original, rdram, ctx, runtime);
}

void systemVmUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    static bool reported[std::size(kSystemVmLoopReturns)] = {};
    for (size_t site = 0; site < std::size(kSystemVmLoopReturns); ++site)
    {
        if (ra == kSystemVmLoopReturns[site])
        {
            if (!reported[site])
            {
                // Once a site: the log then says the case was met.
                reported[site] = true;
                std::printf("[steps] a script run to its end in one frame (SystemVmUpdate from 0x%08X)\n", ra);
                std::fflush(stdout);
            }
            // Outside the movies, as logicStep leaves those to the game.
            if (rg::timing::logicFrame(rdram, ctx) == rg::timing::LogicFrame::AsIs)
            {
                g_systemVmUpdate(rdram, ctx, runtime);
            }
            else
            {
                wholeStep(g_systemVmUpdate, rdram, ctx, runtime);
            }
            return;
        }
    }
    logicStep(g_systemVmUpdate, rdram, ctx, runtime);
}

void mesMgrUpdateAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    logicStep(g_mesMgrUpdateAll, rdram, ctx, runtime);
}

PS2Runtime::RecompiledFunction g_menuOriginals[std::size(kMenuTaskSteps)] = {};

template <size_t Index>
void menuTaskStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    logicStep(g_menuOriginals[Index], rdram, ctx, runtime);
}

template <size_t... Index>
bool installMenuHooks(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    bool complete = true;
    ((complete &= rg::timing::hookFunction(runtime, kMenuTaskSteps[Index], &menuTaskStep<Index>,
                                           g_menuOriginals[Index])),
     ...);
    return complete;
}

} // namespace

void trace()
{
    if (!g_trace)
    {
        return;
    }
    std::printf("[steps] NPC updates %u, turns %u, judge steps %u, gravity %u (held %u), launches %u, terminal "
                "clamps %u, air moves %u\n",
                g_counts.npc, g_counts.turns, g_counts.judge, g_counts.gravity, g_counts.held, g_counts.launches,
                g_counts.clamps, g_counts.air);
    g_counts = Counts{};
}

void install(PS2Runtime &runtime)
{
    if (!rg::timing::sixtyFps() || !rg::timing::stepFixes())
    {
        return;
    }
    g_trace = rg::timing::traceEnabled();
    using rg::timing::hookFunction;
    using rg::timing::stepFix;
    using rg::timing::StepFix;
    bool complete = true;
    if (stepFix(StepFix::Npc))
    {
        complete &= hookFunction(runtime, kTaskStepNpc, &taskStepNpc, g_taskStepNpc);
        complete &= hookFunction(runtime, kAngleStepToward, &angleStepToward, g_angleStepToward);
    }
    if (stepFix(StepFix::Judge))
    {
        complete &= hookFunction(runtime, kJudgeObjectsStep, &judgeObjectsStep, g_judgeObjectsStep);
    }
    if (stepFix(StepFix::Gravity))
    {
        complete &= hookFunction(runtime, kGetTimeStep, &getTimeStep, g_getTimeStep);
        complete &= hookFunction(runtime, kGetScaledGravity, &getScaledGravity, g_getScaledGravity);
        complete &= hookFunction(runtime, kGetThrowGravity, &getThrowGravity, g_getThrowGravity);
        complete &= hookFunction(runtime, kCharaMoveCollide, &charaMoveCollide, g_charaMoveCollide);
    }
    if (stepFix(StepFix::Event))
    {
        complete &= hookFunction(runtime, kSystemVmUpdate, &systemVmUpdate, g_systemVmUpdate);
        complete &= hookFunction(runtime, kMesMgrUpdateAll, &mesMgrUpdateAll, g_mesMgrUpdateAll);
    }
    if (stepFix(StepFix::Menu))
    {
        complete &= installMenuHooks(runtime, std::make_index_sequence<std::size(kMenuTaskSteps)>{});
    }
    if (!complete)
    {
        std::fprintf(stderr, "[rg] 60 fps: some step hooks are missing, see above\n");
    }
}

} // namespace rg::steps
