#include "scene_timing.h"

#include <algorithm>
#include <cmath>
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
constexpr uint32_t kLocked = 0x5E14u;    // u8: the game's overlay mode holds the scale
// The draw context (GetDrawCtx, 0x15DE80, reads it off gp), its environment
// at +0x270 with the vsync interval the flip waits for at +0x24
// (DrawEnvSetVsyncInterval, 0x144140), and the last frame's time in vblanks
// at +0xF10, which GfxEndFrame clamps to between one and two intervals.
constexpr int32_t kDrawCtxOffGp = -0x40CC;
constexpr uint32_t kDrawEnv = 0x270u;
constexpr uint32_t kVsyncInterval = 0x24u;
constexpr uint32_t kFrameTime = 0xF10u;
// A movie plays between these two; the game's own flag (MovieIsStarted,
// 0x1D5640) stays set until the next movie opens.
constexpr uint32_t kMoviePlayerStart = 0x001D5B50u;
constexpr uint32_t kMoviePlayerShutdown = 0x001D56C0u;

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_movieStartOriginal = nullptr;
PS2Runtime::RecompiledFunction g_movieShutdownOriginal = nullptr;
bool g_moviePlaying = false;
bool g_sixty = false;    // RG_FPS=60
bool g_adaptive = false; // RG_TIME_SCALE=game
bool g_trace = false;    // RG_TIME_TRACE=1
// What beforeFlip set this frame's flip to wait for, in vblanks.
uint32_t g_interval = 2u;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + physical, sizeof(T));
    return true;
}

template <typename T>
void writeGuest(uint8_t *rdram, uint32_t address, T value)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) <= PS2_RAM_SIZE)
    {
        std::memcpy(rdram + physical, &value, sizeof(T));
    }
}

void trace(float frameTime, float scale)
{
    static uint32_t count = 0u;
    static float low = 0.0f, high = 0.0f, sum = 0.0f;
    if (count == 0u)
    {
        low = high = frameTime;
    }
    low = std::min(low, frameTime);
    high = std::max(high, frameTime);
    sum += frameTime;
    if (++count == 300u)
    {
        std::printf("[timing] 300 updates: frame time %.2f..%.2f vblanks, mean %.2f; interval %u, scale %.2f\n", low,
                    high, sum / 300.0f, g_interval, scale);
        count = 0u;
        sum = 0.0f;
    }
}

// What the update itself leaves when the frame was on time, or at 60 fps
// the half step.
void afterUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t scene)
{
    float scale = 1.0f;
    if (g_sixty)
    {
        // The game's own overlays run at 60 Hz with the scale held: theirs.
        uint8_t locked = 0u;
        const bool haveLock = readGuest(rdram, scene + kLocked, locked);
        if (g_trace)
        {
            static uint32_t shown = 0u;
            if (shown < 8u)
            {
                ++shown;
                std::printf("[timing] update: scene 0x%08X locked %u (read %d)\n", scene, locked, haveLock ? 1 : 0);
            }
        }
        if (haveLock && locked != 0u)
        {
            return;
        }
        // A movie frame keeps the 30 fps step.
        const float base = g_interval == 2u ? 1.0f : 0.5f;
        scale = base;
        uint32_t drawCtx = 0u;
        float frameTime = 0.0f;
        const bool measured = readGuest(rdram, getRegU32(ctx, 28) + kDrawCtxOffGp, drawCtx) &&
                              readGuest(rdram, drawCtx + kFrameTime, frameTime) && std::isfinite(frameTime);
        if (g_adaptive && measured)
        {
            scale = std::clamp(frameTime * 0.5f, base, 2.0f);
        }
        if (g_trace)
        {
            trace(measured ? frameTime : 0.0f, scale);
        }
    }
    const int32_t one = 1;
    writeGuest(rdram, scene + kStepCount, one);
    writeGuest(rdram, scene + kTimeScale, scale);
}

void timeScaleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // After the update has really returned: a safe point inside it would
    // otherwise let it finish, and store its own scale, after these writes.
    rg::hooks::callThen(g_original, rdram, ctx, runtime, &afterUpdate, getRegU32(ctx, 4));
}

void movieStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_moviePlaying = true;
    g_movieStartOriginal(rdram, ctx, runtime);
}

void movieShutdown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_moviePlaying = false;
    g_movieShutdownOriginal(rdram, ctx, runtime);
}

bool hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
          PS2Runtime::RecompiledFunction &original)
{
    original = runtime.lookupFunction(address);
    if (!original || !runtime.replaceFunction(address, replacement))
    {
        std::fprintf(stderr, "[rg] 0x%08X is not in the function table\n", address);
        original = nullptr;
        return false;
    }
    return true;
}

} // namespace

void beforeFlip(uint8_t *rdram, R5900Context *ctx)
{
    if (!g_sixty)
    {
        return;
    }
    const uint32_t gp = getRegU32(ctx, 28);
    uint32_t drawCtx = 0u, env = 0u;
    if (!readGuest(rdram, gp + kDrawCtxOffGp, drawCtx) || !readGuest(rdram, drawCtx + kDrawEnv, env))
    {
        return;
    }
    const uint8_t movie = g_moviePlaying ? 1u : 0u;
    g_interval = movie != 0u ? 2u : 1u;
    if (g_trace)
    {
        static uint32_t shown = 0u;
        static uint8_t lastMovie = 0xFFu;
        uint32_t before = 0u;
        readGuest(rdram, env + kVsyncInterval, before);
        if (shown < 5u || movie != lastMovie)
        {
            ++shown;
            lastMovie = movie;
            std::printf("[timing] flip: gp 0x%08X drawctx 0x%08X env 0x%08X interval %u -> %u, movie %u\n", gp, drawCtx,
                        env, before, g_interval, movie);
        }
    }
    writeGuest(rdram, env + kVsyncInterval, g_interval);
}

void install(PS2Runtime &runtime)
{
    const char *fps = std::getenv("RG_FPS");
    g_sixty = fps && std::strcmp(fps, "60") == 0;
    const char *mode = std::getenv("RG_TIME_SCALE");
    // At 60 fps the step follows the frame time unless RG_TIME_SCALE=fixed:
    // a host that only reaches 35 frames a second would otherwise play at
    // 60% speed.
    g_adaptive = g_sixty ? !(mode && std::strcmp(mode, "fixed") == 0) : (mode && std::strcmp(mode, "game") == 0);
    const char *traceMode = std::getenv("RG_TIME_TRACE");
    g_trace = traceMode && *traceMode && *traceMode != '0';
    if (!g_sixty && g_adaptive)
    {
        return;
    }
    if (!hook(runtime, kGameSceneTimeScaleUpdate, &timeScaleUpdate, g_original))
    {
        std::fprintf(stderr, "[rg] the game scales its own steps\n");
        g_sixty = false;
        return;
    }
    if (g_sixty)
    {
        if (!hook(runtime, kMoviePlayerStart, &movieStart, g_movieStartOriginal) ||
            !hook(runtime, kMoviePlayerShutdown, &movieShutdown, g_movieShutdownOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: movies will flip every vblank too\n");
        }
        std::printf("[rg] 60 fps: the flip waits one vblank and the scene takes %s\n",
                    g_adaptive ? "steps sized by the frame time, half at full rate" : "half steps");
    }
}

} // namespace rg::timing
