#include "step_fixes.h"

#include <algorithm>
#include <cmath>
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
// The characters' AI. AiUpdate (0x2C05B0) runs once a frame for each
// character CharaAiHandlePrmtRequestsMaybe thinks for (a0 the AI block at
// chara+0x590, a1 a stack slot holding the character; its result unused):
// it counts its timers in frames (+0x146, +0x174 of the block), adds moves
// of its own (CharaAddReqMove) and resumes the AI script, whose commands
// move the character by a step a call: AiCmdCharaSetPosById (0x2C9EE0) to
// the position plus a step, AiCmdMoveByLocalOffset (0x2C90F0) as a
// requested move (+0xD0), which CharaMoveCollideMaybe applies whole and
// clears (0x234EAC, 0x236448). At 60 fps it ran twice as often while what
// ends such a move (a motion, a timer that takes the step) took as long:
// the gate fight's boss charged 54 steps of 29.8 units instead of 27, left
// its arena and hung in the air (2026-10-20). It runs on the logic frames
// as a whole step, like the scripts; on 60 fps frames the move it made, by
// position or by requested move, is shared half and half with the next
// frame, so the character moves at the console's speed without stepping at
// 30 Hz. Its walk and run are inputs held for the frame: its caller clears
// the character's flags (+0x568) before it, and AiMoveWalkOrRunToTarget
// and the like add 1 (walk), 2 (run), 0x10000 and the ladder's 0x80020 and
// 0x80040, which the character's own update reads after. A frame without
// the AI's run would leave them clear and the party following its leader
// walked in little steps and stops (2026-10-21): on those frames the inputs
// the AI held last are held again, as the console held them for its 30th
// of a second.
constexpr uint32_t kAiUpdate = 0x002C05B0u;
constexpr uint32_t kCharaFlags = 0x568u;
// The low word: what the AI holds. The high one is the hit and action
// state others set (BattleApplyHit, ActionMgrRunActions).
constexpr uint64_t kAiHeldFlags = 0xFFFFFFFFull;
constexpr uint32_t kAiBlock = 0x590u;
constexpr uint32_t kCharaPosition = 0x70u;
constexpr uint32_t kCharaRequestedMove = 0xD0u;
// A larger change is a placement, made at once.
constexpr float kMaxSharedMove = 100.0f;
// Not in the list: TaskStepLiveTalk (0x321210), which besides the party's
// remarks sequences the skill actions (SkillActionFindPending,
// SkillTimerStartMaybe, SkillActionEnd, an immediate script) in step with
// the battle; on logic frames a skill froze the game (2026-10-19).
// Texture animations. TexAnimSeqUpdate (0x163DD0; a0 the sequence, f12 the
// step) advances the layers of a texture's animation by the step: a
// flip-book's frame time, and a scrolling layer's position on each axis
// (+0x20 and +0x24 of the layer, wrapped at its period in frames, +0x1C and
// +0x1E, whose sign is the direction). TexAnimeTableApply (0x162F50) draws
// the layer shifted by position / period of its size (type 1), or by a sine
// of 2 pi x position / period (type 2). The map's textures get the scene's
// step (MapTickAnims, MapMgrAuxAreaTexAnimUpdate) and the models their
// update's, but most of the rest a constant 1.0 a draw (DrawListAddTexLoad
// from the effects, the HUD, the chests; EffTexResStepAnim): those ran at
// twice the speed, and take the 60 fps share of a frame. A period of one
// frame is how the data keeps an axis still (the shift divides by the
// period): at whole steps the position wraps to 0 at every update, at half
// steps it was 0.5 every other frame and the layer jumped by half its size
// and back, which redrew the Monography Shot's platform as tiles instead of
// circles (2026-10-21). A sine over two frames is as still at whole steps
// (sin 0, sin pi). Those positions are put back where whole steps leave them.
constexpr uint32_t kTexAnimSeqUpdate = 0x00163DD0u;
constexpr uint32_t kSeqColumnCount = 0x0u;
constexpr uint32_t kSeqActive = 0x4u;
constexpr uint32_t kSeqHead = 0xC4u;
constexpr uint32_t kSeqColumns = 48u;
// In a node, after the link to the next.
constexpr uint32_t kLayer = 0x10u;
constexpr uint32_t kLayerPeriod = 0x1Cu;
constexpr uint32_t kLayerPosition = 0x20u;

PS2Runtime::RecompiledFunction g_taskStepNpc = nullptr;
PS2Runtime::RecompiledFunction g_angleStepToward = nullptr;
PS2Runtime::RecompiledFunction g_judgeObjectsStep = nullptr;
PS2Runtime::RecompiledFunction g_getTimeStep = nullptr;
PS2Runtime::RecompiledFunction g_getScaledGravity = nullptr;
PS2Runtime::RecompiledFunction g_getThrowGravity = nullptr;
PS2Runtime::RecompiledFunction g_systemVmUpdate = nullptr;
PS2Runtime::RecompiledFunction g_mesMgrUpdateAll = nullptr;
PS2Runtime::RecompiledFunction g_charaMoveCollide = nullptr;
PS2Runtime::RecompiledFunction g_aiUpdate = nullptr;
PS2Runtime::RecompiledFunction g_texAnimSeqUpdate = nullptr;
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
    uint32_t aiShared = 0u;
    uint32_t texSteps = 0u;
    uint32_t stillLayers = 0u;
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

