#include "quick_scenes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::quickscenes
{
namespace
{

constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kGamePhase = 0x004B51E0u;
constexpr uint32_t kModeTransporterWarp = 11u;
constexpr uint32_t kModeTreasureBox = 13u;
constexpr double kFast = 3.0;
// Mode 11 runs the area script its warp request named (s32 at 0x4821E0):
// the transporter's on leaving (0x320) and arriving (0x321); the doors' and
// lifts' (0x226) and story scripts too, which keep their pace.
constexpr uint32_t kWarpScript = 0x004821E0u;
constexpr int32_t kScriptTransporterLeave = 0x320;
constexpr int32_t kScriptTransporterArrive = 0x321;

// The main menu's state machine (a pointer): +4 the state running, an entry
// of its table whose first word is the state's id.
constexpr uint32_t kMenuMachine = 0x0048235Cu;
// The Revelation Flow's board, each frame of its screen (from
// MenuRevelationBoardState): +0x76E the step of placing items in a cell,
// 0 none; 1 to 8 the zoom on the cell, the items going in, its glow, the
// ability won (6 waits for the player).
constexpr uint32_t kRevelationBoardUpdate = 0x0034C290u;
constexpr uint32_t kBoardPlacing = 0x76Eu;
constexpr int16_t kPlacingWaits = 6;

std::array<std::atomic<bool>, 3> g_enabled{};
PS2Runtime::RecompiledFunction g_revelationBoardUpdate = nullptr;
// When the board last ran with items going in, on the steady clock (the
// game thread writes, the main thread reads).
std::atomic<int64_t> g_placingAt{0};
bool g_trace = false;
uint32_t g_lastMode = ~0u;
int32_t g_lastPhase = 0;
uint32_t g_lastMenuState = ~0u;

double seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

uint32_t word(const uint8_t *rdram, uint32_t address)
{
    address &= 0x1FFFFFFFu;
    if (address == 0u || address + 4u > PS2_RAM_SIZE)
    {
        return 0u;
    }
    uint32_t value = 0u;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

void traceMenu(const uint8_t *rdram)
{
    const uint32_t entry = word(rdram, word(rdram, kMenuMachine) + 4u);
    const uint32_t state = entry ? word(rdram, entry) : 0u;
    if (state != g_lastMenuState)
    {
        std::fprintf(stderr, "[scene] %.2f s: menu state %#x\n", seconds(), state);
        g_lastMenuState = state;
    }
}

bool on(Scene scene)
{
    return g_enabled[static_cast<size_t>(scene)].load(std::memory_order_relaxed);
}

int64_t nowNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void revelationBoardUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t board = getRegU32(ctx, 4) & 0x1FFFFFFFu;
    if (board != 0u && board + kBoardPlacing + 2u <= PS2_RAM_SIZE)
    {
        int16_t step = 0;
        std::memcpy(&step, rdram + board + kBoardPlacing, sizeof(step));
        if (step != 0 && step != kPlacingWaits)
        {
            g_placingAt.store(nowNanos(), std::memory_order_relaxed);
        }
        static int16_t lastStep = 0;
        if (g_trace && step != lastStep)
        {
            std::fprintf(stderr, "[scene] %.2f s: Revelation Flow placing step %d\n", seconds(), step);
        }
        lastStep = step;
    }
    g_revelationBoardUpdate(rdram, ctx, runtime);
}

// Items went into a cell within the last tenth of a second.
bool placingItems()
{
    return nowNanos() - g_placingAt.load(std::memory_order_relaxed) < 100'000'000;
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *trace = std::getenv("RG_SCENE_TRACE");
    g_trace = trace && *trace && *trace != '0';
    if (!rg::timing::hookFunction(runtime, kRevelationBoardUpdate, &revelationBoardUpdate, g_revelationBoardUpdate))
    {
        std::fprintf(stderr, "[rg] the Revelation Flow keeps its pace\n");
    }
}

void setEnabled(Scene scene, bool enabled)
{
    g_enabled[static_cast<size_t>(scene)].store(enabled, std::memory_order_relaxed);
}

double speed(const uint8_t *rdram)
{
    uint32_t mode = 0u;
    int32_t phase = 0;
    std::memcpy(&mode, rdram + kGameMode, sizeof(mode));
    std::memcpy(&phase, rdram + kGamePhase, sizeof(phase));
    if (g_trace && (mode != g_lastMode || phase != g_lastPhase))
    {
        std::fprintf(stderr, "[scene] %.2f s: mode %u phase %d\n", seconds(), mode, phase);
        g_lastMode = mode;
        g_lastPhase = phase;
    }
    if (g_trace)
    {
        traceMenu(rdram);
    }
    bool hurry = false;
    switch (mode)
    {
    case kModeTreasureBox:
        hurry = on(Scene::Chests);
        break;
    case kModeTransporterWarp:
    {
        int32_t script = 0;
        std::memcpy(&script, rdram + kWarpScript, sizeof(script));
        hurry = (script == kScriptTransporterLeave || script == kScriptTransporterArrive) && on(Scene::Transporter);
        break;
    }
    default:
        hurry = placingItems() && on(Scene::Revelation);
        break;
    }
    return hurry ? kFast : 1.0;
}

} // namespace rg::quickscenes
