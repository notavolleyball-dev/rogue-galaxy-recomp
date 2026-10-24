#include "dev_tools.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::devtools
{
namespace
{

constexpr uint32_t kDebugSwitch = 0x00207FE0u;

PS2Runtime::RecompiledFunction g_original = nullptr;
std::atomic<bool> g_enabled{false};

void debugSwitch(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    setReturnS32(ctx, g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
    // A host function stands in for the guest's `jr ra` too (see
    // libc_rand.cpp).
    ctx->pc = getRegU32(ctx, 31);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_DEV_TOOLS");
    if (enabled && *enabled && *enabled != '0')
    {
        setEnabled(true);
    }
    if (!rg::timing::hookFunction(runtime, kDebugSwitch, &debugSwitch, g_original))
    {
        std::fprintf(stderr, "[rg] the developer tools stay off\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

} // namespace rg::devtools
