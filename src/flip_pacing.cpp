#include "flip_pacing.h"

#include "native_effects.h"

#include <atomic>
#include <cstdio>

#include "gfx/native/native_pipeline.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "runtime/ee_scheduler.h"
#include "scene_timing.h"

namespace rg::graphics
{
namespace
{

// GfxFlipBuffersMaybe: waits for the vsync interval, then copies the frame
// into the display buffer through the GIF; once it returns, that copy is in
// the stream.
constexpr uint32_t kGfxFlipBuffers = 0x00143650u;

PS2Runtime::RecompiledFunction g_originalFlip = nullptr;
std::atomic<uint64_t> g_flips{0u};

// The native pipeline's producer for the frame's end (docs/NATIVE-PIPELINE.md):
// a Present of the scene target at each flip. The target itself is the
// game's frame, declared where the game names it (native_producer.h); the
// renderer takes 640x448 until then.
void presentNative(uint64_t frame)
{
    namespace native = dq8::gfx::native;
    native::Present present;
    present.target = rg::nativeproducer::kSceneTarget;
    present.rect = {0, 0, 640u, 448u};
    present.frame = frame;
    native::push(std::move(present));
}

void afterFlip(uint8_t *, R5900Context *, PS2Runtime *runtime, uint32_t)
{
    // Unpaced, the scheduler gives the game its vsyncs a frame at a time.
    runtime->eeScheduler().noteGuestFrame();
    rg::nativefx::endFlip();
    // The presenting thread shows the frame the Present record closed
    // (graphics.cpp's presentNative, step 7d).
    presentNative(g_flips.load(std::memory_order_relaxed) + 1u);
    g_flips.fetch_add(1u, std::memory_order_release);
}

void flipHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The vsync interval this flip waits for (RG_FPS=60).
    rg::timing::beforeFlip(rdram, ctx);
    // The copy it sends is drawn as a native effect (src/native_effects.h).
    rg::nativefx::beginFlip();
    // Its vsync wait blocks on a fiber and keeps this native frame, but a
    // safe point inside the flip unwinds it: the frame is presented when the
    // flip has really returned (return_hook.h).
    rg::hooks::callThen(g_originalFlip, rdram, ctx, runtime, &afterFlip, 0u);
}

} // namespace

void installFlipHook(PS2Runtime &runtime)
{
    g_originalFlip = runtime.lookupFunction(kGfxFlipBuffers);
    if (!g_originalFlip || !runtime.replaceFunction(kGfxFlipBuffers, &flipHook))
    {
        // Without it no frame is ever presented.
        std::fprintf(stderr, "[rg] flip hook: 0x%08X is not in the function table; no frame will show\n",
                     kGfxFlipBuffers);
    }
}

uint64_t guestFlips()
{
    return g_flips.load(std::memory_order_relaxed);
}

} // namespace rg::graphics
