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
// GameData (0x4B8B30) +0x18 is the party; the leader's character id is the
// halfword at +0x18E of it (PlayerControlUpdateMaybe, 0x1FC578).
constexpr uint32_t kGameData = 0x004B8B30u;
constexpr uint32_t kParty = 0x18u;
constexpr uint32_t kLeaderId = 0x18Eu;
// Lines after the character has come to rest, so the landing shows.
constexpr unsigned kTailMoves = 4u;
constexpr unsigned kMaxLines = 20000u;

PS2Runtime::RecompiledFunction g_moveCollide = nullptr;
std::chrono::steady_clock::time_point g_start;
unsigned g_tail = 0u;
bool g_everyMove = false; // RG_JUMP_TRACE=2: on the ground as well (speed ramps)
unsigned g_lines = 0u;
uint32_t g_moves = 0u;

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
    if (active)
    {
        g_tail = kTailMoves;
    }
    else if (g_tail != 0u)
    {
        --g_tail;
    }
    else
    {
        return;
    }
    if (++g_lines > kMaxLines)
    {
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::printf("[jump] %9.3f s move %u: y %8.3f vy %7.3f dy %7.3f xz (%.2f, %.2f) dxz (%.3f, %.3f) vxz (%.3f, %.3f) "
                "step %.2f f23 %.3f mode %d move %d/%d%s%s%s\n",
                seconds, g_moves, pos[1], velocity[1], delta[1], pos[0], pos[2], delta[0], delta[2], velocity[0],
                velocity[2], rg::timing::sceneStep(rdram, ctx), ctx->f[23], mode, moveId, movePhase,
                (flags & kFlagNoGravity) ? " no-gravity" : "",
                (flags & kFlagZeroGravityA) ? " zero-gravity-a" : "", (flags & kFlagZeroGravityB) ? " zero-gravity-b" : "");
}

void moveCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t chara = getRegU32(ctx, 4);
    if (isLeader(rdram, chara))
    {
        ++g_moves;
        report(rdram, ctx, chara);
    }
    g_moveCollide(rdram, ctx, runtime);
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
    const bool complete = rg::timing::hookFunction(runtime, kCharaMoveCollide, &moveCollide, g_moveCollide);
    std::printf("[rg] jump trace: the leader's moves off the ground go to the log%s\n",
                complete ? "" : " (the hook is missing, see above)");
}

} // namespace rg::debug
