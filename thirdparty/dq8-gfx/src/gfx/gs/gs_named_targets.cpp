#include "gfx/gs/gs_named_targets.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace dq8::gfx::namedtargets {
namespace {

constexpr uint32_t kBlocks = 0x4000u;
// The screen is 512x448 (NTSC, interlaced frame); anything this size or
// larger is the frame or a full-screen work buffer.
constexpr uint32_t kScreenPixels = 512u * 384u;

struct Registry {
    std::atomic<bool> active{false};
    std::atomic<bool> screensNamed{false};
    // Per block base: 0 none, 1 offscreen, 2 screen-sized.
    std::array<std::atomic<uint8_t>, kBlocks> kind{};
    std::array<std::atomic<uint8_t>, kBlocks> psm{};
    std::mutex mutex;
    std::array<Target, kBlocks> targets{};
};

Registry &registry() {
    static Registry instance;
    return instance;
}

} // namespace

void declare(const Target &target) {
    if (target.base >= kBlocks || target.width == 0u || target.height == 0u) return;
    Registry &r = registry();
    // 1 offscreen, 2 screen-sized, 3 frame, 4 display; a frame or display
    // naming sticks over what SetFrameFromDesc says of the same base.
    uint8_t kind = target.width * target.height >= kScreenPixels ? 2u : 1u;
    if (target.kind == Kind::Frame)
        kind = 3u;
    else if (target.kind == Kind::Display)
        kind = 4u;
    else if (r.kind[target.base].load(std::memory_order_relaxed) >= 3u)
        return;
    if (kind >= 3u)
        r.screensNamed.store(true, std::memory_order_release);
    if (r.kind[target.base].load(std::memory_order_relaxed) != kind ||
        r.psm[target.base].load(std::memory_order_relaxed) != target.psm) {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.targets[target.base] = target;
        r.psm[target.base].store(static_cast<uint8_t>(target.psm), std::memory_order_relaxed);
        r.kind[target.base].store(kind, std::memory_order_release);
    }
    r.active.store(true, std::memory_order_release);
}

bool active() {
    return registry().active.load(std::memory_order_acquire);
}

bool offscreen(uint32_t base, uint32_t psm) {
    if (base >= kBlocks) return false;
    const Registry &r = registry();
    static const bool keepScreens = std::getenv("DQ8_GFX_KEEP_SCREEN_IMPORTS") != nullptr;
    const uint8_t kind = r.kind[base].load(std::memory_order_acquire);
    const bool work = kind == 1u || (kind == 2u && !keepScreens && r.screensNamed.load(std::memory_order_acquire));
    return work && r.psm[base].load(std::memory_order_relaxed) == (psm & 0x3fu);
}

bool find(uint32_t base, Target &out) {
    if (base >= kBlocks) return false;
    Registry &r = registry();
    if (r.kind[base].load(std::memory_order_acquire) == 0u) return false;
    std::lock_guard<std::mutex> lock(r.mutex);
    out = r.targets[base];
    return true;
}

void reset() {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (auto &k : r.kind) k.store(0u, std::memory_order_relaxed);
    r.active.store(false, std::memory_order_release);
}

} // namespace dq8::gfx::namedtargets
