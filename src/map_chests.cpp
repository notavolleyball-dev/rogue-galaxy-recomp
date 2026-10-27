#include "map_chests.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::mapchests
{
namespace
{

constexpr uint32_t kSavePointAreaCompleteTest = 0x003EC7A0u;
// Where MinimapDrawEntry's call before its chest pass returns to.
constexpr uint32_t kChestPassReturn = 0x003267B0u;

PS2Runtime::RecompiledFunction g_original = nullptr;
std::atomic<bool> g_enabled{false};

void savePointAreaCompleteTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_enabled.load(std::memory_order_relaxed) || getRegU32(ctx, 31) != kChestPassReturn)
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
    const char *enabled = std::getenv("RG_MAP_CHESTS");
    if (enabled && *enabled && *enabled != '0')
    {
        setEnabled(true);
    }
    if (!rg::timing::hookFunction(runtime, kSavePointAreaCompleteTest, &savePointAreaCompleteTest, g_original))
    {
        std::fprintf(stderr, "[rg] the map shows chests only once a planet is complete\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

} // namespace rg::mapchests
