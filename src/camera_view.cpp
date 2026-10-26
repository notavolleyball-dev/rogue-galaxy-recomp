#include "camera_view.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::cameraview
{
namespace
{

constexpr uint32_t kViewSetProjection = 0x00143000u;
constexpr uint32_t kCameraSetPresetParams = 0x00309100u;
constexpr uint32_t kCameraSetDefaultParams = 0x00309260u;
constexpr uint32_t kCameraOrbitControl = 0x00161260u;
// The sticks, left and right and up and down (PadGetAnalogLXNorm, LYNorm,
// RXNorm and RYNorm), and the calls to them of the cameras that turn by a
// stick a frame's worth each frame, by where they return to. The battle
// camera locked on a target (CameraModeLockTargetUpdate) turns by the right
// stick's left and right; its up and down zooms past a threshold and stays
// as it is. The one following it (CameraTargetFollowUpdate) turns by the
// same; its tilt eases toward the right stick's up and down times 15
// degrees (+0x40 of its parameters), a tilt the stick holds whatever the
// frame rate, so it stays as it is too. The first-person view
// (CameraFirstPersonUpdate, camera mode 1, R3 exploring or in battle)
// turns and tilts by both sticks added.
constexpr uint32_t kPadReads[] = {0x00183AF0u, 0x00183A50u, 0x00183C30u, 0x00183B90u};
constexpr uint32_t kTurnReturns[] = {
    0x003065E0u, // CameraModeLockTargetUpdate: the right stick's left and right
    0x003073ACu, // CameraTargetFollowUpdate: the same
    0x00307F98u, // CameraFirstPersonUpdate: the left stick's left and right,
    0x00307FA8u, // the right stick's,
    0x00307FB8u, // the left stick's up and down
    0x00307FC8u, // and the right stick's
};
// The block CameraSetDefaultParams writes the distances into, in the camera.
constexpr uint32_t kDistanceBlock = 0x15Cu;
constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kModeField = 6u;
constexpr uint32_t kModeBattle = 7u;
constexpr int kGameFieldOfView = 52;
constexpr float kPi = 3.14159265358979f;

PS2Runtime::RecompiledFunction g_viewSetProjection = nullptr;
PS2Runtime::RecompiledFunction g_cameraSetPresetParams = nullptr;
PS2Runtime::RecompiledFunction g_cameraSetDefaultParams = nullptr;
PS2Runtime::RecompiledFunction g_cameraOrbitControl = nullptr;
PS2Runtime::RecompiledFunction g_padReads[std::size(kPadReads)] = {};
std::atomic<int> g_fieldOfView{kGameFieldOfView};
std::atomic<int> g_distance{100};
std::atomic<bool> g_fasterTurn{false};

// A camera's distances as the game last wrote them, and the percentage of
// them its block holds now. The game keeps two cameras (0x4B5950 and
// 0x4B6220); the game thread's alone.
struct Distances
{
    uint32_t block = 0u;
    float base[2] = {};
    int applied = 100;
};
std::array<Distances, 4> g_cameras{};
size_t g_nextCamera = 0u;

void writeDistances(uint8_t *rdram, Distances &camera, int percent)
{
    float distances[2];
    for (int index = 0; index < 2; ++index)
    {
        distances[index] = camera.base[index] * static_cast<float>(percent) / 100.0f;
    }
    std::memcpy(rdram + camera.block, distances, sizeof(distances));
    camera.applied = percent;
}

// The game has just written a camera's nearest (+0x0) and farthest (+0x4)
// distances at `block`: they are kept as its base and scaled.
void distancesWritten(uint8_t *rdram, uint32_t block)
{
    block &= 0x1FFFFFFFu;
    if (block == 0u || block + 8u > PS2_RAM_SIZE)
    {
        return;
    }
    Distances *camera = nullptr;
    for (Distances &known : g_cameras)
    {
        if (known.block == block)
        {
            camera = &known;
        }
    }
    if (!camera)
    {
        camera = &g_cameras[g_nextCamera];
        g_nextCamera = (g_nextCamera + 1u) % g_cameras.size();
        camera->block = block;
    }
    std::memcpy(camera->base, rdram + block, sizeof(camera->base));
    writeDistances(rdram, *camera, g_distance.load(std::memory_order_relaxed));
}

// The camera distance moved in the menu: the cameras take it now. A block
// still holding what was last written says it is the same camera; one the
// game has let go or set again is forgotten.
void followDistance(uint8_t *rdram, uint32_t mode)
{
    const int percent = g_distance.load(std::memory_order_relaxed);
    if (mode != kModeField && mode != kModeBattle)
    {
        return;
    }
    for (Distances &camera : g_cameras)
    {
        if (camera.block == 0u || camera.applied == percent)
        {
            continue;
        }
        float distances[2];
        std::memcpy(distances, rdram + camera.block, sizeof(distances));
        bool same = true;
        for (int index = 0; index < 2; ++index)
        {
            const float expected = camera.base[index] * static_cast<float>(camera.applied) / 100.0f;
            same = same && std::fabs(distances[index] - expected) <= 0.01f;
        }
        if (same)
        {
            writeDistances(rdram, camera, percent);
        }
        else
        {
            camera.block = 0u;
        }
    }
}

// FovHorizToVert's: the angle up a 4:3 picture whose width spans `degrees`.
float vertical(int degrees)
{
    return 2.0f * std::atan(std::tan(static_cast<float>(degrees) * kPi / 360.0f) * 0.75f);
}

void viewSetProjection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    uint32_t mode = 0u;
    std::memcpy(&mode, rdram + kGameMode, sizeof(mode));
    const int degrees = g_fieldOfView.load(std::memory_order_relaxed);
    if (degrees != kGameFieldOfView)
    {
        static const float game = vertical(kGameFieldOfView);
        if ((mode == kModeField || mode == kModeBattle) && std::fabs(ctx->f[12] - game) < 1e-3f)
        {
            ctx->f[12] = vertical(degrees);
        }
    }
    followDistance(rdram, mode);
    g_viewSetProjection(rdram, ctx, runtime);
}

// The battle camera's: 150 to 190 up, 120 to 160 down.
void cameraSetPresetParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t block = getRegU32(ctx, 4);
    g_cameraSetPresetParams(rdram, ctx, runtime);
    distancesWritten(rdram, block);
}

