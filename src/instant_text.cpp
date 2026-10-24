#include "instant_text.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::instanttext
{
namespace
{

constexpr uint32_t kMesWinGetInstant = 0x00277600u;

PS2Runtime::RecompiledFunction g_original = nullptr;
std::atomic<bool> g_enabled{false};

void mesWinGetInstant(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        g_original(rdram, ctx, runtime);
        return;
    }
    setReturnS32(ctx, 1);
    // A host function stands in for the guest's `jr ra` too (see
    // libc_rand.cpp).
    ctx->pc = getRegU32(ctx, 31);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_INSTANT_TEXT");
    if (enabled && *enabled && *enabled != '0')
    {
        setEnabled(true);
    }
    if (!rg::timing::hookFunction(runtime, kMesWinGetInstant, &mesWinGetInstant, g_original))
    {
        std::fprintf(stderr, "[rg] the text stays letter by letter\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

} // namespace rg::instanttext
