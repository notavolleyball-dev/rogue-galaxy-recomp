#include "draw_distance.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::drawdistance
{
namespace
{

constexpr uint32_t kFarClipRecParseArgs = 0x0018C850u;
constexpr uint32_t kMapModelCfgFarClip = 0x00195DC0u;
constexpr uint32_t kMapSceneCfgFog = 0x00192BB0u;
constexpr uint32_t kModelConfig = 0x00481328u;
constexpr uint32_t kSceneConfig = 0x00481304u;

PS2Runtime::RecompiledFunction g_farClipRecParseArgs = nullptr;
PS2Runtime::RecompiledFunction g_mapModelCfgFarClip = nullptr;
PS2Runtime::RecompiledFunction g_mapSceneCfgFog = nullptr;
std::atomic<int> g_percent{100};

// The `count` distances at `address`, those above 0 (0 or -1 is none)
// scaled.
void scale(uint8_t *rdram, uint32_t address, size_t count)
{
    address &= 0x1FFFFFFFu;
    const int percent = g_percent.load(std::memory_order_relaxed);
    if (percent == 100 || address == 0u || address + count * sizeof(float) > PS2_RAM_SIZE)
    {
        return;
    }
    for (size_t index = 0u; index < count; ++index)
    {
        float distance = 0.0f;
        std::memcpy(&distance, rdram + address + index * sizeof(float), sizeof(distance));
        if (distance > 0.0f)
        {
            distance *= static_cast<float>(percent) / 100.0f;
            std::memcpy(rdram + address + index * sizeof(float), &distance, sizeof(distance));
        }
    }
}

uint32_t pointerAt(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0u;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

void farClipRecParseArgs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t record = getRegU32(ctx, 4);
    g_farClipRecParseArgs(rdram, ctx, runtime);
    scale(rdram, record + 0xCu, 1u);
}

void mapModelCfgFarClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_mapModelCfgFarClip(rdram, ctx, runtime);
    if (const uint32_t config = pointerAt(rdram, kModelConfig))
    {
        scale(rdram, config + 0x14u, 1u);
    }
}

void mapSceneCfgFog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_mapSceneCfgFog(rdram, ctx, runtime);
    if (const uint32_t config = pointerAt(rdram, kSceneConfig))
    {
        scale(rdram, config + 0x1A4u, 2u);
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_DRAW_DISTANCE"))
    {
        setPercent(std::atoi(value));
    }
    const bool complete =
        rg::timing::hookFunction(runtime, kFarClipRecParseArgs, &farClipRecParseArgs, g_farClipRecParseArgs) &&
        rg::timing::hookFunction(runtime, kMapModelCfgFarClip, &mapModelCfgFarClip, g_mapModelCfgFarClip) &&
        rg::timing::hookFunction(runtime, kMapSceneCfgFog, &mapSceneCfgFog, g_mapSceneCfgFog);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] draw distance: some hooks are missing, see above\n");
    }
}

void setPercent(int percent)
{
    if (percent > 0)
    {
        g_percent.store(percent, std::memory_order_relaxed);
    }
}

} // namespace rg::drawdistance
