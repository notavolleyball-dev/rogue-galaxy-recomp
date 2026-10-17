#include "scene_timing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"

namespace rg::timing
{
namespace
{

constexpr uint32_t kGameSceneTimeScaleUpdate = 0x001CEBE0u;
// GameScene fields the update writes (RGDecomp T1cebe0).
constexpr uint32_t kStepCount = 0x5E04u; // s32: logic steps this frame
constexpr uint32_t kTimeScale = 0x5E0Cu; // f32: time scale this frame

PS2Runtime::RecompiledFunction g_original = nullptr;

// What the update itself leaves when the frame was on time.
void afterUpdate(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t scene)
{
    const uint32_t steps = (scene + kStepCount) & 0x1FFFFFFCu;
    const uint32_t scale = (scene + kTimeScale) & 0x1FFFFFFCu;
    // Each on its own: the mask can wrap one and not the other.
    if (steps + 4u <= PS2_RAM_SIZE && scale + 4u <= PS2_RAM_SIZE)
    {
        const int32_t one = 1;
        const float unit = 1.0f;
        std::memcpy(rdram + steps, &one, sizeof(one));
        std::memcpy(rdram + scale, &unit, sizeof(unit));
    }
}

void timeScaleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // After the update has really returned: a safe point inside it would
    // otherwise let it finish, and store its own scale, after these writes.
    rg::hooks::callThen(g_original, rdram, ctx, runtime, &afterUpdate, getRegU32(ctx, 4));
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
