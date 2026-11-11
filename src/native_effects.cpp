#include "native_effects.h"
#include "native_emit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "gs_writes.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::nativefx
{
namespace
{

constexpr uint32_t kGlareFilterDraw = 0x0017A0D0u; // GlareFilterDrawMaybe
constexpr uint32_t kPlaneShadowEnd = 0x00146CC0u;   // DrawListEndPlaneShadowPass
// ModelDrawOffscreenCompositeAtMaybe: the composite's only way in (it jumps
// into ModelDrawOffscreenCompositeMaybe_16aac0 past the function table).
constexpr uint32_t kCompositeAt = 0x0016B8A0u;
constexpr uint32_t kModelEmitDrawPackets = 0x0014EC20u; // ModelEmitDrawPacketsMaybe
constexpr uint32_t kOverlayMulti = 0x0016A510u;         // ScreenTexOverlayDrawMultiMaybe
constexpr uint32_t kOverlaySingle = 0x0016A780u;        // ScreenTexOverlayDrawSingleMaybe
constexpr uint32_t kDrawListFrameBegin = 0x001425D0u;
constexpr uint32_t kCBlurTexBlur = 0x001789E0u; // CBlurTexBlurMaybe
constexpr uint32_t kBloom = 0x00179360u;        // BloomExtractBlurMaybe
constexpr uint32_t kDof = 0x0017D150u;          // DofFilterDrawMaybe
constexpr uint32_t kCBlurDraw = 0x001784E0u;    // CBlurDrawMaybe
constexpr uint32_t kRefraction = 0x0013FFD0u;   // RefractionCaptureFrame

// The draw context (GetDrawCtx_15de80 reads it at gp-0x40cc): the frame's
// buffer index at +0 and its memory stacks at +8, each 8 bytes into the
// pointer there, with its base at +0x28 and the quadwords used at +0x2C.
// Every packet of the frame is written in the stack of the buffer being
// built.
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;

uint8_t *g_rdram = nullptr;
bool g_log = false;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

uint32_t read32(uint32_t address)
{
    uint32_t value = 0u;
    if (g_rdram)
    {
        std::memcpy(&value, g_rdram + (physical(address) & ~3u), sizeof(value));
    }
    return value;
}

// The memory stack of the buffer being built, 0 when there is none.
uint32_t frameStack()
{
    const uint32_t ctx = read32(kDrawCtxPointer);
    if (ctx == 0u)
    {
        return 0u;
    }
    const uint32_t stack = read32(ctx + 8u + read32(ctx) * 4u);
    return stack == 0u ? 0u : stack + 8u;
}

uint32_t stackTop()
{
    const uint32_t stack = frameStack();
    return stack == 0u ? 0u : read32(stack + 0x28u) + read32(stack + 0x2Cu) * 16u;
}

std::mutex g_mutex;
// M4's census: every draw task's packet range with the task (its index in
// kDrawTasks), apart from the layers (an outer task would replace an inner
// one's layer there).
struct TaskRange
{
    uint32_t start = 0u, end = 0u;
    uint16_t task = 0u;
};
std::vector<TaskRange> g_taskRanges; // under g_mutex, by start

// A frame begins in a buffer: what the effects wrote there before is some
// other packet's now. (The game waits for the lists of a buffer to finish
// before it builds in it again.)
void dropRanges(uint32_t start, uint32_t end)
{
    start = physical(start);
    end = physical(end);
    std::lock_guard lock(g_mutex);
    g_taskRanges.erase(std::remove_if(g_taskRanges.begin(), g_taskRanges.end(),
                                      [&](const TaskRange &r) { return r.start < end && start < r.end; }),
                       g_taskRanges.end());
}

PS2Runtime::RecompiledFunction g_frameBeginOriginal = nullptr;

void afterFrameBegin(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    // The frame's list starts at the cursor (src/native_emit.h follows the
    // VU1 state it puts).
    if (const uint32_t dctx = read32(kDrawCtxPointer); dctx != 0u)
        rg::nativeemit::listBegun(read32(dctx + 4u));
    // The stack's base and its capacity in quadwords.
    const uint32_t stack = frameStack();
    if (stack != 0u)
    {
        const uint32_t base = read32(stack + 0x28u);
        dropRanges(base, base + read32(stack + 0x34u) * 16u);
    }
}

void drawListFrameBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_frameBeginOriginal, rdram, ctx, runtime, &afterFrameBegin, 0u);
}

// M4's census: a draw task's range (an inner task's, written first, stays:
// only the outer one's own stretches are its).
void addTaskRange(uint32_t start, uint32_t end, uint16_t task)
{
    start = physical(start);
    end = physical(end);
    if (end <= start || end - start > (4u << 20))
        return;
    std::lock_guard lock(g_mutex);
    const TaskRange range{start, end, task};
    g_taskRanges.insert(std::upper_bound(g_taskRanges.begin(), g_taskRanges.end(), range,
                                         [](const TaskRange &x, const TaskRange &y) { return x.start < y.start; }),
                        range);
    if (g_taskRanges.size() > 65536u)
        g_taskRanges.erase(g_taskRanges.begin(), g_taskRanges.begin() + 32768);
}

// The draw tasks (TaskDraw*), each with the layer its 2D is: by what the
// task draws (its name; docs/NATIVE-ONLY-NOTES.md, "Draw tasks and layers").
enum class Layer : uint8_t // native::Layer's values
{
    Scene,
    Hud,
    Menu,
    Text,
    Fade,
    Overlay,
};
struct DrawTask
{
    uint32_t address;
    Layer layer;
};
constexpr DrawTask kDrawTasks[] = {
    {0x001F70F0u, Layer::Scene}, // TaskDrawShadowMap_1f70f0
    {0x001F7360u, Layer::Scene}, // TaskDrawShadow_1f7360
    {0x001F7AB0u, Layer::Scene}, // TaskDrawChara3_1f7ab0
    {0x001F8BA0u, Layer::Scene}, // TaskDrawCharaPre_1f8ba0
    {0x001F9350u, Layer::Scene}, // TaskDrawCharaBlur_1f9350
    {0x001F93A0u, Layer::Hud}, // TaskDrawAttackChain2_1f93a0
    {0x001FB150u, Layer::Menu}, // TaskDrawInsectCage_1fb150
    {0x001FFB00u, Layer::Text}, // TaskDrawCaption_1ffb00
    {0x001FFB60u, Layer::Menu}, // TaskDrawMenuWarp_1ffb60
    {0x001FFCF0u, Layer::Scene}, // TaskDrawLensFlare_1ffcf0
    {0x001FFD20u, Layer::Scene}, // TaskDrawSandStorm_1ffd20
    {0x001FFD50u, Layer::Scene}, // TaskDrawSunLight_1ffd50
    {0x00200BD0u, Layer::Menu}, // TaskDrawTransPorter_200bd0
    {0x00200D10u, Layer::Scene}, // TaskDrawSky_200d10
    {0x00202630u, Layer::Hud}, // TaskDrawIconStealth_202630
    {0x00202DD0u, Layer::Menu}, // TaskDrawMenuInsectResultThunk_202dd0
    {0x00202E90u, Layer::Menu}, // TaskDrawMenuInsectVs_202e90
    {0x002030A0u, Layer::Menu}, // TaskDrawMenuEasyChange_2030a0
    {0x00203180u, Layer::Menu}, // TaskDrawMenuInsectCage_203180
    {0x00203280u, Layer::Menu}, // TaskDrawMenuHelp_203280
    {0x00203360u, Layer::Menu}, // TaskDrawMenuInsectEntry_203360
    {0x00203450u, Layer::Menu}, // TaskDrawMenuCompleteCheck_203450
    {0x00203530u, Layer::Menu}, // TaskDrawMenu7thMystery_203530
    {0x00203620u, Layer::Menu}, // TaskDrawMenuThink_203620
    {0x00203720u, Layer::Menu}, // TaskDrawMenuShop_203720
    {0x00203830u, Layer::Menu}, // TaskDrawMenuTrans_203830
    {0x00204320u, Layer::Menu}, // TaskDrawMenuWin_204320
    {0x002046F0u, Layer::Menu}, // TaskDrawMenuSpace_2046f0
    {0x00204850u, Layer::Menu}, // TaskDrawMenuFactory2_204850
    {0x00204860u, Layer::Menu}, // TaskDrawMenuFactory_204860
    {0x002049D0u, Layer::Menu}, // TaskDrawMenuMain_2049d0
    {0x00204AB0u, Layer::Menu}, // TaskDrawMenuBtlChg_204ab0
    {0x002058B0u, Layer::Hud}, // TaskDrawStatus_2058b0
    {0x0027A8B0u, Layer::Text}, // TaskDrawMessage_27a8b0
    {0x0027AB20u, Layer::Scene}, // TaskDrawEvent4_27ab20
    {0x0027AB30u, Layer::Scene}, // TaskDrawEvent3_27ab30
    {0x0027AB40u, Layer::Scene}, // TaskDrawEvent2_27ab40
    {0x0027AB50u, Layer::Scene}, // TaskDrawEvent1_27ab50
    {0x002CEE40u, Layer::Hud}, // TaskDrawObjectCursor_2cee40
    {0x002CEEA0u, Layer::Menu}, // TaskDrawMonography_2ceea0
    {0x002D85B0u, Layer::Hud}, // TaskDrawStatusEffect_2d85b0
    {0x002D85D0u, Layer::Scene}, // TaskDrawProgEffect_2d85d0
    {0x002D8860u, Layer::Scene}, // TaskDrawEffect_2d8860
    {0x002EE9D0u, Layer::Overlay}, // TaskDrawGrayScaleFilter_2ee9d0
    {0x002F5AD0u, Layer::Menu}, // TaskDrawDiscChange_2f5ad0
    {0x002F5AE0u, Layer::Overlay}, // TaskDrawPauseFilter_2f5ae0
    {0x002F6840u, Layer::Scene}, // TaskDrawFocus_2f6840
    {0x002F6A00u, Layer::Scene}, // TaskDrawDof_2f6a00
    {0x002F6B20u, Layer::Scene}, // TaskDrawGlare_2f6b20
    {0x002F6DA0u, Layer::Scene}, // TaskDrawMapEffect_2f6da0
    {0x002F6E60u, Layer::Fade}, // TaskDrawFade_2f6e60
    {0x003127E0u, Layer::Scene}, // TaskDrawNpc_3127e0
    {0x00313580u, Layer::Hud}, // TaskDrawTargetMng2_313580
    {0x00313590u, Layer::Hud}, // TaskDrawTargetMng1_313590
    {0x003135B0u, Layer::Hud}, // TaskDrawJudgeObject_3135b0
    {0x00319550u, Layer::Menu}, // TaskDrawMenuInsectResult_319550
    {0x00319DC0u, Layer::Menu}, // TaskDrawMenuInsect_319dc0
    {0x00319E40u, Layer::Scene}, // TaskDrawInsect_319e40
    {0x003387A0u, Layer::Text}, // TaskDrawTBox_3387a0
    {0x00343070u, Layer::Overlay}, // TaskDrawDebugAction_343070
    {0x00345D50u, Layer::Hud}, // TaskDrawThinkCircle_345d50
    {0x00367930u, Layer::Menu}, // TaskDrawFactory_367930
};
constexpr size_t kDrawTaskCount = sizeof(kDrawTasks) / sizeof(kDrawTasks[0]);
PS2Runtime::RecompiledFunction g_drawTaskOriginals[kDrawTaskCount] = {};

