#include "gfx/gs/gs_named_textures.h"

#include <algorithm>
#include <atomic>
#include <cstring>
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

struct HeldQueue {
    std::mutex mutex;
    std::deque<Held> held;
    std::atomic<bool> any{false};
};

HeldQueue &heldQueue() {
    static HeldQueue instance;
    return instance;
}

} // namespace

void hold(Held held) {
    HeldQueue &q = heldQueue();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.held.push_back(std::move(held));
    q.any.store(true, std::memory_order_release);
}

bool takeHeld(uint64_t transfer, Held &out) {
    HeldQueue &q = heldQueue();
    if (!q.any.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(q.mutex);
    // Ones for transfers already gone by never reached the backend as held
    // (none should): dropped.
    while (!q.held.empty() && q.held.front().transfer < transfer)
        q.held.pop_front();
    bool found = false;
    if (!q.held.empty() && q.held.front().transfer == transfer) {
        out = std::move(q.held.front());
        q.held.pop_front();
        found = true;
    }
    q.any.store(!q.held.empty(), std::memory_order_release);
    return found;
}

bool anyHeld() {
    return heldQueue().any.load(std::memory_order_acquire);
}

// Four lanes of eight bytes (one serial chain per byte was most of a named
// texture's identity check).
uint64_t imageHash(const uint8_t *at, size_t size) {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t lane[4] = {k, k ^ 1u, k ^ 2u, k ^ 3u};
    size_t left = size;
    while (left >= 32u) {
        for (int i = 0; i < 4; ++i) {
            uint64_t word;
            std::memcpy(&word, at + i * 8, 8);
            lane[i] = (lane[i] ^ word) * k;
            lane[i] ^= lane[i] >> 29;
        }
        at += 32;
        left -= 32u;
    }
    uint64_t tail = 0u;
    if (left)
        std::memcpy(&tail, at, left);
    uint64_t h = size;
    for (uint64_t v : {lane[0], lane[1], lane[2], lane[3], tail}) {
        h = (h ^ v) * k;
        h ^= h >> 31;
    }
    return h | 1u;
}

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
