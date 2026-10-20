#include "debug/sampler.h"

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
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

// stopSampler sets g_stop: an open window ends early and is written (the
// player closing the game a second before a window's end lost the whole
// profile, 2026-10-20); g_windowOpen tells stopSampler to wait for that.
std::atomic<bool> g_stop{false};
std::atomic<bool> g_windowOpen{false};

// What a profile file holds: the counts of a stretch of sampling.
struct Samples
{
    std::map<std::pair<std::string, uint64_t>, uint64_t> counts;
    // Samples outside the executable, by thread and exe callers.
    std::map<std::pair<std::string, std::string>, uint64_t> callers;
    // RG_PROFILE_STACKS=1: samples inside the executable with their callers
    // too, for inclusive times.
    std::map<std::pair<std::string, std::string>, uint64_t> stacks;
    uint64_t rounds = 0;

    void add(const Samples &other)
    {
        for (const auto &[key, n] : other.counts)
        {
            counts[key] += n;
        }
        for (const auto &[key, n] : other.callers)
        {
            callers[key] += n;
        }
        for (const auto &[key, n] : other.stacks)
        {
            stacks[key] += n;
        }
        rounds += other.rounds;
    }
};

// Samples every thread of the process but its own, one round at a time.
class Sampler
{
public:
    Sampler()
    {
        const char *stacksEnv = std::getenv("RG_PROFILE_STACKS");
        m_allStacks = stacksEnv && *stacksEnv && *stacksEnv != '0';
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m_exeBase);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(m_exeBase + dos->e_lfanew);
        m_exeEnd = m_exeBase + nt->OptionalHeader.SizeOfImage;
        const char *onlyEnv = std::getenv("RG_PROFILE_THREAD");
        m_only = onlyEnv ? onlyEnv : "";
    }
    ~Sampler() { closeAll(); }
    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    void round(Samples &into)
    {
        const auto now = std::chrono::steady_clock::now();
        // The unwind tables of the loaded modules, again every few seconds:
        // a sampler running from the start would otherwise miss the GPU
        // driver, which loads later.
        if (now - m_imagesLoaded >= std::chrono::seconds(5))
        {
            m_images = loadUnwindImages();
            m_imagesLoaded = now;
        }
        if (now - m_listed >= std::chrono::milliseconds(250))
        {
            listThreads();
            m_listed = std::chrono::steady_clock::now();
        }
        for (const Sampled &th : m_threads)
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
                const bool outside = got && (ctx.Rip < m_exeBase || ctx.Rip >= m_exeEnd);
                if (outside || (got && m_allStacks))
                {
                    depth = exeCallers(m_images, ctx, m_exeBase, m_exeEnd, chain);
                }
                ResumeThread(t);
                if (got)
                {
                    ++into.counts[{it->second, ctx.Rip}];
                    if (outside)
                    {
                        ++into.callers[{it->second, callerKey(chain, depth)}];
                    }
                    else if (m_allStacks)
                    {
                        // The leaf first, then its callers.
                        const std::string callersOnly = callerKey(chain, depth);
                        char head[24];
                        std::snprintf(head, sizeof(head), "0x%llx", static_cast<unsigned long long>(ctx.Rip));
                        ++into.stacks[{it->second, callersOnly.empty() ? std::string(head)
                                                                      : std::string(head) + "," + callersOnly}];
                    }
                }
            }
        }
        ++into.rounds;
    }

