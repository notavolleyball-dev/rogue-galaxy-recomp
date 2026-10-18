#include "scene_timing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "step_fixes.h"

namespace rg::timing
{
namespace
{

constexpr uint32_t kGameSceneTimeScaleUpdate = 0x001CEBE0u;
// GameScene fields the update writes (RGDecomp T1cebe0).
constexpr uint32_t kStepCount = 0x5E04u; // s32: logic steps this frame
constexpr uint32_t kBaseScale = 0x5E08u; // f32: the game's slow-motion knob (GameSceneSetTimeScale)
constexpr uint32_t kTimeScale = 0x5E0Cu; // f32: time scale this frame
constexpr uint32_t kLocked = 0x5E14u;    // u8: the game's overlay mode holds the scale
// GameSceneGetTimeStep (0x1CEBA0) is scale x base, and the only way the game
// reads them. The scene is static; GetCurrentScene (0x1F5BE0) reads its
// pointer off gp, MoviePlayModal (0x267C60) another copy.
constexpr int32_t kSceneOffGp = -0x37D8;
constexpr int32_t kMovieSceneOffGp = -0x31F8;
// The draw context (GetDrawCtx, 0x15DE80, reads it off gp), its environment
// at +0x270 with the vsync interval the flip waits for at +0x24
// (DrawEnvSetVsyncInterval, 0x144140), and the last frame's time in vblanks
// at +0xF10, which GfxEndFrame clamps to between one and two intervals.
constexpr int32_t kDrawCtxOffGp = -0x40CC;
constexpr uint32_t kDrawEnv = 0x270u;
constexpr uint32_t kVsyncInterval = 0x24u;
constexpr uint32_t kFrameTime = 0xF10u;
// DrawEnvGetVsyncInterval (0x144130), where the game reads the interval for
// the vblanks a logic frame spans rather than for the wait: PlayTimeStep
// (0x2F7570: tick = interval x step, the play clock and the per-second
// event triggers), MinimapUpdate (0x3295A0: halves the step at interval 1,
// the game's own 60 Hz menus) and TextEntrySetupState (0x4359F0). The
// return addresses of those calls. PauseStepUpdate (the quit-to-title
// prompt, one update per vblank of the interval) and the insect cage's own
// halving are right with the wait's value.
constexpr uint32_t kDrawEnvGetVsyncInterval = 0x00144130u;
constexpr uint32_t kIntervalLogicReaders[] = {0x002F75BCu, 0x0032963Cu, 0x00435A28u};
// A movie plays between these two; the game's own flag (MovieIsStarted,
// 0x1D5640) stays set until the next movie opens.
constexpr uint32_t kMoviePlayerStart = 0x001D5B50u;
constexpr uint32_t kMoviePlayerShutdown = 0x001D56C0u;
// The modal loop a movie plays in: its own GfxEndFrame and PadUpdate, never
// the time-scale update. It reads the step once on entry for its sound
// fades (0x267CB0), and EventCueStep's fades read it every frame.
constexpr uint32_t kMoviePlayModal = 0x00267C60u;
// GameData (0x4B8B30) + 0xC: the block PlayTimeTick advances; the play time
// is a double at its +0x8, in vblanks.
constexpr uint32_t kPlayTimeBlock = 0x004B8B3Cu;
constexpr uint32_t kPlayTime = 0x8u;

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_movieStartOriginal = nullptr;
PS2Runtime::RecompiledFunction g_movieShutdownOriginal = nullptr;
PS2Runtime::RecompiledFunction g_movieModalOriginal = nullptr;
PS2Runtime::RecompiledFunction g_intervalOriginal = nullptr;
bool g_moviePlaying = false;
bool g_inMovieModal = false;
bool g_sixty = false;     // RG_FPS=60
bool g_adaptive = false;  // RG_TIME_SCALE=game
bool g_stepFixes = true;  // RG_STEP_FIXES=0 turns the frame-counted systems' hooks off
bool g_trace = false;     // RG_TIME_TRACE=1
// What beforeFlip set this frame's flip to wait for, in vblanks.
uint32_t g_interval = 2u;
// The vblanks a logic frame spans for the readers above: 2 while the
// scene's scale is ours (half steps at interval 1), 0 to leave the field's
// value alone (the game's overlays, or the 30 fps mode).
uint32_t g_logicInterval = 0u;
uint32_t g_intervalRewrites = 0u;

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

void trace(const uint8_t *rdram, float frameTime, float scale)
{
    using Clock = std::chrono::steady_clock;
    static uint32_t count = 0u;
    static float low = 0.0f, high = 0.0f, sum = 0.0f;
    static Clock::time_point wallStart{};
    static double clockStart = 0.0;
    uint32_t block = 0u;
    double playTime = 0.0;
    const bool haveClock =
        readGuest(rdram, kPlayTimeBlock, block) && block != 0u && readGuest(rdram, block + kPlayTime, playTime);
    if (count == 0u)
    {
        low = high = frameTime;
        wallStart = Clock::now();
        clockStart = haveClock ? playTime : 0.0;
    }
    low = std::min(low, frameTime);
    high = std::max(high, frameTime);
    sum += frameTime;
    if (++count == 300u)
    {
        const double wall = std::chrono::duration<double>(Clock::now() - wallStart).count();
        std::printf("[timing] 300 updates: frame time %.2f..%.2f vblanks, mean %.2f; interval %u, scale %.2f; play "
                    "clock +%.0f vblanks in %.2f s; interval rewrites %u\n",
                    low, high, sum / 300.0f, g_interval, scale, haveClock ? playTime - clockStart : 0.0, wall,
                    g_intervalRewrites);
        count = 0u;
        sum = 0.0f;
        g_intervalRewrites = 0u;
        rg::steps::trace();
    }
}

// What the update itself leaves when the frame was on time, or at 60 fps
// the half step.
void afterUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t scene)
{
    float scale = 1.0f;
    g_logicInterval = 0u;
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
        g_logicInterval = 2u;
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
            trace(rdram, measured ? frameTime : 0.0f, scale);
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

// The scene's per-frame scale, through both pointers the game reads it by.
void writeScale(uint8_t *rdram, uint32_t gp, float scale)
{
    uint32_t scene = 0u, movieScene = 0u;
    readGuest(rdram, gp + kSceneOffGp, scene);
    readGuest(rdram, gp + kMovieSceneOffGp, movieScene);
    if (scene != 0u)
    {
        writeGuest(rdram, scene + kTimeScale, scale);
    }
    if (movieScene != 0u && movieScene != scene)
    {
        writeGuest(rdram, movieScene + kTimeScale, scale);
    }
}

void movieStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_moviePlaying = true;
    if (g_sixty && g_stepFixes)
    {
        // Nothing runs the update while a movie plays, and its frames flip
        // every two vblanks: whole steps, or the sound fades crawl.
        writeScale(rdram, getRegU32(ctx, 28), 1.0f);
    }
    g_movieStartOriginal(rdram, ctx, runtime);
}

void movieShutdown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Inside the modal loop the flag holds until the loop returns: its
    // fade-out after the shutdown flips at the movie's rate too.
    if (!g_inMovieModal)
    {
        g_moviePlaying = false;
    }
    g_movieShutdownOriginal(rdram, ctx, runtime);
}

