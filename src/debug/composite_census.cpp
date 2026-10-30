#include "debug/composite_census.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scene_timing.h"

namespace rg::debug
{
namespace
{

// ModelDrawOffscreenCompositeAtMaybe: its only caller's way in, which sets
// the settings' position (+0x30) and jumps into the function, past the
// function table.
constexpr uint32_t kComposite = 0x0016B8A0u;

PS2Runtime::RecompiledFunction g_original = nullptr;
uint8_t *g_rdram = nullptr;
std::mutex g_mutex;
std::map<std::string, uint64_t> g_kinds;
uint64_t g_calls = 0u;
std::chrono::steady_clock::time_point g_last;

uint32_t word(uint32_t address)
{
    uint32_t value = 0u;
    std::memcpy(&value, g_rdram + ((address & (PS2_RAM_SIZE - 1u)) & ~3u), sizeof(value));
    return value;
}

float real(uint32_t address)
{
    const uint32_t bits = word(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// 0, 1 and the steps between, coarsely: what the branches compare against.
std::string level(float value)
{
    char text[24];
    if (value <= 0.0f || value >= 1.0f)
        std::snprintf(text, sizeof(text), "%g", value);
    else
        std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

void composite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t settings = getRegU32(ctx, 4);
    const float glowAlpha = ctx->f[12], alpha = ctx->f[13];
    std::string kind;
    if (word(settings + 0x5Cu) == 0u || word(settings + 0x50u) == 0u)
    {
        kind = "nothing drawn";
    }
    else
    {
        const float strength = real(settings + 0x24u);
        const bool offscreen = strength >= 0.0f && word(settings + 0x20u) != 0u;
        char text[160];
        std::snprintf(text, sizeof(text), "%s strength %s f12 %s f13 %s +28 %s +60 %s",
                      offscreen ? "offscreen" : "direct", level(strength).c_str(), level(glowAlpha).c_str(),
                      level(alpha).c_str(), word(settings + 0x28u) ? "set" : "0",
                      word(settings + 0x60u) ? "set" : "0");
        kind = text;
    }
    {
        std::lock_guard lock(g_mutex);
        ++g_kinds[kind];
        ++g_calls;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_last > std::chrono::seconds(10))
        {
            g_last = now;
            std::fprintf(stderr, "[composite] %llu calls so far:\n", static_cast<unsigned long long>(g_calls));
            for (const auto &[name, count] : g_kinds)
                std::fprintf(stderr, "[composite]   %8llu  %s\n", static_cast<unsigned long long>(count), name.c_str());
        }
    }
    // RG_COMPOSITE_DIRECT=1 (an experiment): the game's direct path for
    // every object, as its settings choose for a negative strength.
    static const bool direct = std::getenv("RG_COMPOSITE_DIRECT") != nullptr;
    const uint32_t saved = word(settings + 0x24u);
    if (direct)
    {
        const float minusOne = -1.0f;
        std::memcpy(g_rdram + ((settings + 0x24u) & (PS2_RAM_SIZE - 1u)), &minusOne, sizeof(minusOne));
    }
    g_original(rdram, ctx, runtime);
    if (direct)
        std::memcpy(g_rdram + ((settings + 0x24u) & (PS2_RAM_SIZE - 1u)), &saved, sizeof(saved));
}

} // namespace

void installCompositeCensus(PS2Runtime &runtime)
{
    if (!std::getenv("RG_COMPOSITE_CENSUS"))
        return;
    g_rdram = runtime.memory().getRDRAM();
    g_last = std::chrono::steady_clock::now();
    if (!rg::timing::hookFunction(runtime, kComposite, &composite, g_original))
        std::fprintf(stderr, "[composite] could not hook ModelDrawOffscreenCompositeAt\n");
}

} // namespace rg::debug