private:
    struct Sampled
    {
        HANDLE handle;
        std::pair<DWORD, std::string> label;
    };

    // The process's threads, listed again every quarter second: a snapshot
    // walks every thread of the system, and taking one each round held the
    // sampler to about 13 rounds a second. RG_PROFILE_THREAD=NAME samples
    // only the threads whose name starts with NAME (e.g. VIF1), for many
    // more samples of one hot function.
    void listThreads()
    {
        closeAll();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != m_pid || te.th32ThreadID == m_self)
            {
                continue;
            }
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!t)
            {
                continue;
            }
            auto named = m_names.find(te.th32ThreadID);
            if (named == m_names.end())
            {
                named = m_names.emplace(te.th32ThreadID, threadName(t) + "#" + std::to_string(te.th32ThreadID)).first;
            }
            if (!m_only.empty() && named->second.compare(0, m_only.size(), m_only) != 0)
            {
                CloseHandle(t);
                continue;
            }
            m_threads.push_back({t, *named});
        }
        CloseHandle(snap);
    }

    void closeAll()
    {
        for (const Sampled &th : m_threads)
        {
            CloseHandle(th.handle);
        }
        m_threads.clear();
    }

    const DWORD m_self = GetCurrentThreadId();
    const DWORD m_pid = GetCurrentProcessId();
    const uintptr_t m_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t m_exeEnd = 0;
    bool m_allStacks = false;
    std::string m_only;
    std::vector<UnwindImage> m_images;
    std::chrono::steady_clock::time_point m_imagesLoaded{};
    std::map<DWORD, std::string> m_names;
    std::vector<Sampled> m_threads;
    std::chrono::steady_clock::time_point m_listed{};
};

void writeProfile(const std::string &path, const Samples &samples)
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f)
    {
        return;
    }
    std::fprintf(f, "base 0x%llx rounds %llu\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                 static_cast<unsigned long long>(samples.rounds));
    for (const auto &[key, n] : samples.counts)
    {
        std::fprintf(f, "%s 0x%llx %llu\n", key.first.c_str(), static_cast<unsigned long long>(key.second),
                     static_cast<unsigned long long>(n));
    }
    for (const auto &[key, n] : samples.callers)
    {
        if (!key.second.empty())
        {
            std::fprintf(f, "callers %s %s %llu\n", key.first.c_str(), key.second.c_str(),
                         static_cast<unsigned long long>(n));
        }
    }
    for (const auto &[key, n] : samples.stacks)
    {
        std::fprintf(f, "stack %s %s %llu\n", key.first.c_str(), key.second.c_str(),
                     static_cast<unsigned long long>(n));
    }
    // The loaded modules, so samples outside the executable can be put
    // down to the driver, the C runtime or a kernel wait.
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
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
    std::fprintf(stderr, "[profile] %llu rounds written to %s\n",
                 static_cast<unsigned long long>(samples.rounds), path.c_str());
}

std::chrono::milliseconds roundPause(int replayDefault);

void sampleWindow(const std::string &path, double seconds)
{
    const auto pause = roundPause(1);
    Sampler sampler;
    Samples samples;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end && !g_stop.load(std::memory_order_relaxed))
    {
        sampler.round(samples);
        std::this_thread::sleep_for(pause);
    }
    writeProfile(path, samples);
}

// RG_PROFILE_ON_MARK=1: a window opens at each F9 instead of at a fixed time,
// into PATH with "-1", "-2"... before its extension, so the profile holds the
// moment the player marked and nothing else (Rogue Galaxy: added). Presses
// during a window are folded into it.
std::atomic<unsigned> g_windowRequests{0u};
bool g_onMark = false;

// RG_PROFILE_BEFORE=S (with RG_PROFILE_ON_MARK=1): sampling runs all the
// time, kept in quarter-second slices for the last S seconds, and a window
// holds the S seconds before its mark besides RG_PROFILE_SECONDS after it:
// a short hitch is over before F9 is pressed (Rogue Galaxy: added,
// 2026-10-20). RG_PROFILE_AUTO_MS=N with it also opens a window at a hitch
// nobody marked (noteFrameTime), the hitch and half a second either side,
// at most kMaxAutoWindows a run.
std::atomic<unsigned> g_autoRequests{0u};
std::atomic<uint32_t> g_autoFrameMillis{0u};
std::atomic<uint32_t> g_autoRecentMillis{0u};
double g_autoThresholdMillis = 0.0;
constexpr unsigned kMaxAutoWindows = 30u;

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