uint8_t g_currentLayer = 0u; // game thread: the running draw task's

void afterDrawTask(uint8_t *, R5900Context *, PS2Runtime *, uint32_t data)
{
    // data: the task's index, and where its packets began.
    const uint32_t index = data >> 24, top = data & 0xFFFFFFu;
    g_currentLayer = 0u;
    if (index < kDrawTaskCount)
        addTaskRange(top, stackTop(), static_cast<uint16_t>(index));
}

template <size_t I> void drawTask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t top = physical(stackTop());
    if (top > 0xFFFFFFu)
    {
        g_drawTaskOriginals[I](rdram, ctx, runtime);
        return;
    }
    g_currentLayer = static_cast<uint8_t>(kDrawTasks[I].layer);
    rg::hooks::callThen(g_drawTaskOriginals[I], rdram, ctx, runtime, &afterDrawTask,
                        (static_cast<uint32_t>(I) << 24) | top);
}

template <size_t... I> void hookDrawTasks(PS2Runtime &runtime, std::index_sequence<I...>)
{
    size_t hooked = 0u;
    ((hooked += rg::timing::hookFunction(runtime, kDrawTasks[I].address, &drawTask<I>, g_drawTaskOriginals[I]) ? 1u : 0u),
     ...);
    if (g_log)
        std::fprintf(stderr, "[nativefx] draw tasks hooked for layers: %zu of %zu\n", hooked, kDrawTaskCount);
}

// M3: the effects sent as passes (native_records.h's Pass), their
// parameters read from the function's arguments when it is called, kept by
// the unit their packet range carries until the VIF1 thread reaches it.
std::mutex g_passMutex;
std::vector<std::pair<uint32_t, rg::nativeproducer::PassRequest>> g_passes; // by unit
uint32_t g_nextPassUnit = 0u;
// The unit of the call in progress (game thread), and where it began.
uint32_t g_passUnit = 0u;

// A pass kept under `unit` (several may share one: sent in order).
void keepPassAs(uint32_t unit, const rg::nativeproducer::PassRequest &request)
{
    std::lock_guard lock(g_passMutex);
    if (g_passes.size() >= 1024u) // a few frames' worth; an older one was never reached
        g_passes.erase(g_passes.begin());
    g_passes.emplace_back(unit, request);
}

uint32_t keepPass(const rg::nativeproducer::PassRequest &request)
{
    uint32_t unit;
    {
        std::lock_guard lock(g_passMutex);
        unit = (++g_nextPassUnit & 0xFFFFFFu) | (1u << 24);
    }
    keepPassAs(unit, request);
    return unit;
}

// A descriptor's GS base (TEX0's TBP0 at +0x38, as the target declarations
// read it).
uint32_t descriptorBase(uint32_t desc)
{
    if (desc == 0u || !g_rdram)
        return 0u;
    uint64_t tex0 = 0u;
    std::memcpy(&tex0, g_rdram + (physical(desc) & ~7u) + 0x38u, sizeof(tex0));
    return static_cast<uint32_t>(tex0 & 0x3FFFu) & ~31u;
}

float readFloatAt(uint32_t address)
{
    const uint32_t bits = read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A name-table node's side: its width at +2, its height at +4 (s16).
uint32_t nodeSide(uint32_t node, uint32_t at)
{
    if (node == 0u || !g_rdram)
        return 0u;
    int16_t v = 0;
    std::memcpy(&v, g_rdram + physical(node + at), sizeof(v));
    return v > 0 ? static_cast<uint32_t>(v) : 0u;
}

std::string readString(uint32_t address, size_t most = 128u)
{
    std::string out;
    if (address == 0u || !g_rdram)
        return out;
    for (size_t i = 0; i < most; ++i)
    {
        const char c = static_cast<char>(g_rdram[physical(address + static_cast<uint32_t>(i))]);
        if (c == 0)
            break;
        out.push_back(c);
    }
    return out;
}

// NameTableFindByName_159fb0(table, name, -1): the name with the table's
// suffix (+0x1E4), hashed into one of 101 buckets at +0x30, each a list of
// {entry, next} whose entry's name is at +8. The entry, or 0.
uint32_t findByName(uint32_t table, uint32_t nameAt)
{
    if (table == 0u || nameAt == 0u)
        return 0u;
    const std::string name = readString(nameAt) + readString(table + 0x1E4u);
    int32_t h = 0;
    for (const char c : name)
        h = ((((h & 0xFF) << 8) + static_cast<int32_t>(static_cast<int8_t>(c))) % 101) & 0xFF;
    uint32_t node = read32(table + 0x30u + 4u * static_cast<uint32_t>(h));
    for (int guard = 0; node != 0u && guard < 4096; ++guard)
    {
        const uint32_t entry = read32(node);
        if (entry != 0u && readString(entry + 8u) == name)
            return entry;
        node = read32(node + 4u);
    }
    return 0u;
}

// The effect calls in progress (game thread), and how many have begun: a
// 2D block drawn inside one is the effect's (src/native_blocks.cpp keeps
// such blocks written as the game writes them).
std::vector<rg::hooks::After> g_effectAfters;
int g_effectDepth = 0;
uint64_t g_effectCalls = 0u;
void afterEffect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t data)
{
    const rg::hooks::After after = g_effectAfters.empty() ? nullptr : g_effectAfters.back();
    if (!g_effectAfters.empty())
        g_effectAfters.pop_back();
    if (g_effectDepth > 0)
        --g_effectDepth;
    if (after)
        after(rdram, ctx, runtime, data);
}
// rg::hooks::callThen for an effect's hook, counted.
void callEffect(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                rg::hooks::After after, uint32_t data)
{
    ++g_effectDepth;
    ++g_effectCalls;
    g_effectAfters.push_back(after);
    rg::hooks::callThen(original, rdram, ctx, runtime, &afterEffect, data);
}

PS2Runtime::RecompiledFunction g_glareOriginal = nullptr;

bool writePassMarker(uint32_t unit);

// (Its marker could not be written before the call: at its end.)
void afterGlare(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_passUnit != 0u)
        writePassMarker(g_passUnit);
    g_passUnit = 0u;
}

