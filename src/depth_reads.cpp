#include "depth_reads.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#if defined(RG_HAS_SDLGPU)
#include "gfx/gs/gs_depth_probe.h"
#endif

namespace rg::graphics
{
namespace
{

constexpr uint32_t kLocalToHost = 1u;
constexpr uint8_t kPsmZ32 = 0x30u;
constexpr uint8_t kPsmZ24 = 0x31u;
constexpr uint8_t kPsmZ16 = 0x32u;
constexpr uint8_t kPsmZ16S = 0x3Au;

class DepthReadBackend final : public GSRasterBackend
{
public:
    explicit DepthReadBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner)) {}

    void Initialize(uint8_t *vram, uint32_t vramSize) override
    {
        m_vram = vram;
        m_vramSize = vramSize;
        m_inner->Initialize(vram, vramSize);
    }
    void Reset() override
    {
        m_bytes.clear();
        m_readPos = 0u;
        m_inner->Reset();
    }

    void BeginTransfer(const GSTransferCommand &command) override
    {
        const uint8_t psm = command.bitbltbuf.spsm & 0x3Fu;
        const bool depth = psm == kPsmZ32 || psm == kPsmZ24 || psm == kPsmZ16 || psm == kPsmZ16S;
        if (command.direction == kLocalToHost && depth && probe(command))
        {
            return;
        }
        if (command.direction == kLocalToHost && depth && m_vram)
        {
            read(command, psm);
            return;
        }
        m_bytes.clear();
        m_readPos = 0u;
        m_inner->BeginTransfer(command);
    }

    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        if (m_readPos < m_bytes.size())
        {
            const size_t count = std::min<size_t>(maxBytes, m_bytes.size() - m_readPos);
            std::memcpy(dst, m_bytes.data() + m_readPos, count);
            m_readPos += count;
            return static_cast<uint32_t>(count);
        }
        return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    }

    GSTransferSnapshot GetTransferSnapshot() const override
    {
        if (m_readPos < m_bytes.size())
        {
            GSTransferSnapshot snapshot = m_snapshot;
            snapshot.localToHostPendingBytes = m_bytes.size() - m_readPos;
            return snapshot;
        }
        return m_inner->GetTransferSnapshot();
    }

    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        m_inner->SubmitNativeMesh(mesh, state);
    }
    bool SupportsNativeEffects() const override { return m_inner->SupportsNativeEffects(); }
    void SubmitNativeEffect(const GSNativeEffect &effect) override { m_inner->SubmitNativeEffect(effect); }
    void Submit(const GSPrimitiveBatch &batch) override { m_inner->Submit(batch); }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        m_inner->SubmitMany(batches, count);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { m_inner->UploadImage(data, sizeBytes); }
    void Flush() override { m_inner->Flush(); }
    void TextureFlush() override { m_inner->TextureFlush(); }
    void Sync(GSSyncReason reason) override { m_inner->Sync(reason); }
    PresentationFrame Present(const GSPresentationRequest &request) override { return m_inner->Present(request); }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        return m_inner->ClearFramebuffer(context, rgba);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        m_inner->WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
    bool SupportsPreparedPresentation() const override { return m_inner->SupportsPreparedPresentation(); }
    bool QueuesPreparedPresentation() const override { return m_inner->QueuesPreparedPresentation(); }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override { m_inner->CancelPreparedPresentations(); }

private:
    // With the SDL GPU backend, depth is drawn on the GPU only: the block
    // comes from what it last copied out of its depth target, a frame or two
    // old (dq8-gfx's gfx/gs/gs_depth_probe.h). False when that backend does
    // not serve the read.
    bool probe(const GSTransferCommand &command)
    {
#if defined(RG_HAS_SDLGPU)
        namespace probe = dq8::gfx::depthprobe;
        if (!probe::served())
        {
            return false;
        }
        probe::Request request;
        request.base = command.bitbltbuf.sbp;
        request.bufferWidth = std::max<uint32_t>(command.bitbltbuf.sbw, 1u);
        request.psm = command.bitbltbuf.spsm & 0x3Fu;
        request.x = command.trxpos.ssax;
        request.y = command.trxpos.ssay;
        request.width = command.trxreg.rrw;
        request.height = command.trxreg.rrh;
        if (!probe::read(request, m_bytes))
        {
            return false;
        }
        m_readPos = 0u;
        m_snapshot = {};
        m_snapshot.totalPixels = request.width * request.height;
        m_snapshot.copiedPixels = m_snapshot.totalPixels;
        m_snapshot.direction = kLocalToHost;
        return true;
#else
        (void)command;
        return false;
#endif
    }

    // GsTransferEngine::performLocalToHost for the depth formats: pixels in
    // row order, little-endian, 32, 24 or 16 bits each.
    void read(const GSTransferCommand &command, uint8_t psm)
    {
        const uint32_t rrw = command.trxreg.rrw;
        const uint32_t rrh = command.trxreg.rrh;
        const uint32_t sbp = command.bitbltbuf.sbp;
        const uint32_t sbw = std::max<uint32_t>(command.bitbltbuf.sbw, 1u);
        const uint32_t bytesPerPixel = psm == kPsmZ32 ? 4u : psm == kPsmZ24 ? 3u : 2u;
        m_bytes.clear();
        m_bytes.reserve(static_cast<size_t>(rrw) * rrh * bytesPerPixel);
        m_readPos = 0u;
        for (uint32_t y = 0; y < rrh; ++y)
        {
            for (uint32_t x = 0; x < rrw; ++x)
            {
                const uint32_t sx = x + command.trxpos.ssax;
                const uint32_t sy = y + command.trxpos.ssay;
                uint32_t value = 0u;
                switch (psm)
                {
                case kPsmZ32: value = GSMem::ReadZ32(m_vram, sbp, sbw, sx, sy); break;
                case kPsmZ24: value = GSMem::ReadZ24(m_vram, sbp, sbw, sx, sy); break;
                case kPsmZ16: value = GSMem::ReadZ16(m_vram, sbp, sbw, sx, sy); break;
                default: value = GSMem::ReadZ16S(m_vram, sbp, sbw, sx, sy); break;
                }
                for (uint32_t b = 0; b < bytesPerPixel; ++b)
                {
                    m_bytes.push_back(static_cast<uint8_t>(value >> (8u * b)));
                }
            }
        }
        m_snapshot = {};
        m_snapshot.totalPixels = rrw * rrh;
        m_snapshot.copiedPixels = rrw * rrh;
        m_snapshot.direction = kLocalToHost;
    }

    std::unique_ptr<GSRasterBackend> m_inner;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0u;
    std::vector<uint8_t> m_bytes;
    size_t m_readPos = 0u;
    GSTransferSnapshot m_snapshot{};
};

} // namespace

std::unique_ptr<GSRasterBackend> serveDepthReadsLocally(std::unique_ptr<GSRasterBackend> backend)
{
    const char *mode = std::getenv("RG_GS_DEPTH_READS");
    if (mode && std::strcmp(mode, "queue") == 0)
    {
        return backend;
    }
    return std::make_unique<DepthReadBackend>(std::move(backend));
}

} // namespace rg::graphics
