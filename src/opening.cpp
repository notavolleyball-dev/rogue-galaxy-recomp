#include "opening.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "overlay/overlay_manager.h"
#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::opening
{
namespace
{

constexpr uint32_t kPadIsTriggered = 0x001839F0u;
// TitleOpeningMovieTick's first check, PadIsTriggered(0x4B2570, 0x10), in
// the title overlay; a press goes on to the skip only once the opening has
// been seen (the halfword at 0x481B90).
constexpr uint32_t kOpeningCheckReturn = 0x004EBB48u;
constexpr uint32_t kPad = 0x004B2570u;
constexpr uint32_t kOpeningButton = 0x10u;
constexpr uint32_t kOpeningSeen = 0x00481B90u;

PS2Runtime::RecompiledFunction g_padIsTriggered = nullptr;
std::atomic<bool> g_skip{false};

void padIsTriggered(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_padIsTriggered(rdram, ctx, runtime);
    if (getRegU32(ctx, 31) == kOpeningCheckReturn && getRegU32(ctx, 4) == kPad &&
        getRegU32(ctx, 5) == kOpeningButton && g_skip.load(std::memory_order_relaxed) &&
        std::strcmp(rg::overlay::residentName(), "title") == 0)
    {
        const uint16_t seen = 1u;
        std::memcpy(rdram + kOpeningSeen, &seen, sizeof(seen));
        setReturnS32(ctx, 1);
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *skip = std::getenv("RG_SKIP_OPENING");
    if (skip && *skip && *skip != '0')
    {
        setSkip(true);
    }
    if (!rg::timing::hookFunction(runtime, kPadIsTriggered, &padIsTriggered, g_padIsTriggered))
    {
        std::fprintf(stderr, "[rg] the opening cannot be skipped\n");
    }
}

void setSkip(bool on)
{
    g_skip.store(on, std::memory_order_relaxed);
}

} // namespace rg::opening
