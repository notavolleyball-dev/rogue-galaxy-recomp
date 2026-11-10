#include "guest_write_watch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "runtime/guest_host_writes.h"

#if defined(__linux__)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace rg::writewatch
{
namespace
{
constexpr size_t kPage = 4096u;

// The whole pages inside guest RAM, [g_first, g_first + g_pages * kPage).
uint8_t *g_first = nullptr;
size_t g_pages = 0u;
// Per page: watched (read-only) or not, and the stamp of its last write
// seen (a fault or an unwatch).
std::unique_ptr<std::atomic<uint8_t>[]> g_watched;
std::unique_ptr<std::atomic<uint64_t>[]> g_lastWrite;
std::atomic<uint64_t> g_stamp{1u};
// State changes and their mprotect together: watch on the producer's
// thread, the fault handler on whichever wrote. A thread holding it never
// writes guest RAM, so the handler never waits on its own thread.
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
bool g_enabled = false;

struct Lock
{
    Lock()
    {
        while (g_lock.test_and_set(std::memory_order_acquire))
        {
        }
    }
    ~Lock() { g_lock.clear(std::memory_order_release); }
};

#if defined(__linux__)
struct sigaction g_previous{};

// A run of pages made writable, each marked written. Caller holds the lock.
void release(size_t first, size_t count)
{
    const uint64_t stamp = g_stamp.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    bool any = false;
    for (size_t p = first; p < first + count; ++p)
    {
        g_lastWrite[p].store(stamp, std::memory_order_release);
        any |= g_watched[p].exchange(0u, std::memory_order_acq_rel) != 0u;
    }
    if (any)
        mprotect(g_first + first * kPage, count * kPage, PROT_READ | PROT_WRITE);
}

void onFault(int signal, siginfo_t *info, void *context)
{
    const auto *at = static_cast<const uint8_t *>(info->si_addr);
    if (g_first && at >= g_first && at < g_first + g_pages * kPage)
    {
        const size_t page = static_cast<size_t>(at - g_first) / kPage;
        Lock lock;
        if (g_watched[page].load(std::memory_order_acquire) != 0u)
        {
            release(page, 1u);
            return; // the write runs again, now allowed
        }
        // Not ours to fault (another thread released it meanwhile): the
        // write runs again as well.
        return;
    }
    // Not a watched page: whatever handled it before.
    if (g_previous.sa_flags & SA_SIGINFO)
    {
        if (g_previous.sa_sigaction)
        {
            g_previous.sa_sigaction(signal, info, context);
            return;
        }
    }
    else if (g_previous.sa_handler != SIG_DFL && g_previous.sa_handler != SIG_IGN && g_previous.sa_handler)
    {
        g_previous.sa_handler(signal);
        return;
    }
    // The default: the fault again, unhandled.
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
}
#endif

// The range's pages, [first, end); false when it is not all whole pages
// of guest RAM.
bool pagesOf(const uint8_t *at, size_t size, size_t &first, size_t &end)
{
    if (!g_enabled || !at || size == 0u || at < g_first || at + size > g_first + g_pages * kPage)
        return false;
    first = static_cast<size_t>(at - g_first) / kPage;
    end = (static_cast<size_t>(at - g_first) + size + kPage - 1u) / kPage;
    return true;
}

void beforeHostWriteHook(void *at, size_t size)
{
    unwatch(at, size);
}
} // namespace

bool enabled()
{
    return g_enabled;
}

void init(uint8_t *ram, size_t size)
{
#if defined(__linux__)
    if (g_first || !ram || size < 2u * kPage)
        return;
    if (const char *v = std::getenv("RG_WRITE_WATCH"); v && *v == '0')
        return;
    const uintptr_t start = (reinterpret_cast<uintptr_t>(ram) + kPage - 1u) & ~uintptr_t(kPage - 1u);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(ram) + size) & ~uintptr_t(kPage - 1u);
    if (end <= start || sysconf(_SC_PAGESIZE) != static_cast<long>(kPage))
        return;
    g_pages = (end - start) / kPage;
    g_watched = std::make_unique<std::atomic<uint8_t>[]>(g_pages);
    g_lastWrite = std::make_unique<std::atomic<uint64_t>[]>(g_pages);
    for (size_t p = 0; p < g_pages; ++p)
    {
        g_watched[p].store(0u, std::memory_order_relaxed);
        g_lastWrite[p].store(0u, std::memory_order_relaxed);
    }
    struct sigaction action{};
    action.sa_sigaction = &onFault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &g_previous) != 0)
        return;
    g_first = reinterpret_cast<uint8_t *>(start);
    g_beforeHostWrite = &beforeHostWriteHook;
    g_enabled = true;
    std::fprintf(stderr, "[write-watch] guest RAM watched by page (%zu pages)\n", g_pages);
#else
    (void)ram;
    (void)size;
#endif
}

uint64_t watch(const uint8_t *at, size_t size)
{
#if defined(__linux__)
    size_t first, end;
    if (!pagesOf(at, size, first, end))
        return 0u;
    Lock lock;
    // Runs of pages not yet watched, made read-only together.
    for (size_t p = first; p < end;)
    {
        if (g_watched[p].load(std::memory_order_acquire) != 0u)
        {
            ++p;
            continue;
        }
        size_t q = p;
        while (q < end && g_watched[q].load(std::memory_order_acquire) == 0u)
            g_watched[q++].store(1u, std::memory_order_release);
        mprotect(g_first + p * kPage, (q - p) * kPage, PROT_READ);
        p = q;
    }
    // After the pages are read-only: a write from here on is after the stamp.
    return g_stamp.load(std::memory_order_acquire);
#else
    (void)at;
    (void)size;
    return 0u;
#endif
}

bool unchanged(const uint8_t *at, size_t size, uint64_t stamp)
{
    size_t first, end;
    if (stamp == 0u || !pagesOf(at, size, first, end))
        return false;
    for (size_t p = first; p < end; ++p)
        if (g_watched[p].load(std::memory_order_acquire) == 0u ||
            g_lastWrite[p].load(std::memory_order_acquire) > stamp)
            return false;
    return true;
}

void unwatch(void *at, size_t size)
{
#if defined(__linux__)
    if (!g_enabled || !at || size == 0u)
        return;
    // The part of it inside the watched pages.
    const auto *lo = static_cast<const uint8_t *>(at);
    const uint8_t *hi = lo + size;
    const uint8_t *top = g_first + g_pages * kPage;
    if (hi <= g_first || lo >= top)
        return;
    lo = lo < g_first ? g_first : lo;
    hi = hi > top ? top : hi;
    const size_t first = static_cast<size_t>(lo - g_first) / kPage;
    const size_t end = (static_cast<size_t>(hi - g_first) + kPage - 1u) / kPage;
    Lock lock;
    release(first, end - first);
#else
    (void)at;
    (void)size;
#endif
}

} // namespace rg::writewatch
