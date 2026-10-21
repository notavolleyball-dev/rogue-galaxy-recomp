#include "loading_screen.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::loading
{
namespace
{

constexpr uint32_t kLoadingScreenStart = 0x00343240u;
constexpr uint32_t kLoadingScreenStop = 0x003431E0u;
constexpr uint32_t kLoadingScreenFrame = 0x003435D0u;
// LoadingScreenFrame's fade-in (0x3445B4..0x344628): while the lines shown
// (0x4E8990) are fewer than the recap's (0x4E8D84), the next line's alpha
// (floats at 0x4E8D14, one a line, at most 14: LoadingScreenResetEntries)
// gains a constant a frame, and at 1.0 the next line starts.
// LoadingScreenTryFinish lets the screen go to "Press X" once every line
// is shown.
constexpr uint32_t kLinesShown = 0x004E8990u;
constexpr uint32_t kLineCount = 0x004E8D84u;
constexpr uint32_t kLineAlpha = 0x004E8D14u;
constexpr int32_t kMaxLines = 14;

PS2Runtime::RecompiledFunction g_start = nullptr;
PS2Runtime::RecompiledFunction g_stop = nullptr;
PS2Runtime::RecompiledFunction g_frame = nullptr;
bool g_active = false;
std::atomic<bool> g_instantRecap{false};

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

void start(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_active = true;
    g_start(rdram, ctx, runtime);
}

void afterStop(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    g_active = false;
}

void stop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // It waits for the screen's thread to finish its last frames.
    rg::hooks::callThen(g_stop, rdram, ctx, runtime, &afterStop, 0u);
}

void frame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    int32_t lines = 0;
    if (g_instantRecap.load(std::memory_order_relaxed) && readGuest(rdram, kLineCount, lines) && lines > 0)
    {
        lines = std::min(lines, kMaxLines);
        for (int32_t line = 0; line < lines; ++line)
        {
            writeGuest(rdram, kLineAlpha + 4u * static_cast<uint32_t>(line), 1.0f);
        }
        writeGuest(rdram, kLinesShown, lines);
    }
    g_frame(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *instant = std::getenv("RG_INSTANT_RECAP");
    if (instant && *instant && *instant != '0')
    {
        setInstantRecap(true);
    }
    const bool complete = rg::timing::hookFunction(runtime, kLoadingScreenStart, &start, g_start) &&
                          rg::timing::hookFunction(runtime, kLoadingScreenStop, &stop, g_stop) &&
                          rg::timing::hookFunction(runtime, kLoadingScreenFrame, &frame, g_frame);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] loading screen: some hooks are missing, see above\n");
    }
}

bool active()
{
    return g_active;
}

void setInstantRecap(bool on)
{
    g_instantRecap.store(on, std::memory_order_relaxed);
}

} // namespace rg::loading
