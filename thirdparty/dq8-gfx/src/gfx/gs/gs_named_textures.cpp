#include "gfx/gs/gs_named_textures.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace dq8::gfx::namedtextures {
namespace {

// A frame uploads a few hundred; the bound keeps records that never reach
// the GS (a list built and dropped) from piling up.
constexpr size_t kMaxQueued = 4096u;
// How far past the head a transfer may match.
constexpr size_t kLookahead = 64u;

struct Queue {
    std::mutex mutex;
    std::deque<Upload> records;
    std::atomic<bool> active{false};
    Stats stats;
};

Queue &queue() {
    static Queue instance;
    return instance;
}

} // namespace

void push(const Upload &upload) {
    Queue &q = queue();
    std::lock_guard<std::mutex> lock(q.mutex);
    if (q.records.size() >= kMaxQueued) {
        q.records.pop_front();
        ++q.stats.dropped;
    }
    q.records.push_back(upload);
    ++q.stats.pushed;
    q.active.store(true, std::memory_order_release);
}

bool take(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, Upload &out) {
    Queue &q = queue();
    if (!q.active.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(q.mutex);
    const size_t limit = std::min(q.records.size(), kLookahead);
    for (size_t i = 0; i < limit; ++i) {
        const Upload &r = q.records[i];
        if (r.dbp == dbp && r.dbw == dbw && (r.dpsm & 0x3fu) == (dpsm & 0x3fu) && r.width == width &&
            r.height == height) {
            out = r;
            q.stats.dropped += i;
            q.records.erase(q.records.begin(), q.records.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            ++q.stats.matched;
            return true;
        }
    }
    return false;
}

bool peek(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height) {
    Queue &q = queue();
    if (!q.active.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(q.mutex);
    const size_t limit = std::min(q.records.size(), kLookahead);
    for (size_t i = 0; i < limit; ++i) {
        const Upload &r = q.records[i];
        if (r.dbp == dbp && r.dbw == dbw && (r.dpsm & 0x3fu) == (dpsm & 0x3fu) && r.width == width &&
            r.height == height)
            return true;
    }
    return false;
}

bool active() {
    return queue().active.load(std::memory_order_acquire);
}

void reset() {
    Queue &q = queue();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.records.clear();
}

Stats stats() {
    Queue &q = queue();
    std::lock_guard<std::mutex> lock(q.mutex);
    return q.stats;
}

} // namespace dq8::gfx::namedtextures