// GlareFilterDrawMaybe_17a0d0(arg): arg holds the two buffers' descriptors
// (+0, +4), the alpha a pixel must be over (+8, s32), the streaks' strength
// (+0xC), their spread (+0x10) and the strength laid over the frame
// (+0x14), floats (RGDecomp's C; docs/NATIVE-ONLY-NOTES.md).
// M3 step 2: a pass in place of its function's packets. The function is
// not run; the list gets a marker where its packets would have been (a cnt
// tag, NOP and DIRECT 2, and a GIF A+D write of the pass's unit to a
// register the GS does not have), and the VIF1 thread sends the pass when
// it reaches it. False: no list being built.
bool writePassMarker(uint32_t unit)
{
    const uint32_t drawCtx = read32(kDrawCtxPointer);
    if (drawCtx == 0u || !g_rdram)
        return false;
    const uint32_t dst = read32(drawCtx + 4u);
    if (dst == 0u)
        return false;
    const uint64_t words[6] = {
        2ull | (1ull << 28),                    // cnt, 2 quadwords
        uint64_t(0x50000002u) << 32,            // VIF NOP, DIRECT 2
        1ull | (1ull << 15) | (1ull << 60),     // GIF: NLOOP 1, EOP, PACKED, NREG 1
        0xEull,                                 // A+D
        unit,                                   // the pass's unit
        rg::nativefx::kPassRegister,            // to the marker's register
    };
    std::memcpy(g_rdram + physical(dst), words, sizeof(words));
    const uint32_t next = dst + 48u;
    std::memcpy(g_rdram + physical(drawCtx + 4u), &next, sizeof(next));
    return true;
}

void returnFromHook(R5900Context *ctx)
{
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}

void glareFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t arg = getRegU32(ctx, 4);
    g_passUnit = 0u;
    if (arg != 0u && read32(arg) != 0u && read32(arg + 4u) != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 0u; // native::PassKind::Glare
        request.workBases[0] = descriptorBase(read32(arg));
        request.workBases[1] = descriptorBase(read32(arg + 4u));
        const int32_t over = static_cast<int32_t>(read32(arg + 8u));
        request.params[0] = float(over) / 128.0f;
        request.params[1] = readFloatAt(arg + 0x0Cu); // 128 xC bytes over 0x80
        request.params[2] = 24.0f * readFloatAt(arg + 0x10u);
        const int32_t final = static_cast<int32_t>(128.0f * readFloatAt(arg + 0x14u));
        request.params[3] = float(std::clamp(final, 0, 255)) / 128.0f;
        request.params[4] = 255.0f / 128.0f;
        // The glare has no side effects outside its packets (its note): the
        // pass alone, at a marker, and the function is not run.
        const uint32_t unit = keepPass(request);
        if (writePassMarker(unit))
        {
            returnFromHook(ctx);
            return;
        }
        g_passUnit = unit;
    }
    callEffect(g_glareOriginal, rdram, ctx, runtime, &afterGlare, stackTop());
}

PS2Runtime::RecompiledFunction g_shadowOriginal = nullptr;

uint32_t g_shadowUnit = 0u; // game thread

void afterShadow(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (g_shadowUnit != 0u)
        writePassMarker(g_shadowUnit);
    g_shadowUnit = 0u;
}

// DrawListEndPlaneShadowPass_146cc0(ctx): nothing unless ctx+0x3890 (set by
// the begin); the mask's node at ctx+0x38D0, the blurred copy's at
// ctx+0x39B0, flags at ctx+0x39A0 (blur), +0x39A4 (red and green onto blue
// and alpha), +0x39A8 (colored), the strength at +0x3A18 and the color at
// +0x3A20; the blur's offsets are the tables at 0x449CD0 and 0x449CF0
// (docs/NATIVE-ONLY-NOTES.md). The call runs: it also reapplies the
// camera and writes the light state's packet.
void planeShadowEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a = getRegU32(ctx, 4);
    g_shadowUnit = 0u;
    if (a != 0u && read32(a + 0x3890u) != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 3u; // native::PassKind::PlaneShadow
        const uint32_t mask = a + 0x38D0u, blurred = a + 0x39B0u;
        request.workBases[0] = descriptorBase(mask);
        request.workBases[1] = descriptorBase(blurred);
        request.workSizes[0][0] = nodeSide(mask, 2u);
        request.workSizes[0][1] = nodeSide(mask, 4u);
        request.workSizes[1][0] = nodeSide(blurred, 2u);
        request.workSizes[1][1] = nodeSide(blurred, 4u);
        request.params[0] = readFloatAt(a + 0x3A18u);
        for (int c = 0; c < 3; ++c)
            request.params[1 + c] = readFloatAt(a + 0x3A20u + 4u * c);
        request.params[4] = float((read32(a + 0x39A0u) != 0u ? 1 : 0) | (read32(a + 0x39A4u) != 0u ? 2 : 0) |
                                  (read32(a + 0x39A8u) != 0u ? 4 : 0));
        request.params[5] = 255.0f / 128.0f;
        for (int k = 0; k < 6; ++k)
        {
            request.params[6 + 2 * k] = float(static_cast<int32_t>(read32(0x449CD0u + 4u * k)) - 8) / 16.0f;
            request.params[7 + 2 * k] = float(static_cast<int32_t>(read32(0x449CF0u + 4u * k)) - 8) / 16.0f;
        }
        g_shadowUnit = keepPass(request);
    }
    callEffect(g_shadowOriginal, rdram, ctx, runtime, &afterShadow, stackTop());
}

PS2Runtime::RecompiledFunction g_cblurOriginal = nullptr;
uint32_t g_cblurUnit = 0u; // game thread

void afterCBlur(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (g_cblurUnit != 0u)
        writePassMarker(g_cblurUnit);
    g_cblurUnit = 0u;
}

// CBlurTexBlurMaybe_1789e0(obj): nothing unless obj+0; the trail P named at
// obj+8 and its history Q at obj+0x28 (the draw context's name table,
// ctx+0x3BA0); obj+0x6C picks the feedback blur over the history, obj+0x70
// the history's strength; the blur's taps are the s32 tables at 0x47DFD0
// (x) and 0x47DFE0 (y) (docs/NATIVE-ONLY-NOTES.md).
void cblurTexBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a = getRegU32(ctx, 4);
    g_cblurUnit = 0u;
    if (a != 0u && read32(a) != 0u)
    {
        const uint32_t table = read32(read32(kDrawCtxPointer) + 0x3BA0u);
        const uint32_t p = findByName(table, a + 8u), q = findByName(table, a + 0x28u);
        if (p != 0u && q != 0u)
        {
            rg::nativeproducer::PassRequest request;
            request.kind = 4u; // native::PassKind::CBlur
            request.workBases[0] = descriptorBase(p);
            request.workBases[1] = descriptorBase(q);
            request.workSizes[0][0] = nodeSide(p, 2u);
            request.workSizes[0][1] = nodeSide(p, 4u);
            request.workSizes[1][0] = nodeSide(q, 2u);
            request.workSizes[1][1] = nodeSide(q, 4u);
            request.params[0] = read32(a + 0x6Cu) != 0u ? 1.0f : 0.0f;
            request.params[1] = float(int32_t(128.0f * readFloatAt(a + 0x70u))) / 128.0f;
            request.params[2] = 255.0f / 128.0f;
            for (int k = 0; k < 4; ++k)
            {
                // UVs of 16 + 8 tap sixteenths: a pixel and a half-tap.
                request.params[3 + 2 * k] = 1.0f + float(static_cast<int32_t>(read32(0x47DFD0u + 4u * k))) * 0.5f;
                request.params[4 + 2 * k] = 1.0f + float(static_cast<int32_t>(read32(0x47DFE0u + 4u * k))) * 0.5f;
            }
            g_cblurUnit = keepPass(request);
        }
    }
    callEffect(g_cblurOriginal, rdram, ctx, runtime, &afterCBlur, stackTop());
}

PS2Runtime::RecompiledFunction g_bloomOriginal = nullptr;
uint32_t g_bloomUnit = 0u; // game thread

void afterBloom(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_bloomUnit != 0u)
        writePassMarker(g_bloomUnit);
    g_bloomUnit = 0u;
}

// BloomExtractBlurMaybe_179360(obj): obj +0 and +4 the two buffers'
// descriptors, +0x10, +0x14, +0x18 the color (times 64) and +0x1C the alpha
// (times 128) it is added at, +0x20 the blur's offset (times 16). It also
// clears a word of the draw context (`ctx->f270->f80.hi`), which is why
// the call still runs (docs/NATIVE-ONLY-NOTES.md).
void bloomExtractBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    g_bloomUnit = 0u;
    if (obj != 0u && read32(obj) != 0u && read32(obj + 4u) != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 1u; // native::PassKind::Bloom
        request.workBases[0] = descriptorBase(read32(obj));
        request.workBases[1] = descriptorBase(read32(obj + 4u));
        request.params[0] = float(static_cast<int32_t>(16.0f * readFloatAt(obj + 0x20u)));
        for (int c = 0; c < 3; ++c)
            request.params[1 + c] = float(static_cast<int32_t>(64.0f * readFloatAt(obj + 0x10u + 4u * c))) / 128.0f;
        request.params[4] = float(static_cast<int32_t>(128.0f * readFloatAt(obj + 0x1Cu))) / 128.0f;
        const uint32_t unit = keepPass(request);
        // Its one write outside its packets (0x1793e4-0x179420): the high
        // word of the draw environment's quadword at +0x80 cleared. Made
        // here, the pass at a marker, the function not run.
        const uint32_t env = read32(read32(kDrawCtxPointer) + 0x270u);
        if (env != 0u && writePassMarker(unit))
        {
            const uint32_t zero = 0u;
            std::memcpy(g_rdram + physical(env + 0x84u), &zero, sizeof(zero));
            returnFromHook(ctx);
            return;
        }
        g_bloomUnit = unit;
    }
    callEffect(g_bloomOriginal, rdram, ctx, runtime, &afterBloom, stackTop());
}

