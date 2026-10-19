#include "pos_trace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "return_hook.h"
#include "scene_timing.h"

namespace rg::debug
{
namespace
{

// The character object: its SRT at +0x50 holds the position at +0x70 and
// the velocity at +0x80 (CharaGetPos jumps to SrtGetPos(chara + 0x50));
// CharaGetId reads a halfword at +0x424; the collision ignore mask is the
// halfword at +0x40E and the 64-bit state flags at +0x3D0.
constexpr uint32_t kCharaPos = 0x70u;
constexpr uint32_t kCharaVelocityY = 0x84u;
constexpr uint32_t kCharaFlags = 0x3D0u;
constexpr uint32_t kCharaIgnoreMask = 0x40Eu;
constexpr uint32_t kCharaId = 0x424u;
// CharaSetPos(chara, vec *), CharaSetPosXYZ(chara, f12, f13, f14) and
// CharaPlaceAt(id, vec *, f12 yaw, flags), the three ways the game moves a
// character somewhere it did not walk to.
constexpr uint32_t kCharaSetPos = 0x00213A40u;
constexpr uint32_t kCharaSetPosXYZ = 0x002139E0u;
constexpr uint32_t kCharaPlaceAt = 0x002F3A40u;
// CharaMoveCollideMaybe(chara, context), once a frame per moving
// character, with the velocity already scaled by the step. The context:
// +0x8 the capacity of the triangle list at +0xC (count +0x10), which
// MapEvCollectCollTrisNearMaybe fills with the event objects' and treasure
// boxes' polygons first and the map's triangles after; the move sorts it
// by |normal.y| into the lists at +0x14 (count +0x18), +0x1C (count +0x20,
// |ny| >= 0.5: the floor and ceiling candidates) and +0x24 (count +0x28,
// walls). Records are 0x60 bytes: three vec4 vertices, the plane at +0x30,
// attribute halfwords from +0x40 (+0x46 is the kind the ignore mask tests).
constexpr uint32_t kCharaMoveCollide = 0x00234DC0u;
constexpr uint32_t kContextCapacity = 0x8u;
constexpr uint32_t kContextTris = 0xCu;
constexpr uint32_t kContextTriCount = 0x10u;
constexpr uint32_t kContextFloorTris = 0x1Cu;
constexpr uint32_t kContextFloorCount = 0x20u;
constexpr uint32_t kContextListACount = 0x18u;
constexpr uint32_t kContextWallCount = 0x28u;
constexpr uint32_t kTriSize = 0x60u;
// CharaCollClampFloorCeilingMaybe, called by the move with the floor list:
// returns 1 when it put the character on a floor (or under a ceiling), 0
// when nothing was within reach. On the ground it is 1 every frame.
constexpr uint32_t kFloorClamp = 0x002364E0u;
// EventCollectCollPolys: the event objects' and treasure boxes' polygons,
// returns how many it wrote.
constexpr uint32_t kEventCollectPolys = 0x001FF3B0u;
// CollSweepCollectHits(sweep, hits, max, tris, count, mask): the swept
// sphere the floor clamp casts down (then up for the ceiling). The sweep
// holds its start at +0, its direction at +0x10, the radius at +0x20; a
// hit record is 0x20 bytes: the point, the triangle index at +0x10 and the
// parameter along the sweep at +0x14. Returns the hit count.
constexpr uint32_t kSweepCollectHits = 0x001C7550u;
// After the floor goes missing, this many more moves of that character are
// reported in full.
constexpr uint32_t kWatchFrames = 8u;
// Falling: the scaled vertical velocity below this (units are 1/20 m) for
// this many consecutive updates. A jump lasts about 30 frames at 30 fps.
constexpr float kFallVelocity = -1.5f;
constexpr uint32_t kFallFrames = 45u;
constexpr unsigned kMaxDumps = 12u;

PS2Runtime::RecompiledFunction g_setPos = nullptr;
PS2Runtime::RecompiledFunction g_setPosXYZ = nullptr;
PS2Runtime::RecompiledFunction g_placeAt = nullptr;
PS2Runtime::RecompiledFunction g_moveCollide = nullptr;
PS2Runtime::RecompiledFunction g_floorClamp = nullptr;
PS2Runtime::RecompiledFunction g_eventPolys = nullptr;
PS2Runtime::RecompiledFunction g_sweepHits = nullptr;

// What the floor clamp was asked this move, and what its sweeps found.
struct ClampArgs
{
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float height = 0.0f, snap = 0.0f, probe = 0.0f;
    uint32_t count = 0u, mask = 0u, floorOn = 0u, ceilingOn = 0u;
};
struct SweepRecord
{
    float fromY = 0.0f, dirY = 0.0f, radius = 0.0f;
    uint32_t count = 0u;
    struct Hit
    {
        float y, t;
        uint32_t tri;
    } hits[6] = {};
    uint32_t hitsAddress = 0u;
};
ClampArgs g_clampArgs;
SweepRecord g_sweeps[2];
unsigned g_sweepCount = 0u;

struct Fall
{
    uint32_t frames = 0u;
    bool reported = false;
    float from[3] = {0.0f, 0.0f, 0.0f};
};
std::unordered_map<uint32_t, Fall> g_falls;
struct Floor
{
    bool known = false;
    bool had = false;
    uint32_t watch = 0u; // moves still to report after the floor went
};
std::unordered_map<uint32_t, Floor> g_floor;
// Placements that leave the position as it was, by caller: counted, not
// listed (an object that carries characters re-applies theirs every frame).
std::unordered_map<uint32_t, uint32_t> g_identity;
// The move in progress on the game's thread (the move is not reentrant).
uint32_t g_currentChara = 0u;
uint32_t g_currentContext = 0u;
int32_t g_lastEventPolys = -1;
unsigned g_dumps = 0u;

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

int16_t charaId(const uint8_t *rdram, uint32_t chara)
{
    int16_t id = -1;
    readGuest(rdram, chara + kCharaId, id);
    return id;
}

void report(const char *what, const uint8_t *rdram, uint32_t chara, const float to[3], uint32_t returnPc)
{
    float from[3] = {0.0f, 0.0f, 0.0f};
    readVec(rdram, chara + kCharaPos, from);
    if (from[0] == to[0] && from[1] == to[1] && from[2] == to[2])
    {
        const uint32_t count = ++g_identity[returnPc];
        if (count == 1u || count % 600u == 0u)
        {
            std::printf("[pos] %s chara 0x%08X id %d kept at (%.1f, %.1f, %.1f) from 0x%08X (%u identity placements "
                        "from there so far)\n",
                        what, chara, charaId(rdram, chara), to[0], to[1], to[2], returnPc, count);
        }
        return;
    }
    std::printf("[pos] %s chara 0x%08X id %d (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) from 0x%08X\n", what, chara,
                charaId(rdram, chara), from[0], from[1], from[2], to[0], to[1], to[2], returnPc);
}

void setPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    float to[3] = {0.0f, 0.0f, 0.0f};
    readVec(rdram, getRegU32(ctx, 5), to);
    report("SetPos", rdram, getRegU32(ctx, 4), to, getRegU32(ctx, 31));
    g_setPos(rdram, ctx, runtime);
}

void setPosXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const float to[3] = {ctx->f[12], ctx->f[13], ctx->f[14]};
    report("SetPosXYZ", rdram, getRegU32(ctx, 4), to, getRegU32(ctx, 31));
    g_setPosXYZ(rdram, ctx, runtime);
}

void placeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    float to[3] = {0.0f, 0.0f, 0.0f};
    readVec(rdram, getRegU32(ctx, 5), to);
    std::printf("[pos] PlaceAt id %d (%.1f, %.1f, %.1f) yaw %.2f flags 0x%X from 0x%08X\n",
                static_cast<int>(getRegU32(ctx, 4)), to[0], to[1], to[2], ctx->f[12], getRegU32(ctx, 6),
                getRegU32(ctx, 31));
    g_placeAt(rdram, ctx, runtime);
}

// The character's state and its collision lists, when the floor goes
// missing under it: the counts to the log, the triangles to a file.
void dumpContext(const uint8_t *rdram, uint32_t chara, const char *why)
{
    float at[3] = {0.0f, 0.0f, 0.0f};
    readVec(rdram, chara + kCharaPos, at);
    int16_t mask = 0;
    uint64_t flags = 0u;
    readGuest(rdram, chara + kCharaIgnoreMask, mask);
    readGuest(rdram, chara + kCharaFlags, flags);
    uint32_t capacity = 0u, tris = 0u, count = 0u, floorTris = 0u, floorCount = 0u, listA = 0u, walls = 0u;
    const uint32_t context = g_currentContext;
    readGuest(rdram, context + kContextCapacity, capacity);
    readGuest(rdram, context + kContextTris, tris);
    readGuest(rdram, context + kContextTriCount, count);
    readGuest(rdram, context + kContextFloorTris, floorTris);
    readGuest(rdram, context + kContextFloorCount, floorCount);
    readGuest(rdram, context + kContextListACount, listA);
    readGuest(rdram, context + kContextWallCount, walls);
    std::printf("[pos] chara 0x%08X id %d %s at (%.1f, %.1f, %.1f): ignore mask 0x%04X flags 0x%016llX; collected "
                "%u of %u triangles (event/treasure polys %d), floor list %u, list A %u, walls %u\n",
                chara, charaId(rdram, chara), why, at[0], at[1], at[2], static_cast<uint16_t>(mask),
                static_cast<unsigned long long>(flags), count, capacity, g_lastEventPolys, floorCount, listA, walls);
    if (g_dumps >= kMaxDumps || context == 0u)
    {
        return;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "build/run/fall_%u.bin", g_dumps++);
    if (FILE *f = std::fopen(path, "wb"))
    {
        // Header: chara position, then the raw list and the floor list, each
        // as a count followed by its 0x60-byte records.
        std::fwrite(at, sizeof(float), 3, f);
        const uint32_t lists[2][2] = {{tris, count}, {floorTris, floorCount}};
        for (const auto &list : lists)
        {
            const uint32_t n = list[1] < 4096u ? list[1] : 0u;
            std::fwrite(&n, sizeof(n), 1, f);
            const uint32_t physical = list[0] & 0x1FFFFFFFu;
            if (n != 0u && physical + n * kTriSize <= PS2_RAM_SIZE)
            {
                std::fwrite(rdram + physical, kTriSize, n, f);
            }
        }
        std::fclose(f);
        std::printf("[pos]   triangle lists written to %s\n", path);
    }
    // The surfaces straight under (and over) the character in each list,
    // in the log too: a vertical line through its position against every
    // triangle.
    const struct
    {
        const char *name;
        uint32_t tris, count;
    } lists[2] = {{"raw", tris, count}, {"floor", floorTris, floorCount}};
    for (const auto &list : lists)
    {
        for (uint32_t i = 0; i < list.count && i < 4096u; ++i)
        {
            const uint32_t tri = list.tris + i * kTriSize;
            float v[3][3];
            float ny = 0.0f;
            int16_t kind = 0;
            if (!readVec(rdram, tri, v[0]) || !readVec(rdram, tri + 0x10u, v[1]) ||
                !readVec(rdram, tri + 0x20u, v[2]))
            {
                break;
            }
            const float det = (v[1][0] - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (v[1][2] - v[0][2]);
            if (std::fabs(det) < 1e-6f)
            {
                continue;
            }
            const float l1 = ((at[0] - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (at[2] - v[0][2])) / det;
            const float l2 = ((v[1][0] - v[0][0]) * (at[2] - v[0][2]) - (at[0] - v[0][0]) * (v[1][2] - v[0][2])) / det;
            if (l1 < -0.01f || l2 < -0.01f || l1 + l2 > 1.01f)
            {
                continue;
            }
            const float y = v[0][1] + l1 * (v[1][1] - v[0][1]) + l2 * (v[2][1] - v[0][1]);
            readGuest(rdram, tri + 0x34u, ny);
            readGuest(rdram, tri + 0x46u, kind);
            std::printf("[pos]   %s tri %u under the character: surface y %.2f, ny %.3f, kind 0x%04X: (%.1f, %.1f, "
                        "%.1f) (%.1f, %.1f, %.1f) (%.1f, %.1f, %.1f)\n",
                        list.name, i, y, ny, static_cast<uint16_t>(kind), v[0][0], v[0][1], v[0][2], v[1][0], v[1][1],
                        v[1][2], v[2][0], v[2][1], v[2][2]);
        }
    }
    std::fflush(stdout);
}

void printClamp(const char *when, uint32_t chara, const uint8_t *rdram, uint32_t result)
{
    const ClampArgs &a = g_clampArgs;
    float now[3] = {0.0f, 0.0f, 0.0f};
    readVec(rdram, chara + kCharaPos, now);
    std::printf("[pos]   %s: floor clamp at (%.2f, %.2f, %.2f) height %.2f snap %.2f probe %.2f, %u tris, mask "
                "0x%04X, floor %u ceiling %u -> %u, position now (%.2f, %.2f, %.2f)\n",
                when, a.pos[0], a.pos[1], a.pos[2], a.height, a.snap, a.probe, a.count, a.mask, a.floorOn, a.ceilingOn,
                result, now[0], now[1], now[2]);
    for (unsigned s = 0; s < g_sweepCount && s < 2u; ++s)
    {
        const SweepRecord &r = g_sweeps[s];
        std::printf("[pos]     sweep %u from y %.2f dir y %.2f radius %.2f: %u hits", s, r.fromY, r.dirY, r.radius,
                    r.count);
        for (uint32_t h = 0; h < r.count && h < 6u; ++h)
        {
            std::printf(" [tri %u y %.2f t %.3f]", r.hits[h].tri, r.hits[h].y, r.hits[h].t);
        }
        std::printf("\n");
    }
}

void afterSweepHits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    if (g_sweepCount == 0u || g_sweepCount > 2u)
    {
        return;
    }
    SweepRecord &r = g_sweeps[g_sweepCount - 1u];
    r.count = getRegU32(ctx, 2);
    for (uint32_t h = 0; h < r.count && h < 6u; ++h)
    {
        const uint32_t hit = r.hitsAddress + h * 0x20u;
        readGuest(rdram, hit + 0x4u, r.hits[h].y);
        readGuest(rdram, hit + 0x10u, r.hits[h].tri);
        readGuest(rdram, hit + 0x14u, r.hits[h].t);
    }
}

void sweepHits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_sweepCount < 2u)
    {
        SweepRecord &r = g_sweeps[g_sweepCount];
        r = SweepRecord{};
        const uint32_t sweep = getRegU32(ctx, 4);
        readGuest(rdram, sweep + 0x4u, r.fromY);
        readGuest(rdram, sweep + 0x14u, r.dirY);
        readGuest(rdram, sweep + 0x20u, r.radius);
        r.hitsAddress = getRegU32(ctx, 5);
    }
    ++g_sweepCount;
    rg::hooks::callThen(g_sweepHits, rdram, ctx, runtime, &afterSweepHits, 0u);
}