// The next number after the files already there: numbering from 1 at each
// start overwrote the last session's profiles.
std::string nextNumberedPath(const std::string &path, unsigned &index)
{
    for (;;)
    {
        const std::string out = numberedPath(path, ++index);
        if (GetFileAttributesA(out.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return out;
        }
    }
}

// While the sampler has a thread suspended, nothing of the game's may wait on
// it: at normal priority a preemption between SuspendThread and ResumeThread
// left the VIF1 thread or the GS worker frozen for a scheduler quantum. The
// sampler sleeps between rounds, so the top priority costs no one else.
void raisePriority()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// Opens a window unless the game is stopping. g_windowOpen goes up before
// g_stop is read, and stopSampler sets g_stop before reading g_windowOpen
// (both sequentially consistent), so either stopSampler waits for this
// window or this sees the stop and opens none: a window opened after
// stopSampler had looked would be cut mid-write by the exit.
bool openWindow()
{
    g_windowOpen.store(true);
    if (g_stop.load())
    {
        g_windowOpen.store(false);
        return false;
    }
    return true;
}

// The pause between two rounds. A round suspends every thread once and walks
// its stack while it waits: at a round a millisecond (about 380 a second)
// the VIF1 thread took 16% longer a frame at the gate, harmless for a
// 20-second window but not for sampling all the time, so the replay
// defaults to 5 ms (RG_PROFILE_INTERVAL_MS overrides both).
std::chrono::milliseconds roundPause(int replayDefault)
{
    const char *value = std::getenv("RG_PROFILE_INTERVAL_MS");
    return std::chrono::milliseconds(value && *value ? std::max(0, std::atoi(value)) : replayDefault);
}

void runReplay(const std::string &path, double before, double after)
{
    using Clock = std::chrono::steady_clock;
    raisePriority();
    const auto pause = roundPause(5);
    const auto seconds = [](double s) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
    };
    constexpr auto kSlice = std::chrono::milliseconds(250);
    Sampler sampler;
    // Each slice runs from its start to the next one's; while a window is
    // open only those reaching into it are kept, otherwise the last S s.
    std::deque<std::pair<Clock::time_point, Samples>> slices;
    slices.emplace_back(Clock::now(), Samples{});
    unsigned servedMarks = 0u, servedAuto = 0u, index = 0u, autoWindows = 0u;
    bool open = false;
    Clock::time_point from{}, until{};
    std::string out;
    const auto write = [&] {
        Samples merged;
        for (const auto &slice : slices)
        {
            merged.add(slice.second);
        }
        writeProfile(out, merged);
        open = false;
        g_windowOpen.store(false);
    };
    while (!g_stop.load(std::memory_order_relaxed))
    {
        sampler.round(slices.back().second);
        const Clock::time_point now = Clock::now();
        if (now - slices.back().first >= kSlice)
        {
            slices.emplace_back(now, Samples{});
        }
        const unsigned marks = g_windowRequests.load(std::memory_order_acquire);
        const unsigned autos = g_autoRequests.load(std::memory_order_acquire);
        if (marks != servedMarks || autos != servedAuto)
        {
            const bool marked = marks != servedMarks;
            servedMarks = marks;
            servedAuto = autos;
            if (!open && (marked || autoWindows < kMaxAutoWindows) && openWindow())
            {
                autoWindows += marked ? 0u : 1u;
                open = true;
                // A hitch found by noteFrameTime is the frame that just
                // ended: its own length and half a second either side,
                // so the profile is mostly the hitch. F9 comes late by a
                // reaction time, hence its seconds before.
                const double hitch = g_autoFrameMillis.load() / 1000.0;
                from = now - seconds(marked ? before : hitch + 0.5);
                until = now + seconds(marked ? after : 0.5);
                out = nextNumberedPath(path, index);
                // On stderr, the stats lines' stream: its lock keeps a line
                // whole, where printf on stdout cut a [gfx] line in two.
                if (marked)
                {
                    std::fprintf(stderr,
                                 "[profile] window %u: the %.0f s before the mark and %.0f s after, into %s\n",
                                 index, before, after, out.c_str());
                }
                else
                {
                    std::fprintf(stderr,
                                 "[profile] window %u: a hitch (a %u ms frame, %u ms on average lately), the "
                                 "%.1f s around it, into %s\n",
                                 index, g_autoFrameMillis.load(), g_autoRecentMillis.load(), hitch + 1.0,
                                 out.c_str());
                }
            }
        }
        if (open && now >= until)
        {
            write();
        }
        const Clock::time_point keepFrom = open ? from : now - seconds(before);
        while (slices.size() > 1u && slices[1].first <= keepFrom)
        {
            slices.pop_front();
        }
        std::this_thread::sleep_for(pause);
    }
    if (open)
    {
        write();
    }
}

