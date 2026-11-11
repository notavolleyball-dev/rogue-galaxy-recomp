#include "hud_size.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "overlay/overlay_manager.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"
#include "native_blocks.h"

namespace rg::hudsize
{
namespace
{

constexpr uint32_t kGfxEndFrame = 0x00141C20u;

struct Range
{
    uint32_t start, end;
};
constexpr Range kCompassHudRender{0x00370FB0u, 0x00371470u};
// The transporter menu's markers (the normal overlay's), drawn while it is
// up: its map and compass are inside its panel then.
constexpr Range kTransMenuMarkersDraw{0x005174D0u, 0x00517C88u};
// MinimapDrawEntry's blocks drawn to the frame: the map's view and the
// player's arrow (its first, from 0x325944, draws the map's tiles into a
// texture of its own, which stays).
constexpr uint32_t kMinimapViewDraw = 0x003279F4u;
constexpr uint32_t kMinimapArrowDraw = 0x00328AE0u;
// ViewSetScreenMatrix(view, params): CompassHudRender's own centre, x at
// params+0x24 (1866.9, from 0x47D830) and y at +0x28 (2212.2), its scale at
// +0x14 and +0x18, before its 3D dial.
constexpr uint32_t kViewSetScreenMatrix = 0x001562B0u;
constexpr uint32_t kCompassCentreX = 0x0047D830u;
constexpr uint32_t kScreenParamsScaleX = 0x14u;
constexpr uint32_t kScreenParamsScaleY = 0x18u;
constexpr uint32_t kScreenParamsCentreX = 0x24u;
constexpr uint32_t kScreenParamsCentreY = 0x28u;
// The compass's corner, from its centre: its letters reach 46 pixels left
// and 44 down.
constexpr float kCompassLeft = 46.0f;
constexpr float kCompassDown = 44.0f;
// The map view's width past which it is not shrunk (a map of the area's
// own, larger than the field's), in pixels.
constexpr float kLargestMinimap = 240.0f;

PS2Runtime::RecompiledFunction g_endFrameOriginal = nullptr;
PS2Runtime::RecompiledFunction g_screenMatrixOriginal = nullptr;

std::atomic<int> g_minimapPercent{100};
std::atomic<int> g_compassPercent{100};

// The transporter menu's markers drawn in this frame, and in the last.
bool g_transMenuDrawn = false;
bool g_transMenuShown = false;

enum class Hud : uint8_t
{
    None,
    MinimapView,
    MinimapArrow,
    CompassLetters,
};

// The map view's corner its blocks shrink toward (game thread).
bool g_minimapAnchored = false;
float g_minimapAnchorX = 0.0f, g_minimapAnchorY = 0.0f;

bool inside(Range range, uint32_t address)
{
    return address >= range.start && address < range.end;
}

template <typename T>
bool load(uint8_t *rdram, uint32_t address, T &out)
{
    const uint8_t *p = getConstMemPtr(rdram, address);
    if (!p)
    {
        return false;
    }
    std::memcpy(&out, p, sizeof(T));
    return true;
}

template <typename T>
void store(uint8_t *rdram, uint32_t address, T value)
{
    if (uint8_t *p = getMemPtr(rdram, address))
    {
        std::memcpy(p, &value, sizeof(T));
    }
}

Hud hudOf(uint32_t owner)
{
    if (g_transMenuShown)
    {
        return Hud::None;
    }
    if (g_minimapPercent.load(std::memory_order_relaxed) < 100)
    {
        if (owner == kMinimapViewDraw)
        {
            return Hud::MinimapView;
        }
        if (owner == kMinimapArrowDraw)
        {
            return Hud::MinimapArrow;
        }
    }
    if (g_compassPercent.load(std::memory_order_relaxed) < 100 && inside(kCompassHudRender, owner))
    {
        return Hud::CompassLetters;
    }
    return Hud::None;
}

// Each of a piece's quads (its triangles (b, b+1, b+3), (b, b+3, b+2), as
// the converter makes a sprite) about its own centre.
void scaleQuads(rg::NativeMesh &mesh, float scale)
{
    std::vector<rg::NativeMesh::Vertex> &v = mesh.vertices;
    for (size_t b = 0; b + 4u <= v.size(); b += 4u)
    {
        const float cx = (rg::nativeblocks::vertexX(mesh, v[b]) + rg::nativeblocks::vertexX(mesh, v[b + 3u])) * 0.5f;
        const float cy = (rg::nativeblocks::vertexY(mesh, v[b]) + rg::nativeblocks::vertexY(mesh, v[b + 3u])) * 0.5f;
        rg::NativeMesh one;
        one.header = mesh.header;
        one.vertices.assign(v.begin() + b, v.begin() + b + 4u);
        rg::nativeblocks::transform(one, scale, scale, cx * (1.0f - scale), cy * (1.0f - scale));
        std::copy(one.vertices.begin(), one.vertices.end(), v.begin() + b);
    }
}

void gfxEndFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_transMenuShown = g_transMenuDrawn;
    g_transMenuDrawn = false;
    g_endFrameOriginal(rdram, ctx, runtime);
}

void viewSetScreenMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t params = getRegU32(ctx, 5);
    const int percent = g_compassPercent.load(std::memory_order_relaxed);
    float centre = 0.0f, compass = 0.0f;
    // The compass's own view, not the scene's put back after it.
    if (percent < 100 && !g_transMenuShown && inside(kCompassHudRender, getRegU32(ctx, 31)) &&
        load(rdram, params + kScreenParamsCentreX, centre) && load(rdram, kCompassCentreX, compass) &&
        centre == compass)
    {
        // Its scale smaller, and its centre in toward its corner.
        const float scale = percent / 100.0f;
        float scaleX = 0.0f, scaleY = 0.0f, centreY = 0.0f;
        if (load(rdram, params + kScreenParamsScaleX, scaleX) && load(rdram, params + kScreenParamsScaleY, scaleY) &&
            load(rdram, params + kScreenParamsCentreY, centreY))
        {
            store(rdram, params + kScreenParamsScaleX, scaleX * scale);
            store(rdram, params + kScreenParamsScaleY, scaleY * scale);
            store(rdram, params + kScreenParamsCentreX, centre - kCompassLeft * (1.0f - scale));
            store(rdram, params + kScreenParamsCentreY, centreY + kCompassDown * (1.0f - scale));
        }
    }
    g_screenMatrixOriginal(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_MINIMAP_SIZE"))
    {
        setMinimapSize(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_COMPASS_SIZE"))
    {
        setCompassSize(std::atoi(value));
    }
    const bool complete =
        rg::timing::hookFunction(runtime, kGfxEndFrame, &gfxEndFrame, g_endFrameOriginal) &&
        rg::timing::hookFunction(runtime, kViewSetScreenMatrix, &viewSetScreenMatrix, g_screenMatrixOriginal);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] minimap and compass sizes: some hooks are missing, see above\n");
    }
}

void reshape(uint32_t owner, std::vector<rg::nativeblocks::BlockMesh> &meshes)
{
    if (inside(kTransMenuMarkersDraw, owner) && std::strcmp(rg::overlay::residentName(), "normal") == 0)
    {
        g_transMenuDrawn = true;
    }
    const Hud hud = hudOf(owner);
    if (hud == Hud::None || meshes.empty())
    {
        return;
    }
    if (hud == Hud::CompassLetters)
    {
        // Each letter about its own centre: the dial's smaller view has
        // already brought their centres in.
        const float scale = g_compassPercent.load(std::memory_order_relaxed) / 100.0f;
        for (rg::nativeblocks::BlockMesh &m : meshes)
            scaleQuads(*m.mesh, scale);
        return;
    }
    const float scale = g_minimapPercent.load(std::memory_order_relaxed) / 100.0f;
    if (hud == Hud::MinimapView)
    {
        // Toward the view's bottom right corner (and the arrow after it).
        float left = 1e30f, right = -1e30f, bottom = -1e30f;
        for (const rg::nativeblocks::BlockMesh &m : meshes)
            for (const rg::NativeMesh::Vertex &v : m.mesh->vertices)
            {
                const float x = rg::nativeblocks::vertexX(*m.mesh, v), y = rg::nativeblocks::vertexY(*m.mesh, v);
                left = std::min(left, x);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        g_minimapAnchored = left <= right && right - left <= kLargestMinimap;
        g_minimapAnchorX = right;
        g_minimapAnchorY = bottom;
    }
    if (!g_minimapAnchored)
    {
        return;
    }
    for (rg::nativeblocks::BlockMesh &m : meshes)
        rg::nativeblocks::transform(*m.mesh, scale, scale, g_minimapAnchorX * (1.0f - scale),
                                    g_minimapAnchorY * (1.0f - scale));
}

void setMinimapSize(int percent)
{
    g_minimapPercent.store(std::clamp(percent, 50, 100), std::memory_order_relaxed);
}

void setCompassSize(int percent)
{
    g_compassPercent.store(std::clamp(percent, 50, 100), std::memory_order_relaxed);
}

} // namespace rg::hudsize
