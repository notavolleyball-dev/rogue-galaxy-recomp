#include "debug/sampler.h"

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rg::debug
{
namespace
{

std::string threadName(HANDLE thread)
{
    using GetDescription = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto get = reinterpret_cast<GetDescription>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    std::string out;
    PWSTR wide = nullptr;
    if (get && SUCCEEDED(get(thread, &wide)) && wide)
    {
        for (const wchar_t *p = wide; *p; ++p)
        {
            out += (*p < 128 && *p > 32) ? static_cast<char>(*p) : '_';
        }
        LocalFree(wide);
    }
    return out.empty() ? "unnamed" : out;
}

// A loaded image's unwind table (.pdata), read once up front.
struct UnwindImage
{
    uintptr_t base = 0;
    uintptr_t end = 0;
    const RUNTIME_FUNCTION *functions = nullptr;
    size_t count = 0;
};

std::vector<UnwindImage> loadUnwindImages()
{
    std::vector<UnwindImage> images;
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(modules, &me); ok; ok = Module32NextW(modules, &me))
    {
        UnwindImage image;
        image.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        image.end = image.base + me.modBaseSize;
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image.base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image.base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (dir.VirtualAddress && dir.Size)
        {
            image.functions = reinterpret_cast<const RUNTIME_FUNCTION *>(image.base + dir.VirtualAddress);
            image.count = dir.Size / sizeof(RUNTIME_FUNCTION);
        }
        images.push_back(image);
    }
    CloseHandle(modules);
    std::sort(images.begin(), images.end(), [](const UnwindImage &a, const UnwindImage &b) { return a.base < b.base; });
    return images;
}

// RtlLookupFunctionEntry without its locks, which a suspended thread may hold.
const RUNTIME_FUNCTION *lookupUnwind(const std::vector<UnwindImage> &images, uintptr_t rip, uintptr_t &imageBase)
{
    auto it = std::upper_bound(images.begin(), images.end(), rip,
                               [](uintptr_t value, const UnwindImage &image) { return value < image.base; });
    if (it == images.begin())
    {
        return nullptr;
    }
    const UnwindImage &image = *--it;
    if (rip >= image.end || !image.functions)
    {
        return nullptr;
    }
    imageBase = image.base;
    const uint32_t rva = static_cast<uint32_t>(rip - image.base);
    size_t lo = 0, hi = image.count;
    while (lo < hi)
    {
        const size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION &fn = image.functions[mid];
        if (rva < fn.BeginAddress)
        {
            hi = mid;
        }
        else if (rva >= fn.EndAddress)
        {
            lo = mid + 1;
        }
        else
        {
            return &fn;
        }
    }
    return nullptr;
}

constexpr int kCallerDepth = 16;

// The innermost return addresses inside the executable, for a sample that
// landed outside it (a driver, the C runtime, a kernel wait): walks the
// stack with the x64 unwind tables, which clang's MinGW target emits. Runs
// while the thread is suspended, so it takes no locks and allocates nothing.
int exeCallers(const std::vector<UnwindImage> &images, CONTEXT ctx, uintptr_t exeBase, uintptr_t exeEnd,
               uint64_t (&out)[kCallerDepth])
{
    int found = 0;
    for (int depth = 0; depth < 48 && found < kCallerDepth && ctx.Rip; ++depth)
    {
        uintptr_t imageBase = 0;
        const RUNTIME_FUNCTION *fn = lookupUnwind(images, ctx.Rip, imageBase);
        if (!fn)
        {
            // A leaf function: the return address is at the stack pointer.
            DWORD64 ret = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(ctx.Rsp), &ret, sizeof(ret),
                                   nullptr))
            {
                break;
            }
            ctx.Rip = ret;
            ctx.Rsp += 8;
        }
        else
        {
            void *handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, const_cast<PRUNTIME_FUNCTION>(fn), &ctx,
                             &handlerData, &establisher, nullptr);
        }
        if (ctx.Rip >= exeBase && ctx.Rip < exeEnd)
        {
            out[found++] = ctx.Rip;
        }
    }
    return found;
}

