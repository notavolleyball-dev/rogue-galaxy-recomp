#include "debug/bug_mark.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "Stubs/Pad.h"
#include "flip_pacing.h"
#include "gfx/native/native_pipeline.h"
#include "runtime/ps2_pad_host.h"

#if __has_include(<renderdoc_app.h>)
#include <renderdoc_app.h>
#define RG_HAVE_RENDERDOC 1
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

#ifndef _WIN32
extern char **environ;
#endif

namespace rg::debug
{
namespace
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::atomic<unsigned> g_requests{0u};
Clock::time_point g_start{};
// Flips a capture runs over. The renderer's thread draws behind the game's
// flips and nothing here says where its frame starts, so the capture is
// started and ended by hand (which also works with a hidden window, where
// nothing is presented for RenderDoc to count) and is long enough to hold
// one whole frame: tools/bugmark picks the last complete one.
constexpr uint64_t kCaptureFlips = 3u;

// The mark being finished: its pieces come over the next frames (the capture
// at the next present, the dumps at the next flip).
struct Open
{
    fs::path directory;
    Clock::time_point at{};
    fs::file_time_type filesFrom{};
    uintmax_t logFrom = 0u;
    uint32_t capturesBefore = 0u;
    // RenderDoc is recording until this flip (or a second has passed).
    uint64_t captureUntilFlip = 0u;
    bool capturing = false;
    bool open = false;
};
Open g_open;

#ifdef RG_HAVE_RENDERDOC
// RenderDoc's in-application API when the game runs under it (its Vulkan
// layer has loaded the library by the time the window exists), else null.
// Its own capture key and overlay go: F12 is the menu's.
RENDERDOC_API_1_6_0 *renderDoc()
{
    static RENDERDOC_API_1_6_0 *api = [] {
        RENDERDOC_API_1_6_0 *found = nullptr;
        pRENDERDOC_GetAPI getApi = nullptr;
#ifdef _WIN32
        if (HMODULE module = GetModuleHandleA("renderdoc.dll"))
            getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
#else
        if (void *module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD))
            getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
#endif
        if (getApi && getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&found)) == 1 && found)
        {
            found->SetCaptureKeys(nullptr, 0);
            found->MaskOverlayBits(0u, 0u);
            // Each mesh call named in the capture (native_pipeline.h).
            dq8::gfx::native::setDrawLabels(true);
            std::fprintf(stderr, "[bugmark] RenderDoc is here: F8 captures the frame\n");
            return found;
        }
        return static_cast<RENDERDOC_API_1_6_0 *>(nullptr);
    }();
    return api;
}
#endif

uint32_t padReads()
{
    return ps2_stubs::getPadDebugSnapshot().ports[0][0].readCount;
}

// RG_BUG_MARK_AT's pad reads, ascending; each marks once.
bool scriptedMarkDue()
{
    static std::vector<uint32_t> reads = [] {
        std::vector<uint32_t> parsed;
        const char *value = std::getenv("RG_BUG_MARK_AT");
        for (const char *at = value; at && *at;)
        {
            char *end = nullptr;
            const unsigned long read = std::strtoul(at, &end, 10);
            if (end == at)
                break;
            parsed.push_back(static_cast<uint32_t>(read));
            at = *end == ',' ? end + 1 : end;
        }
        return parsed;
    }();
    static size_t next = 0u;
    if (next >= reads.size() || padReads() < reads[next])
        return false;
    ++next;
    return true;
}

