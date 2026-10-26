#include "free_camera.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::freecamera
{
namespace
{

constexpr uint32_t kCameraModeUpdate = 0x00309770u;
// CameraModeDebugFreeUpdate, which tail-jumps to CameraDebugFreeUpdate
// (0x305C60): a `j` the generated code makes a direct call, past the table.
constexpr uint32_t kCameraModeDebugFreeUpdate = 0x00307AF0u;
constexpr uint32_t kScePadRead = 0x0012B800u;
constexpr uint32_t kModeOverride = 0x26Cu;
constexpr int8_t kFreeMode = 13;
constexpr int8_t kNoOverride = -1;
// The game's mode: exploring (6) or fighting (7); its menus are others.
constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kModeField = 6u;
constexpr uint32_t kModeBattle = 7u;
// Start in the pad report's first button byte, active low.
constexpr uint8_t kReportStart = 0x08u;
// The pad objects, and in each the state PadUpdate (0x1843A0) keeps: this
// frame's and the last (buttons, then the sticks at +0x8 to +0x14, ...).
constexpr uint32_t kPad1 = 0x004B2570u;
constexpr uint32_t kPad2 = 0x004B27E0u;
constexpr uint32_t kPadState = 0x4u;
constexpr uint32_t kPadStateSize = 0xB0u;
constexpr uint32_t kPadSticks = 0x8u;
// PadGetAnalog*Norm's dead zone: 50 either side of the centre counts 0.
constexpr int32_t kStickCentre = 0x80;
constexpr int32_t kDeadZone = 49;

PS2Runtime::RecompiledFunction g_cameraModeUpdate = nullptr;
PS2Runtime::RecompiledFunction g_cameraModeDebugFreeUpdate = nullptr;
PS2Runtime::RecompiledFunction g_scePadRead = nullptr;
std::atomic<bool> g_enabled{false};
// The camera whose override is ours, and whether it held 13 at its last
// update while exploring or fighting; then whether pad 1 stays untouched
// until the controller lets go of every button, so what ended the free
// camera does not reach the game, and Start's state at the last read (the
// game thread's alone).
uint32_t g_camera = 0u;
bool g_active = false;
bool g_holdPad = false;
bool g_startDown = false;
// Pad 1's state while the camera's update has pad 2's.
uint8_t g_pad1[kPadStateSize];

int8_t modeOverride(const uint8_t *rdram, uint32_t camera)
{
    camera &= 0x1FFFFFFFu;
    if (camera == 0u || camera + kModeOverride >= PS2_RAM_SIZE)
    {
        return kNoOverride;
    }
    return static_cast<int8_t>(rdram[camera + kModeOverride]);
}

void cameraModeUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t camera = getRegU32(ctx, 4) & 0x1FFFFFFFu;
    const bool on = g_enabled.load(std::memory_order_relaxed);
    uint32_t gameMode = 0u;
    std::memcpy(&gameMode, rdram + kGameMode, sizeof(gameMode));
    const bool playing = gameMode == kModeField || gameMode == kModeBattle;
    int8_t mode = modeOverride(rdram, camera);
    if (camera != 0u && camera + kModeOverride < PS2_RAM_SIZE)
    {
        // The override written as CameraSetModeOverride writes it, without
        // the setup it runs then: mode 13's only snaps the camera where it
        // is, and the follow camera, back, catches up with the party on its
        // own.
        if (on && playing && mode != kFreeMode)
        {
            mode = kFreeMode;
            g_camera = camera;
            rdram[camera + kModeOverride] = static_cast<uint8_t>(mode);
        }
        else if (!on && camera == g_camera && mode == kFreeMode)
        {
            mode = kNoOverride;
            g_camera = 0u;
            rdram[camera + kModeOverride] = static_cast<uint8_t>(mode);
        }
    }
    const bool wasActive = g_active;
    g_active = on && playing && camera == g_camera && mode == kFreeMode;
    if (wasActive && !g_active)
    {
        g_holdPad = true;
    }
    g_cameraModeUpdate(rdram, ctx, runtime);
}

// Past the dead zone, `scale` of the stick's push.
int32_t scaleStick(int32_t raw, float scale)
{
    const int32_t push = raw - kStickCentre;
    if (push > kDeadZone)
    {
        return kStickCentre + kDeadZone + static_cast<int32_t>(static_cast<float>(push - kDeadZone) * scale);
    }
    if (push < -kDeadZone - 1)
    {
        return kStickCentre - kDeadZone - 1 + static_cast<int32_t>(static_cast<float>(push + kDeadZone + 1) * scale);
    }
    return raw;
}

void restorePad1(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    std::memcpy(rdram + kPad1 + kPadState, g_pad1, kPadStateSize);
}

void cameraModeDebugFreeUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_active || (getRegU32(ctx, 4) & 0x1FFFFFFFu) != g_camera)
    {
        g_cameraModeDebugFreeUpdate(rdram, ctx, runtime);
        return;
    }
    std::memcpy(g_pad1, rdram + kPad1 + kPadState, kPadStateSize);
    std::memcpy(rdram + kPad1 + kPadState, rdram + kPad2 + kPadState, kPadStateSize);
    if (rg::timing::sixtyFps())
    {
        for (uint32_t stick = 0u; stick < 4u; ++stick)
        {
            int32_t raw = 0;
            uint8_t *at = rdram + kPad1 + kPadSticks + stick * 4u;
            std::memcpy(&raw, at, sizeof(raw));
            raw = scaleStick(raw, 0.5f);
            std::memcpy(at, &raw, sizeof(raw));
        }
    }
    rg::hooks::callThen(g_cameraModeDebugFreeUpdate, rdram, ctx, runtime, &restorePad1, 0u);
}

void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t port = getRegU32(ctx, 4);
    const uint32_t data = getRegU32(ctx, 6) & 0x1FFFFFFFu;
    g_scePadRead(rdram, ctx, runtime);
    if (port != 0u || data == 0u || data + 8u > PS2_RAM_SIZE)
    {
        return;
    }
    // The pad report: buttons, active low, then the four sticks.
    uint8_t *report = rdram + data;
    const bool startDown = (report[2] & kReportStart) == 0u;
    if (g_active && startDown && !g_startDown)
    {
        // Start leaves the free camera.
        setEnabled(false);
        g_holdPad = true;
    }
    g_startDown = startDown;
    if (g_holdPad && report[2] == 0xFFu && report[3] == 0xFFu)
    {
        g_holdPad = false;
    }
    if (g_active || g_holdPad)
    {
        std::memset(report + 2u, 0xFF, 2u);
        std::memset(report + 4u, kStickCentre, 4u);
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    // RG_FREE_CAMERA=1 starts with it on.
    const char *on = std::getenv("RG_FREE_CAMERA");
    if (on && *on && *on != '0')
    {
        setEnabled(true);
    }
    const bool complete =
        rg::timing::hookFunction(runtime, kCameraModeUpdate, &cameraModeUpdate, g_cameraModeUpdate) &&
        rg::timing::hookFunction(runtime, kCameraModeDebugFreeUpdate, &cameraModeDebugFreeUpdate,
                                 g_cameraModeDebugFreeUpdate) &&
        rg::timing::hookFunction(runtime, kScePadRead, &scePadRead, g_scePadRead);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] free camera: some hooks are missing, see above\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

} // namespace rg::freecamera