std::string callerKey(const uint64_t (&callers)[kCallerDepth], int count)
{
    std::string out;
    char buf[24];
    for (int i = 0; i < count; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%s0x%llx", i ? "," : "", static_cast<unsigned long long>(callers[i]));
        out += buf;
    }
    return out;
}

void sampleWindow(const std::string &path, double seconds)
{
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    std::map<std::pair<std::string, uint64_t>, uint64_t> counts;
    // Samples outside the executable, by thread and exe callers.
    std::map<std::pair<std::string, std::string>, uint64_t> callers;
    // RG_PROFILE_STACKS=1: samples inside the executable with their callers
    // too, for inclusive times.
    std::map<std::pair<std::string, std::string>, uint64_t> stacks;
    const char *stacksEnv = std::getenv("RG_PROFILE_STACKS");
    const bool allStacks = stacksEnv && *stacksEnv && *stacksEnv != '0';
    std::map<DWORD, std::string> names;
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t exeEnd = exeBase;
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exeBase);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exeBase + dos->e_lfanew);
        exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;
    }
    const std::vector<UnwindImage> images = loadUnwindImages();
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    uint64_t rounds = 0;
    // The process's threads, listed again every quarter second: a snapshot
    // walks every thread of the system, and taking one each round held the
    // sampler to about 13 rounds a second. RG_PROFILE_THREAD=NAME samples
    // only the threads whose name starts with NAME (e.g. VIF1), for many
    // more samples of one hot function.
    struct Sampled
    {
        HANDLE handle;
        std::pair<DWORD, std::string> label;
    };
    std::vector<Sampled> threads;
    const char *onlyEnv = std::getenv("RG_PROFILE_THREAD");
    const std::string only = onlyEnv ? onlyEnv : "";
    auto listed = std::chrono::steady_clock::time_point{};
    auto closeAll = [&] {
        for (const Sampled &th : threads)
        {
            CloseHandle(th.handle);
        }
        threads.clear();
    };
    while (std::chrono::steady_clock::now() < end)
    {
        if (std::chrono::steady_clock::now() - listed >= std::chrono::milliseconds(250))
        {
            closeAll();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                {
                    continue;
                }
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                      FALSE, te.th32ThreadID);
                if (!t)
                {
                    continue;
                }
                auto named = names.find(te.th32ThreadID);
                if (named == names.end())
                {
                    named = names.emplace(te.th32ThreadID, threadName(t) + "#" + std::to_string(te.th32ThreadID)).first;
                }
                if (!only.empty() && named->second.compare(0, only.size(), only) != 0)
                {
                    CloseHandle(t);
                    continue;
                }
                threads.push_back({t, *named});
            }
            CloseHandle(snap);
            listed = std::chrono::steady_clock::now();
        }
        for (const Sampled &th : threads)
        {
            HANDLE t = th.handle;
            const auto *it = &th.label;
            if (SuspendThread(t) != static_cast<DWORD>(-1))
            {
                // Nothing that may lock or allocate until the thread runs
                // again: it could be holding the heap or loader lock.
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                const bool got = GetThreadContext(t, &ctx) != 0;
                uint64_t chain[kCallerDepth] = {};
                int depth = 0;
                const bool outside = got && (ctx.Rip < exeBase || ctx.Rip >= exeEnd);
                if (outside || (got && allStacks))
                {
                    depth = exeCallers(images, ctx, exeBase, exeEnd, chain);
                }
                ResumeThread(t);
                if (got)
                {
                    ++counts[{it->second, ctx.Rip}];
                    if (outside)
                    {
                        ++callers[{it->second, callerKey(chain, depth)}];
                    }
                    else if (allStacks)
                    {
                        // The leaf first, then its callers.
                        uint64_t leaf[1] = {ctx.Rip};
                        const std::string callersOnly = callerKey(chain, depth);
                        char head[24];
                        std::snprintf(head, sizeof(head), "0x%llx", static_cast<unsigned long long>(leaf[0]));
                        ++stacks[{it->second, callersOnly.empty() ? std::string(head)
                                                                  : std::string(head) + "," + callersOnly}];
                    }
                }
            }
        }
        ++rounds;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closeAll();
    if (FILE *f = std::fopen(path.c_str(), "w"))
    {
        std::fprintf(f, "base 0x%llx rounds %llu\n",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                     static_cast<unsigned long long>(rounds));
        for (const auto &[key, n] : counts)
        {
            std::fprintf(f, "%s 0x%llx %llu\n", key.first.c_str(), static_cast<unsigned long long>(key.second),
                         static_cast<unsigned long long>(n));
        }
        for (const auto &[key, n] : callers)
        {
            if (!key.second.empty())
            {
                std::fprintf(f, "callers %s %s %llu\n", key.first.c_str(), key.second.c_str(),
                             static_cast<unsigned long long>(n));
            }
        }
        for (const auto &[key, n] : stacks)
        {
            std::fprintf(f, "stack %s %s %llu\n", key.first.c_str(), key.second.c_str(),
                         static_cast<unsigned long long>(n));
        }
        // The loaded modules, so samples outside the executable can be put
        // down to the driver, the C runtime or a kernel wait.
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        for (BOOL ok = Module32FirstW(modules, &me); ok; ok = Module32NextW(modules, &me))
        {
            std::string name;
            for (const wchar_t *p = me.szModule; *p; ++p)
            {
                name += (*p < 128 && *p > 32) ? static_cast<char>(*p) : '_';
            }
            std::fprintf(f, "module 0x%llx 0x%lx %s\n",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(me.modBaseAddr)),
                         static_cast<unsigned long>(me.modBaseSize), name.c_str());
        }
        CloseHandle(modules);
        std::fclose(f);
        std::printf("[profile] %llu rounds written to %s\n", static_cast<unsigned long long>(rounds), path.c_str());
        std::fflush(stdout);
    }
}