std::string commandLine(const char *command)
{
    std::string text;
#ifndef _WIN32
    if (FILE *pipe = popen(command, "r"))
    {
        char line[256];
        if (std::fgets(line, sizeof(line), pipe))
            text = line;
        pclose(pipe);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
#else
    (void)command;
#endif
    return text;
}

void touch(const char *path)
{
    if (FILE *file = std::fopen(path, "w"))
        std::fclose(file);
}

void begin(SDL_Window *window)
{
    const std::time_t now = std::time(nullptr);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::error_code error;
    fs::path directory = fs::path("build/run/marks") / stamp;
    for (int n = 2; fs::exists(directory, error); ++n)
        directory = fs::path("build/run/marks") / (std::string(stamp) + "-" + std::to_string(n));
    fs::create_directories(directory, error);
    if (error)
    {
        std::fprintf(stderr, "[bugmark] cannot make %s: %s\n", directory.string().c_str(),
                     error.message().c_str());
        return;
    }

    int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
    float mouseX = 0.0f, mouseY = 0.0f;
    if (window)
    {
        SDL_GetWindowSize(window, &width, &height);
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    }
    SDL_GetMouseState(&mouseX, &mouseY);
    const bool mouseInside = window && SDL_GetMouseFocus() == window;
    const float scaleX = width > 0 ? static_cast<float>(pixelWidth) / static_cast<float>(width) : 1.0f;
    const float scaleY = height > 0 ? static_cast<float>(pixelHeight) / static_cast<float>(height) : 1.0f;

    if (FILE *info = std::fopen((directory / "info.txt").string().c_str(), "w"))
    {
        std::fprintf(info, "time %s\n", stamp);
        std::fprintf(info, "seconds %.1f\n", std::chrono::duration<double>(Clock::now() - g_start).count());
        std::fprintf(info, "branch %s\n", commandLine("git rev-parse --abbrev-ref HEAD 2>/dev/null").c_str());
        std::fprintf(info, "commit %s\n", commandLine("git rev-parse --short HEAD 2>/dev/null").c_str());
        std::fprintf(info, "dirty %s\n",
                     commandLine("git status --porcelain --untracked-files=no 2>/dev/null | wc -l").c_str());
        std::fprintf(info, "flip %llu\n", static_cast<unsigned long long>(rg::graphics::guestFlips()));
        std::fprintf(info, "pad_read %u\n", padReads());
        std::fprintf(info, "guest_frame %llu\n", static_cast<unsigned long long>(ps2PadCurrentGuestFrame()));
        std::fprintf(info, "window %d %d\n", width, height);
        std::fprintf(info, "window_pixels %d %d\n", pixelWidth, pixelHeight);
        // In window pixels, as the swapchain image has them; -1 -1 when the
        // mouse was not over the window (a scripted mark, a pad player).
        if (mouseInside)
            std::fprintf(info, "mouse_pixels %d %d\n", static_cast<int>(mouseX * scaleX),
                         static_cast<int>(mouseY * scaleY));
        else
            std::fprintf(info, "mouse_pixels -1 -1\n");
#ifndef _WIN32
        for (char **entry = environ; entry && *entry; ++entry)
            if (std::strncmp(*entry, "RG_", 3) == 0 || std::strncmp(*entry, "DQ8_", 4) == 0)
                std::fprintf(info, "env %s\n", *entry);
#endif
        std::fclose(info);
    }
    if (const char *settings = std::getenv("RG_SETTINGS_FILE"); settings && *settings)
        fs::copy_file(settings, directory / "settings.ini", fs::copy_options::overwrite_existing, error);

    g_open = {};
    g_open.directory = directory;
    g_open.at = Clock::now();
    g_open.filesFrom = fs::file_time_type::clock::now();
    if (const char *log = std::getenv("RG_LOG_FILE"); log && *log)
    {
        const uintmax_t size = fs::file_size(log, error);
        g_open.logFrom = error ? 0u : size;
    }
    g_open.open = true;

    bool capture = false;
#ifdef RG_HAVE_RENDERDOC
    if (RENDERDOC_API_1_6_0 *api = renderDoc())
    {
        g_open.capturesBefore = api->GetNumCaptures();
        api->SetCaptureFilePathTemplate((fs::absolute(directory, error) / "frame").string().c_str());
        api->StartFrameCapture(nullptr, nullptr);
        g_open.captureUntilFlip = rg::graphics::guestFlips() + kCaptureFlips;
        g_open.capturing = true;
        capture = true;
    }
#endif
    // Announced before the dumps are asked for, so they follow it in the log.
    std::fprintf(stderr, "[bugmark] begin %s (flip %llu, pad read %u, %s)\n", directory.string().c_str(),
                 static_cast<unsigned long long>(rg::graphics::guestFlips()), padReads(),
                 capture ? "with a RenderDoc capture" : "no RenderDoc: no frame capture");
    touch("build/run/dump-now");
    touch("build/run/depth-log-now");
}

// The pieces that came since: the depth logs, and the log from a little
// before the mark (the stats line it follows) to now.
void finish()
{
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator("build/run", error))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("depthlog-", 0) == 0 && entry.is_regular_file(error) &&
            entry.last_write_time(error) >= g_open.filesFrom)
            fs::rename(entry.path(), g_open.directory / name, error);
    }
    if (const char *log = std::getenv("RG_LOG_FILE"); log && *log)
    {
        constexpr uintmax_t kBefore = 64u * 1024u;
        std::ifstream in(log, std::ios::binary);
        std::ofstream out(g_open.directory / "log.txt", std::ios::binary);
        if (in && out)
        {
            in.seekg(static_cast<std::streamoff>(g_open.logFrom > kBefore ? g_open.logFrom - kBefore : 0u));
            out << in.rdbuf();
        }
    }
    unsigned captures = 0u;
#ifdef RG_HAVE_RENDERDOC
    if (RENDERDOC_API_1_6_0 *api = renderDoc())
        captures = api->GetNumCaptures() - g_open.capturesBefore;
#endif
    std::fprintf(stderr, "[bugmark] done %s (%u capture%s)\n", g_open.directory.string().c_str(), captures,
                 captures == 1u ? "" : "s");
    g_open.open = false;
}

} // namespace

void requestBugMark()
{
    g_requests.fetch_add(1u, std::memory_order_relaxed);
}

void pollBugMarks(SDL_Window *window)
{
    if (g_start == Clock::time_point{})
        g_start = Clock::now();
#ifdef RG_HAVE_RENDERDOC
    renderDoc();
#endif
    if (g_open.open)
    {
        // One at a time: a second key while one is open is the same mark.
        g_requests.store(0u, std::memory_order_relaxed);
#ifdef RG_HAVE_RENDERDOC
        if (g_open.capturing && (rg::graphics::guestFlips() >= g_open.captureUntilFlip ||
                                 Clock::now() - g_open.at >= std::chrono::seconds(1)))
        {
            g_open.capturing = false;
            if (RENDERDOC_API_1_6_0 *api = renderDoc())
                api->EndFrameCapture(nullptr, nullptr);
        }
#endif
        if (Clock::now() - g_open.at >= std::chrono::seconds(2))
            finish();
        return;
    }
    // Looked for a few times a second, not at every pump.
    static Clock::time_point lastLook{};
    bool wanted = g_requests.exchange(0u, std::memory_order_relaxed) != 0u;
    if (const Clock::time_point now = Clock::now(); !wanted && now - lastLook >= std::chrono::milliseconds(250))
    {
        lastLook = now;
        wanted = std::remove("build/run/mark-now") == 0;
    }
    if (!wanted)
        wanted = scriptedMarkDue();
    if (wanted)
        begin(window);
}

} // namespace rg::debug
