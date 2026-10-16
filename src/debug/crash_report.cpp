#include "debug/crash_report.h"

#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>

#include <cstdio>

namespace rg::debug
{
namespace
{

// "module+0xoffset" for an address, without allocating: the heap may be
// what broke.
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

LONG WINAPI report(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    char where[MAX_PATH + 32];
    describe(reinterpret_cast<DWORD64>(record->ExceptionAddress), where, sizeof(where));
    std::fprintf(stderr, "\n[crash] exception 0x%08lX at %s on thread %lu\n", record->ExceptionCode, where,
                 GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        std::fprintf(stderr, "[crash] %s address 0x%llx\n", record->ExceptionInformation[0] == 1 ? "writing" : "reading",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    CONTEXT ctx = *info->ContextRecord;
    for (int frame = 0; frame < 48 && ctx.Rip; ++frame)
    {
        describe(ctx.Rip, where, sizeof(where));
        std::fprintf(stderr, "[crash]   #%02d %s\n", frame, where);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            DWORD64 ret = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(ctx.Rsp), &ret, sizeof(ret),
                                   nullptr))
            {
                break;
            }
            ctx.Rip = ret;
            ctx.Rsp += 8;
            continue;
        }
        void *handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void installCrashReport()
{
    SetUnhandledExceptionFilter(&report);
}

} // namespace rg::debug

#else

namespace rg::debug
{
void installCrashReport() {}
} // namespace rg::debug

#endif