void afterFloorClamp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t chara)
{
    const uint32_t result = getRegU32(ctx, 2);
    const bool has = result != 0u;
    Floor &floor = g_floor[chara];
    if (floor.known && floor.had && !has)
    {
        dumpContext(rdram, chara, "lost the floor");
        printClamp("this move", chara, rdram, result);
        floor.watch = kWatchFrames;
    }
    else if (floor.watch != 0u)
    {
        --floor.watch;
        char when[48];
        std::snprintf(when, sizeof(when), "%u moves after losing it", kWatchFrames - floor.watch);
        printClamp(when, chara, rdram, result);
        if (!has)
        {
            dumpContext(rdram, chara, "still without a floor");
        }
        if (has)
        {
            floor.watch = 0u;
        }
    }
    floor.known = true;
    floor.had = has;
}

void floorClamp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    ClampArgs &a = g_clampArgs;
    readVec(rdram, getRegU32(ctx, 5), a.pos);
    a.height = ctx->f[12];
    a.snap = ctx->f[13];
    a.probe = ctx->f[14];
    a.count = getRegU32(ctx, 8);
    a.mask = getRegU32(ctx, 9) & 0xFFFFu;
    a.floorOn = getRegU32(ctx, 10);
    a.ceilingOn = getRegU32(ctx, 11);
    g_sweepCount = 0u;
    rg::hooks::callThen(g_floorClamp, rdram, ctx, runtime, &afterFloorClamp, g_currentChara);
}

