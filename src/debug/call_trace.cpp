#include "debug/call_trace.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

namespace rg::debug
{
namespace
{

struct Traced
{
    PS2Runtime::RecompiledFunction original;
    unsigned calls = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Traced> g_traced;
std::vector<uint32_t> g_peek;
std::vector<uint32_t> g_arg0Peek;
unsigned g_limit = 16;

std::vector<uint32_t> parseList(const char *list)
{
    std::vector<uint32_t> out;
    for (const char *p = list; p && *p;)
    {
        char *end = nullptr;
        const uint32_t value = static_cast<uint32_t>(std::strtoul(p, &end, 0));
        if (end == p)
        {
            break;
        }
        out.push_back(value);
        p = (*end == ',') ? end + 1 : end;
    }
    return out;
}

void printPeek(const uint8_t *rdram)
{
    for (uint32_t address : g_peek)
    {
        const uint32_t physical = address & 0x1FFFFFFCu;
        if (physical + 4u <= PS2_RAM_SIZE)
        {
            uint32_t word = 0;
            std::memcpy(&word, rdram + physical, sizeof(word));
            std::printf(" [0x%08X]=0x%08X", address, word);
        }
    }
}

// Words at a0+offset, each also shown as a float.
void printArg0Peek(const uint8_t *rdram, uint32_t a0)
{
    for (uint32_t offset : g_arg0Peek)
    {
        const uint32_t physical = (a0 + offset) & 0x1FFFFFFCu;
        if (physical + 4u <= PS2_RAM_SIZE)
        {
            uint32_t word = 0;
            float value = 0.0f;
            std::memcpy(&word, rdram + physical, sizeof(word));
            std::memcpy(&value, &word, sizeof(value));
            std::printf(" +0x%X=0x%08X(%g)", offset, word, value);
        }
    }
}

// Printable text at a guest address, or empty if it does not look like text.
std::string text(const uint8_t *rdram, uint32_t address)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical >= PS2_RAM_SIZE || physical < 0x1000u)
    {
        return {};
    }
    std::string out;
    for (uint32_t i = 0; i < 48 && physical + i < PS2_RAM_SIZE; ++i)
    {
        const unsigned char c = rdram[physical + i];
        if (c == 0)
        {
            return out.empty() ? std::string() : "\"" + out + "\"";
        }
        if (!std::isprint(c))
        {
            return {};
        }
        out += static_cast<char>(c);
    }
    return "\"" + out + "...\"";
}

// The dispatcher sets ctx->pc to the callee's address before calling it,
// which is how one wrapper serves every traced address.
void traced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t address = ctx->pc;
    PS2Runtime::RecompiledFunction original = nullptr;
    unsigned call = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Traced &t = g_traced.at(address);
        original = t.original;
        call = ++t.calls;
    }
    const bool log = call <= g_limit;
    const uint32_t a0 = getRegU32(ctx, 4);
    if (log)
    {
        const GuestThread *thread = runtime->eeScheduler().currentThread();
        std::printf("[trace] 0x%08X #%u thread=%d sp=0x%08X ra=0x%08X", address, call, thread ? thread->id : -1,
                    getRegU32(ctx, 29), getRegU32(ctx, 31));
        for (int reg = 4; reg <= 7; ++reg)
        {
            const uint32_t value = getRegU32(ctx, reg);
            const std::string s = text(rdram, value);
            std::printf(" a%d=0x%08X%s%s", reg - 4, value, s.empty() ? "" : " ", s.c_str());
        }
        printPeek(rdram);
        printArg0Peek(rdram, a0);
        std::printf("\n");
    }
    const auto started = std::chrono::steady_clock::now();
    original(rdram, ctx, runtime);
    if (log)
    {
        // pc other than ra: the callee yielded to the scheduler and resumes later.
        // The host time the call took, to tell a slow function from a slow frame.
        std::printf("[trace] 0x%08X #%u -> v0=0x%08X pc=0x%08X (%.2f ms)", address, call, getRegU32(ctx, 2), ctx->pc,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        printPeek(rdram);
        printArg0Peek(rdram, a0);
        if (!g_arg0Peek.empty())
        {
            std::printf(" f0=%g", ctx->f[0]);
        }
        std::printf("\n");
        std::fflush(stdout);
    }
}

