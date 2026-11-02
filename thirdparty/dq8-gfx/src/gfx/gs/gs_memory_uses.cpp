#include "gfx/gs/gs_memory_uses.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>

namespace dq8::gfx::memoryuses {
namespace {

constexpr size_t kKinds = static_cast<size_t>(Kind::Count);
std::array<std::atomic<uint64_t>, kKinds> g_counts{};

const char *name(Kind kind) {
    switch (kind) {
    case Kind::TextureFromMemory: return "texture";
    case Kind::LocalCopy: return "copy";
    case Kind::UploadToMemory: return "upload";
    case Kind::HostRead: return "read";
    case Kind::Resolve: return "resolve";
    default: return "?";
    }
}

} // namespace

void note(Kind kind, const char *shape) {
    g_counts[static_cast<size_t>(kind)].fetch_add(1u, std::memory_order_relaxed);
    static const bool warn = std::getenv("DQ8_GFX_WARN_MEMORY") != nullptr;
    if (!warn || !shape)
        return;
    static std::mutex mutex;
    static std::array<std::set<std::string>, kKinds> seen;
    std::lock_guard<std::mutex> lock(mutex);
    std::string line = std::string(name(kind)) + ": " + shape;
    std::set<std::string> &mine = seen[static_cast<size_t>(kind)];
    if (mine.size() < 128u && mine.insert(line).second)
        std::fprintf(stderr, "[memory-use] %s\n", line.c_str());
}

uint64_t count(Kind kind) {
    return g_counts[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
}

std::string summary() {
    char text[160];
    std::snprintf(text, sizeof text, "texture %llu, copy %llu, upload %llu, read %llu, resolve %llu",
                  static_cast<unsigned long long>(count(Kind::TextureFromMemory)),
                  static_cast<unsigned long long>(count(Kind::LocalCopy)),
                  static_cast<unsigned long long>(count(Kind::UploadToMemory)),
                  static_cast<unsigned long long>(count(Kind::HostRead)),
                  static_cast<unsigned long long>(count(Kind::Resolve)));
    return text;
}

} // namespace dq8::gfx::memoryuses