// ScreenColorFilterMaybe(r, g, b) (RGDecomp's C; GameSceneTintApply's, for
// the grayscale filter task): the frame's alpha from its green, through a
// CT16 view of it drawn onto itself 8 pixels over, then the frame Cs Ad:
// (r, g, b) times the frame's green / 128. The colour is the range's unit
// (and bit 24, so none is 0).
constexpr uint32_t kColorFilter = 0x00179C60u;
PS2Runtime::RecompiledFunction g_colorFilterOriginal = nullptr;
uint32_t g_colorFilterUnit = 0u; // game thread only

void afterColorFilter(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_colorFilterUnit != 0u)
        writePassMarker(g_colorFilterUnit);
    g_colorFilterUnit = 0u;
}

// The colour filter's pass, for (r, g, b) bytes (0x80 = 1).
uint32_t colorFilterPass(uint32_t r, uint32_t g, uint32_t b)
{
    rg::nativeproducer::PassRequest request;
    request.kind = 5u; // native::PassKind::ColorFilter
    request.params[0] = float(r & 0xFFu) / 128.0f;
    request.params[1] = float(g & 0xFFu) / 128.0f;
    request.params[2] = float(b & 0xFFu) / 128.0f;
    request.params[3] = 128.0f; // a byte a unit over 0x80
    return keepPass(request);
}

void colorFilter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_colorFilterUnit = colorFilterPass(getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6));
    // No side effects outside its packets (its CT16 view is a local node):
    // the pass alone, at a marker.
    if (g_colorFilterUnit != 0u && writePassMarker(g_colorFilterUnit))
    {
        g_colorFilterUnit = 0u;
        returnFromHook(ctx);
        return;
    }
    callEffect(g_colorFilterOriginal, rdram, ctx, runtime, &afterColorFilter, stackTop());
}

// RG_DEBUG_COLOR_FILTER=RRGGBB: GameSceneTintApply (the grayscale filter
// task's, each frame the task runs) calls the filter with this colour as if
// the scene's tint were on, to see it where no scene tints. It calls the
// filter directly and marks its packets itself: callThen's return hook is
// for calls the game makes, not for one made from native code.
uint32_t debugColorFilter()
{
    static const uint32_t color = [] {
        const char *value = std::getenv("RG_DEBUG_COLOR_FILTER");
        return value && *value ? (static_cast<uint32_t>(std::strtoul(value, nullptr, 16)) | (1u << 24)) : 0u;
    }();
    return color;
}

constexpr uint32_t kGameSceneTintApply = 0x001CE8F0u;
PS2Runtime::RecompiledFunction g_tintApplyOriginal = nullptr;

void gameSceneTintApply(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (const uint32_t color = debugColorFilter(); color != 0u)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[nativefx] debug colour filter %06x from GameSceneTintApply\n", color & 0xFFFFFFu);
        }
        // The call GameSceneTintApply makes when the scene's tint is on.
        const R5900Context saved = *ctx;
        const auto set = [&](int reg, uint32_t value) {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        };
        set(4, (color >> 16) & 0xFFu);
        set(5, (color >> 8) & 0xFFu);
        set(6, color & 0xFFu);
        if (g_colorFilterOriginal)
        {
            g_colorFilterOriginal(rdram, ctx, runtime);
            writePassMarker(colorFilterPass(color >> 16, color >> 8, color));
        }
        else if (PS2Runtime::RecompiledFunction filter = runtime->lookupFunction(kColorFilter))
        {
            filter(rdram, ctx, runtime);
        }
        // GameSceneTintApply itself is not run: return to its caller, as a
        // replaced function must (a hook that left pc where it was froze
        // the game before).
        *ctx = saved;
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_tintApplyOriginal(rdram, ctx, runtime);
}

PS2Runtime::RecompiledFunction g_cblurDrawOriginal = nullptr;
uint32_t g_cblurDrawUnit = 0u; // game thread

void afterCBlurDraw(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (g_cblurDrawUnit != 0u)
        writePassMarker(g_cblurDrawUnit);
    g_cblurDrawUnit = 0u;
}

// CBlurDrawMaybe_1784e0(obj, color, preset, small, dx, dy): nothing unless
// obj+0 and not obj+0x68; the trail P named at obj+8, the sprite S at
// obj+0x28 (small) or obj+0x48 (the frame's copy); the color's r, g, b, a
// floats; dx and dy in the frame's pixels, scaled to S when small
// (docs/NATIVE-ONLY-NOTES.md).
void cblurDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a = getRegU32(ctx, 4), color = getRegU32(ctx, 5);
    const int32_t preset = static_cast<int32_t>(getRegU32(ctx, 6));
    const bool small = getRegU32(ctx, 7) != 0u;
    int32_t dx = static_cast<int32_t>(getRegU32(ctx, 8)), dy = static_cast<int32_t>(getRegU32(ctx, 9));
    g_cblurDrawUnit = 0u;
    if (a != 0u && color != 0u && read32(a) != 0u && read32(a + 0x68u) == 0u)
    {
        const uint32_t drawCtx = read32(kDrawCtxPointer);
        const uint32_t table = read32(drawCtx + 0x3BA0u);
        const uint32_t p = findByName(table, a + 8u), sprite = findByName(table, a + (small ? 0x28u : 0x48u));
        const uint32_t env = read32(drawCtx + 0x270u);
        const int32_t W = static_cast<int32_t>(read32(env + 4u)), H = static_cast<int32_t>(read32(env + 8u));
        const int32_t w = static_cast<int32_t>(nodeSide(sprite, 2u)), h = static_cast<int32_t>(nodeSide(sprite, 4u));
        if (p != 0u && sprite != 0u && env != 0u && W > 0 && H > 0)
        {
            if (small)
            {
                dx = dx * w / W;
                dy = dy * h / H;
            }
            rg::nativeproducer::PassRequest request;
            request.kind = 12u; // native::PassKind::CBlurDraw
            request.workBases[0] = descriptorBase(p);
            request.workBases[1] = descriptorBase(sprite);
            request.workSizes[0][0] = nodeSide(p, 2u);
            request.workSizes[0][1] = nodeSide(p, 4u);
            request.workSizes[1][0] = static_cast<uint32_t>(w);
            request.workSizes[1][1] = static_cast<uint32_t>(h);
            request.params[0] = float(dx);
            request.params[1] = float(dy);
            request.params[2] = float(preset);
            request.params[3] = small ? 1.0f : 0.0f;
            for (int c = 0; c < 3; ++c)
                request.params[4 + c] = float(int32_t(128.0f * readFloatAt(color + 4u * c))) / 128.0f;
            request.params[7] = float(int32_t(64.0f * readFloatAt(color + 12u))) / 128.0f;
            request.params[8] = float(W);
            request.params[9] = float(H);
            request.params[10] = float(static_cast<int32_t>(read32(env + 0x14u)));
            request.params[11] = float(static_cast<int32_t>(read32(env + 0x18u)));
            g_cblurDrawUnit = keepPass(request);
        }
    }
    callEffect(g_cblurDrawOriginal, rdram, ctx, runtime, &afterCBlurDraw, stackTop());
}

// FocusFilterDrawMaybe and ShadowMapBlur: passes, as the bloom; both use the
// CT16 view of a buffer for a channel shuffle. ShadowMapRender is not hooked:
// its models are VU1 draws (native already).
constexpr uint32_t kFocusFilter = 0x0017C000u;
constexpr uint32_t kShadowMapBlur = 0x00145B60u;
PS2Runtime::RecompiledFunction g_focusOriginal = nullptr;
PS2Runtime::RecompiledFunction g_shadowMapBlurOriginal = nullptr;

int32_t projectDepth(float d);

// The frames an effect's call sets, in order (game thread): its buffers
// when the call finds them itself (by name, as the focus filter does).
bool g_recordingFrames = false;
std::vector<uint32_t> g_framesSet;
rg::nativeproducer::PassRequest g_focusRequest; // game thread

void afterFocus(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    g_recordingFrames = false;
    uint32_t unit = 0u;
    // The frames it set: A, C, then A', A, A', A, then C', C, C', C (the
    // rounds), C's CT16 view, A (docs/NATIVE-ONLY-NOTES.md).
    if (g_framesSet.size() >= 7u)
    {
        rg::nativeproducer::PassRequest request = g_focusRequest;
        request.workBases[0] = g_framesSet[0];
        request.workBases[1] = g_framesSet[2];
        request.workBases[2] = g_framesSet[1];
        request.workBases[3] = g_framesSet[6];
        unit = keepPass(request);
    }
    g_framesSet.clear();
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (unit != 0u)
        writePassMarker(unit);
}

