#include "held_uploads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "runtime/gs/gs_backend.h"

#if defined(RG_HAS_SDLGPU)
#include "gfx/gs/gs_named_textures.h"
#endif

namespace rg::graphics
{
#if defined(RG_HAS_SDLGPU)
namespace
{

namespace named = dq8::gfx::namedtextures;
using Clock = std::chrono::steady_clock;

// Bits per pixel of a host-to-local transfer in this format; 0 for one not
// held.
uint32_t transferBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u; // CT32
    case 0x02u: case 0x0Au: return 16u; // CT16, CT16S
    case 0x13u: return 8u; // T8
    case 0x14u: return 4u; // T4
    default: return 0u;
    }
}

enum class Mode { Off, Measure, On };

Mode modeFromEnv()
{
    const char *bypass = std::getenv("DQ8_GFX_NATIVE_BYPASS");
    if (!bypass || std::strcmp(bypass, "0") == 0)
        return Mode::Off;
    const char *value = std::getenv("RG_HELD_UPLOADS");
    if (value && std::strcmp(value, "0") == 0)
        return Mode::Off;
    if (value && std::strcmp(value, "measure") == 0)
        return Mode::Measure;
    return Mode::On;
}

class HeldUploadBackend final : public GSRasterBackend
{
public:
    HeldUploadBackend(std::unique_ptr<GSRasterBackend> inner, Mode mode) : m_inner(std::move(inner)), m_mode(mode) {}

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override
    {
        settle();
        m_inner->Reset();
    }
    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        settle();
        m_inner->SubmitNativeMesh(mesh, state);
    }
    bool SupportsNativeEffects() const override { return m_inner->SupportsNativeEffects(); }
    void SubmitNativeEffect(const GSNativeEffect &effect) override
    {
        settle();
        m_inner->SubmitNativeEffect(effect);
    }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        settle();
        m_inner->Submit(batch);
    }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        settle();
        m_inner->SubmitMany(batches, count);
    }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        std::lock_guard lock(m_mutex);
        release();
        const uint32_t bits = transferBits(command.bitbltbuf.dpsm);
        const uint32_t dbw = std::max<uint32_t>(command.bitbltbuf.dbw, 1u);
        const size_t expected = (static_cast<size_t>(command.trxreg.rrw) * command.trxreg.rrh * bits + 7u) / 8u;
        if (command.direction != 0u || bits == 0u || expected == 0u || command.trxpos.dsax != 0u ||
            command.trxpos.dsay != 0u ||
            !named::peek(command.bitbltbuf.dbp, dbw, command.bitbltbuf.dpsm, command.trxreg.rrw, command.trxreg.rrh))
        {
            forwardBegin(command);
            return;
        }
        m_command = command;
        m_expected = expected;
        m_bytes = std::make_shared<std::vector<uint8_t>>();
        m_bytes->reserve(expected);
        m_holding.store(true, std::memory_order_release);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        if (!m_holding.load(std::memory_order_acquire))
        {
            m_inner->UploadImage(data, sizeBytes);
            return;
        }
        std::lock_guard lock(m_mutex);
        if (!m_holding.load(std::memory_order_relaxed))
        {
            m_inner->UploadImage(data, sizeBytes);
            return;
        }
        const size_t take = std::min<size_t>(m_expected - m_bytes->size(), sizeBytes);
        m_bytes->insert(m_bytes->end(), data, data + take);
        if (m_bytes->size() >= m_expected)
            complete();
        if (take < sizeBytes)
            m_inner->UploadImage(data + take, static_cast<uint32_t>(sizeBytes - take));
    }
    void Flush() override
    {
        settle();
        m_inner->Flush();
    }
    void TextureFlush() override
    {
        settle();
        m_inner->TextureFlush();
    }
    void Sync(GSSyncReason reason) override
    {
        settle();
        m_inner->Sync(reason);
    }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        settle();
        return m_inner->Present(request);
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        settle();
        return m_inner->ClearFramebuffer(context, rgba);
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        settle();
        return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        settle();
        m_inner->WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
    GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }
    bool SupportsPreparedPresentation() const override { return m_inner->SupportsPreparedPresentation(); }
    bool QueuesPreparedPresentation() const override { return m_inner->QueuesPreparedPresentation(); }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        settle();
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override { m_inner->CancelPreparedPresentations(); }