void afterEventPolys(uint8_t *, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    g_lastEventPolys = static_cast<int32_t>(getRegU32(ctx, 2));
}

void eventPolys(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_eventPolys, rdram, ctx, runtime, &afterEventPolys, 0u);
}

void moveCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t chara = getRegU32(ctx, 4);
    g_currentChara = chara;
    g_currentContext = getRegU32(ctx, 5);
    // How full the collision list runs, now and then: a full one is a
    // floor about to go missing.
    static uint32_t moves = 0u;
    if (++moves % 1800u == 1u)
    {
        uint32_t capacity = 0u, count = 0u;
        readGuest(rdram, g_currentContext + kContextCapacity, capacity);
        readGuest(rdram, g_currentContext + kContextTriCount, count);
        std::printf("[pos] move %u: chara 0x%08X id %d collected %u of %u triangles (event/treasure polys %d)\n", moves,
                    chara, charaId(rdram, chara), count, capacity, g_lastEventPolys);
    }
    float velocityY = 0.0f;
    if (readGuest(rdram, chara + kCharaVelocityY, velocityY))
    {
        Fall &fall = g_falls[chara];
        if (velocityY < kFallVelocity)
        {
            if (fall.frames == 0u)
            {
                readVec(rdram, chara + kCharaPos, fall.from);
            }
            ++fall.frames;
            if (fall.frames == kFallFrames)
            {
                float at[3] = {0.0f, 0.0f, 0.0f};
                readVec(rdram, chara + kCharaPos, at);
                std::printf("[pos] chara 0x%08X id %d falling for %u updates: from (%.1f, %.1f, %.1f), now (%.1f, "
                            "%.1f, %.1f), step %.2f\n",
                            chara, charaId(rdram, chara), fall.frames, fall.from[0], fall.from[1], fall.from[2],
                            at[0], at[1], at[2], rg::timing::sceneStep(rdram, ctx));
                fall.reported = true;
            }
        }
        else if (fall.frames != 0u)
        {
            if (fall.reported)
            {
                float at[3] = {0.0f, 0.0f, 0.0f};
                readVec(rdram, chara + kCharaPos, at);
                std::printf("[pos] chara 0x%08X id %d stopped falling after %u updates at (%.1f, %.1f, %.1f)\n", chara,
                            charaId(rdram, chara), fall.frames, at[0], at[1], at[2]);
            }
            fall = Fall{};
        }
    }
    g_moveCollide(rdram, ctx, runtime);
}

} // namespace

void installPosTrace(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_POS_TRACE");
    if (!enabled || !*enabled || *enabled == '0')
    {
        return;
    }
    using rg::timing::hookFunction;
    bool complete = hookFunction(runtime, kCharaSetPos, &setPos, g_setPos);
    complete &= hookFunction(runtime, kCharaSetPosXYZ, &setPosXYZ, g_setPosXYZ);
    complete &= hookFunction(runtime, kCharaPlaceAt, &placeAt, g_placeAt);
    complete &= hookFunction(runtime, kCharaMoveCollide, &moveCollide, g_moveCollide);
    complete &= hookFunction(runtime, kFloorClamp, &floorClamp, g_floorClamp);
    complete &= hookFunction(runtime, kEventCollectPolys, &eventPolys, g_eventPolys);
    complete &= hookFunction(runtime, kSweepCollectHits, &sweepHits, g_sweepHits);
    std::printf("[rg] position trace: character placements, lost floors and long falls go to the log%s\n",
                complete ? "" : " (some hooks are missing, see above)");
}

} // namespace rg::debug