// RG_DUMP=0xFUNC:path: FUNC(dst, src, size) calls, each appended to path as
// dst and size (two little-endian words) and the size bytes at src.
PS2Runtime::RecompiledFunction g_dumpOriginal = nullptr;
FILE *g_dumpFile = nullptr;

void dumped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t dst = getRegU32(ctx, 4);
    const uint32_t src = getRegU32(ctx, 5);
    const uint32_t size = getRegU32(ctx, 6);
    const uint32_t physical = src & 0x1FFFFFFFu;
    if (size <= 0x100000u && physical + size <= PS2_RAM_SIZE)
    {
        const uint32_t header[2] = {dst, size};
        std::fwrite(header, sizeof(header), 1, g_dumpFile);
        std::fwrite(rdram + physical, 1, size, g_dumpFile);
        std::fflush(g_dumpFile);
    }
    g_dumpOriginal(rdram, ctx, runtime);
}

void installDump(PS2Runtime &runtime)
{
    const char *spec = std::getenv("RG_DUMP");
    if (!spec || !*spec)
    {
        return;
    }
    char *end = nullptr;
    const uint32_t address = static_cast<uint32_t>(std::strtoul(spec, &end, 0));
    if (!end || *end != ':')
    {
        return;
    }
    g_dumpOriginal = runtime.lookupFunction(address);
    g_dumpFile = std::fopen(end + 1, "wb");
    if (g_dumpOriginal && g_dumpFile && runtime.replaceFunction(address, &dumped))
    {
        std::printf("[trace] dumping calls to 0x%08X into %s\n", address, end + 1);
        return;
    }
    std::fprintf(stderr, "[trace] RG_DUMP: 0x%08X is not a function in the main table, or %s cannot be written\n",
                 address, end + 1);
    if (g_dumpFile)
    {
        std::fclose(g_dumpFile);
        g_dumpFile = nullptr;
    }
}

} // namespace

void installWatch(PS2Runtime &runtime)
{
    std::vector<uint32_t> watch = parseList(std::getenv("RG_WATCH"));
    if (watch.empty())
    {
        return;
    }
    const char *ms = std::getenv("RG_WATCH_MS");
    const auto period = std::chrono::milliseconds(ms ? std::strtoul(ms, nullptr, 0) : 1000u);
    // Racy by design: a debugging aid that reads through the same path the
    // guest does, I/O registers included, while the guest runs.
    std::thread([&runtime, watch, period]() {
        for (unsigned tick = 1;; ++tick)
        {
            std::this_thread::sleep_for(period);
            std::printf("[watch] #%u", tick);
            for (uint32_t address : watch)
            {
                std::printf(" [0x%08X]=0x%08X", address, runtime.memory().read32(address));
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }).detach();
}

void installCallTrace(PS2Runtime &runtime)
{
    installWatch(runtime);
    installDump(runtime);
    const char *list = std::getenv("RG_TRACE");
    if (!list || !*list)
    {
        return;
    }
    if (const char *limit = std::getenv("RG_TRACE_LIMIT"))
    {
        g_limit = static_cast<unsigned>(std::strtoul(limit, nullptr, 0));
    }
    g_peek = parseList(std::getenv("RG_TRACE_PEEK"));
    g_arg0Peek = parseList(std::getenv("RG_TRACE_ARG0"));
    for (uint32_t address : parseList(list))
    {
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
        if (original && runtime.replaceFunction(address, &traced))
        {
            g_traced[address] = Traced{original};
            std::printf("[trace] tracing 0x%08X\n", address);
        }
        else
        {
            std::fprintf(stderr, "[trace] 0x%08X is not a function in the main table\n", address);
        }
    }
}

} // namespace rg::debug
