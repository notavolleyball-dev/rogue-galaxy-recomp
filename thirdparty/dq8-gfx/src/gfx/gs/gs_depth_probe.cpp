#include "gfx/gs/gs_depth_probe.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace dq8::gfx::depthprobe {
namespace {

constexpr uint32_t kPsmZ32 = 0x30u;
constexpr uint32_t kPsmZ24 = 0x31u;
constexpr uint32_t kPsmZ16 = 0x32u;
constexpr uint32_t kPsmZ16S = 0x3Au;

struct Result {
    bool valid = false;
    Request request;
    std::vector<uint32_t> values;
};

struct Mailbox {
    std::mutex mutex;
    std::vector<Request> wanted;
    Result results[kMaxRequests];
    Stats stats;
};

Mailbox &mailbox() {
    static Mailbox instance;
    return instance;
}

std::atomic<bool> g_served{false};

bool depthPsm(uint32_t psm) {
    return psm == kPsmZ32 || psm == kPsmZ24 || psm == kPsmZ16 || psm == kPsmZ16S;
}

uint32_t bytesPerPixel(uint32_t psm) {
    return psm == kPsmZ32 ? 4u : psm == kPsmZ24 ? 3u : 2u;
}

} // namespace

void setServed(bool served) {
    g_served.store(served, std::memory_order_release);
}

bool served() {
    return g_served.load(std::memory_order_acquire);
}

bool read(const Request &request, std::vector<uint8_t> &out) {
    const uint32_t psm = request.psm & 0x3fu;
    const uint32_t pixels = request.width * request.height;
    if (!depthPsm(psm) || pixels == 0u || request.width > kMaxSide || request.height > kMaxSide)
        return false;
    const uint32_t bpp = bytesPerPixel(psm);
    out.assign(size_t(pixels) * bpp, 0u);

    Mailbox &box = mailbox();
    std::lock_guard lock(box.mutex);
    const size_t index = box.wanted.size();
    if (index < kMaxRequests)
        box.wanted.push_back(request);
    ++box.stats.reads;
    if (index >= kMaxRequests)
        return true;
    const Result &result = box.results[index];
    // A block of another size, or of another buffer, is not this read's.
    if (!result.valid || result.request.width != request.width || result.request.height != request.height ||
        result.request.base != request.base)
        return true;
    ++box.stats.answered;
    for (uint32_t i = 0u; i < pixels; ++i) {
        const uint32_t value = result.values[i];
        for (uint32_t b = 0u; b < bpp; ++b)
            out[size_t(i) * bpp + b] = uint8_t(value >> (8u * b));
    }
    return true;
}

std::vector<Request> takeRequests() {
    Mailbox &box = mailbox();
    std::lock_guard lock(box.mutex);
    std::vector<Request> taken;
    taken.swap(box.wanted);
    return taken;
}

void publish(uint32_t index, const Request &request, const uint32_t *values) {
    if (index >= kMaxRequests)
        return;
    const uint32_t pixels = request.width * request.height;
    if (pixels == 0u || request.width > kMaxSide || request.height > kMaxSide)
        return;
    Mailbox &box = mailbox();
    std::lock_guard lock(box.mutex);
    Result &result = box.results[index];
    result.valid = true;
    result.request = request;
    result.values.assign(values, values + pixels);
    ++box.stats.published;
}

Stats stats() {
    Mailbox &box = mailbox();
    std::lock_guard lock(box.mutex);
    return box.stats;
}

void reset() {
    Mailbox &box = mailbox();
    std::lock_guard lock(box.mutex);
    box.wanted.clear();
    for (Result &result : box.results)
        result = {};
}

} // namespace dq8::gfx::depthprobe