private:
    // Anything else reaching the GS while an upload is held part-way (a
    // draw between its payloads, a cut transfer) sends what came of it, as
    // it came, first.
    void settle()
    {
        if (!m_holding.load(std::memory_order_acquire))
            return;
        std::lock_guard lock(m_mutex);
        release();
    }
    void release()
    {
        if (!m_holding.load(std::memory_order_relaxed))
            return;
        m_holding.store(false, std::memory_order_release);
        forwardBegin(m_command);
        if (!m_bytes->empty())
            m_inner->UploadImage(m_bytes->data(), static_cast<uint32_t>(m_bytes->size()));
        m_bytes.reset();
        ++m_stats.cut;
    }
    // Every transfer sent on is numbered, as the backend numbers the ones it
    // is sent (the queue and the wrappers behind it pass each one on).
    void forwardBegin(const GSTransferCommand &command)
    {
        ++m_forwarded;
        m_inner->BeginTransfer(command);
    }
    void complete()
    {
        m_holding.store(false, std::memory_order_release);
        if (m_mode == Mode::Measure)
        {
            const auto start = Clock::now();
            const uint64_t hash = named::imageHash(m_bytes->data(), m_bytes->size());
            m_stats.hashNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            measure(hash);
            forwardBegin(m_command);
            m_inner->UploadImage(m_bytes->data(), static_cast<uint32_t>(m_bytes->size()));
            m_bytes.reset();
            return;
        }
        named::Held held;
        held.transfer = m_forwarded + 1u;
        held.hash = named::imageHash(m_bytes->data(), m_bytes->size());
        held.bytes = std::move(m_bytes);
        named::hold(std::move(held));
        forwardBegin(m_command);
    }

    void measure(uint64_t hash)
    {
        const GSTransferCommand &c = m_command;
        const std::array<uint32_t, 4> shape = {c.bitbltbuf.dbw, c.bitbltbuf.dpsm & 0x3Fu, c.trxreg.rrw, c.trxreg.rrh};
        ++m_stats.held;
        m_stats.bytes += m_bytes->size();
        auto &last = m_lastAt[c.bitbltbuf.dbp];
        if (last.first == shape && last.second == hash)
            ++m_stats.sameAtBase;
        last = {shape, hash};
        if (m_seen.count(hash))
            ++m_stats.seenBefore;
        m_seen[hash] = 1u;
        const auto now = Clock::now();
        if (now - m_lastPrint >= std::chrono::seconds(2))
        {
            std::fprintf(stderr,
                         "[held-uploads] held %llu (%.1f MB) same-at-base %llu seen-before %llu cut %llu "
                         "hash %.2f ms, distinct %zu bases %zu\n",
                         (unsigned long long)m_stats.held, m_stats.bytes / 1048576.0,
                         (unsigned long long)m_stats.sameAtBase, (unsigned long long)m_stats.seenBefore,
                         (unsigned long long)m_stats.cut, m_stats.hashNanos / 1e6, m_seen.size(), m_lastAt.size());
            m_stats = {};
            m_lastPrint = now;
            if (m_seen.size() > 65536u)
                m_seen.clear();
        }
    }

    std::unique_ptr<GSRasterBackend> m_inner;
    Mode m_mode;
    std::mutex m_mutex;
    std::atomic<bool> m_holding{false};
    GSTransferCommand m_command{};
    size_t m_expected = 0u;
    std::shared_ptr<std::vector<uint8_t>> m_bytes;
    uint64_t m_forwarded = 0u;

    struct Stats
    {
        uint64_t held = 0u, bytes = 0u, sameAtBase = 0u, seenBefore = 0u, cut = 0u, hashNanos = 0u;
    } m_stats;
    std::map<uint32_t, std::pair<std::array<uint32_t, 4>, uint64_t>> m_lastAt;
    std::unordered_map<uint64_t, uint32_t> m_seen;
    Clock::time_point m_lastPrint = Clock::now();
};

} // namespace
#endif

std::unique_ptr<GSRasterBackend> holdNamedUploads(std::unique_ptr<GSRasterBackend> backend)
{
#if defined(RG_HAS_SDLGPU)
    const Mode mode = modeFromEnv();
    if (mode == Mode::Off)
        return backend;
    std::printf("[rg] named uploads held before the GS queue%s\n", mode == Mode::Measure ? " (measure only)" : "");
    return std::make_unique<HeldUploadBackend>(std::move(backend), mode);
#else
    return backend;
#endif
}

} // namespace rg::graphics
