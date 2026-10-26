#include "battle_rates.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <smmintrin.h>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::battlerates
{
namespace
{

constexpr uint32_t kEncounterDistTick = 0x003A5890u;
constexpr uint32_t kBattleCalcResults = 0x003A2100u;
constexpr uint32_t kPartyAddMoney = 0x003C8E90u;
// PartyAddMoney's call for an enemy's money in BattleCalcResults.
constexpr uint32_t kEnemyMoneyReturn = 0x003A2944u;
constexpr uint32_t kResultsBlock = 0x248u;
constexpr uint32_t kExperienceCap = 0x00465C90u;
constexpr uint32_t kActionGaugeRegen = 0x003B6490u;
// The party (PartyHasMember, 0x3C1E70): its pointer, and in it the number
// of members fighting and their ids, which a character keeps at +0xC.
constexpr uint32_t kParty = 0x00483820u;
constexpr uint32_t kPartyCount = 0x18Cu;
constexpr uint32_t kPartyIds = 0x190u;
constexpr uint32_t kCharaId = 0xCu;

PS2Runtime::RecompiledFunction g_encounterDistTick = nullptr;
PS2Runtime::RecompiledFunction g_battleCalcResults = nullptr;
PS2Runtime::RecompiledFunction g_partyAddMoney = nullptr;
PS2Runtime::RecompiledFunction g_actionGaugeRegen = nullptr;
std::atomic<int> g_encounterRate{100};
std::atomic<int> g_experience{100};
std::atomic<int> g_money{100};
std::atomic<int> g_actionGauge{100};
// BattleCalcResults's argument while it runs, and whether its experience
// has been scaled yet (the game thread's alone).
uint32_t g_results = 0u;
bool g_experienceScaled = false;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    address &= 0x1FFFFFFFu;
    if (address + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + address, sizeof(T));
    return true;
}

void encounterDistTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int percent = g_encounterRate.load(std::memory_order_relaxed);
    if (percent != 100)
    {
        ctx->f[12] *= static_cast<float>(percent) / 100.0f;
    }
    g_encounterDistTick(rdram, ctx, runtime);
}

void battleCalcResults(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_results = getRegU32(ctx, 4);
    g_experienceScaled = false;
    g_battleCalcResults(rdram, ctx, runtime);
    g_results = 0u;
}

void scaleExperience(uint8_t *rdram)
{
    const int percent = g_experience.load(std::memory_order_relaxed);
    uint32_t block = 0u;
    int32_t total = 0, cap = 0;
    if (percent == 100 || !readGuest(rdram, g_results + kResultsBlock, block) || block == 0u ||
        !readGuest(rdram, block, total) || !readGuest(rdram, kExperienceCap, cap))
    {
        return;
    }
    const int64_t scaled = static_cast<int64_t>(total) * percent / 100;
    total = static_cast<int32_t>(std::clamp<int64_t>(scaled, total > 0 && percent > 0 ? 1 : 0, cap));
    std::memcpy(rdram + (block & 0x1FFFFFFFu), &total, sizeof(total));
}

void partyAddMoney(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_results != 0u && getRegU32(ctx, 31) == kEnemyMoneyReturn)
    {
        if (!g_experienceScaled)
        {
            g_experienceScaled = true;
            scaleExperience(rdram);
        }
        const int percent = g_money.load(std::memory_order_relaxed);
        if (percent != 100)
        {
            const int64_t amount = _mm_cvtsi128_si64(ctx->r[5]);
            ctx->r[5] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[5], 1), amount * percent / 100);
        }
    }
    g_partyAddMoney(rdram, ctx, runtime);
}

bool inParty(const uint8_t *rdram, uint32_t chara)
{
    uint32_t party = 0u;
    int32_t id = 0;
    int16_t count = 0;
    if (!readGuest(rdram, kParty, party) || party == 0u || !readGuest(rdram, chara + kCharaId, id) ||
        !readGuest(rdram, party + kPartyCount, count))
    {
        return false;
    }
    for (int16_t i = 0; i < count && i < 32; ++i)
    {
        int16_t member = -1;
        if (readGuest(rdram, party + kPartyIds + 2u * static_cast<uint32_t>(i), member) && member == id)
        {
            return true;
        }
    }
    return false;
}

void actionGaugeRegen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int percent = g_actionGauge.load(std::memory_order_relaxed);
    if (percent != 100 && inParty(rdram, getRegU32(ctx, 4)))
    {
        ctx->f[12] *= static_cast<float>(percent) / 100.0f;
    }
    g_actionGaugeRegen(rdram, ctx, runtime);
}

void setFrom(const char *name, void (*set)(int))
{
    if (const char *value = std::getenv(name))
    {
        set(std::atoi(value));
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    setFrom("RG_ENCOUNTER_RATE", &setEncounterRate);
    setFrom("RG_BATTLE_EXPERIENCE", &setExperience);
    setFrom("RG_BATTLE_MONEY", &setMoney);
    setFrom("RG_ACTION_GAUGE", &setActionGauge);
    const bool complete =
        rg::timing::hookFunction(runtime, kEncounterDistTick, &encounterDistTick, g_encounterDistTick) &&
        rg::timing::hookFunction(runtime, kBattleCalcResults, &battleCalcResults, g_battleCalcResults) &&
        rg::timing::hookFunction(runtime, kPartyAddMoney, &partyAddMoney, g_partyAddMoney) &&
        rg::timing::hookFunction(runtime, kActionGaugeRegen, &actionGaugeRegen, g_actionGaugeRegen);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] battle rates: some hooks are missing, see above\n");
    }
}

void setEncounterRate(int percent)
{
    g_encounterRate.store(std::max(percent, 0), std::memory_order_relaxed);
}

void setExperience(int percent)
{
    g_experience.store(std::max(percent, 0), std::memory_order_relaxed);
}

void setMoney(int percent)
{
    g_money.store(std::max(percent, 0), std::memory_order_relaxed);
}

void setActionGauge(int percent)
{
    g_actionGauge.store(std::max(percent, 0), std::memory_order_relaxed);
}

} // namespace rg::battlerates