// FocusFilterDrawMaybe_17c000(arg): arg +0x84 and +0x88 the focus distances;
// its four buffers are found by name, so they are the frames it sets.
void focusFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t arg = getRegU32(ctx, 4);
    rg::nativeproducer::PassRequest &request = g_focusRequest;
    request = {};
    request.kind = 9u; // native::PassKind::Focus
    const float f = readFloatAt(arg + 0x84u), f2 = readFloatAt(arg + 0x88u);
    const float distances[4] = {0.5f * f, 0.667f * f, f + 0.75f * (f - f2), f}; // 0.667: the float at 0x47D848
    const float alphas[4] = {0.75f, 0.35f, 0.25f, 0.0f};
    for (int i = 0; i < 4; ++i)
    {
        request.params[i] = float(projectDepth(distances[i]));
        request.depthParams |= 1u << i;
        request.params[4 + i] = float(static_cast<int32_t>(128.0f * alphas[i])) / 128.0f;
    }
    request.params[8] = float(0x50) / 128.0f;  // the first blur's lerp (FIX)
    request.params[9] = float(0x3C) / 128.0f;  // the mask's color
    request.params[10] = float(0x40) / 128.0f; // the composite's alpha
    request.params[11] = 18.0f / 16.0f;        // the first blur's offset
    request.params[12] = 24.0f / 16.0f;        // the second's
    request.sceneDepth = true;
    g_framesSet.clear();
    g_recordingFrames = true;
    callEffect(g_focusOriginal, rdram, ctx, runtime, &afterFocus, stackTop());
}

uint32_t g_shadowMapUnit = 0u; // game thread

void afterShadowMapBlur(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (g_shadowMapUnit != 0u)
        writePassMarker(g_shadowMapUnit);
    g_shadowMapUnit = 0u;
}

// ShadowMapBlur_145b60(ctx): the map's node at ctx+0x3A70 (w +2, h +4, TEX0
// +0x38), its partner's at *(ctx+0x3AE8); the colors 0x2E and 0x26, and the
// offsets of the tables at 0x449D10 and 0x449D30 (20 sixteenths along each
// axis, less 8), are the function's (docs/NATIVE-ONLY-NOTES.md). The call
// runs: it also pops the view stack and writes the light state's packet.
void shadowMapBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a = getRegU32(ctx, 4);
    g_shadowMapUnit = 0u;
    if (a != 0u && read32(a + 0x3AE8u) != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 10u; // native::PassKind::ShadowMap
        const uint32_t map = a + 0x3A70u, partner = read32(a + 0x3AE8u);
        request.workBases[0] = descriptorBase(map);
        request.workBases[1] = descriptorBase(partner);
        const auto side = [](uint32_t node, int at) {
            int16_t v = 0;
            std::memcpy(&v, g_rdram + physical(node + static_cast<uint32_t>(at)), sizeof(v));
            return v > 0 ? static_cast<uint32_t>(v) : 0u;
        };
        request.workSizes[0][0] = side(map, 2);
        request.workSizes[0][1] = side(map, 4);
        request.workSizes[1][0] = side(partner, 2);
        request.workSizes[1][1] = side(partner, 4);
        request.params[0] = float(0x2E) / 128.0f;
        request.params[1] = float(0x26) / 128.0f;
        constexpr int32_t kOffsetX[4] = {20, -20, 0, 0}, kOffsetY[4] = {0, 0, 20, -20};
        for (int j = 0; j < 4; ++j)
        {
            request.params[2 + 2 * j] = float(kOffsetX[j] - 8) / 16.0f;
            request.params[3 + 2 * j] = float(kOffsetY[j] - 8) / 16.0f;
        }
        g_shadowMapUnit = keepPass(request);
    }
    callEffect(g_shadowMapBlurOriginal, rdram, ctx, runtime, &afterShadowMapBlur, stackTop());
}

PS2Runtime::RecompiledFunction g_refractionOriginal = nullptr;

uint32_t g_refractionCtx = 0u, g_refractionSide = 0u; // game thread

// After RefractionCaptureFrame_13ffd0(ctx, shift): its node at ctx+0x3BD8 is
// a square of 1 << shift texels whose TBP (ctx+0x3C10, 14 bits) the call
// just set; the frame was drawn into it whole, unblended. That node is what
// the refraction materials sample, so the call runs; its pass is made here,
// once the buffer's base is known (docs/NATIVE-ONLY-NOTES.md).
void afterRefraction(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    uint32_t unit = 0u;
    if (g_refractionCtx != 0u && g_refractionSide != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 7u; // native::PassKind::Capture
        uint16_t tbp = 0u;
        std::memcpy(&tbp, g_rdram + physical(g_refractionCtx + 0x3C10u), sizeof(tbp));
        request.workBases[0] = static_cast<uint32_t>(tbp & 0x3FFFu) & ~31u;
        request.workSizes[0][0] = request.workSizes[0][1] = g_refractionSide;
        unit = keepPass(request);
    }
    // M6: its 2D blocks are not written (src/native_blocks.cpp): the pass
    // comes at a marker where the call's output ends.
    if (unit != 0u)
        writePassMarker(unit);
}

void refractionCaptureFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_refractionCtx = getRegU32(ctx, 4);
    const uint32_t shift = getRegU32(ctx, 5);
    g_refractionSide = shift < 12u ? 1u << shift : 0u;
    callEffect(g_refractionOriginal, rdram, ctx, runtime, &afterRefraction, stackTop());
}

PS2Runtime::RecompiledFunction g_dofOriginal = nullptr;

uint32_t g_dofUnit = 0u; // game thread

void afterDof(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_dofUnit != 0u)
        writePassMarker(g_dofUnit);
    g_dofUnit = 0u;
}

// ProjectDepth_15d0a0(d): the GS window Z of the point (0, 0, -d, 1) through
// the matrix at *(*(0x4810A4) + 0xF20) + 0x200 (row vector times matrix,
// MtxMulPoint_13c890), z / w truncated.
int32_t projectDepth(float d)
{
    const uint32_t ctx = read32(kDrawCtxPointer);
    const uint32_t m = (ctx != 0u ? read32(ctx + 0xF20u) : 0u) + 0x200u;
    const float m22 = readFloatAt(m + 0x28u), m23 = readFloatAt(m + 0x2Cu);
    const float m32 = readFloatAt(m + 0x38u), m33 = readFloatAt(m + 0x3Cu);
    const float z = -d * m22 + m32, w = -d * m23 + m33;
    return static_cast<int32_t>(z * (1.0f / w));
}

// DofFilterDrawMaybe_17d150(n, distances, work, scale): see
// docs/NATIVE-ONLY-NOTES.md. Each level's two depths are the GS Z of its
// distance and of 1.1 times it.
void dofFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = getRegU32(ctx, 4), distances = getRegU32(ctx, 5), work = getRegU32(ctx, 6);
    g_dofUnit = 0u;
    if (work != 0u && distances != 0u && n != 0u)
    {
        rg::nativeproducer::PassRequest request;
        request.kind = 2u; // native::PassKind::DepthOfField
        request.workBases[0] = descriptorBase(work);
        const uint32_t levels = std::min<uint32_t>(n, 7u);
        request.params[0] = float(levels);
        request.params[1] = float(static_cast<int32_t>(128.0f * ctx->f[12])) / 128.0f;
        for (uint32_t i = 0; i < levels; ++i)
        {
            const float d = readFloatAt(distances + 4u * i);
            request.params[2 + 2 * i] = float(projectDepth(d));
            request.params[3 + 2 * i] = float(projectDepth(d / 10.0f + d));
            request.depthParams |= 3u << (2 + 2 * i);
        }
        request.sceneDepth = true;
        // No side effects outside its packets (it reads and copies nodes
        // into locals only): the pass alone, at a marker.
        const uint32_t unit = keepPass(request);
        if (writePassMarker(unit))
        {
            returnFromHook(ctx);
            return;
        }
        g_dofUnit = unit;
    }
    callEffect(g_dofOriginal, rdram, ctx, runtime, &afterDof, stackTop());
}

// Stage E, an object at a time. The composite draws offscreen when its
// settings (a0) have +0x50 and +0x5C, a buffer at +0x20 and a strength at
// +0x24 not negative (the archive branch's docs/NATIVE-RENDERER.md, "Stage E, why offscreen"):
// the packets up to the end of its last ModelEmitDrawPackets are the head,
// the rest the tail. (Game thread only.)
struct Composite
{
    // Its tail's unit ((7 << 24) | n: its passes, kept under it, and the
    // marker that sends them); its head's marker is (6 << 24) | n.
    uint32_t unit = 0u;
    // Whether its head's layer was begun (a marker written with the box its
    // clear covers), and whether a part was emitted (its blocks are then the
    // tail's).
    bool begun = false;
    bool emitted = false;
    // The call's arguments (a0 its settings, a1 the object, f13 the alpha)
    // and what its overlay calls left (the tail's passes, docs/NATIVE-ONLY-NOTES.md).
    uint32_t settings = 0u, object = 0u;
    float alpha = 1.0f;
    int singles = 0;
    bool haveRect = false;
    int32_t rect[4] = {}; // lx, ly, hx, hy: the first single's, centered sixteenths
};
Composite g_composite;
uint32_t g_units = 0u;
PS2Runtime::RecompiledFunction g_compositeOriginal = nullptr;
PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;

