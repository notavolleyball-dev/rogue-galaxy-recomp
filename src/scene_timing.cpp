#include "scene_timing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"

namespace rg::timing
{
namespace
{

constexpr uint32_t kGameSceneTimeScaleUpdate = 0x001CEBE0u;
// GameScene fields the update writes (RGDecomp T1cebe0).
constexpr uint32_t kStepCount = 0x5E04u; // s32: logic steps this frame
constexpr uint32_t kTimeScale = 0x5E0Cu; // f32: time scale this frame

PS2Runtime::RecompiledFunction g_original = nullptr;

void timeScaleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    g_original(rdram, ctx, runtime);
    // What the update itself leaves when the frame was on time.
    const uint32_t steps = (scene + kStepCount) & 0x1FFFFFFCu;
    const uint32_t scale = (scene + kTimeScale) & 0x1FFFFFFCu;
    if (scale + 4u <= PS2_RAM_SIZE)
    {
        const int32_t one = 1;
        const float unit = 1.0f;
        std::memcpy(rdram + steps, &one, sizeof(one));
        std::memcpy(rdram + scale, &unit, sizeof(unit));
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *mode = std::getenv("RG_TIME_SCALE");
    if (mode && std::strcmp(mode, "game") == 0)
    {
        return;
    }
    g_original = runtime.lookupFunction(kGameSceneTimeScaleUpdate);
    if (!g_original || !runtime.replaceFunction(kGameSceneTimeScaleUpdate, &timeScaleUpdate))
    {
        std::fprintf(stderr, "[rg] 0x%08X is not in the function table; the game scales its own steps\n",
                     kGameSceneTimeScaleUpdate);
    }
}

} // namespace rg::timing
