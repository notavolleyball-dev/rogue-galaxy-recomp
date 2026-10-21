#include "movie_filter.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::movie
{
namespace
{

constexpr uint32_t kGsPktSetTexFilter = 0x0013E3D0u;
// MovieDrawFrame's second filter call, for the frame (the first is for the
// black tiles under it). GsPktSetTexture hands the value to
// GsTex1SetFilter: 0 nearest, 1 bilinear.
constexpr uint32_t kMovieFrameFilterReturn = 0x001D5A08u;
constexpr int kFilterRegister = 5;

PS2Runtime::RecompiledFunction g_gsPktSetTexFilter = nullptr;
std::atomic<bool> g_smooth{true};

void gsPktSetTexFilter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kMovieFrameFilterReturn && g_smooth.load(std::memory_order_relaxed))
    {
        ctx->r[kFilterRegister] = _mm_set_epi64x(0, 1);
    }
    g_gsPktSetTexFilter(rdram, ctx, runtime);
}

} // namespace

void installFilter(PS2Runtime &runtime)
{
    const char *smooth = std::getenv("RG_SMOOTH_MOVIES");
    if (smooth && *smooth == '0')
    {
        setSmooth(false);
    }
    if (!rg::timing::hookFunction(runtime, kGsPktSetTexFilter, &gsPktSetTexFilter, g_gsPktSetTexFilter))
    {
        std::fprintf(stderr, "[rg] the movies cannot be smoothed\n");
    }
}

void setSmooth(bool on)
{
    g_smooth.store(on, std::memory_order_relaxed);
}

} // namespace rg::movie
