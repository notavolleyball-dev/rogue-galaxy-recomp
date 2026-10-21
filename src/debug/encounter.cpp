#include "debug/encounter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Stubs/Pad.h"
#include "scene_timing.h"

namespace rg::debug
{
namespace
{

constexpr uint32_t kEncounterDistTick = 0x003A5890u;
// SetInBattle, at each battle's start: the pad read it comes at, for the
// bench's frame and profile window.
constexpr uint32_t kSetInBattle = 0x00288410u;
// The countdown, in map units, and what it is cut to: under one frame's
// walk.
constexpr uint32_t kCountdown = 0x20u;
constexpr double kAlmostThere = 0.001;

PS2Runtime::RecompiledFunction g_tick = nullptr;
PS2Runtime::RecompiledFunction g_setInBattle = nullptr;
unsigned g_cuts = 0u;

uint32_t padReads()
{
    return ps2_stubs::getPadDebugSnapshot().ports[0][0].readCount;
}

void setInBattle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    std::printf("[encounter] battle starts at pad read %u\n", padReads());
    g_setInBattle(rdram, ctx, runtime);
}

void encounterDistTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t countdown = (getRegU32(ctx, 4) + kCountdown) & 0x1FFFFFFFu;
    if (countdown + sizeof(double) <= PS2_RAM_SIZE)
    {
        double remaining = 0.0;
        std::memcpy(&remaining, rdram + countdown, sizeof(remaining));
        if (remaining > kAlmostThere)
        {
            std::memcpy(rdram + countdown, &kAlmostThere, sizeof(kAlmostThere));
            if (g_cuts++ < 4u)
            {
                std::printf("[encounter] countdown %.1f cut to the next step at pad read %u\n", remaining, padReads());
            }
        }
    }
    g_tick(rdram, ctx, runtime);
}

} // namespace

void installForcedEncounter(PS2Runtime &runtime)
{
    const char *force = std::getenv("RG_FORCE_ENCOUNTER");
    if (!force || !*force || *force == '0')
    {
        return;
    }
    if (rg::timing::hookFunction(runtime, kEncounterDistTick, &encounterDistTick, g_tick) &&
        rg::timing::hookFunction(runtime, kSetInBattle, &setInBattle, g_setInBattle))
    {
        std::printf("[encounter] battles at the first steps (RG_FORCE_ENCOUNTER)\n");
    }
    else
    {
        std::fprintf(stderr, "[encounter] the encounter countdown is not in the function table\n");
    }
}

} // namespace rg::debug
