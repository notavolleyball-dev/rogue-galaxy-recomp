#include "debug/host_stacks.h"

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rg::debug
{
namespace
{

// A loaded module: its range, name, unwind table and, for the executable,
// the base it was linked at (the header keeps it after relocation).
struct Image
{
    uintptr_t base = 0;
    uintptr_t end = 0;
    uintptr_t linkBase = 0;
    const RUNTIME_FUNCTION *functions = nullptr;
    size_t count = 0;
    std::string name;
    bool exe = false;
};

std::vector<Image> loadImages()
{
    std::vector<Image> images;
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(modules, &me); ok; ok = Module32NextW(modules, &me))
    {
        Image image;
        image.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        image.end = image.base + me.modBaseSize;
        image.exe = image.base == exeBase;
        for (const wchar_t *p = me.szModule; *p; ++p)
        {
            image.name += (*p < 128 && *p > 32) ? static_cast<char>(*p) : '_';
        }
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image.base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image.base + dos->e_lfanew);
        // The loader writes the load address into the mapped header's
        // ImageBase, so the link-time base (what llvm-nm prints) is read
        // from the file; without it the report showed load addresses.
        image.linkBase = image.base;
        if (FILE *file = _wfopen(me.szExePath, L"rb"))
        {
            unsigned char head[4096];
            const size_t got = std::fread(head, 1, sizeof(head), file);
            std::fclose(file);
            if (got >= sizeof(IMAGE_DOS_HEADER))
            {
                const auto *fileDos = reinterpret_cast<const IMAGE_DOS_HEADER *>(head);
                const size_t ntOffset = static_cast<size_t>(fileDos->e_lfanew);
                if (ntOffset + sizeof(IMAGE_NT_HEADERS64) <= got)
                {
                    image.linkBase = static_cast<uintptr_t>(
                        reinterpret_cast<const IMAGE_NT_HEADERS64 *>(head + ntOffset)->OptionalHeader.ImageBase);
                }
            }
        }
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (dir.VirtualAddress && dir.Size)
        {
            image.functions = reinterpret_cast<const RUNTIME_FUNCTION *>(image.base + dir.VirtualAddress);
            image.count = dir.Size / sizeof(RUNTIME_FUNCTION);
        }
        images.push_back(std::move(image));
    }
    CloseHandle(modules);
    std::sort(images.begin(), images.end(), [](const Image &a, const Image &b) { return a.base < b.base; });
    return images;
}

const Image *imageOf(const std::vector<Image> &images, uintptr_t address)
{
    auto it = std::upper_bound(images.begin(), images.end(), address,
                               [](uintptr_t value, const Image &image) { return value < image.base; });
    if (it == images.begin())
    {
        return nullptr;
    }
    const Image &image = *--it;
    return address < image.end ? &image : nullptr;
}

// RtlLookupFunctionEntry without its locks, which a suspended thread may hold.
const RUNTIME_FUNCTION *lookupUnwind(const Image *image, uintptr_t rip)
{
    if (!image || !image->functions)
    {
        return nullptr;
    }
    const uint32_t rva = static_cast<uint32_t>(rip - image->base);
    size_t lo = 0, hi = image->count;
    while (lo < hi)
    {
        const size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION &fn = image->functions[mid];
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

constexpr int kMaxFrames = 32;

// The thread's frames, innermost first. Runs while it is suspended: no
// locks, no allocation.
int walk(const std::vector<Image> &images, CONTEXT ctx, uintptr_t (&out)[kMaxFrames])
{
    int count = 0;
    while (count < kMaxFrames && ctx.Rip != 0)
    {
        out[count++] = ctx.Rip;
        const Image *image = imageOf(images, ctx.Rip);
        const RUNTIME_FUNCTION *fn = lookupUnwind(image, ctx.Rip);
        const DWORD64 before = ctx.Rsp;
        if (!fn)
        {
            // A leaf function, or code outside any module: the return
            // address is at the stack pointer.
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
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image->base, ctx.Rip, const_cast<PRUNTIME_FUNCTION>(fn), &ctx,
                             &handlerData, &establisher, nullptr);
        }
        if (ctx.Rsp <= before && count > 1)
        {
            break;
        }
    }
    return count;
}

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

std::string describe(const std::vector<Image> &images, uintptr_t address)
{
    char buf[80];
    if (const Image *image = imageOf(images, address))
    {
        if (image->exe)
        {
            std::snprintf(buf, sizeof(buf), "exe!0x%llx",
                          static_cast<unsigned long long>(address - image->base + image->linkBase));
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", image->name.c_str(),
                          static_cast<unsigned long long>(address - image->base));
        }
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(address));
    }
    return buf;
}

} // namespace

void printHostStacks(std::FILE *out, const char *prefix)
{
    const std::vector<Image> images = loadImages();
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    std::fprintf(out, "%s host threads (frames innermost first; exe!0x... are link-time addresses):\n", prefix);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
        {
            continue;
        }
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!t)
        {
            continue;
        }
        const std::string name = threadName(t);
        uintptr_t frames[kMaxFrames] = {};
        int count = 0;
        bool got = false;
        if (SuspendThread(t) != static_cast<DWORD>(-1))
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            got = GetThreadContext(t, &ctx) != 0;
            if (got)
            {
                count = walk(images, ctx, frames);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        std::string line;
        for (int i = 0; i < count; ++i)
        {
            line += i ? " < " : "";
            line += describe(images, frames[i]);
        }
        std::fprintf(out, "%s   %s#%lu: %s\n", prefix, name.c_str(), static_cast<unsigned long>(te.th32ThreadID),
                     got ? line.c_str() : "(no context)");
    }
    CloseHandle(snap);
    std::fflush(out);
}

} // namespace rg::debug

#else

namespace rg::debug
{
void printHostStacks(std::FILE *, const char *) {}
} // namespace rg::debug

#endif