// RG_PROFILE_ON_MARK=1: a window opens at each F9 instead of at a fixed time,
// into PATH with "-1", "-2"... before its extension, so the profile holds the
// moment the player marked and nothing else (Rogue Galaxy: added). Presses
// during a window are folded into it.
std::atomic<unsigned> g_windowRequests{0u};
bool g_onMark = false;

std::string numberedPath(const std::string &path, unsigned index)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    const std::string suffix = "-" + std::to_string(index);
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        return path + suffix;
    }
    return path.substr(0, dot) + suffix + path.substr(dot);
}

void run(std::string path, double startSeconds, double seconds)
{
    if (!g_onMark)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
        sampleWindow(path, seconds);
        return;
    }
    unsigned served = 0u, index = 0u;
    for (;;)
    {
        if (g_windowRequests.load(std::memory_order_acquire) == served)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const std::string out = numberedPath(path, ++index);
        std::printf("[profile] window %u: %.0f s from the mark, into %s\n", index, seconds, out.c_str());
        std::fflush(stdout);
        sampleWindow(out, seconds);
        served = g_windowRequests.load(std::memory_order_acquire);
    }
}

} // namespace

void requestProfileWindow()
{
    g_windowRequests.fetch_add(1u, std::memory_order_acq_rel);
}

void startSampler()
{
    const char *path = std::getenv("RG_PROFILE");
    if (!path || !*path)
    {
        return;
    }
    const char *start = std::getenv("RG_PROFILE_START");
    const char *seconds = std::getenv("RG_PROFILE_SECONDS");
    const char *onMark = std::getenv("RG_PROFILE_ON_MARK");
    g_onMark = onMark && *onMark && *onMark != '0';
    std::thread(run, std::string(path), start ? std::atof(start) : 0.0, seconds ? std::atof(seconds) : 20.0).detach();
}

} // namespace rg::debug

#else

namespace rg::debug
{
void startSampler() {}
void requestProfileWindow() {}
} // namespace rg::debug

#endif
