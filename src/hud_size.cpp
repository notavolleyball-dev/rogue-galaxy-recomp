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

namespace rg::hudsize
{
namespace
{

constexpr uint32_t kDrawListBeginPrim = 0x00141B00u;
constexpr uint32_t kDrawListEndPrim = 0x00141A20u;
constexpr uint32_t kGfxEndFrame = 0x00141C20u;
// The packet a draw list's group is written through, DrawListBeginPrim's
// answer.
constexpr uint32_t kListPacket = 0x130u;
constexpr uint32_t kPacketPrim = 0x70u; // PRIM's staged value, its type in the low 3 bits
constexpr uint32_t kPacketWrite = 0xF4u;
constexpr uint8_t kPrimSprite = 6u;

struct Range
{
    uint32_t start, end;
};
constexpr Range kCompassHudRender{0x00370FB0u, 0x00371470u};
// The transporter menu's markers (the normal overlay's), drawn while it is
// up: its map and compass are inside its panel then.
constexpr Range kTransMenuMarkersDraw{0x005174D0u, 0x00517C88u};
// MinimapDrawEntry's groups drawn to the frame: the map's view and the
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
// The map view's bottom right corner (12.4), toward which it and the
// player's arrow after it shrink; none while the map is larger than the
// field's (a map of the area's own).
constexpr int32_t kLargestMinimap = 240 * 16;

// The setters that write a vertex's XYZ2 (A+D) at the packet's write
// pointer.
constexpr uint32_t kSetters[] = {
    0x0013F110u, // GsPktSetXYZPixel(packet, x, y, z)
    0x0013EF70u, // GsPktSetXYZPixelVec(packet, int xyz[])
    0x0013EF10u, // GsPktSetXYZ(packet, x, y, z), 12.4
    0x0013F060u, // GsPktSetXYZf(packet, f12 x, f13 y, f14 z)
    0x0013EEB0u, // GsPktSetXYZVecI(packet, int xyz[]), 12.4
    0x0013EFE0u, // GsPktSetXYZfVec(packet, float xyz[])
};
constexpr size_t kSetterCount = sizeof(kSetters) / sizeof(kSetters[0]);

PS2Runtime::RecompiledFunction g_beginOriginal = nullptr;
PS2Runtime::RecompiledFunction g_endOriginal = nullptr;
PS2Runtime::RecompiledFunction g_endFrameOriginal = nullptr;
PS2Runtime::RecompiledFunction g_screenMatrixOriginal = nullptr;
PS2Runtime::RecompiledFunction g_setterOriginals[kSetterCount] = {};

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

// The group open, on the game's thread.
struct Vertex
{
    uint32_t address; // of its XYZ2 data
    uint8_t prim;
};
bool g_open = false;
Hud g_hud = Hud::None;
uint32_t g_packet = 0u;
std::vector<Vertex> g_vertices;
bool g_minimapAnchored = false;
int32_t g_minimapAnchorX = 0, g_minimapAnchorY = 0;

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

uint32_t word(uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0u;
    load(rdram, address, value);
    return value;
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

// A vertex's x and y where the GS draws them, 12.4.
bool readXY(uint8_t *rdram, const Vertex &v, int32_t &x, int32_t &y)
{
    uint32_t field = 0u;
    if (!load(rdram, v.address, field))
    {
        return false;
    }
    x = static_cast<int32_t>(field & 0xFFFFu);
    y = static_cast<int32_t>(field >> 16);
    return true;
}

void writeXY(uint8_t *rdram, const Vertex &v, int32_t x, int32_t y)
{
    if (x >= 0 && x <= 0xFFFF && y >= 0 && y <= 0xFFFF)
    {
        store(rdram, v.address, static_cast<uint32_t>(x) | static_cast<uint32_t>(y) << 16);
    }
}

int32_t towards(int32_t value, int32_t anchor, float scale)
{
    return anchor + static_cast<int32_t>(std::lround((value - anchor) * scale));
}

void scaleGroup(uint8_t *rdram)
{
    if (g_hud == Hud::CompassLetters)
    {
        // Each letter about its own centre: the dial's smaller view has
        // already brought their centres in.
        const float scale = g_compassPercent.load(std::memory_order_relaxed) / 100.0f;
        for (size_t i = 0; i + 1 < g_vertices.size(); i += 2u)
        {
            const Vertex &a = g_vertices[i], &b = g_vertices[i + 1];
            int32_t xa = 0, ya = 0, xb = 0, yb = 0;
            if (a.prim != kPrimSprite || b.prim != kPrimSprite || !readXY(rdram, a, xa, ya) ||
                !readXY(rdram, b, xb, yb))
            {
                continue;
            }
            const int32_t cx = (xa + xb) / 2, cy = (ya + yb) / 2;
            writeXY(rdram, a, towards(xa, cx, scale), towards(ya, cy, scale));
            writeXY(rdram, b, towards(xb, cx, scale), towards(yb, cy, scale));
        }
        return;
    }
    const float scale = g_minimapPercent.load(std::memory_order_relaxed) / 100.0f;
    if (g_hud == Hud::MinimapView)
    {
        int32_t left = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
        for (const Vertex &v : g_vertices)
        {
            int32_t x = 0, y = 0;
            if (readXY(rdram, v, x, y))
            {
                left = std::min(left, x);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
        g_minimapAnchored = left <= right && right - left <= kLargestMinimap;
        g_minimapAnchorX = right;
        g_minimapAnchorY = bottom;
    }
    if (!g_minimapAnchored)
    {
        return;
    }
    // Each vertex once: a strip's are its own, but a sprite's may come twice.
    std::vector<uint32_t> done;
    done.reserve(g_vertices.size());
    for (const Vertex &v : g_vertices)
    {
        int32_t x = 0, y = 0;
        if (std::find(done.begin(), done.end(), v.address) != done.end() || !readXY(rdram, v, x, y))
        {
            continue;
        }
        done.push_back(v.address);
        writeXY(rdram, v, towards(x, g_minimapAnchorX, scale), towards(y, g_minimapAnchorY, scale));
    }
}

void closeGroup(uint8_t *rdram)
{
    if (!g_open)
    {
        return;
    }
    g_open = false;
    if (g_hud != Hud::None)
    {
        scaleGroup(rdram);
    }
    g_vertices.clear();
}

void beginPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    closeGroup(rdram);
    const uint32_t owner = getRegU32(ctx, 31);
    if (inside(kTransMenuMarkersDraw, owner) && std::strcmp(rg::overlay::residentName(), "normal") == 0)
    {
        g_transMenuDrawn = true;
    }
    g_open = true;
    g_packet = getRegU32(ctx, 4) + kListPacket;
    g_hud = hudOf(owner);
    g_beginOriginal(rdram, ctx, runtime);
}

void endPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_open && getRegU32(ctx, 4) + kListPacket == g_packet)
    {
        closeGroup(rdram);
    }
    g_endOriginal(rdram, ctx, runtime);
}

template <size_t Index>
void setter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Only the groups to scale keep their vertices.
    if (g_hud != Hud::None && g_open && getRegU32(ctx, 4) == g_packet)
    {
        uint8_t prim = 0u;
        load(rdram, g_packet + kPacketPrim, prim);
        g_vertices.push_back({word(rdram, g_packet + kPacketWrite), static_cast<uint8_t>(prim & 7u)});
    }
    g_setterOriginals[Index](rdram, ctx, runtime);
}

void gfxEndFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // A group left open is scaled now, before the frame goes to the VIF1
    // thread, which reads it from then on.
    closeGroup(rdram);
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

template <size_t... Index>
bool hookSetters(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    return (rg::timing::hookFunction(runtime, kSetters[Index], &setter<Index>, g_setterOriginals[Index]) && ...);
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
    g_vertices.reserve(1024);
    const bool complete =
        rg::timing::hookFunction(runtime, kDrawListBeginPrim, &beginPrim, g_beginOriginal) &&
        rg::timing::hookFunction(runtime, kDrawListEndPrim, &endPrim, g_endOriginal) &&
        rg::timing::hookFunction(runtime, kGfxEndFrame, &gfxEndFrame, g_endFrameOriginal) &&
        rg::timing::hookFunction(runtime, kViewSetScreenMatrix, &viewSetScreenMatrix, g_screenMatrixOriginal) &&
        hookSetters(runtime, std::make_index_sequence<kSetterCount>{});
    if (!complete)
    {
        std::fprintf(stderr, "[rg] minimap and compass sizes: some hooks are missing, see above\n");
    }
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