void afterMovieModal(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    g_inMovieModal = false;
    g_moviePlaying = false;
}

void movieModal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_inMovieModal = true;
    g_moviePlaying = true;
    writeScale(rdram, getRegU32(ctx, 28), 1.0f);
    rg::hooks::callThen(g_movieModalOriginal, rdram, ctx, runtime, &afterMovieModal, 0u);
}

void vsyncInterval(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_intervalOriginal(rdram, ctx, runtime);
    if (g_logicInterval == 0u)
    {
        return;
    }
    for (uint32_t reader : kIntervalLogicReaders)
    {
        if (returnPc == reader)
        {
            setReturnU32(ctx, g_logicInterval);
            ++g_intervalRewrites;
            return;
        }
    }
}

} // namespace

bool hookFunction(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
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

bool sixtyFps()
{
    return g_sixty;
}

bool stepFixes()
{
    return g_stepFixes;
}

bool traceEnabled()
{
    return g_trace;
}

float sceneStep(const uint8_t *rdram, const R5900Context *ctx)
{
    uint32_t scene = 0u;
    float scale = 1.0f, base = 1.0f;
    if (!readGuest(rdram, getRegU32(ctx, 28) + kSceneOffGp, scene) || scene == 0u ||
        !readGuest(rdram, scene + kTimeScale, scale) || !readGuest(rdram, scene + kBaseScale, base))
    {
        return 1.0f;
    }
    const float step = scale * base;
    return std::isfinite(step) && step > 0.0f ? step : 1.0f;
}

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
    const char *fixes = std::getenv("RG_STEP_FIXES");
    g_stepFixes = !(fixes && std::strcmp(fixes, "0") == 0);
    const char *traceMode = std::getenv("RG_TIME_TRACE");
    g_trace = traceMode && *traceMode && *traceMode != '0';
    if (!g_sixty && g_adaptive)
    {
        return;
    }
    if (!hookFunction(runtime, kGameSceneTimeScaleUpdate, &timeScaleUpdate, g_original))
    {
        std::fprintf(stderr, "[rg] the game scales its own steps\n");
        g_sixty = false;
        return;
    }
    if (g_sixty)
    {
        if (!hookFunction(runtime, kMoviePlayerStart, &movieStart, g_movieStartOriginal) ||
            !hookFunction(runtime, kMoviePlayerShutdown, &movieShutdown, g_movieShutdownOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: movies will flip every vblank too\n");
        }
        if (g_stepFixes)
        {
            if (!hookFunction(runtime, kMoviePlayModal, &movieModal, g_movieModalOriginal))
            {
                std::fprintf(stderr, "[rg] 60 fps: movies will fade their sound at half speed\n");
            }
            if (!hookFunction(runtime, kDrawEnvGetVsyncInterval, &vsyncInterval, g_intervalOriginal))
            {
                std::fprintf(stderr, "[rg] 60 fps: the play clock will run at half speed\n");
            }
        }
        std::printf("[rg] 60 fps: the flip waits one vblank and the scene takes %s%s\n",
                    g_adaptive ? "steps sized by the frame time, half at full rate" : "half steps",
                    g_stepFixes ? "" : "; the frame-counted systems are left as they are (RG_STEP_FIXES=0)");
    }
}

} // namespace rg::timing
