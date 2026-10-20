#include "jump_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scene_timing.h"

namespace rg::debug
{
namespace
{

// The character object (pos_trace.cpp has the rest): the position at
// +0x70, the velocity at +0x80 (scaled by the step when the move runs), the
// motion's root displacement at +0xE0 (CharaMoveCollideMaybe adds it to the
// position at 0x235020), the 64-bit state flags at +0x3D0 (bit 46 skips
// the gravity, bits 37 and 39 make it zero: 0x234F6C..0x234FA4), the id at
// +0x424, the action mode at +0xA2E, and the move motion's id and phase at
// +0xA34/+0xA36 (CharaActMoveMotion, 0x21B654..0x21B670).
constexpr uint32_t kCharaPos = 0x70u;
constexpr uint32_t kCharaVelocity = 0x80u;
constexpr uint32_t kCharaMotionDelta = 0xE0u;
constexpr uint32_t kCharaFlags = 0x3D0u;
constexpr uint32_t kCharaId = 0x424u;
constexpr uint32_t kCharaMode = 0xA2Eu;
constexpr uint32_t kCharaMoveId = 0xA34u;
constexpr uint32_t kCharaMovePhase = 0xA36u;
constexpr uint64_t kFlagNoGravity = 1ull << 46;
constexpr uint64_t kFlagZeroGravityA = 1ull << 37;
constexpr uint64_t kFlagZeroGravityB = 1ull << 39;
constexpr uint32_t kCharaMoveCollide = 0x00234DC0u;
// For enemies (RG_JUMP_TRACE=3), what else moves a character: the per-frame
// accelerations at +0xA0 (CharaAddActAccel), +0xB0 (CharaAddActMove) and
// +0xD0 (CharaAddReqMove), and the move request at +0x930: a displacement
// (+0x930), its length in frames (+0x940), its age (+0x944) and its kind
// (+0x948; 2 eases out, a jump's), which CharaUpdateMovement ages and turns
// into +0xD0 once a frame (0x20AF00..0x20AF38). CharaMoveReqSet (0x219310)
// starts one: CharaJumpToTarget, CharaLaunchTowardTarget,
// CharaMoveTowardTarget, CharaCmdReturnToHomePos, BattleApplyHit.
constexpr uint32_t kCharaActAccel = 0xA0u;
constexpr uint32_t kCharaActMove = 0xB0u;
constexpr uint32_t kCharaReqMove = 0xD0u;
constexpr uint32_t kCharaMoveReq = 0x930u;
constexpr uint32_t kMoveReqFrames = 0x10u;
constexpr uint32_t kMoveReqAge = 0x14u;
constexpr uint32_t kMoveReqKind = 0x18u;
constexpr uint32_t kCharaMoveReqSet = 0x00219310u;
// Party members have small ids; enemies 128 and up.
constexpr int16_t kFirstEnemyId = 100;
// GameData (0x4B8B30) +0x18 is the party; the leader's character id is the
// halfword at +0x18E of it (PlayerControlUpdateMaybe, 0x1FC578).
constexpr uint32_t kGameData = 0x004B8B30u;
constexpr uint32_t kParty = 0x18u;
constexpr uint32_t kLeaderId = 0x18Eu;
// Lines after the character has come to rest, so the landing shows.
constexpr unsigned kTailMoves = 4u;
constexpr unsigned kMaxLines = 20000u;
// RG_JUMP_TRACE=3: a whole fight's characters, enemies' leaps included.
constexpr unsigned kMaxLinesAll = 300000u;

PS2Runtime::RecompiledFunction g_moveCollide = nullptr;
PS2Runtime::RecompiledFunction g_moveReqSet = nullptr;
std::chrono::steady_clock::time_point g_start;
bool g_everyMove = false; // RG_JUMP_TRACE=2: on the ground as well (speed ramps)
bool g_allCharas = false; // RG_JUMP_TRACE=3: every character, not only the leader
unsigned g_lines = 0u;
uint32_t g_moves = 0u;

// The landing tail, per character (one, the leader's, unless
// RG_JUMP_TRACE=3); a character past the table's end is traced without one.
struct Tail
{
    uint32_t chara = 0u;
    unsigned moves = 0u;
};
Tail g_tails[32];

unsigned &tailOf(uint32_t chara)
{
    static unsigned spare = 0u;
    for (Tail &tail : g_tails)
    {
        if (tail.chara == chara || tail.chara == 0u)
        {
            tail.chara = chara;
            return tail.moves;
        }
    }
    spare = 0u;
    return spare;
}

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

bool readVec(const uint8_t *rdram, uint32_t address, float out[3])
{
    return readGuest(rdram, address, out[0]) && readGuest(rdram, address + 4u, out[1]) &&
           readGuest(rdram, address + 8u, out[2]);
}

bool isLeader(const uint8_t *rdram, uint32_t chara)
{
    int16_t id = 0;
    int16_t leader = 0;
    uint32_t party = 0u;
    return readGuest(rdram, chara + kCharaId, id) && readGuest(rdram, kGameData + kParty, party) && party != 0u &&
           readGuest(rdram, party + kLeaderId, leader) && id == leader;
}

void report(const uint8_t *rdram, const R5900Context *ctx, uint32_t chara)
{
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float delta[3] = {0.0f, 0.0f, 0.0f};
    uint64_t flags = 0u;
    int16_t mode = 0;
    int16_t moveId = 0;
    int16_t movePhase = 0;
    if (!readVec(rdram, chara + kCharaPos, pos) || !readVec(rdram, chara + kCharaVelocity, velocity) ||
        !readVec(rdram, chara + kCharaMotionDelta, delta) || !readGuest(rdram, chara + kCharaFlags, flags) ||
        !readGuest(rdram, chara + kCharaMode, mode) || !readGuest(rdram, chara + kCharaMoveId, moveId) ||
        !readGuest(rdram, chara + kCharaMovePhase, movePhase))
    {
        return;
    }
    const bool active = g_everyMove || velocity[1] != 0.0f || delta[1] != 0.0f || (flags & kFlagNoGravity) != 0u ||
                        movePhase != 0;
    unsigned &tail = tailOf(chara);
    if (active)
    {
        tail = kTailMoves;
    }
    else if (tail != 0u)
    {
        --tail;
    }
    else
    {
        return;
    }
    if (++g_lines > (g_allCharas ? kMaxLinesAll : kMaxLines))
    {
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char who[48] = "";
    if (g_allCharas)
    {
        int16_t id = 0;
        readGuest(rdram, chara + kCharaId, id);
        std::snprintf(who, sizeof(who), " chara 0x%08X id %d", chara, id);
    }
    std::printf("[jump] %9.3f s move %u%s: y %8.3f vy %7.3f dy %7.3f xz (%.2f, %.2f) dxz (%.3f, %.3f) vxz (%.3f, %.3f) "
                "step %.2f f23 %.3f mode %d move %d/%d%s%s%s\n",
                seconds, g_moves, who, pos[1], velocity[1], delta[1], pos[0], pos[2], delta[0], delta[2], velocity[0],
                velocity[2], rg::timing::sceneStep(rdram, ctx), ctx->f[23], mode, moveId, movePhase,
                (flags & kFlagNoGravity) ? " no-gravity" : "",
                (flags & kFlagZeroGravityA) ? " zero-gravity-a" : "", (flags & kFlagZeroGravityB) ? " zero-gravity-b" : "");
}

double secondsNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

bool isEnemy(const uint8_t *rdram, uint32_t chara)
{
    int16_t id = 0;
    return readGuest(rdram, chara + kCharaId, id) && id >= kFirstEnemyId;
}

// Where each enemy was when its last move ended, to tell the moves this
// hook makes from the ones something else made between two of them.
struct Last
{
    uint32_t chara = 0u;
    float pos[3] = {0.0f, 0.0f, 0.0f};
};
Last g_last[32];

Last *lastOf(uint32_t chara)
{
    for (Last &last : g_last)
    {
        if (last.chara == chara || last.chara == 0u)
        {
            last.chara = chara;
            return &last;
        }
    }
    return nullptr;
}

bool nonZero(const float v[3])
{
    return v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f;
}

// RG_JUMP_TRACE=3, an enemy: every move that changes its position, or that
// something else changed since the last one, with whatever drives it.
void enemyMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t chara)
{
    float before[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float accel[3] = {0.0f, 0.0f, 0.0f};
    float actMove[3] = {0.0f, 0.0f, 0.0f};
    float reqMove[3] = {0.0f, 0.0f, 0.0f};
    float delta[3] = {0.0f, 0.0f, 0.0f};
    float request[3] = {0.0f, 0.0f, 0.0f};
    int32_t frames = 0, age = 0, kind = 0;
    uint64_t flags = 0u;
    int16_t id = 0, mode = 0, moveId = 0, movePhase = 0;
    const bool read = readVec(rdram, chara + kCharaPos, before) && readVec(rdram, chara + kCharaVelocity, velocity) &&
                      readVec(rdram, chara + kCharaActAccel, accel) && readVec(rdram, chara + kCharaActMove, actMove) &&
                      readVec(rdram, chara + kCharaReqMove, reqMove) &&
                      readVec(rdram, chara + kCharaMotionDelta, delta) &&
                      readVec(rdram, chara + kCharaMoveReq, request) &&
                      readGuest(rdram, chara + kCharaMoveReq + kMoveReqFrames, frames) &&
                      readGuest(rdram, chara + kCharaMoveReq + kMoveReqAge, age) &&
                      readGuest(rdram, chara + kCharaMoveReq + kMoveReqKind, kind) &&
                      readGuest(rdram, chara + kCharaFlags, flags) && readGuest(rdram, chara + kCharaId, id) &&
                      readGuest(rdram, chara + kCharaMode, mode) && readGuest(rdram, chara + kCharaMoveId, moveId) &&
                      readGuest(rdram, chara + kCharaMovePhase, movePhase);
    const float step = rg::timing::sceneStep(rdram, ctx);
    g_moveCollide(rdram, ctx, runtime);
    float after[3] = {0.0f, 0.0f, 0.0f};
    Last *last = lastOf(chara);
    if (!read || !readVec(rdram, chara + kCharaPos, after) || !last)
    {
        return;
    }
    const float outside[3] = {before[0] - last->pos[0], before[1] - last->pos[1], before[2] - last->pos[2]};
    const float inside[3] = {after[0] - before[0], after[1] - before[1], after[2] - before[2]};
    std::memcpy(last->pos, after, sizeof(after));
    if (!nonZero(outside) && !nonZero(inside) && !nonZero(velocity) && !nonZero(reqMove) && !nonZero(actMove) &&
        frames == 0)
    {
        return;
    }
    if (++g_lines > kMaxLinesAll)
    {
        return;
    }
    std::printf("[enemy] %9.3f s id %d chara 0x%08X: pos (%.2f, %.2f, %.2f) moved here (%.3f, %.3f, %.3f) "
                "elsewhere (%.3f, %.3f, %.3f) vel (%.3f, %.3f, %.3f) req (%.3f, %.3f, %.3f) act (%.3f, %.3f, %.3f) "
                "accel (%.3f, %.3f, %.3f) motion (%.3f, %.3f, %.3f) request (%.1f, %.1f, %.1f) %d frames age %d kind %d "
                "step %.2f mode %d move %d/%d flags 0x%016llX\n",
                secondsNow(), id, chara, after[0], after[1], after[2], inside[0], inside[1], inside[2], outside[0],
                outside[1], outside[2], velocity[0], velocity[1], velocity[2], reqMove[0], reqMove[1], reqMove[2],
                actMove[0], actMove[1], actMove[2], accel[0], accel[1], accel[2], delta[0], delta[1], delta[2],
                request[0], request[1], request[2], frames, age, kind, step, mode, moveId, movePhase,
                static_cast<unsigned long long>(flags));
}

void moveCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t chara = getRegU32(ctx, 4);
    if (g_allCharas && isEnemy(rdram, chara))
    {
        enemyMove(rdram, ctx, runtime, chara);
        return;
    }
    if (g_allCharas || isLeader(rdram, chara))
    {
        ++g_moves;
        report(rdram, ctx, chara);
    }
    g_moveCollide(rdram, ctx, runtime);
}

// CharaMoveReqSet(request, displacement, frames, kind), for an enemy.
void moveReqSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t chara = getRegU32(ctx, 4) - kCharaMoveReq;
    if (isEnemy(rdram, chara) && ++g_lines <= kMaxLinesAll)
    {
        float displacement[3] = {0.0f, 0.0f, 0.0f};
        readVec(rdram, getRegU32(ctx, 5), displacement);
        int16_t id = 0;
        readGuest(rdram, chara + kCharaId, id);
        std::printf("[enemy] %9.3f s id %d chara 0x%08X: move request (%.2f, %.2f, %.2f) over %d frames, kind %d, "
                    "from 0x%08X, step %.2f\n",
                    secondsNow(), id, chara, displacement[0], displacement[1], displacement[2],
                    static_cast<int32_t>(getRegU32(ctx, 6)), static_cast<int32_t>(getRegU32(ctx, 7)),
                    getRegU32(ctx, 31), rg::timing::sceneStep(rdram, ctx));
    }
    g_moveReqSet(rdram, ctx, runtime);
}

} // namespace

void installJumpTrace(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_JUMP_TRACE");
    if (!enabled || !*enabled || *enabled == '0')
    {
        return;
    }
    g_start = std::chrono::steady_clock::now();
    g_everyMove = *enabled == '2';
    g_allCharas = *enabled == '3';
    bool complete = rg::timing::hookFunction(runtime, kCharaMoveCollide, &moveCollide, g_moveCollide);
    if (g_allCharas)
    {
        complete = rg::timing::hookFunction(runtime, kCharaMoveReqSet, &moveReqSet, g_moveReqSet) && complete;
    }
    std::printf("[rg] jump trace: %s moves off the ground go to the log%s\n",
                g_allCharas ? "every character's" : "the leader's", complete ? "" : " (the hook is missing, see above)");
}

} // namespace rg::debug