float readFloat(uint32_t address)
{
    const uint32_t bits = read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The object layer's passes (native::PassKind::ObjectLayer). work[0]: the
// offscreen buffer (the producer reads the object's layer in its place
// when the head was routed). params: 0 from the layer (the VIF1 thread's),
// 1 into work[1] alpha only (else the frame), 2-5 the rectangle in the
// target's pixels, 6-9 its texels, 10-13 the color, 14 the blend (0 none,
// 1 preset 1, 2 Cs FIX + Cd (1 - FIX)), 15 FIX, 16 the alpha test (5
// GEQUAL, 6 GREATER), 17 its reference, 18 the buffer read as CT24 with
// AEM (black clear), 19 how many offsets, 20-27 the offsets (x, y) in
// pixels.
rg::nativeproducer::PassRequest objectPass(uint32_t settings)
{
    rg::nativeproducer::PassRequest request;
    request.kind = 13u; // native::PassKind::ObjectLayer
    const uint32_t buffer = read32(settings + 0x20u);
    request.workBases[0] = descriptorBase(buffer);
    request.workSizes[0][0] = nodeSide(buffer, 2u);
    request.workSizes[0][1] = nodeSide(buffer, 4u);
    return request;
}

bool descriptorIsCt24(uint32_t desc)
{
    uint64_t tex0 = 0u;
    std::memcpy(&tex0, g_rdram + (physical(desc) & ~7u) + 0x38u, sizeof(tex0));
    return ((tex0 >> 20) & 0x3Fu) == 1u;
}

// ScreenTexOverlayDrawMultiMaybe_16a510 (a1 the rectangle: ly, lx, hy, hx
// in centered sixteenths; a2 the texture; a3 the color; t0 the mode; t1
// the offset) and ScreenTexOverlayDrawSingleMaybe_16a780 (a1, a2, a3 the
// same, no offset): the VU1 overlay program's sprites, inside a composite
// the tail's. Their blend is the composite's (preset 1 and GREATER 0 for
// the multi; for the singles, by the settings' +0x60 and +0x24).
void overlayCall(R5900Context *ctx, bool multi)
{
    Composite &c = g_composite;
    const uint32_t rectAt = getRegU32(ctx, 5), tex = getRegU32(ctx, 6), color = getRegU32(ctx, 7);
    const int32_t mode = multi ? static_cast<int32_t>(getRegU32(ctx, 8)) : 1;
    const uint32_t ofsAt = multi ? getRegU32(ctx, 9) : 0u;
    if (rectAt == 0u || tex == 0u || color == 0u || (multi && (mode <= 0 || mode > 3)))
        return;
    const int32_t ly = static_cast<int32_t>(read32(rectAt)), lx = static_cast<int32_t>(read32(rectAt + 4u));
    const int32_t hy = static_cast<int32_t>(read32(rectAt + 8u)), hx = static_cast<int32_t>(read32(rectAt + 12u));
    const uint32_t env = read32(read32(kDrawCtxPointer) + 0x270u);
    const float W = float(static_cast<int32_t>(read32(env + 4u))), H = float(static_cast<int32_t>(read32(env + 8u)));
    const float cx = float(static_cast<int32_t>(read32(env + 0x0Cu)));
    const float cy = float(static_cast<int32_t>(read32(env + 0x10u)));
    rg::nativeproducer::PassRequest request = objectPass(c.settings);
    request.params[2] = float(lx) / 16.0f + cx;
    request.params[3] = float(ly) / 16.0f + cy;
    request.params[4] = float(hx) / 16.0f + cx;
    request.params[5] = float(hy) / 16.0f + cy;
    request.params[6] = float(lx) / 16.0f + W * 0.5f;
    request.params[7] = float(ly) / 16.0f + H * 0.5f;
    request.params[8] = float(hx) / 16.0f + W * 0.5f;
    request.params[9] = float(hy) / 16.0f + H * 0.5f;
    for (int k = 0; k < 4; ++k)
        request.params[10 + k] = float(static_cast<int32_t>(read32(color + 4u * k))) / 128.0f;
    request.params[18] = descriptorIsCt24(tex) ? 1.0f : 0.0f;
    const int32_t t = int32_t(128.0f * c.alpha);
    if (multi)
    {
        request.params[14] = 1.0f;
        request.params[16] = 6.0f;
        request.params[17] = 0.0f;
        const int32_t ox = static_cast<int32_t>(read32(ofsAt)), oy = static_cast<int32_t>(read32(ofsAt + 4u));
        int32_t d[4][2] = {};
        int n = 0;
        if (mode == 1)
            n = 1;
        else if (mode == 2)
        {
            d[0][0] = ox * 3 / 4, d[0][1] = oy * 3 / 4;
            d[1][0] = -ox * 3 / 4, d[1][1] = -oy * 3 / 4;
            n = 2;
        }
        else
        {
            d[0][0] = ox, d[1][0] = -ox, d[2][1] = oy, d[3][1] = -oy;
            n = 4;
        }
        request.params[19] = float(n);
        for (int k = 0; k < n; ++k)
        {
            request.params[20 + 2 * k] = float(d[k][0]) / 16.0f;
            request.params[21 + 2 * k] = float(d[k][1]) / 16.0f;
        }
    }
    else
    {
        const bool fixedFirst = read32(c.settings + 0x60u) != 0u && readFloatAt(c.settings + 0x24u) > 0.0f;
        const bool fixed = fixedFirst || c.singles > 0;
        request.params[14] = fixed ? 2.0f : 1.0f;
        request.params[15] = float(t) / 128.0f;
        request.params[16] = 5.0f;
        request.params[17] = (c.singles > 0 ? 0x81 : 1) / 128.0f;
        request.params[19] = 1.0f;
        if (!c.haveRect)
        {
            c.haveRect = true;
            c.rect[0] = lx, c.rect[1] = ly, c.rect[2] = hx, c.rect[3] = hy;
        }
        ++c.singles;
    }
    keepPassAs(c.unit, request);
}

PS2Runtime::RecompiledFunction g_overlayMultiOriginal = nullptr;
PS2Runtime::RecompiledFunction g_overlaySingleOriginal = nullptr;

void overlayMulti(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_composite.unit != 0u)
        overlayCall(ctx, true);
    g_overlayMultiOriginal(rdram, ctx, runtime);
}

void overlaySingle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_composite.unit != 0u)
        overlayCall(ctx, false);
    g_overlaySingleOriginal(rdram, ctx, runtime);
}

// The composite's last draw: the object's layer, alpha only, into its
// character blur's trail (the object's +0x454, at the strength +0x458 when
// above the float at 0x47D24C), the layer's texels lx/16 + w/2 + 1 to
// hx/16 + w/2 - 1 scaled to the trail.
void compositeBlurMask()
{
    Composite &c = g_composite;
    if (c.object == 0u || !c.haveRect)
        return;
    const uint32_t trail = read32(c.object + 0x454u);
    const float strength = readFloatAt(c.object + 0x458u);
    if (trail == 0u || strength <= readFloatAt(0x47D24Cu))
        return;
    const uint32_t buffer = read32(c.settings + 0x20u);
    const int32_t w = static_cast<int32_t>(nodeSide(buffer, 2u)), h = static_cast<int32_t>(nodeSide(buffer, 4u));
    const int32_t tw = static_cast<int32_t>(nodeSide(trail, 2u)), th = static_cast<int32_t>(nodeSide(trail, 4u));
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0)
        return;
    const int32_t u0 = c.rect[0] / 16 + w / 2 + 1, v0 = c.rect[1] / 16 + h / 2 + 1;
    const int32_t u1 = c.rect[2] / 16 + w / 2 - 1, v1 = c.rect[3] / 16 + h / 2 - 1;
    rg::nativeproducer::PassRequest request = objectPass(c.settings);
    request.workBases[1] = descriptorBase(trail);
    request.workSizes[1][0] = static_cast<uint32_t>(tw);
    request.workSizes[1][1] = static_cast<uint32_t>(th);
    request.params[1] = 1.0f;
    request.params[2] = float(u0 * tw / w);
    request.params[3] = float(v0 * th / h);
    request.params[4] = float(u1 * tw / w);
    request.params[5] = float(v1 * th / h);
    request.params[6] = float(u0);
    request.params[7] = float(v0);
    request.params[8] = float(u1);
    request.params[9] = float(v1);
    request.params[10] = request.params[11] = request.params[12] = 1.0f;
    request.params[13] = float(int32_t(128.0f * strength)) / 128.0f;
    request.params[14] = 1.0f;
    request.params[16] = 5.0f;
    request.params[17] = 1.0f / 128.0f;
    request.params[18] = read32(c.settings + 0x60u) == 0u ? 1.0f : 0.0f; // the f60 == 0 branch leaves it CT24
    request.params[19] = 1.0f;
    keepPassAs(c.unit, request);
}

void afterComposite(uint8_t *, R5900Context *, PS2Runtime *, uint32_t unit)
{
    if (g_composite.unit != unit)
        return;
    compositeBlurMask();
    // The tail: the layer ended, its passes drawn.
    writePassMarker(unit);
    g_composite = Composite{};
}

void compositeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t settings = getRegU32(ctx, 4);
    const bool offscreen = read32(settings + 0x5Cu) != 0u && read32(settings + 0x50u) != 0u &&
                           read32(settings + 0x20u) != 0u && readFloat(settings + 0x24u) >= 0.0f;
    if (!offscreen || g_composite.unit != 0u)
    {
        g_compositeOriginal(rdram, ctx, runtime);
        return;
    }
    g_units = (g_units + 1u) & 0xFFFFFFu;
    g_units = g_units == 0u ? 1u : g_units;
    g_composite = Composite{};
    g_composite.unit = g_units | (7u << 24);
    g_composite.settings = settings;
    g_composite.object = getRegU32(ctx, 5);
    g_composite.alpha = ctx->f[13];
    callEffect(g_compositeOriginal, rdram, ctx, runtime, &afterComposite, g_composite.unit);
}

// M5's census: the model draws' items by mesh class (the vtable at
// mesh+0x50: 0x470010 ModelMeshLinks, prebuilt chains; 0x470040 ModelMesh,
// rebuilt into the frame's buffer each draw; 0x4701C0 ModelMeshMode1) and
// skinned (a palette packet), by emitter. Printed every 600 frames' worth
// (docs/NATIVE-ONLY-NOTES.md, "Model draws").
struct ModelCensus
{
    uint64_t calls = 0, items = 0, links = 0, plain = 0, mode1 = 0, other = 0, skinned = 0;
};
ModelCensus g_census[3]; // Emit, sorted, shadow casters (game thread)
uint64_t g_censusDraws = 0u;

void censusItems(int emitter, uint32_t request)
{
    ModelCensus &c = g_census[emitter];
    ++c.calls;
    const uint32_t meshes = read32(read32(request + 0x0Cu) + 4u);
    uint32_t node = read32(request + 0x44u);
    for (int guard = 0; node != 0u && guard < 4096; ++guard, node = read32(node))
    {
        if (read32(node + 0x24u) == 0u || read32(node + 0x28u) == 0u)
            continue;
        const uint32_t mesh = meshes ? read32(meshes + 4u * read32(node + 0x18u)) : 0u;
        if (mesh == 0u)
            continue;
        ++c.items;
        switch (read32(mesh + 0x50u))
        {
        case 0x470010u: ++c.links; break;
        case 0x470040u: ++c.plain; break;
        case 0x4701C0u: ++c.mode1; break;
        default: ++c.other; break;
        }
        const uint32_t palette = read32(node + 0x2Cu);
        if (palette != 0u && (read32(palette) & 0xFFFFu) != 0u)
            ++c.skinned;
    }
    if (++g_censusDraws % 60000u == 0u)
    {
        static const char *names[3] = {"emit", "sorted", "shadow casters"};
        for (int e = 0; e < 3; ++e)
        {
            const ModelCensus &k = g_census[e];
            std::fprintf(stderr,
                         "[model-census] %s: calls %llu, items %llu: links %llu, plain %llu, mode1 %llu, other %llu; "
                         "skinned %llu\n",
                         names[e], (unsigned long long)k.calls, (unsigned long long)k.items,
                         (unsigned long long)k.links, (unsigned long long)k.plain, (unsigned long long)k.mode1,
                         (unsigned long long)k.other, (unsigned long long)k.skinned);
        }
    }
}

PS2Runtime::RecompiledFunction g_sortedEmitOriginal = nullptr;
PS2Runtime::RecompiledFunction g_shadowEmitOriginal = nullptr;

void sortedEmit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    censusItems(1, getRegU32(ctx, 6));
    g_sortedEmitOriginal(rdram, ctx, runtime);
}

void shadowEmit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    censusItems(2, getRegU32(ctx, 6));
    g_shadowEmitOriginal(rdram, ctx, runtime);
}

void modelEmitDrawPackets(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    censusItems(0, getRegU32(ctx, 6));
    if (g_composite.unit != 0u)
        g_composite.emitted = true;
    g_emitOriginal(rdram, ctx, runtime);
}

bool g_flip = true; // false when its hook could not be placed
// The flip's draw environment (below, with flipPacket).
constexpr uint32_t kSceGsPutDrawEnv = 0x00108BB8u;
PS2Runtime::RecompiledFunction g_putDrawEnvOriginal = nullptr;
void putDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

// --- Packets -----------------------------------------------------------

constexpr uint8_t kXyzf2 = 0x04u, kXyz2 = 0x05u, kXyzf3 = 0x0Cu, kXyz3 = 0x0Du, kAd = 0x0Eu;

} // namespace

// The packet with every drawing kick made a plain vertex write: XYZ2 and
// XYZF2 as XYZ3 and XYZF3, PACKED ones with ADC set.
void withholdKicks(std::vector<uint8_t> &bytes)
{
    uint8_t *data = bytes.data();
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    uint32_t at = 0u;
    while (at + 16u <= size)
    {
        uint64_t tag, regs;
        std::memcpy(&tag, data + at, sizeof(tag));
        std::memcpy(&regs, data + at + 8u, sizeof(regs));
        const uint32_t tagAt = at;
        at += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        nreg = nreg == 0u ? 16u : nreg;
        if (flg == 0u)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg && at + 16u <= size; ++r, at += 16u)
                {
                    const uint8_t descriptor = static_cast<uint8_t>((regs >> (4u * r)) & 0xFu);
                    if (descriptor == kXyz2 || descriptor == kXyzf2)
                    {
                        data[at + 13u] |= 0x80u; // ADC, bit 111
                    }
                    else if (descriptor == kAd)
                    {
                        uint8_t &reg = data[at + 8u];
                        reg = reg == kXyz2 ? kXyz3 : (reg == kXyzf2 ? kXyzf3 : reg);
                    }
                }
            }
        }
        else if (flg == 1u)
        {
            for (uint32_t r = 0; r < nreg; ++r)
            {
                const uint64_t shift = 4u * r;
                const uint64_t descriptor = (regs >> shift) & 0xFu;
                const uint64_t replaced = descriptor == kXyz2 ? kXyz3 : (descriptor == kXyzf2 ? kXyzf3 : descriptor);
                regs = (regs & ~(0xFull << shift)) | (replaced << shift);
            }
            std::memcpy(data + tagAt + 8u, &regs, sizeof(regs));
            at += ((nloop * nreg * 8u) + 15u) & ~15u;
        }
        else
        {
            at += nloop * 16u;
        }
    }
}

namespace
{
constexpr uint8_t kTex0 = 0x06u, kXyoffset = 0x18u, kPrmode = 0x1Bu, kAlpha = 0x42u, kTest = 0x47u, kFrame = 0x4Cu;
} // namespace

const char *name(Effect effect)
{
    switch (effect)
    {
    case Effect::Glare: return "glare";
    case Effect::Shadow: return "shadow";
    case Effect::CBlur: return "cblur";
    case Effect::Bloom: return "bloom";
    case Effect::Dof: return "dof";
    case Effect::CBlurDraw: return "cblur draw";
    case Effect::Refraction: return "refraction";
    case Effect::CompositeTail: return "composite tail";
    case Effect::Flip: return "flip";
    case Effect::ColorFilter: return "color filter";
    case Effect::Focus: return "focus";
    case Effect::ShadowMapBlur: return "shadow map blur";
    default: return "none";
    }
}

bool enabled()
{
    static const bool on = [] {
        g_log = std::getenv("RG_NATIVE_EFFECTS_LOG") != nullptr;
        return true;
    }();
    return on;
}