void run(std::string path, double startSeconds, double seconds, double before)
{
    raisePriority();
    if (g_onMark && before > 0.0)
    {
        runReplay(path, before, seconds);
        return;
    }
    if (!g_onMark)
    {
        const auto start = std::chrono::steady_clock::now() + std::chrono::duration<double>(startSeconds);
        while (std::chrono::steady_clock::now() < start)
        {
            if (g_stop.load(std::memory_order_relaxed))
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (openWindow())
        {
            sampleWindow(path, seconds);
            g_windowOpen.store(false);
        }
        return;
    }
    unsigned served = 0u, index = 0u;
    while (!g_stop.load(std::memory_order_relaxed))
    {
        if (g_windowRequests.load(std::memory_order_acquire) == served)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!openWindow())
        {
            return;
        }
        const std::string out = nextNumberedPath(path, index);
        std::fprintf(stderr, "[profile] window %u: %.0f s from the mark, into %s\n", index, seconds, out.c_str());
        sampleWindow(out, seconds);
        g_windowOpen.store(false);
        served = g_windowRequests.load(std::memory_order_acquire);
    }
}

} // namespace

void requestProfileWindow()
{
    g_windowRequests.fetch_add(1u, std::memory_order_acq_rel);
}

void noteFrameTime(double seconds)
{
    if (g_autoThresholdMillis <= 0.0)
    {
        return;
    }
    // Called from one thread, the one preparing presentations. A hitch is a
    // frame both RG_PROFILE_AUTO_MS long and three times the usual (a slow
    // scene's every frame is not one), or a few frames in a row near twice
    // the usual: the frame rate halved for a moment.
    static double recent = 1.0 / 60.0; // about the last 5 frames
    static double usual = 1.0 / 60.0;  // about the last 50
    recent += (seconds - recent) * 0.2;
    const bool longFrame = seconds * 1000.0 >= g_autoThresholdMillis && seconds >= 3.0 * usual;
    const bool halved = recent >= 1.8 * usual && recent >= 0.025;
    usual += (seconds - usual) * 0.02;
    if (longFrame || halved)
    {
        g_autoFrameMillis.store(static_cast<uint32_t>(seconds * 1000.0));
        g_autoRecentMillis.store(static_cast<uint32_t>(recent * 1000.0));
        g_autoRequests.fetch_add(1u, std::memory_order_acq_rel);
    }
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
    const char *before = std::getenv("RG_PROFILE_BEFORE");
    const char *autoMs = std::getenv("RG_PROFILE_AUTO_MS");
    g_onMark = onMark && *onMark && *onMark != '0';
    const double beforeSeconds = before ? std::atof(before) : 0.0;
    if (g_onMark && beforeSeconds > 0.0 && autoMs)
    {
        g_autoThresholdMillis = std::atof(autoMs);
    }
    std::thread(run, std::string(path), start ? std::atof(start) : 0.0, seconds ? std::atof(seconds) : 20.0,
                beforeSeconds)
        .detach();
}

void stopSampler()
{
    g_stop.store(true);
    // A window in progress writes what it has; wait for that, not forever.
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_windowOpen.load() && std::chrono::steady_clock::now() < giveUp)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace rg::debug

#else

namespace rg::debug
{
void startSampler() {}
void stopSampler() {}
void requestProfileWindow() {}
void noteFrameTime(double) {}
} // namespace rg::debug

#endif
