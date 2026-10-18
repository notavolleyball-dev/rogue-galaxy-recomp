#include "step_fixes.h"

#include <cstdio>
#include <cstring>

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
constexpr uint32_t kVelocityY = 0x84u;
constexpr float kTerminalVelocity = 10.0f;

PS2Runtime::RecompiledFunction g_taskStepNpc = nullptr;
PS2Runtime::RecompiledFunction g_angleStepToward = nullptr;
PS2Runtime::RecompiledFunction g_judgeObjectsStep = nullptr;
PS2Runtime::RecompiledFunction g_getTimeStep = nullptr;
PS2Runtime::RecompiledFunction g_getScaledGravity = nullptr;
PS2Runtime::RecompiledFunction g_getThrowGravity = nullptr;
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

void charaMoveCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // a0: the character's movement state; its velocity is at +0x80, still
    // scaled by the step when the function returns.
    rg::hooks::callThen(g_charaMoveCollide, rdram, ctx, runtime, &clampTerminal, getRegU32(ctx, 4));
}

} // namespace

void trace()
{
    if (!g_trace)
    {
        return;
    }
    std::printf("[steps] NPC updates %u, turns %u, judge steps %u, gravity %u (held %u), launches %u, terminal "
                "clamps %u\n",
                g_counts.npc, g_counts.turns, g_counts.judge, g_counts.gravity, g_counts.held, g_counts.launches,
                g_counts.clamps);
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
    if (!complete)
    {
        std::fprintf(stderr, "[rg] 60 fps: some step hooks are missing, see above\n");
    }
}

} // namespace rg::steps
