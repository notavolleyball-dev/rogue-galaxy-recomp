#include "save_anywhere.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <smmintrin.h>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"

namespace rg::saveanywhere
{
namespace
{

constexpr uint32_t kPadIsTriggered = 0x001839F0u;
constexpr uint32_t kGameReqQueuePushBack = 0x001F5B50u;
constexpr uint32_t kTriangleCheckReturn = 0x00283840u;
constexpr uint32_t kMainMenuRequestReturn = 0x00283880u;
constexpr int32_t kMainMenuMode = 17;
constexpr int32_t kTransporterMenuMode = 20;
// Pad 1's buttons this frame, in the game's order (R1 0x8).
constexpr uint32_t kPad1 = 0x004B2570u;
constexpr uint32_t kPadButtons = 0x4u;
constexpr uint32_t kButtonR1 = 0x8u;

PS2Runtime::RecompiledFunction g_padIsTriggered = nullptr;
PS2Runtime::RecompiledFunction g_pushBack = nullptr;
std::atomic<bool> g_enabled{false};
// Triangle came with R1 held: the main menu's request that follows is the
// transporter menu's (the game's thread).
bool g_pending = false;

void padIsTriggered(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_padIsTriggered(rdram, ctx, runtime);
    if (returnPc == kTriangleCheckReturn)
    {
        uint32_t buttons = 0u;
        if (const uint8_t *pad = getConstMemPtr(rdram, kPad1 + kPadButtons))
        {
            std::memcpy(&buttons, pad, sizeof(buttons));
        }
        g_pending = getRegU32(ctx, 2) != 0u && (buttons & kButtonR1) != 0u &&
                    g_enabled.load(std::memory_order_relaxed);
    }
}

void pushBack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_pending && getRegU32(ctx, 31) == kMainMenuRequestReturn &&
        static_cast<int32_t>(getRegU32(ctx, 4)) == kMainMenuMode)
    {
        ctx->r[4] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[4], 1), kTransporterMenuMode);
    }
    g_pending = false;
    g_pushBack(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_SAVE_ANYWHERE"))
    {
        setEnabled(*value && *value != '0');
    }
    if (!rg::timing::hookFunction(runtime, kPadIsTriggered, &padIsTriggered, g_padIsTriggered) ||
        !rg::timing::hookFunction(runtime, kGameReqQueuePushBack, &pushBack, g_pushBack))
    {
        std::fprintf(stderr, "[rg] saving away from a transporter is unavailable\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

} // namespace rg::saveanywhere