// --- The characters' AI ---------------------------------------------------

// A character's AI move held over to the next frame, and what its last AI
// run started from.
struct AiShare
{
    uint32_t chara = 0u;
    bool pending = false;
    bool share = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float requested[3] = {0.0f, 0.0f, 0.0f};
    float positionBefore[3] = {0.0f, 0.0f, 0.0f};
    float requestedBefore[3] = {0.0f, 0.0f, 0.0f};
    // The character's flags before the AI's run, and the inputs it added.
    uint64_t flagsBefore = 0u;
    uint64_t heldFlags = 0u;
};
AiShare g_aiShares[64];
// The scene's scale to restore after an AI run at 1.0 (one at a time, on
// the game's thread).
uint32_t g_aiScaleBits = 0u;

// Characters come and go with the battles; a full table frees the slots of
// the ones that have nothing held over.
AiShare *aiShareOf(uint32_t chara)
{
    for (AiShare &share : g_aiShares)
    {
        if (share.chara == chara)
        {
            return &share;
        }
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (AiShare &share : g_aiShares)
        {
            if (share.chara == 0u || (pass == 1 && !share.pending && share.heldFlags == 0u))
            {
                share = AiShare{};
                share.chara = chara;
                return &share;
            }
        }
    }
    return nullptr;
}

bool readVec3(const uint8_t *rdram, uint32_t address, float out[3])
{
    return readGuest(rdram, address, out[0]) && readGuest(rdram, address + 4u, out[1]) &&
           readGuest(rdram, address + 8u, out[2]);
}

void writeVec3(uint8_t *rdram, uint32_t address, const float value[3])
{
    for (unsigned i = 0u; i < 3u; ++i)
    {
        writeGuest(rdram, address + 4u * i, value[i]);
    }
}

// Adds the held-over half of the last AI move, before the AI runs again.
void applyAiShare(uint8_t *rdram, AiShare &share)
{
    if (!share.pending)
    {
        return;
    }
    share.pending = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float requested[3] = {0.0f, 0.0f, 0.0f};
    if (!readVec3(rdram, share.chara + kCharaPosition, position) ||
        !readVec3(rdram, share.chara + kCharaRequestedMove, requested))
    {
        return;
    }
    for (unsigned i = 0u; i < 3u; ++i)
    {
        position[i] += share.position[i];
        requested[i] += share.requested[i];
    }
    writeVec3(rdram, share.chara + kCharaPosition, position);
    writeVec3(rdram, share.chara + kCharaRequestedMove, requested);
}

// Half of `after - before` into `held`, and before + the other half back;
// false when nothing moved or it moved too far to be a step.
bool halve(float before[3], const float after[3], float held[3])
{
    float length = 0.0f;
    for (unsigned i = 0u; i < 3u; ++i)
    {
        held[i] = 0.5f * (after[i] - before[i]);
        length += held[i] * held[i];
    }
    if (length == 0.0f || length > 0.25f * kMaxSharedMove * kMaxSharedMove)
    {
        held[0] = held[1] = held[2] = 0.0f;
        return false;
    }
    for (unsigned i = 0u; i < 3u; ++i)
    {
        before[i] += held[i];
    }
    return true;
}

AiShare *findAiShare(uint32_t chara)
{
    for (AiShare &share : g_aiShares)
    {
        if (share.chara == chara)
        {
            return &share;
        }
    }
    return nullptr;
}

void afterAiUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t chara)
{
    restoreScale(rdram, ctx, runtime, g_aiScaleBits);
    AiShare *share = findAiShare(chara);
    if (!share)
    {
        return;
    }
    uint64_t flags = 0u;
    share->heldFlags =
        readGuest(rdram, chara + kCharaFlags, flags) ? (flags & ~share->flagsBefore) & kAiHeldFlags : 0u;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float requested[3] = {0.0f, 0.0f, 0.0f};
    if (!share->share || !readVec3(rdram, chara + kCharaPosition, position) ||
        !readVec3(rdram, chara + kCharaRequestedMove, requested))
    {
        return;
    }
    // What the AI moved the character by stays half done this frame.
    const bool moved = halve(share->positionBefore, position, share->position);
    const bool requestedMove = halve(share->requestedBefore, requested, share->requested);
    if (moved)
    {
        writeVec3(rdram, chara + kCharaPosition, share->positionBefore);
    }
    if (requestedMove)
    {
        writeVec3(rdram, chara + kCharaRequestedMove, share->requestedBefore);
    }
    share->pending = moved || requestedMove;
    if (share->pending)
    {
        ++g_counts.aiShared;
    }
}

void aiUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t chara = getRegU32(ctx, 4) - kAiBlock;
    uint32_t slot = 0u;
    AiShare *share =
        readGuest(rdram, getRegU32(ctx, 5), slot) && slot == chara ? aiShareOf(chara) : nullptr;
    if (share)
    {
        applyAiShare(rdram, *share);
    }
    switch (rg::timing::logicFrame(rdram, ctx))
    {
    case rg::timing::LogicFrame::AsIs:
        if (share)
        {
            share->heldFlags = 0u;
        }
        g_aiUpdate(rdram, ctx, runtime);
        return;
    case rg::timing::LogicFrame::Skip:
    {
        uint64_t flags = 0u;
        if (share && share->heldFlags != 0u && readGuest(rdram, chara + kCharaFlags, flags))
        {
            writeGuest(rdram, chara + kCharaFlags, flags | share->heldFlags);
        }
        // Its result goes unused; the host stands in for `jr ra`.
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    case rg::timing::LogicFrame::WholeStep:
        break;
    }
    if (!share)
    {
        wholeStep(g_aiUpdate, rdram, ctx, runtime);
        return;
    }
    if (!readGuest(rdram, chara + kCharaFlags, share->flagsBefore))
    {
        share->flagsBefore = 0u;
    }
    const float scale = rg::timing::sceneScale(rdram, ctx);
    std::memcpy(&g_aiScaleBits, &scale, sizeof(scale));
    // At a whole step a frame there is no next frame to share with.
    share->share = scale < 1.0f && readVec3(rdram, chara + kCharaPosition, share->positionBefore) &&
                   readVec3(rdram, chara + kCharaRequestedMove, share->requestedBefore);
    rg::timing::setSceneScale(rdram, ctx, 1.0f);
    rg::hooks::callThen(g_aiUpdate, rdram, ctx, runtime, &afterAiUpdate, chara);
}

// --- Texture animations ---------------------------------------------------

void holdStillLayers(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t sequence)
{
    uint32_t columns = 0u;
    if (!readGuest(rdram, sequence + kSeqColumnCount, columns))
    {
        return;
    }
    columns = std::min(columns, kSeqColumns);
    for (uint32_t i = 0u; i < columns; ++i)
    {
        uint32_t active = 0u;
        uint32_t node = 0u;
        if (!readGuest(rdram, sequence + kSeqActive + 4u * i, active) || active == 0u ||
            !readGuest(rdram, sequence + kSeqHead + 4u * i, node))
        {
            continue;
        }
        // The chain ends with a null; the bound only guards against garbage.
        for (unsigned n = 0u; node != 0u && n < 256u; ++n)
        {
            const uint32_t layer = node + kLayer;
            int8_t type = 0;
            if (readGuest(rdram, layer, type) && (type == 1 || type == 2))
            {
                for (uint32_t axis = 0u; axis < 2u; ++axis)
                {
                    int16_t period = 0;
                    if (!readGuest(rdram, layer + kLayerPeriod + 2u * axis, period))
                    {
                        continue;
                    }
                    const int frames = period < 0 ? -period : period;
                    if (frames == 1 || (type == 2 && frames == 2))
                    {
                        // Wrapped to 0 forwards, to the period backwards.
                        writeGuest(rdram, layer + kLayerPosition + 4u * axis,
                                   period > 0 ? 0.0f : static_cast<float>(frames));
                        ++g_counts.stillLayers;
                    }
                }
            }
            if (!readGuest(rdram, node, node))
            {
                break;
            }
        }
    }
}

void texAnimSeqUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The game's own scale (its 60 Hz menus, the movies): the console's
    // frames.
    if (rg::timing::logicFrame(rdram, ctx) == rg::timing::LogicFrame::AsIs)
    {
        g_texAnimSeqUpdate(rdram, ctx, runtime);
        return;
    }
    if (ctx->f[12] == 1.0f)
    {
        // The frame's share, not the scene's step: on the console a slowed
        // scene does not slow these.
        ctx->f[12] = rg::timing::sceneScale(rdram, ctx);
        ++g_counts.texSteps;
    }
    const float step = ctx->f[12];
    if (step == std::floor(step))
    {
        g_texAnimSeqUpdate(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_texAnimSeqUpdate, rdram, ctx, runtime, &holdStillLayers, getRegU32(ctx, 4));
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
                "clamps %u, air moves %u, AI moves shared %u, texture steps %u, still layers %u\n",
                g_counts.npc, g_counts.turns, g_counts.judge, g_counts.gravity, g_counts.held, g_counts.launches,
                g_counts.clamps, g_counts.air, g_counts.aiShared, g_counts.texSteps, g_counts.stillLayers);
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
    if (stepFix(StepFix::Ai))
    {
        complete &= hookFunction(runtime, kAiUpdate, &aiUpdate, g_aiUpdate);
    }
    if (stepFix(StepFix::Texture))
    {
        complete &= hookFunction(runtime, kTexAnimSeqUpdate, &texAnimSeqUpdate, g_texAnimSeqUpdate);
    }
    if (!complete)
    {
        std::fprintf(stderr, "[rg] 60 fps: some step hooks are missing, see above\n");
    }
}

} // namespace rg::steps
