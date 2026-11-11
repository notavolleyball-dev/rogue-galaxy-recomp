#include "gs_null.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "runtime/gs/gs_backend.h"

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace rg::graphics
{
namespace
{

bool abortOnCrossing()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_GS");
        return value && std::strcmp(value, "abort") == 0;
    }();
    return on;
}

enum Call : size_t
{
    kSubmit,
    kSubmitMany,
    kSubmitNativeMesh,
    kBeginTransfer,
    kUploadImage,
    kFlush,
    kTextureFlush,
    kSync,
    kPresent,
    kClearFramebuffer,
    kConsumeLocalToHost,
    kReadVram,
    kWriteVram,
    kSnapshotVram,
    kTransferSnapshot,
    kPreparePresentation,
    kCallCount
};
constexpr std::array<const char *, kCallCount> kNames = {
    "Submit",       "SubmitMany",       "SubmitNativeMesh", "BeginTransfer",
    "UploadImage",  "Flush",            "TextureFlush",     "Sync",               "Present",
    "ClearFramebuffer", "ConsumeLocalToHostBytes", "ReadVram", "WriteVram",       "SnapshotVram",
    "GetTransferSnapshot", "PreparePresentation"};

std::array<std::atomic<uint64_t>, kCallCount> g_counts{};

// A crossing of `call`: counted, and the first of each kind printed.
void cross(Call call)
{
    const uint64_t n = g_counts[call].fetch_add(1u, std::memory_order_relaxed);
    if (n != 0u)
        return;
    static std::mutex printing;
    std::lock_guard lock(printing);
    std::fprintf(stderr, "[native-gs] the GS backend was entered: %s\n", kNames[call]);
#if defined(__linux__) || defined(__APPLE__)
    void *frames[32];
    const int depth = backtrace(frames, 32);
    backtrace_symbols_fd(frames, depth, STDERR_FILENO);
#endif
    if (abortOnCrossing())
        std::abort();
}

class NullGsBackend final : public GSRasterBackend
{
public:
    ~NullGsBackend() override
    {
        std::fprintf(stderr, "[native-gs] GS backend calls:");
        bool any = false;
        for (size_t i = 0; i < kCallCount; ++i)
            if (const uint64_t n = g_counts[i].load())
            {
                std::fprintf(stderr, " %s %llu", kNames[i], static_cast<unsigned long long>(n));
                any = true;
            }
        std::fprintf(stderr, "%s\n", any ? "" : " none");
    }

    // The runtime hands over its GS local memory; nothing here mirrors it.
    void Initialize(uint8_t *, uint32_t) override {}
    void Reset() override {}
    void Submit(const GSPrimitiveBatch &) override { cross(kSubmit); }
    void SubmitMany(const GSPrimitiveBatch *const *, size_t) override { cross(kSubmitMany); }
    void SubmitNativeMesh(const GSNativeMesh &, const GSDrawState &) override { cross(kSubmitNativeMesh); }
    void BeginTransfer(const GSTransferCommand &) override { cross(kBeginTransfer); }
    void UploadImage(const uint8_t *, uint32_t) override { cross(kUploadImage); }
    void Flush() override { cross(kFlush); }
    void TextureFlush() override { cross(kTextureFlush); }
    void Sync(GSSyncReason) override { cross(kSync); }
    PresentationFrame Present(const GSPresentationRequest &) override
    {
        cross(kPresent);
        return {};
    }
    bool ClearFramebuffer(const GSContext &, uint32_t) override
    {
        cross(kClearFramebuffer);
        return true;
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override
    {
        cross(kConsumeLocalToHost);
        return 0u;
    }
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override
    {
        cross(kReadVram);
        return 0u;
    }
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { cross(kWriteVram); }
    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        cross(kSnapshotVram);
        out.clear();
    }
    GSTransferSnapshot GetTransferSnapshot() const override
    {
        cross(kTransferSnapshot);
        return {};
    }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &) override
    {
        cross(kPreparePresentation);
        return {};
    }
};

} // namespace

std::unique_ptr<GSRasterBackend> nullGsBackend()
{
    return std::make_unique<NullGsBackend>();
}

} // namespace rg::graphics
