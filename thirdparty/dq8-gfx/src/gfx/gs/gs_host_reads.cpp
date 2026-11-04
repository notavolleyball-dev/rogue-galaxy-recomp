#include "gfx/gs/gs_host_reads.h"
#include "runtime/gs/gs_boundary.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

namespace dq8::gfx::hostreads {
namespace {

struct Slot {
    Request request;
    std::vector<uint8_t> bytes;
    bool done = false;
    bool ok = false;
};

struct Mailbox {
    std::mutex mutex;
    std::condition_variable ready;
    std::map<uint32_t, Slot> slots;
    uint32_t next = 1u;
    std::atomic<bool> served{false};
};

Mailbox &mailbox() {
    static Mailbox instance;
    return instance;
}

} // namespace

void setServed(bool served) {
    mailbox().served.store(served, std::memory_order_release);
}

bool served() {
    return mailbox().served.load(std::memory_order_acquire);
}

uint32_t begin(const Request &request) {
    GS_BOUNDARY(kRead, "hostreads::begin (a target by GS address)");
    Mailbox &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    const uint32_t id = m.next;
    m.next = (m.next % 0xFFFFu) + 1u;
    Slot &slot = m.slots[id];
    slot = Slot{};
    slot.request = request;
    return id;
}

bool wait(uint32_t id, std::vector<uint8_t> &out, std::chrono::milliseconds timeout) {
    Mailbox &m = mailbox();
    std::unique_lock<std::mutex> lock(m.mutex);
    const bool finished = m.ready.wait_for(lock, timeout, [&] {
        auto it = m.slots.find(id);
        return it == m.slots.end() || it->second.done;
    });
    auto it = m.slots.find(id);
    if (it == m.slots.end())
        return false;
    const bool ok = finished && it->second.done && it->second.ok;
    if (ok)
        out = std::move(it->second.bytes);
    m.slots.erase(it);
    return ok;
}

bool pending(uint32_t id, Request &out) {
    Mailbox &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    auto it = m.slots.find(id);
    if (it == m.slots.end() || it->second.done)
        return false;
    out = it->second.request;
    return true;
}

void complete(uint32_t id, std::vector<uint8_t> &&bytes, bool ok) {
    Mailbox &m = mailbox();
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.slots.find(id);
        if (it == m.slots.end())
            return;
        it->second.bytes = std::move(bytes);
        it->second.ok = ok;
        it->second.done = true;
    }
    m.ready.notify_all();
}

} // namespace dq8::gfx::hostreads