void install(PS2Runtime &runtime)
{
    if (!enabled())
    {
        return;
    }
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kDrawListFrameBegin, &drawListFrameBegin, g_frameBeginOriginal))
    {
        std::fprintf(stderr, "[nativefx] the effects stay GS packets\n");
        return;
    }
    hookDrawTasks(runtime, std::make_index_sequence<kDrawTaskCount>{});
    // M5's census (the Emit hook is the composites' below).
    rg::timing::hookFunction(runtime, 0x0014F0D0u, &sortedEmit, g_sortedEmitOriginal);   // ModelQueueSortedPackets
    rg::timing::hookFunction(runtime, 0x0014E780u, &shadowEmit, g_shadowEmitOriginal);   // ModelEmitShadowCasterPacketsMaybe
    if (!rg::timing::hookFunction(runtime, kGlareFilterDraw, &glareFilterDraw, g_glareOriginal))
    {
        std::fprintf(stderr, "[nativefx] the glare stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kPlaneShadowEnd, &planeShadowEnd, g_shadowOriginal))
    {
        std::fprintf(stderr, "[nativefx] the plane shadow stays GS packets\n");
    }
    if ((!rg::timing::hookFunction(runtime, kCBlurTexBlur, &cblurTexBlur, g_cblurOriginal) ||
                    !rg::timing::hookFunction(runtime, kCBlurDraw, &cblurDraw, g_cblurDrawOriginal)))
    {
        std::fprintf(stderr, "[nativefx] the CBlur stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kBloom, &bloomExtractBlur, g_bloomOriginal))
    {
        std::fprintf(stderr, "[nativefx] the bloom stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kDof, &dofFilterDraw, g_dofOriginal))
    {
        std::fprintf(stderr, "[nativefx] the depth of field stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kRefraction, &refractionCaptureFrame, g_refractionOriginal))
    {
        std::fprintf(stderr, "[nativefx] the refraction's capture stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kFocusFilter, &focusFilterDraw, g_focusOriginal))
    {
        std::fprintf(stderr, "[nativefx] the focus filter stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kShadowMapBlur, &shadowMapBlur, g_shadowMapBlurOriginal))
    {
        std::fprintf(stderr, "[nativefx] the shadow map's blur stays GS packets\n");
    }
    if (debugColorFilter() != 0u)
        rg::timing::hookFunction(runtime, kGameSceneTintApply, &gameSceneTintApply, g_tintApplyOriginal);
    if ((!rg::timing::hookFunction(runtime, kCompositeAt, &compositeAt, g_compositeOriginal) ||
         !rg::timing::hookFunction(runtime, kModelEmitDrawPackets, &modelEmitDrawPackets, g_emitOriginal) ||
         !rg::timing::hookFunction(runtime, kOverlayMulti, &overlayMulti, g_overlayMultiOriginal) ||
         !rg::timing::hookFunction(runtime, kOverlaySingle, &overlaySingle, g_overlaySingleOriginal)))
    {
        std::fprintf(stderr, "[nativefx] the composites are not hooked: drawn into their buffer, no tail\n");
    }
    if (
        !rg::timing::hookFunction(runtime, kColorFilter, &colorFilter, g_colorFilterOriginal))
    {
        std::fprintf(stderr, "[nativefx] the colour filter stays GS packets\n");
    }
    if (!rg::timing::hookFunction(runtime, kSceGsPutDrawEnv, &putDrawEnv, g_putDrawEnvOriginal))
    {
        g_flip = false;
        std::fprintf(stderr, "[nativefx] the flip's copy stays GS packets\n");
    }
}

bool flipNative()
{
    return enabled() && g_flip;
}

namespace
{
// Game thread: inside the flip, and the last packet it sent there (the draw
// environment the copy's sprites are drawn under).
thread_local bool t_inFlip = false;
thread_local std::vector<uint8_t> t_flipEnvironment;

// GfxFlipBuffersMaybe's sprite packet: its blend, test and PRMODE, and
// sprites; nothing else the game sends from its thread looks so.
bool isFlipCopy(const uint8_t *data, uint32_t size)
{
    std::vector<rg::gswrites::Write> writes;
    const rg::gswrites::Read read = rg::gswrites::read(data, size, writes);
    bool alpha = false, test = false, prmode = false, sprite = false;
    for (const auto &[reg, value] : writes)
    {
        alpha |= reg == kAlpha && value == 0x8000000064ull;
        test |= reg == kTest && value == 0x20002ull;
        prmode |= reg == kPrmode && value == 0x110ull;
        sprite |= reg == rg::gswrites::kPrim && (value & 7u) == 6u;
    }
    return alpha && test && prmode && sprite && read.kicks;
}
} // namespace

// sceGsPutDrawEnv is the runtime's (Kernel/Stubs/GS.cpp): it writes its eight
// register pairs into the GS and sends no packet. Inside the flip, the pairs
// as an A+D packet stand for the draw environment the copy is drawn under:
// the context registers alone (the first "pair" is the environment's GIF
// tag, which the stub reads as one).
namespace
{
void putDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (t_inFlip && g_rdram)
    {
        const uint32_t address = getRegU32(ctx, 4);
        std::vector<uint8_t> packet(16u);
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            uint64_t pair[2];
            std::memcpy(pair, g_rdram + physical(address + i * 16u), sizeof(pair));
            const uint64_t reg = pair[1];
            if (reg < 0x14u || reg > 0x63u)
                continue;
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(pair);
            packet.insert(packet.end(), bytes, bytes + 16u);
            ++count;
        }
        // NLOOP pairs, EOP, PACKED, one register: A+D.
        const uint64_t tag[2] = {static_cast<uint64_t>(count) | (1ull << 15) | (1ull << 60), 0xEull};
        std::memcpy(packet.data(), tag, sizeof(tag));
        if (count != 0u)
            t_flipEnvironment = std::move(packet);
    }
    g_putDrawEnvOriginal(rdram, ctx, runtime);
}
} // namespace

void beginFlip()
{
    t_inFlip = flipNative();
    t_flipEnvironment.clear();
}

void endFlip()
{
    t_inFlip = false;
    t_flipEnvironment.clear();
}

bool flipPacket(const uint8_t *data, uint32_t size, uint32_t &source, uint32_t &target)
{
    if (!t_inFlip)
        return false;
    if (!isFlipCopy(data, size))
    {
        t_flipEnvironment.assign(data, data + size);
        return false;
    }
    // The copy reads TEX0's buffer (the frame) into FRAME's (the display
    // buffer): the last of each, context 1 or 2.
    source = target = 0u;
    std::vector<rg::gswrites::Write> writes;
    if (!t_flipEnvironment.empty())
        rg::gswrites::read(t_flipEnvironment.data(), static_cast<uint32_t>(t_flipEnvironment.size()), writes);
    rg::gswrites::read(data, size, writes);
    for (const auto &[reg, value] : writes)
    {
        if (reg == kFrame || reg == kFrame + 1u)
            target = static_cast<uint32_t>(value & 0x1FFu) << 5u;
        else if (reg == kTex0 || reg == kTex0 + 1u)
            source = static_cast<uint32_t>(value & 0x3FFFu);
    }
    t_flipEnvironment.clear();
    const bool known = target != 0u || source != 0u;
    if (!known || source == target)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[nativefx] the flip's copy stays GS packets: its buffers unknown\n");
        }
        return false;
    }
    return true;
}


bool writeMarker(uint32_t unit)
{
    return writePassMarker(unit);
}

bool inEffectCall()
{
    return g_effectDepth > 0;
}

bool inCompositeCall()
{
    return g_composite.unit != 0u;
}

bool compositeTail()
{
    return g_composite.unit != 0u && g_composite.emitted;
}

namespace
{
std::mutex g_boxMutex;
std::vector<std::pair<uint32_t, Box>> g_boxes; // by the head marker's unit
} // namespace

bool compositeBegin(const Box &box)
{
    Composite &c = g_composite;
    if (c.unit == 0u || c.begun || c.emitted)
        return false;
    const uint32_t unit = (c.unit & 0xFFFFFFu) | (6u << 24);
    {
        std::lock_guard lock(g_boxMutex);
        if (g_boxes.size() >= 256u) // a few frames' worth; an older one was never reached
            g_boxes.erase(g_boxes.begin());
        g_boxes.emplace_back(unit, box);
    }
    c.begun = writePassMarker(unit);
    return c.begun;
}

bool takeCompositeBox(uint32_t unit, Box &box)
{
    std::lock_guard lock(g_boxMutex);
    for (auto it = g_boxes.begin(); it != g_boxes.end(); ++it)
        if (it->first == unit)
        {
            box = it->second;
            g_boxes.erase(it);
            return true;
        }
    return false;
}



uint8_t currentLayer()
{
    return g_currentLayer;
}

uint32_t taskAt(uint32_t address)
{
    address = physical(address);
    std::lock_guard lock(g_mutex);
    // The innermost range holding it: the last whose start is at most it
    // and whose end is past it, searching back over the outer ones.
    auto it = std::upper_bound(g_taskRanges.begin(), g_taskRanges.end(), address,
                               [](uint32_t value, const TaskRange &range) { return value < range.start; });
    for (int back = 0; it != g_taskRanges.begin() && back < 8; ++back)
    {
        --it;
        if (address < it->end)
            return kDrawTasks[it->task].address;
    }
    return 0u;
}



void noteFrameDesc(uint32_t desc)
{
    if (g_recordingFrames)
        g_framesSet.push_back(descriptorBase(desc));
}

bool isPass(Effect effect)
{
    return effect == Effect::Glare || effect == Effect::Bloom || effect == Effect::ColorFilter ||
           effect == Effect::Refraction || effect == Effect::ShadowMapBlur || effect == Effect::Dof ||
           effect == Effect::Focus || effect == Effect::Shadow ||
           effect == Effect::CBlur || effect == Effect::CBlurDraw || effect == Effect::CompositeTail;
}

bool emitPassUnit(uint32_t unit)
{
    return emitPass(Effect::Glare, unit, false);
}

bool emitPass(Effect effect, uint32_t unit, bool fromLayer)
{
    if (!isPass(effect) || unit == 0u)
        return false;
    std::vector<rg::nativeproducer::PassRequest> requests;
    {
        std::lock_guard lock(g_passMutex);
        for (auto it = g_passes.begin(); it != g_passes.end();)
        {
            if (it->first == unit)
            {
                requests.push_back(it->second);
                it = g_passes.erase(it);
            }
            else
                ++it;
        }
    }
    if (requests.empty())
        return false;
    bool all = true;
    for (rg::nativeproducer::PassRequest &request : requests)
    {
        if (request.kind == 13u)
            request.params[0] = fromLayer ? 1.0f : 0.0f;
        all = rg::nativeproducer::pass(request) && all;
    }
    return all;
}




} // namespace rg::nativefx
