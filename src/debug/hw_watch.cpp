#include "debug/hw_watch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace rg::debug
{
namespace
{

DWORD64 g_hostAddress = 0;
uint32_t g_guestAddress = 0;
uint32_t g_low = 0x01FD0000u;
uint32_t g_high = 0x02000000u;
std::atomic<uint64_t> g_hits{0};
std::atomic<uint32_t> g_reports{0};
constexpr uint32_t kMaxReports = 64;

void describe(DWORD64 address, char *out, size_t size)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *name = path;
        for (const char *p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
            {
                name = p + 1;
            }
        }
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
        return;
    }
    std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

// The last writes to the slot, with the writer's call stack, for when a
// function reads back a value that another one left there.
struct Write
{
    uint32_t value = 0;
    uint32_t previous = 0;
    DWORD thread = 0;
    uint64_t hit = 0;
    DWORD64 frames[14] = {};
};
Write g_ring[8];
std::atomic<uint32_t> g_ringNext{0};
uint32_t g_lastValue = 0;
DWORD64 g_lastWriter = 0;  // native entry of the function that wrote it
bool g_watchReads = false;
DWORD64 g_readerBegin = 0; // native entry of RG_HWWATCH_READER's function, if set

void unwind(const CONTEXT &start, DWORD64 *frames, int count)
{
    CONTEXT ctx = start;
    for (int frame = 0; frame < count && ctx.Rip; ++frame)
    {
        frames[frame] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            ctx.Rip = *reinterpret_cast<const DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void *handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
}

void printFrames(const DWORD64 *frames, int count)
{
    char where[MAX_PATH + 32];
    for (int frame = 0; frame < count && frames[frame]; ++frame)
    {
        describe(frames[frame], where, sizeof(where));
        std::fprintf(stderr, "[hwwatch]     #%02d %s\n", frame, where);
    }
}

DWORD64 functionOf(DWORD64 rip)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
    return fn ? imageBase + fn->BeginAddress : 0;
}

LONG WINAPI onException(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_hostAddress == 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // DR6 names the cause. Another slot's breakpoint (B1-B3) or a debugger's
    // single step (BS) without our B0 is somebody else's trap; a zero DR6
    // says nothing, and ours is the only watch, so that one is ours. The
    // bits stay set until cleared, so clear them for the next trap.
    const DWORD64 cause = info->ContextRecord->Dr6 & 0x400Full;
    if (cause != 0 && (cause & 0x1ull) == 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    info->ContextRecord->Dr6 = 0;
    // Reading the slot here traps again when reads are watched.
    static thread_local bool t_inside = false;
    if (t_inside)
    {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    struct Inside
    {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void *>(g_hostAddress), sizeof(value));
    const uint64_t hit = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool inRange = value >= g_low && value < g_high;
    if (!g_watchReads)
    {
        // Writes only: report the ones that leave a value in range.
        if (inRange && g_reports.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
        {
            std::fprintf(stderr, "[hwwatch] write #%llu to 0x%08X left 0x%08X, thread %lu:\n",
                         static_cast<unsigned long long>(hit), g_guestAddress, value, GetCurrentThreadId());
            DWORD64 frames[40] = {};
            unwind(*info->ContextRecord, frames, 40);
            printFrames(frames, 40);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (value != g_lastValue)
    {
        Write &entry = g_ring[g_ringNext.fetch_add(1, std::memory_order_relaxed) % 8u];
        entry = Write{};
        entry.value = value;
        entry.previous = g_lastValue;
        entry.thread = GetCurrentThreadId();
        entry.hit = hit;
        unwind(*info->ContextRecord, entry.frames, 14);
        g_lastValue = value;
        g_lastWriter = functionOf(info->ContextRecord->Rip);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const DWORD64 reader = functionOf(info->ContextRecord->Rip);
    if (inRange && reader != g_lastWriter && (!g_readerBegin || reader == g_readerBegin) &&
        g_reports.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
    {
        std::fprintf(stderr, "[hwwatch] access #%llu: a function read 0x%08X from 0x%08X that another left, thread %lu\n",
                     static_cast<unsigned long long>(hit), value, g_guestAddress, GetCurrentThreadId());
        DWORD64 frames[24] = {};
        unwind(*info->ContextRecord, frames, 24);
        printFrames(frames, 24);
        const uint32_t next = g_ringNext.load(std::memory_order_relaxed);
        for (uint32_t i = next >= 8u ? next - 8u : 0u; i < next; ++i)
        {
            const Write &entry = g_ring[i % 8u];
            std::fprintf(stderr, "[hwwatch]   earlier write (access #%llu, thread %lu): 0x%08X -> 0x%08X\n",
                         static_cast<unsigned long long>(entry.hit), entry.thread, entry.previous, entry.value);
            printFrames(entry.frames, 14);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool arm(DWORD threadId)
{
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (!thread)
    {
        return false;
    }
    bool armed = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1))
    {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &ctx))
        {
            ctx.Dr0 = g_hostAddress;
            // Slot 0: local enable, break on writes (or on any access, to
            // catch the reader), 4 bytes.
            const DWORD64 rw = g_watchReads ? 0x3ull : 0x1ull;
            ctx.Dr7 = (ctx.Dr7 & ~(0x3ull | (0xFull << 16))) | 0x1ull | (rw << 16) | (0x3ull << 18);
            armed = SetThreadContext(thread, &ctx) != 0;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return armed;
}

// Arms every thread of the process, again as new ones appear. No allocation
// while a thread is suspended: it may hold the heap lock.
void armLoop(unsigned delaySeconds)
{
    // Until then the slot may hold something else.
    std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
    constexpr size_t kMaxThreads = 1024;
    static DWORD armedIds[kMaxThreads];
    size_t armedCount = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    auto lastSummary = std::chrono::steady_clock::now();
    for (;;)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                {
                    continue;
                }
                bool known = false;
                for (size_t i = 0; i < armedCount && !known; ++i)
                {
                    known = armedIds[i] == entry.th32ThreadID;
                }
                if (!known && armedCount < kMaxThreads && arm(entry.th32ThreadID))
                {
                    armedIds[armedCount++] = entry.th32ThreadID;
                }
            }
            CloseHandle(snap);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSummary > std::chrono::seconds(10))
        {
            lastSummary = now;
            std::fprintf(stderr, "[hwwatch] %zu threads armed, %llu writes to 0x%08X so far\n", armedCount,
                         static_cast<unsigned long long>(g_hits.load()), g_guestAddress);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

void installHardwareWatch(PS2Runtime &runtime)
{
    const char *spec = std::getenv("RG_HWWATCH");
    if (!spec || !*spec)
    {
        return;
    }
    // Physical, like the other tools take their addresses, so a KSEG0/1 form
    // works and the range check below cannot wrap.
    const uint32_t address = static_cast<uint32_t>(std::strtoul(spec, nullptr, 0)) & 0x1FFFFFFCu;
    if (address + 4u > PS2_RAM_SIZE)
    {
        std::fprintf(stderr, "[hwwatch] 0x%08X is outside RAM\n", address);
        return;
    }
    if (const char *range = std::getenv("RG_HWWATCH_RANGE"))
    {
        char *end = nullptr;
        g_low = static_cast<uint32_t>(std::strtoul(range, &end, 0));
        if (end && *end == '-')
        {
            g_high = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0));
        }
    }
    if (const char *reader = std::getenv("RG_HWWATCH_READER"))
    {
        const uint32_t guest = static_cast<uint32_t>(std::strtoul(reader, nullptr, 0));
        if (runtime.hasFunction(guest))
        {
            g_readerBegin = reinterpret_cast<DWORD64>(runtime.lookupFunction(guest));
        }
    }
    const char *reads = std::getenv("RG_HWWATCH_READS");
    g_watchReads = g_readerBegin != 0 || (reads && *reads && *reads != '0');
    g_guestAddress = address;
    std::memcpy(&g_lastValue, runtime.memory().getRDRAM() + address, sizeof(g_lastValue));
    g_hostAddress = reinterpret_cast<DWORD64>(runtime.memory().getRDRAM() + address);
    AddVectoredExceptionHandler(1, &onException);
    const char *delay = std::getenv("RG_HWWATCH_DELAY");
    std::thread(armLoop, delay ? static_cast<unsigned>(std::strtoul(delay, nullptr, 0)) : 0u).detach();
    std::fprintf(stderr, "[hwwatch] watching writes to 0x%08X that leave 0x%08X-0x%08X\n", address, g_low,
                 g_high);
}

} // namespace rg::debug

#else

namespace rg::debug
{
void installHardwareWatch(PS2Runtime &) {}
} // namespace rg::debug

#endif