// Every camera's first, the field's for good: 65 to 120.
void cameraSetDefaultParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t camera = getRegU32(ctx, 4);
    g_cameraSetDefaultParams(rdram, ctx, runtime);
    distancesWritten(rdram, camera + kDistanceBlock);
}

// What a frame's turn by the stick is multiplied by: the frame's share of a
// step in the 60 fps mode (a half at full rate), so the camera turns at the
// console's speed, and twice that with "Faster camera rotation".
float turnFactor(const uint8_t *rdram, const R5900Context *ctx)
{
    float factor = g_fasterTurn.load(std::memory_order_relaxed) ? 2.0f : 1.0f;
    if (rg::timing::sixtyFps() && rg::timing::stepFix(rg::timing::StepFix::Camera))
    {
        // The frame's share alone, as the free camera takes it: the game's
        // slow motion (the step's base) never slowed the camera's turn.
        factor *= std::min(rg::timing::sceneScale(rdram, ctx), 1.0f);
    }
    return factor;
}

// The field camera's turn this frame: a1 points at the stick's turn and
// tilt (two floats), which it adds as they are.
void cameraOrbitControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const float factor = turnFactor(rdram, ctx);
    const uint32_t input = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    if (factor != 1.0f && input != 0u && input + 8u <= PS2_RAM_SIZE)
    {
        float turn[2];
        std::memcpy(turn, rdram + input, sizeof(turn));
        for (float &value : turn)
        {
            value *= factor;
        }
        std::memcpy(rdram + input, turn, sizeof(turn));
    }
    g_cameraOrbitControl(rdram, ctx, runtime);
}

// A stick's push (f0), when a camera reads it to turn: the reads are leaves
// and return to the hook.
template <size_t Index>
void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_padReads[Index](rdram, ctx, runtime);
    for (uint32_t site : kTurnReturns)
    {
        if (returnPc == site)
        {
            ctx->f[0] *= turnFactor(rdram, ctx);
            break;
        }
    }
}

template <size_t... Index>
bool installPadReads(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    bool complete = true;
    ((complete &= rg::timing::hookFunction(runtime, kPadReads[Index], &padRead<Index>, g_padReads[Index])), ...);
    return complete;
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_FIELD_OF_VIEW"))
    {
        setFieldOfView(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_CAMERA_DISTANCE"))
    {
        setDistance(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_FASTER_CAMERA"))
    {
        setFasterTurn(*value && *value != '0');
    }
    if (!rg::timing::hookFunction(runtime, kViewSetProjection, &viewSetProjection, g_viewSetProjection))
    {
        std::fprintf(stderr, "[rg] the field of view stays the game's\n");
    }
    if (!rg::timing::hookFunction(runtime, kCameraSetPresetParams, &cameraSetPresetParams, g_cameraSetPresetParams) ||
        !rg::timing::hookFunction(runtime, kCameraSetDefaultParams, &cameraSetDefaultParams, g_cameraSetDefaultParams))
    {
        std::fprintf(stderr, "[rg] the camera distance stays the game's\n");
    }
    if (!rg::timing::hookFunction(runtime, kCameraOrbitControl, &cameraOrbitControl, g_cameraOrbitControl) ||
        !installPadReads(runtime, std::make_index_sequence<std::size(kPadReads)>{}))
    {
        std::fprintf(stderr, "[rg] the camera turns at the game's speed\n");
    }
}

void setFieldOfView(int degrees)
{
    if (degrees > 0)
    {
        g_fieldOfView.store(degrees, std::memory_order_relaxed);
    }
}

void setDistance(int percent)
{
    if (percent > 0)
    {
        g_distance.store(percent, std::memory_order_relaxed);
    }
}

void setFasterTurn(bool on)
{
    g_fasterTurn.store(on, std::memory_order_relaxed);
}

} // namespace rg::cameraview
