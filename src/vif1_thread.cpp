#include "vif1_thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <string>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xmmintrin.h>

#include "ThreadNaming.h"
#include "runtime/gs/gs_boundary.h"
#include "native_producer.h"
#if defined(RG_HAS_SDLGPU)
#include "gfx/native/native_pipeline.h"
#endif
#include "debug/draw_origins.h"
#include "debug/vu1_replay.h"
#include "native_effects.h"
#include "native_renderer.h"
#include "vu1_native.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

namespace rg::vif1
{
namespace
{
// Nanoseconds the worker spent inside processVIF1Data, for busyReport().
std::atomic<uint64_t> g_busyNanos{0u};
// And the GIF stage in the GS, for the same line.
std::atomic<uint64_t> g_gifBusyNanos{0u};
// Set on the VIF1 thread: its GS packets go to the GIF stage.
thread_local bool t_onVif1Thread = false;
// The VU1 program the VIF1 thread is running, as DQ8_GFX_PROFILE_CYCLE names
// it for the draw tables (0 outside one, and unless the cycle is armed or
// RG_JITTER is set):
// 0x80000000 | its entry (a pair index) << 16 | a hash of the microcode there.
thread_local uint32_t t_vu1Tag = 0;
// And the camera it ran under (ps2_gs_detail::CameraRegistry), 0 for none.
thread_local uint32_t t_vu1Camera = 0;
// RG_DRAW_ORIGINS (debug/draw_origins.h): the program's name, when
// src/vu1_native.cpp knows it (in any RG_VU1_NATIVE mode), null otherwise.
thread_local const char *t_vu1Name = nullptr;
// RG_VU1_NATIVE (src/vu1_native.h): while a chunk done natively runs on the
// VU1, its packets are collected (check) or dropped (replace).
thread_local std::vector<std::vector<uint8_t>> *t_vu1Packets = nullptr;
thread_local bool t_dropVu1Packets = false;
// Read when the thread starts, after the menu's native renderer setting.
rg::vu1native::Mode nativeMode()
{
    static const rg::vu1native::Mode mode = rg::vu1native::modeFromEnvironment();
    return mode;
}
// RG_VU1_NATIVE_CHECK_EVERY=N: check mode compares one chunk in N (each costs
// the VU1's run, the native build and two decodes, on this thread).
const uint32_t g_checkEvery = [] {
    const char *value = std::getenv("RG_VU1_NATIVE_CHECK_EVERY");
    const unsigned long every = value ? std::strtoul(value, nullptr, 10) : 1ul;
    return static_cast<uint32_t>(every == 0ul ? 1ul : every);
}();
const bool g_drawOrigins = rg::debug::drawOriginsEnabled();
// Whether the GPU path takes the chunks slot E clips: with the native
// renderer, unless RG_VU1_CLIP_ON_VU1=1 (the VU1 clips them, as before).
// Read at first use, after the menu's setting.
struct GpuClips
{
    operator bool() const
    {
        static const bool on = [] {
            const char *vu1 = std::getenv("RG_VU1_CLIP_ON_VU1");
            return rg::nativerenderer::enabled() && !(vu1 && *vu1 && std::strcmp(vu1, "0") != 0);
        }();
        return on;
    }
} g_gpuClips;
// The native renderer's effects (src/native_effects.h), drawn by the
// renderer itself; read at first use, after the menu's setting.
bool nativeEffects()
{
    static const bool on = rg::nativefx::enabled();
    return on;
}
// RG_NATIVE_COMPOSITE: stage E's objects in object layers (deliverPacket, below).
bool composites()
{
    static const bool on = rg::nativefx::compositesBatched();
    return on;
}
// For the VIF code a VU1 program's packets came from (vif1Cursor).
PS2Memory *g_vif1Memory = nullptr;
// The VIF1 thread's list: its bytes and where each piece came from.
thread_local const uint8_t *t_listData = nullptr;
thread_local size_t t_listSize = 0u;
thread_local const std::vector<std::pair<uint32_t, uint32_t>> *t_listOrigins = nullptr;
const bool g_tagPrograms =
    std::getenv("DQ8_GFX_PROFILE_CYCLE") != nullptr || std::getenv("RG_JITTER") != nullptr || g_drawOrigins;
// RG_VU1_CAPTURE=<file> records the VU1 data memory each program starts with
// (16 KiB a start), for tools/vu1/capture_report.py; RG_VU1_CAPTURE_MAX caps
// the starts kept.
FILE *const g_capture = [] {
    const char *path = std::getenv("RG_VU1_CAPTURE");
    return path ? std::fopen(path, "wb") : nullptr;
}();
// After those, starts keep only the matrices (16 quads) for this many more
// DMA lists, to follow objects from one list to the next.
uint32_t g_captureLightLists = [] {
    const char *lists = std::getenv("RG_VU1_CAPTURE_LISTS");
    return lists ? static_cast<uint32_t>(std::strtoul(lists, nullptr, 10)) : 1500u;
}();
uint32_t g_captureLightFrom = 0u;
uint32_t g_captureLeft = [] {
    const char *max = std::getenv("RG_VU1_CAPTURE_MAX");
    return max ? static_cast<uint32_t>(std::strtoul(max, nullptr, 10)) : 4000u;
}();
} // namespace
namespace
{

constexpr uint32_t kVif1Channel = 0x10009000u;
constexpr uint32_t kGifChannel = 0x1000A000u;
constexpr uint32_t kChcrTte = 1u << 6;
constexpr uint32_t kChcrTie = 1u << 7;

// libdma, bound to runtime stubs (config/SCUS_974.90/recomp/main.toml).
constexpr uint32_t kSceDmaSend = 0x0010A040u;
constexpr uint32_t kSceDmaSendN = 0x0010A0A8u;
constexpr uint32_t kSceDmaSendI = 0x0010A120u;
constexpr uint32_t kSceDmaSync = 0x0010A2E0u;
// libgraph stubs that reach the GS paths or wait on them.
constexpr uint32_t kGsWaiters[] = {
    0x00108110u, // sceGsResetGraph
    0x001082B0u, // sceGsResetPath
    0x00108BB8u, // sceGsPutDrawEnv
    0x00108CA0u, // sceGsSetDefDBuff
    0x00108F38u, // sceGsSwapDBuff
    0x00109030u, // sceGsSyncPath
    0x00109488u, // sceGsExecStoreImage
};

// The runtime's address forms for a DMA start (Helpers/Support.h toDmaPhys).
uint32_t toDmaPhys(uint32_t addr)
{
    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        if (lower >= PS2_SCRATCHPAD_BASE && lower < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
        {
            return lower;
        }
    }
    return addr & 0x1FFFFFFFu;
}

// Bytes from local memory or scratchpad, wrapping as the runtime's DMA does.
void appendBytes(PS2Memory &memory, std::vector<uint8_t> &out, uint32_t address, uint32_t bytes)
{
    const bool scratch = memory.isScratchpad(address);
    uint32_t source = memory.translateAddress(address);
    const uint8_t *base = scratch ? memory.getScratchpad() : memory.getRDRAM();
    const uint32_t size = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
    while (bytes > 0u)
    {
        if (source >= size)
        {
            source = 0u;
        }
        const uint32_t chunk = std::min(bytes, size - source);
        out.insert(out.end(), base + source, base + source + chunk);
        bytes -= chunk;
        source += chunk;
    }
}

// The tags of a VIF1 source chain, the way PS2Memory::writeIORegister
// follows them: refe/cnt/next/ref/refs/call/ret/end, stopping at end, at an
// empty return stack or at a TIE interrupt. The visitor gets each tag's
// address and its payload's address and size, and ends the walk with false.
template <typename Visitor>
void walkChain(PS2Memory &memory, uint32_t tadr, bool tie, Visitor &&visit)
{
    uint32_t asr0 = 0u, asr1 = 0u, asp = 0u;
    for (uint32_t tags = 0u; tags < (1u << 20); ++tags)
    {
        const uint32_t tagAddress = tadr;
        uint64_t tag = 0u;
        try
        {
            const bool scratch = memory.isScratchpad(tagAddress);
            const uint32_t phys = memory.translateAddress(tagAddress);
            const uint8_t *base = scratch ? memory.getScratchpad() : memory.getRDRAM();
            const uint32_t size = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (phys + 16u > size)
            {
                break;
            }
            std::memcpy(&tag, base + phys, sizeof(tag));
        }
        catch (const std::exception &)
        {
            break;
        }
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
        const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
        const bool irq = ((tag >> 31) & 0x1u) != 0u;
        const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFu);
        uint32_t data = 0u;
        bool end = false;
        switch (id)
        {
        case 0: data = addr; tadr += 16u; end = true; break;
        case 1: data = tadr + 16u; tadr = data + qwc * 16u; break;
        case 2: data = tadr + 16u; tadr = addr; break;
        case 3:
        case 4: data = addr; tadr += 16u; break;
        case 5:
            data = tadr + 16u;
            if (asp == 0u) { asr0 = data + qwc * 16u; asp = 1u; }
            else if (asp == 1u) { asr1 = data + qwc * 16u; asp = 2u; }
            tadr = addr;
            break;
        case 6:
            data = tadr + 16u;
            if (asp == 2u) { tadr = asr1; asp = 1u; }
            else if (asp == 1u) { tadr = asr0; asp = 0u; }
            else { end = true; }
            break;
        default: data = tadr + 16u; end = true; break;
        }
        if (!visit(tagAddress, data, qwc))
        {
            break;
        }
        if (end || (irq && tie))
        {
            break;
        }
    }
}

// The bytes the chain hands the VIF, into `out`: the tag's upper half first
// when TTE is set, then its payload. The tags are walked once, their spans
// kept aside, and the copy happens once into a buffer reserved for the
// total; the game's lists run to several megabytes a frame, and growing a
// vector through them copied most of that twice, while a second walk for
// the size alone read every tag again (thousands a frame, 2% of the game
// thread in a battle).
struct ListExternal
{
    uint32_t at;
    const uint8_t *data;
    uint32_t qwc;
    uint32_t code; // the DIRECT, for the fallback
};

// RG_VIF1_REF_IMAGES (on with the native renderer): texture bytes a list
// sends by reference stay in main memory (gatherChain, List::External).
bool refImages()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_VIF1_REF_IMAGES");
        return value ? std::strcmp(value, "0") != 0 : rg::nativerenderer::enabled();
    }();
    return on;
}
std::atomic<uint64_t> g_refImageBytes{0u}, g_refImageFallbacks{0u}, g_refImageCount{0u};

void gatherChain(PS2Memory &memory, uint32_t tadr, bool tte, bool tie, std::vector<uint8_t> &out,
                 std::vector<std::pair<uint32_t, uint32_t>> *origins = nullptr,
                 std::vector<ListExternal> *externals = nullptr)
{
    out.clear();
    if (externals)
        externals->clear();
    // (address, bytes) of each piece, in order; bytes 0 for a payload left in
    // place (its quadwords and DIRECT in `kept`). send() runs on the game
    // thread alone.
    thread_local std::vector<std::pair<uint32_t, uint32_t>> spans;
    thread_local std::vector<std::pair<uint32_t, uint32_t>> kept;
    spans.clear();
    kept.clear();
    size_t total = 0u;
    const uint8_t *const rdram = memory.getRDRAM();
    walkChain(memory, tadr, tie, [&](uint32_t tagAddress, uint32_t data, uint32_t qwc) {
        if (tte)
        {
            // With TTE every tag supplies two VIFcodes before its payload.
            // A texture sent by reference (GsBuildImageUploadRefPkt_158390,
            // RGDecomp: a REF whose second VIFcode is DIRECT of exactly its
            // payload, the IMAGE tag in the CNT before it) is copied up to
            // that DIRECT; its bytes go from main memory when the list runs.
            if (externals && qwc >= 8u && !memory.isScratchpad(tagAddress) && !memory.isScratchpad(data))
            {
                const uint32_t tagPhys = memory.translateAddress(tagAddress);
                const uint32_t dataPhys = memory.translateAddress(data);
                uint32_t code = 0u;
                if (tagPhys + 16u <= PS2_RAM_SIZE)
                    std::memcpy(&code, rdram + tagPhys + 12u, 4u);
                if ((code & 0x7F000000u) == 0x50000000u && (code & 0xFFFFu) == qwc &&
                    dataPhys + qwc * 16u <= PS2_RAM_SIZE && rg::nativefx::textureSource(data, qwc * 16u))
                {
                    spans.emplace_back(tagAddress + 8u, 4u);
                    total += 4u;
                    spans.emplace_back(dataPhys, 0u);
                    kept.emplace_back(qwc, code);
                    return true;
                }
            }
            spans.emplace_back(tagAddress + 8u, 8u);
            total += 8u;
        }
        if (qwc != 0u)
        {
            spans.emplace_back(data, qwc * 16u);
            total += static_cast<size_t>(qwc) * 16u;
        }
        return true;
    });
    try
    {
        // A garbage chain can sum to more than a buffer can hold; the list
        // then ends where the copy stops, like one that cannot be read.
        out.reserve(total);
        size_t keptIndex = 0u;
        for (const auto &[address, bytes] : spans)
        {
            if (bytes == 0u)
            {
                const auto &[qwc, code] = kept[keptIndex++];
                externals->push_back({static_cast<uint32_t>(out.size()), rdram + address, qwc, code});
                continue;
            }
            if (origins)
            {
                origins->emplace_back(static_cast<uint32_t>(out.size()), address);
            }
            appendBytes(memory, out, address, bytes);
        }
    }
    catch (const std::exception &)
    {
        // As before: the list ends at the piece that could not be read.
    }
}

// The GS half of the lists. What the VIF1 thread hands the GS -- the
// packets its VU1 programs XGKICK, DIRECT packets, and the textures that
// continue across DIRECTs -- is copied here and run on a thread of its own,
// in the order the VIF1 thread made it, as the PS2's GS draws beside VU1.
// At a boss gate (40 fps, 2026-10-20) the VIF1 thread was 94% busy, a third
// of it in the GS front end and in queueing each of its 42,000 primitives a
// frame for the GS worker, and it waited for that worker at every FINISH;
// copying the packets costs a few percent. RG_GIF_THREAD=0 keeps the GS on
// the VIF1 thread, as before.
//
// Only the GS state lock is shared: the front end calls nothing back on the
// game's side (SIGNAL and FINISH set CSR bits, which are atomic), so waiting
// for this stage cannot deadlock a thread that does not hold that lock.
class GifStage
{
public:
    explicit GifStage(GS &gs) : m_gs(gs)
    {
        m_thread = std::thread([this] { run(); });
        m_id = m_thread.get_id();
    }

    std::thread::id id() const { return m_id; }

    // From the VIF1 thread: a packet or an image continuation, copied (the
    // VU reuses its XGKICK buffer, the list its bytes).
    void packet(const uint8_t *data, uint32_t size) { append(Kind::Packet, data, size); }
    void image(const uint8_t *data, uint32_t size) { append(Kind::Image, data, size); }
    // A strip of a VU1 program done natively (RG_VU1_NATIVE=gpu): its header,
    // then its vertices, for GS::submitNativeMesh.
    // With `state`, under that draw state (the converter's, phase 2 of
    // docs/NATIVE-SUBMIT-PLAN.md) rather than the front end's.
    void mesh(const GSNativeMesh &mesh, const GSDrawState *state = nullptr)
    {
        const size_t stateBytes = state ? kStateBytes : 0u;
        m_meshBytes.resize(stateBytes + sizeof(GSNativeMesh::Header) +
                           mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
        if (state)
            std::memcpy(m_meshBytes.data(), state, sizeof(*state));
        std::memcpy(m_meshBytes.data() + stateBytes, &mesh.header, sizeof(mesh.header));
        if (!mesh.vertices.empty())
        {
            std::memcpy(m_meshBytes.data() + stateBytes + sizeof(mesh.header), mesh.vertices.data(),
                        mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
        }
        append(state ? Kind::StatedMesh : Kind::Mesh, m_meshBytes.data(), static_cast<uint32_t>(m_meshBytes.size()));
    }
    // A post effect the renderer draws itself, for GS::submitNativeEffect.
    void effect(const GSNativeEffect &effect)
    {
        // The ops it uses: a route command has none.
        const size_t used = offsetof(GSNativeEffect, ops) + effect.opCount * sizeof(GSNativeEffect::Op);
        append(Kind::Effect, reinterpret_cast<const uint8_t *>(&effect), static_cast<uint32_t>(used));
    }

    // From the VIF1 thread, at the end of each list and as a list fills a
    // buffer: hands over what it staged.
    void publish()
    {
        if (m_staged.empty())
        {
            return;
        }
        const size_t bytes = m_staged.size();
        {
            std::unique_lock lock(m_mutex);
            // A GS that falls behind holds the VIF1 thread back instead of
            // letting the copies pile up.
            m_space.wait(lock, [&] { return m_outstanding == 0u || m_outstanding + bytes <= kCapacity; });
            m_buffers.push_back(std::move(m_staged));
            m_outstanding += bytes;
            ++m_published;
            m_staged.clear();
            if (!m_pool.empty())
            {
                m_staged = std::move(m_pool.back());
                m_pool.pop_back();
            }
        }
        m_work.notify_one();
    }

    // Waits until everything handed over has run; a no-op on this stage's
    // own thread.
    void drain()
    {
        if (std::this_thread::get_id() == m_id)
        {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [&] { return m_done == m_published; });
    }

    bool busy()
    {
        std::lock_guard lock(m_mutex);
        return m_done != m_published;
    }

    // Runs what is queued, then ends the thread.
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    enum class Kind : uint32_t
    {
        Packet,
        Image,
        Mesh,
        Effect,
        StatedMesh,
    };
    // A GSDrawState ahead of a stated mesh's header, padded to keep the
    // header and vertices 16-byte aligned.
    static constexpr size_t kStateBytes = (sizeof(GSDrawState) + 15u) & ~size_t{15u};
    // A multiple of sixteen bytes, so the data after it stays quadword aligned.
    struct Header
    {
        Kind kind;
        uint32_t size;
        // The VIF1 thread's MXCSR when it handed the packet over, without
        // the sticky exception flags. The GS front end's float work (Q, S
        // and T among them) ran under it: VU1 programs select rounding
        // toward zero, and under this thread's default the logged frames
        // differed in the last bit of those values.
        uint32_t mxcsr;
        // t_vu1Tag when the VIF1 thread handed the packet over.
        uint32_t tag;
        // t_vu1Camera likewise.
        uint32_t camera;
        uint32_t reserved[3];
    };
    static_assert(sizeof(Header) == 32u);

    void append(Kind kind, const uint8_t *data, uint32_t size)
    {
        if (!data || size == 0u)
        {
            return;
        }
        const Header header{kind, size, _mm_getcsr() & ~kMxcsrFlags, t_vu1Tag, t_vu1Camera, {}};
        const auto *raw = reinterpret_cast<const uint8_t *>(&header);
        m_staged.insert(m_staged.end(), raw, raw + sizeof(header));
        m_staged.insert(m_staged.end(), data, data + size);
        m_staged.resize((m_staged.size() + 15u) & ~size_t{15u});
        if (m_staged.size() >= kPublishBytes)
        {
            publish();
        }
    }

    void run()
    {
        ThreadNaming::SetCurrentThreadName("GIF");
        uint32_t mxcsr = _mm_getcsr() & ~kMxcsrFlags;
        for (;;)
        {
            std::vector<uint8_t> buffer;
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return !m_buffers.empty() || m_stop; });
                if (m_buffers.empty())
                {
                    return;
                }
                buffer = std::move(m_buffers.front());
                m_buffers.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            size_t at = 0u;
            while (at + sizeof(Header) <= buffer.size())
            {
                Header header;
                std::memcpy(&header, buffer.data() + at, sizeof(header));
                const uint8_t *data = buffer.data() + at + sizeof(Header);
                if (header.mxcsr != mxcsr)
                {
                    _mm_setcsr(header.mxcsr);
                    mxcsr = header.mxcsr;
                }
                ps2_gs_detail::t_sourceTag = header.tag;
                ps2_gs_detail::t_sourceCamera = header.camera;
                const gs_boundary::FromScope boundaryFrom(gs_boundary::kFromGifStage);
                try
                {
                    if (header.kind == Kind::Packet)
                    {
                        m_gs.processGIFPacket(data, header.size);
                    }
                    else if (header.kind == Kind::Mesh || header.kind == Kind::StatedMesh)
                    {
                        const bool stated = header.kind == Kind::StatedMesh;
                        const size_t skip = stated ? kStateBytes : 0u;
                        if (stated)
                            std::memcpy(&m_state, data, sizeof(m_state));
                        std::memcpy(&m_mesh.header, data + skip, sizeof(m_mesh.header));
                        m_mesh.vertices.resize((header.size - skip - sizeof(m_mesh.header)) /
                                               sizeof(GSNativeMesh::Vertex));
                        if (!m_mesh.vertices.empty())
                        {
                            std::memcpy(m_mesh.vertices.data(), data + skip + sizeof(m_mesh.header),
                                        m_mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
                        }
                        if (stated)
                            m_gs.submitNativeMesh(m_mesh, m_state);
                        else
                            m_gs.submitNativeMesh(m_mesh);
                    }
                    else if (header.kind == Kind::Effect)
                    {
                        std::memcpy(&m_effect, data, std::min<size_t>(header.size, sizeof(m_effect)));
                        m_gs.submitNativeEffect(m_effect);
                    }
                    else
                    {
                        m_gs.processImageContinuation(data, header.size);
                    }
                }
                catch (const std::exception &error)
                {
                    std::fprintf(stderr, "[gif] packet failed: %s\n", error.what());
                }
                at += sizeof(Header) + ((static_cast<size_t>(header.size) + 15u) & ~size_t{15u});
            }
            g_gifBusyNanos.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()),
                std::memory_order_relaxed);
            const size_t bytes = buffer.size();
            {
                std::lock_guard lock(m_mutex);
                m_outstanding -= bytes;
                ++m_done;
                if (m_pool.size() < kPoolSize && buffer.capacity() <= 4u * kPublishBytes)
                {
                    buffer.clear();
                    m_pool.push_back(std::move(buffer));
                }
            }
            m_space.notify_one();
            m_idle.notify_all();
        }
    }

    // A buffer goes over once it holds this much: a frame is several
    // megabytes, so the stage works beside the list, not after it.
    static constexpr size_t kPublishBytes = 128u * 1024u;
    static constexpr size_t kCapacity = 64u * 1024u * 1024u;
    static constexpr size_t kPoolSize = 16u;
    static constexpr uint32_t kMxcsrFlags = 0x3Fu;

    GS &m_gs;
    std::thread m_thread;
    std::thread::id m_id;
    std::mutex m_mutex;
    std::condition_variable m_work, m_space, m_idle;
    std::deque<std::vector<uint8_t>> m_buffers; // guarded by m_mutex
    std::vector<std::vector<uint8_t>> m_pool;   // guarded by m_mutex
    std::vector<uint8_t> m_staged;              // the VIF1 thread's
    std::vector<uint8_t> m_meshBytes;           // the VIF1 thread's
    GSNativeMesh m_mesh;                        // the stage's
    GSDrawState m_state{};                      // the stage's
    GSNativeEffect m_effect;
    size_t m_outstanding = 0u;
    uint64_t m_published = 0u;
    uint64_t m_done = 0u;
    bool m_stop = false;
};

GifStage *g_stage = nullptr;
// A GS packet on its way, as the memory's GIF callback sends it (below).
void deliverPacket(GS &gs, const uint8_t *data, uint32_t size);
// RG_NATIVE_GLARE: sends on the effect the VIF1 thread holds the packets of.
void flushEffect();
bool vu1Meshes();
bool inCompositeTail();
// Set while what build2D made goes to the GS: its own writes are already in
// its mirror.
thread_local bool t_converterOutput = false;
struct ConverterOutput
{
    bool previous = t_converterOutput;
    ConverterOutput() { t_converterOutput = true; }
    ~ConverterOutput() { t_converterOutput = previous; }
};
bool anyConversion();
// RG_NATIVE_COMPOSITE: sends on the composites' tails it holds.
void flushComposites();
// A native strip likewise, in order with the packets.
// `vu1Data`: the VU1 data memory a program's mesh was built from, for the
// native pipeline's mesh draw (rg::nativeproducer::mesh); null for others.
void deliverMesh(GS &gs, const GSNativeMesh &mesh, const GSDrawState *given = nullptr,
                 const uint8_t *vu1Data = nullptr);
// For busyReport's VU1 pair counts.
PS2Runtime *g_runtime = nullptr;

// One handed-over list, with the VU1 D/T-bit enables (VU0 FBRST bits 10 and
// 11) the game thread had when it started it.
struct List
{
    std::vector<uint8_t> data;
    // Texture bytes left in main memory (gatherChain): a REF payload whose
    // DIRECT continues an image transfer goes from there, at `at` in data.
    using External = ListExternal;
    std::vector<External> externals;
    bool dBit = false;
    bool tBit = false;
    // With RG_VU1_CAPTURE or RG_DRAW_ORIGINS: where each piece of `data`
    // came from, as (offset in data, EE address); and the list's number.
    std::vector<std::pair<uint32_t, uint32_t>> origins;
    uint32_t number = 0u;
};

class Vif1Thread
{
public:
    explicit Vif1Thread(PS2Runtime &runtime) : m_runtime(runtime)
    {
        m_thread = std::thread([this] { run(); });
        m_workerId = m_thread.get_id();
    }

    // Runs what is queued, then ends the thread. Before the runtime goes: a
    // list finishing after it posted its interrupt to a freed scheduler.
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    void submit(List list)
    {
        {
            std::lock_guard lock(m_mutex);
            m_lists.push_back(std::move(list));
            ++m_submitted;
        }
        m_work.notify_one();
    }

    // A buffer for the next list, with the capacity of one that has run:
    // allocating a list's megabytes afresh every time, and freeing them on
    // the other thread, was a measurable part of the game thread's send.
    std::vector<uint8_t> takeBuffer()
    {
        std::lock_guard lock(m_mutex);
        if (m_pool.empty())
        {
            return {};
        }
        std::vector<uint8_t> buffer = std::move(m_pool.back());
        m_pool.pop_back();
        return buffer;
    }

    // MSCAL/MSCNT from the list being run. The runtime's own callbacks read
    // the D/T enables from, and write VPU_STAT into, the context of whatever
    // guest thread the scheduler is running, which from this thread can be
    // mid-change or already freed; these use the list's enables and keep the
    // stop bits for the game thread (takeVpuStat).
    void runVu1(bool resume, uint32_t startPC, uint32_t top, uint32_t itop)
    {
        PS2Memory &memory = m_runtime.memory();
        VU1Interpreter &vu1 = m_runtime.vu1();
        vu1.state().dBitEnabled = m_current.dBit;
        vu1.state().tBitEnabled = m_current.tBit;
        if (g_capture && !resume && (g_captureLeft || g_captureLightLists))
        {
            const bool full = g_captureLeft != 0u;
            if (full)
            {
                --g_captureLeft;
            }
            else if (!g_captureLightFrom)
            {
                g_captureLightFrom = m_current.number;
            }
            else if (m_current.number - g_captureLightFrom >= g_captureLightLists)
            {
                g_captureLightLists = 0u;
                std::fflush(g_capture);
                goto captured;
            }
            const uint32_t tag = programTag(memory, startPC);
            const uint8_t *data = memory.getVU1Data();
            // Where the model matrix (quads 60-63) came from: found in the
            // list's bytes, its piece names the EE address. 0 if not found.
            uint32_t source = 0u;
            const auto &bytes = m_current.data;
            // (The search reads the whole list: full records only.)
            const uint8_t *found = full ? std::search(bytes.data(), bytes.data() + bytes.size(),
                                                      data + 60u * 16u, data + 64u * 16u)
                                        : bytes.data() + bytes.size();
            if (found != bytes.data() + bytes.size())
            {
                const uint32_t offset = static_cast<uint32_t>(found - bytes.data());
                for (const auto &[start, address] : m_current.origins)
                {
                    if (start > offset)
                    {
                        break;
                    }
                    source = address + (offset - start);
                }
            }
            const uint32_t header[7] = {full ? 0x44315556u : 0x4C315556u, tag, startPC, top, itop,
                                        m_current.number, source};
            std::fwrite(header, sizeof(header), 1, g_capture);
            if (full)
            {
                std::fwrite(data, 1, PS2_VU1_DATA_SIZE, g_capture);
            }
            else
            {
                // A hash of the mesh's shape, which does not move with the
                // buffer or the pose: the vertex count (quad 104 x) and the
                // distances between the four section pointers (quad 103). And
                // one of the first 64 quads at TOP.
                auto fnv = [&](uint64_t hash, const uint8_t *at, size_t count) {
                    for (size_t i = 0; i < count; ++i)
                    {
                        hash = (hash ^ at[i]) * 1099511628211ull;
                    }
                    return hash;
                };
                const bool inside = (top + 64u) * 16u <= PS2_VU1_DATA_SIZE;
                uint32_t section[4];
                uint32_t count;
                std::memcpy(section, data + 103u * 16u, sizeof(section));
                std::memcpy(&count, data + 104u * 16u, sizeof(count));
                const uint32_t relative[4] = {count, section[1] - section[0], section[2] - section[1],
                                              section[3] - section[2]};
                const uint64_t shape = fnv(1469598103934665603ull, reinterpret_cast<const uint8_t *>(relative),
                                           sizeof(relative));
                uint64_t whole = 1469598103934665603ull;
                if (inside)
                {
                    whole = fnv(whole, data + top * 16u, 64u * 16u);
                }
                const uint32_t hashes[2] = {static_cast<uint32_t>(shape ^ (shape >> 32)),
                                            static_cast<uint32_t>(whole ^ (whole >> 32))};
                std::fwrite(hashes, sizeof(hashes), 1, g_capture);
                // Projection, view, model, and the clip-space product.
                for (const uint32_t quad : {8u, 24u, 60u, 72u})
                {
                    std::fwrite(data + quad * 16u, 16u, 4u, g_capture);
                }
            }
        }
    captured:
        if (g_tagPrograms)
        {
            // A continuation (MSCNT) is the same program; a DIRECT packet
            // after the program ends carries no tag.
            if (!resume)
            {
                m_programTag = programTag(memory, startPC);
                if (g_drawOrigins)
                {
                    const rg::vu1native::Program *program =
                        rg::vu1native::identify(memory.getVU1Code(), startPC);
                    m_programName = program ? program->name : nullptr;
                }
            }
            t_vu1Tag = m_programTag;
            t_vu1Name = m_programName;
            // The matrices this program transforms with: viewport (4-7),
            // projection (8-11) and view (24-27). Quads are 16-byte rows.
            ps2_gs_detail::SceneCamera camera;
            const uint8_t *data = memory.getVU1Data();
            std::memcpy(camera.viewport, data + 4u * 16u, sizeof(camera.viewport));
            std::memcpy(camera.projection, data + 8u * 16u, sizeof(camera.projection));
            std::memcpy(camera.view, data + 24u * 16u, sizeof(camera.view));
            t_vu1Camera = ps2_gs_detail::CameraRegistry::add(camera);
        }
        // RG_VU1_NATIVE: the chunk of a program done natively, built from the
        // data memory before the VU1 changes it. The MSCAL names the program
        // (once per code image and entry), its MSCNTs run the chunks; or the
        // MSCAL is the chunk (drawsAtCall).
        const rg::vu1native::Program *native = nullptr;
        if (nativeMode() != rg::vu1native::Mode::Off)
        {
            if (!resume)
            {
                // identify hashes every candidate piece byte by byte (a
                // quarter of this thread at the boss, 2026-11-01). The game
                // composes its programs from pieces per object, so the code
                // changes often but is one of a few images: each image is
                // hashed once as it changes (a word at a time) and what each
                // entry of it is stays known for the run.
                if (memory.getVU1CodeGeneration() != m_nativeGeneration)
                {
                    m_nativeGeneration = memory.getVU1CodeGeneration();
                    m_codeHash = imageHash(memory.getVU1Code());
                }
                m_nativePC = startPC;
                const uint64_t key = m_codeHash ^ (static_cast<uint64_t>(startPC) * 0x9E3779B97F4A7C15ull);
                auto found = m_identified.find(key);
                if (found == m_identified.end())
                {
                    if (m_identified.size() >= 4096u)
                        m_identified.clear();
                    found = m_identified.emplace(key, rg::vu1native::identify(memory.getVU1Code(), startPC)).first;
                }
                m_nativeProgram = found->second;
            }
            else if (memory.getVU1CodeGeneration() != m_nativeGeneration)
            {
                // An MPG between the MSCAL and its MSCNTs: the code the
                // program was identified in is gone.
                m_nativeProgram = nullptr;
            }
            if (m_nativeProgram && resume != rg::vu1native::drawsAtCall(*m_nativeProgram))
            {
                const bool gpu = nativeMode() == rg::vu1native::Mode::Gpu;
                // The chunks that clip (slot E) go to the GPU with the native
                // renderer: the mesh carries E's near and far planes and the
                // shader clips there (gl_ClipDistance), so no driver is left
                // to cut at the eye on its own. Otherwise, and in the packet
                // modes, they go to the VU1.
                const char *reason =
                    rg::vu1native::declineChunk(*m_nativeProgram, memory.getVU1Data(), top, gpu && g_gpuClips);
                if (reason)
                {
                    m_nativeMonitor.declined(*m_nativeProgram, reason);
                }
                else if (nativeMode() == rg::vu1native::Mode::Check && m_checkCount++ % g_checkEvery != 0u)
                {
                    // Not this one: the VU1 runs it, unchecked.
                }
                else
                {
                    native = m_nativeProgram;
                    if (gpu)
                    {
                        rg::vu1native::buildMesh(*native, memory.getVU1Data(), top, m_nativePackets, m_nativeMesh);
                    }
                    else
                    {
                        rg::vu1native::buildChunk(*native, memory.getVU1Data(), top, m_nativePackets);
                    }
                    m_nativeMonitor.native(*native);
                }
            }
            m_nativeMonitor.tick();
        }
        if (!native || (nativeMode() != rg::vu1native::Mode::Skip && nativeMode() != rg::vu1native::Mode::Gpu))
        {
            if (native && nativeMode() == rg::vu1native::Mode::Check)
            {
                m_vu1Packets.clear();
                t_vu1Packets = &m_vu1Packets;
                // Its inputs, to save it if it differs.
                m_checkData.assign(memory.getVU1Data(), memory.getVU1Data() + PS2_VU1_DATA_SIZE);
                m_checkState = vu1.state();
            }
            t_dropVu1Packets = native && nativeMode() == rg::vu1native::Mode::Replace;
            static const bool replay = rg::vu1replay::enabled();
            if (replay)
            {
                rg::vu1replay::begin(resume, startPC, top, itop, m_current.number, memory.getVU1Code(),
                                     memory.getVU1CodeGeneration(), memory.getVU1Data(), vu1.state());
            }
            if (resume)
            {
                vu1.resume(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                           m_runtime.gs(), &memory, top, itop, 65536);
            }
            else
            {
                vu1.execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                            m_runtime.gs(), &memory, startPC, top, itop, 65536);
            }
            if (replay)
            {
                rg::vu1replay::end(vu1.state(), memory.getVU1Data());
            }
            t_vu1Packets = nullptr;
            t_dropVu1Packets = false;
        }
        if (native)
        {
            if (nativeMode() == rg::vu1native::Mode::Check)
            {
                const char *kind = m_nativeMonitor.check(*native, m_vu1Packets, m_nativePackets);
                if (!kind && !rg::vu1native::memoryEffectsMatch(*native, m_checkData.data(), top,
                                                                memory.getVU1Data()))
                {
                    kind = "VU1 memory";
                    m_nativeMonitor.noteKind(*native, kind);
                }
                if (kind)
                {
                    rg::vu1replay::saveChunk(native->name, kind, memory.getVU1Code(), m_nativePC, top, itop,
                                             m_current.number, m_checkData.data(), m_checkState, m_vu1Packets,
                                             vu1.state(), rg::vu1native::drawsAtCall(*native),
                                             memory.getVU1Data());
                }
            }
            else
            {
                // RG_NATIVE_VU1_MESHES: the programs with no mesh of their own
                // (outlines, shadow volumes, particles, the overlay, the
                // object set-up) have their native packets' primitives drawn
                // as meshes too, as the GsPkt blocks are (build2D): only the
                // register state goes on to the GS.
                const bool asMeshes = vu1Meshes();
                thread_local std::vector<rg::nativefx::Piece2D> pieces;
                std::string refused;
                if (asMeshes && nativeMode() == rg::vu1native::Mode::Gpu && !rg::vu1native::hasMesh(*native) &&
                    !inCompositeTail() && rg::nativefx::build2D(m_nativePackets, pieces, refused))
                {
                    const ConverterOutput converted;
                    for (const rg::nativefx::Piece2D &piece : pieces)
                    {
                        if (piece.isPacket())
                            deliverPacket(m_runtime.gs(), piece.bytes(), piece.size());
                        else
                            deliverMesh(m_runtime.gs(), piece.mesh, piece.stated ? &piece.state : nullptr);
                    }
                }
                else
                {
                    for (const auto &packet : m_nativePackets)
                    {
                        deliverPacket(m_runtime.gs(), packet.data(), static_cast<uint32_t>(packet.size()));
                    }
                }
                if (nativeMode() == rg::vu1native::Mode::Gpu && rg::vu1native::hasMesh(*native))
                {
                    deliverMesh(m_runtime.gs(), m_nativeMesh, nullptr, memory.getVU1Data());
                }
                // What the VU1 would have left in its memory, where it did not run.
                if (nativeMode() != rg::vu1native::Mode::Replace)
                {
                    rg::vu1native::applyMemoryEffects(*native, memory.getVU1Data(), top);
                }
            }
        }
        t_vu1Tag = 0u;
        t_vu1Camera = 0u;
        t_vu1Name = nullptr;
        m_vpuStat.store((vu1.state().stoppedByD ? 0x0200u : 0u) | (vu1.state().stoppedByT ? 0x0400u : 0u),
                        std::memory_order_release);
    }

    // The tag of the program starting at pair `startPC`: the entry and a hash
    // of the 1 KiB of microcode from it, which names the program whatever else
    // the code memory holds. Kept until the code memory is next written.
    uint32_t programTag(PS2Memory &memory, uint32_t startPC)
    {
        const uint64_t generation = memory.getVU1CodeGeneration();
        if (generation != m_tagGeneration)
        {
            m_tagCache.clear();
            m_tagGeneration = generation;
        }
        const auto [entry, inserted] = m_tagCache.try_emplace(startPC, 0u);
        if (inserted)
        {
            const uint32_t first = std::min<uint32_t>(startPC * 8u, PS2_VU1_CODE_SIZE);
            const uint32_t last = std::min<uint32_t>(first + 1024u, PS2_VU1_CODE_SIZE);
            const uint8_t *code = memory.getVU1Code();
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t at = first; at < last; ++at)
            {
                hash = (hash ^ code[at]) * 1099511628211ull;
            }
            const uint32_t folded = static_cast<uint16_t>(hash ^ (hash >> 16) ^ (hash >> 32) ^ (hash >> 48));
            entry->second = 0x80000000u | ((startPC & 0x7FFu) << 16) | folded;
            static std::atomic<uint32_t> logged{0u};
            if (logged.fetch_add(1u, std::memory_order_relaxed) < 6u)
            {
                std::fprintf(stderr, "[profile] VU1 program: pc 0x%03x, tag %08x\n", startPC & 0x7FFu, entry->second);
            }
        }
        return entry->second;
    }

    // VPU_STAT's VU1 D/T stop bits after the last MSCAL/MSCNT.
    uint32_t takeVpuStat() const { return m_vpuStat.load(std::memory_order_acquire); }

    // Waits until every list handed over has run, and the GS packets they
    // made with them. A no-op on the VIF1 thread itself, which never starts
    // DMA but could reach a hook, and on the GIF stage's.
    void drain()
    {
        if (std::this_thread::get_id() == m_workerId || (g_stage && std::this_thread::get_id() == g_stage->id()))
        {
            return;
        }
        {
            std::unique_lock lock(m_mutex);
            m_idle.wait(lock, [&] { return m_completed == m_submitted; });
        }
        if (g_stage)
        {
            g_stage->drain();
        }
    }

    bool busy()
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_completed != m_submitted)
            {
                return true;
            }
        }
        return g_stage && g_stage->busy();
    }

    // Lists that finished since the last call, for their DMAC completion.
    uint64_t takeFinished()
    {
        std::lock_guard lock(m_mutex);
        const uint64_t finished = m_completed - m_reported;
        m_reported = m_completed;
        return finished;
    }

private:
    // The list in pieces: its bytes up to each texture left in main memory,
    // that texture's bytes into the open image transfer from where they
    // lie, and so on. When no transfer is open for them after all, they go
    // through the interpreter as the copy would have had them.
    void runWithExternals(PS2Memory &memory)
    {
        const uint8_t *const data = m_current.data.data();
        const uint32_t size = static_cast<uint32_t>(m_current.data.size());
        uint32_t from = 0u;
        for (const List::External &external : m_current.externals)
        {
            if (external.at > from)
                memory.processVIF1Data(data + from, external.at - from);
            from = external.at;
            g_refImageBytes.fetch_add(external.qwc * 16u, std::memory_order_relaxed);
            g_refImageCount.fetch_add(1u, std::memory_order_relaxed);
            if (!memory.continueVIF1Image(external.data, external.qwc))
            {
                thread_local std::vector<uint8_t> copy;
                copy.resize(4u + external.qwc * 16u);
                std::memcpy(copy.data(), &external.code, 4u);
                std::memcpy(copy.data() + 4u, external.data, external.qwc * 16u);
                memory.processVIF1Data(copy.data(), static_cast<uint32_t>(copy.size()));
                g_refImageFallbacks.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        if (size > from)
            memory.processVIF1Data(data + from, size - from);
        const uint64_t count = g_refImageCount.load(std::memory_order_relaxed);
        static uint64_t told = 0u;
        if (count >= told + 200000u || told == 0u)
        {
            told = count;
            std::fprintf(stderr, "[vif1] textures from main memory: %llu (%llu MB), %llu through a copy\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(g_refImageBytes.load() >> 20),
                         static_cast<unsigned long long>(g_refImageFallbacks.load()));
        }
    }

    void run()
    {
        ThreadNaming::SetCurrentThreadName("VIF1");
        t_onVif1Thread = true;
        PS2Memory &memory = m_runtime.memory();
        for (;;)
        {
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return !m_lists.empty() || m_stop; });
                if (m_lists.empty())
                {
                    return;
                }
                m_current = std::move(m_lists.front());
                m_lists.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            try
            {
                if (g_drawOrigins)
                {
                    rg::debug::drawOriginsList(m_current.data.data(), m_current.data.size(), &m_current.origins);
                }
                t_listData = m_current.data.data();
                t_listSize = m_current.data.size();
                t_listOrigins = &m_current.origins;
                if (!m_current.externals.empty())
                {
                    runWithExternals(memory);
                }
                else if (!m_current.data.empty())
                {
                    memory.processVIF1Data(m_current.data.data(), static_cast<uint32_t>(m_current.data.size()));
                }
                flushEffect();
                flushComposites();
            }
            catch (const std::exception &error)
            {
                std::fprintf(stderr, "[vif1] list failed: %s\n", error.what());
            }
            // The list's GS packets go over before it counts as done, so a
            // drain that sees it done waits for them too.
            if (g_stage)
            {
                g_stage->publish();
            }
            g_busyNanos.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()),
                std::memory_order_relaxed);
            {
                std::lock_guard lock(m_mutex);
                ++m_completed;
                if (m_pool.size() < kPoolSize)
                {
                    m_current.data.clear();
                    m_pool.push_back(std::move(m_current.data));
                }
            }
            m_idle.notify_all();
            // Wakes the EE so it delivers the list's VIFcode interrupt now,
            // not at its next vblank.
            m_runtime.eeScheduler().postEvent(EeEvent{EeEventType::Dmac, 1u, 0u});
        }
    }

    PS2Runtime &m_runtime;
    std::thread m_thread;
    std::thread::id m_workerId;
    std::mutex m_mutex;
    std::condition_variable m_work;
    std::condition_variable m_idle;
    std::deque<List> m_lists;
    List m_current; // the VIF1 thread's
    static constexpr size_t kPoolSize = 8u;
    std::vector<std::vector<uint8_t>> m_pool; // guarded by m_mutex
    std::atomic<uint32_t> m_vpuStat{0u};
    // The VIF1 thread's: the running program's tag, and the tags by entry for
    // the current contents of the code memory (programTag).
    uint32_t m_programTag = 0u;
    const char *m_programName = nullptr;
    // RG_VU1_NATIVE: the program the last MSCAL started, for its MSCNTs, the
    // code image it was found in, and the packets of a chunk.
    const rg::vu1native::Program *m_nativeProgram = nullptr;
    uint64_t m_nativeGeneration = ~0ull;
    uint32_t m_nativePC = 0u;
    // The programs identified, by code image (its hash) and entry.
    std::unordered_map<uint64_t, const rg::vu1native::Program *> m_identified;
    uint64_t m_codeHash = 0u;

    // The 16 KiB of micro memory, a word at a time.
    static uint64_t imageHash(const uint8_t *code)
    {
        uint64_t hash = 0x84222325CBF29CE4ull;
        for (uint32_t i = 0; i < PS2_VU1_CODE_SIZE; i += 8u)
        {
            uint64_t word;
            std::memcpy(&word, code + i, sizeof(word));
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }
    uint32_t m_checkCount = 0u;
    rg::vu1native::Monitor m_nativeMonitor{nativeMode()};
    std::vector<std::vector<uint8_t>> m_nativePackets;
    GSNativeMesh m_nativeMesh;
    std::vector<std::vector<uint8_t>> m_vu1Packets;
    std::vector<uint8_t> m_checkData;
    VU1State m_checkState{};
    uint64_t m_tagGeneration = 0u;
    std::unordered_map<uint32_t, uint32_t> m_tagCache;
    uint64_t m_submitted = 0u;
    uint64_t m_completed = 0u;
    uint64_t m_reported = 0u;
    bool m_stop = false;
};

Vif1Thread *g_thread = nullptr;

void sendPacket(GS &gs, const uint8_t *data, uint32_t size);
bool convertAll();
void sendRaw(GS &gs, const uint8_t *data, uint32_t size);
void payOwed(GS &gs);

// RG_NATIVE_GLARE (src/native_effects.h): the packets of an effect the
// renderer draws itself, held on the VIF1 thread until something else goes to
// the GS (the effect's packets come in one run) or the list ends.
struct HeldEffect
{
    rg::nativefx::Effect effect = rg::nativefx::Effect::None;
    uint32_t unit = 0u; // a probe's
    GS *gs = nullptr;
    std::vector<std::vector<uint8_t>> packets;
};
thread_local HeldEffect t_held;
std::atomic<uint64_t> g_effectsNative{0u};
std::atomic<uint64_t> g_effectsAsPackets{0u};
std::atomic<uint64_t> g_prim2DNative{0u}, g_prim2DPackets{0u};

// The EE address of a packet in the VIF1 thread's list, or ~0u.
uint32_t listAddress(const uint8_t *data)
{
    if (!t_listData || !t_listOrigins || data < t_listData || data >= t_listData + t_listSize)
    {
        return ~0u;
    }
    const uint32_t offset = static_cast<uint32_t>(data - t_listData);
    auto it = std::upper_bound(t_listOrigins->begin(), t_listOrigins->end(), offset,
                               [](uint32_t value, const auto &piece) { return value < piece.first; });
    if (it == t_listOrigins->begin())
    {
        return ~0u;
    }
    --it;
    return it->second + (offset - it->first);
}

void flushEffect()
{
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    if (t_held.effect == rg::nativefx::Effect::None)
    {
        return;
    }
    GS &gs = *t_held.gs;
    // RG_NATIVE_2D: a GsPkt block, its primitives as native meshes and its
    // register state and uploads on to the GS, in their order.
    if (t_held.effect == rg::nativefx::Effect::Prim2D)
    {
        thread_local std::vector<rg::nativefx::Piece2D> pieces;
        std::string refused;
        if (rg::nativefx::build2D(t_held.packets, pieces, refused))
        {
            const ConverterOutput converted;
            for (const rg::nativefx::Piece2D &piece : pieces)
            {
                // Decoded once already: with every packet converted, on as
                // they are rather than through build2D again.
                if (piece.isPacket() && convertAll())
                    sendRaw(gs, piece.bytes(), piece.size());
                else if (piece.isPacket())
                    sendPacket(gs, piece.bytes(), piece.size());
                else if ((g_stage && !t_onVif1Thread ? (g_thread->drain(), true) : true) &&
                         rg::nativeproducer::piece(piece.mesh, piece.stated ? &piece.state : nullptr))
                    continue; // step 5b: the renderer's, after the lists in flight
                else if (g_stage)
                    g_stage->mesh(piece.mesh, piece.stated ? &piece.state : nullptr);
                else if (piece.stated)
                    gs.submitNativeMesh(piece.mesh, piece.state);
                else
                    gs.submitNativeMesh(piece.mesh);
            }
            ++g_prim2DNative;
        }
        else
        {
            for (const std::vector<uint8_t> &packet : t_held.packets)
                sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
            static std::vector<std::string> told;
            if (told.size() < 8u && std::find(told.begin(), told.end(), refused) == told.end())
            {
                told.push_back(refused);
                std::fprintf(stderr, "[nativefx] a 2D block stays packets: %s\n", refused.c_str());
            }
            ++g_prim2DPackets;
        }
        const uint64_t blocks = g_prim2DNative.load() + g_prim2DPackets.load();
        if (blocks == 1u || blocks % 36000u == 0u)
            std::fprintf(stderr, "[nativefx] 2D blocks: %llu native, %llu as packets\n",
                         static_cast<unsigned long long>(g_prim2DNative.load()),
                         static_cast<unsigned long long>(g_prim2DPackets.load()));
        t_held.effect = rg::nativefx::Effect::None;
        t_held.unit = 0u;
        t_held.packets.clear();
        return;
    }
    static const bool supported = gs.supportsNativeEffects();
    thread_local GSNativeEffect effect;
    thread_local std::vector<std::vector<uint8_t>> withheld;
    std::string why = "the renderer draws no effects";
    if (supported && rg::nativefx::build(t_held.effect, t_held.packets, effect, withheld, why, t_held.unit))
    {
        for (const std::vector<uint8_t> &packet : withheld)
        {
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
        }
        rg::nativeproducer::noteEffect(effect);
        if (!rg::nativeproducer::effect(effect)) // step 5b: the renderer's then, not the GS's
        {
            payOwed(gs); // an effect may read the front end's TEXA
            if (g_stage)
            {
                g_stage->effect(effect);
            }
            else
            {
                gs.submitNativeEffect(effect);
            }
        }
        const uint64_t count = ++g_effectsNative;
        if (count == 1u || count % 3600u == 0u)
        {
            std::fprintf(stderr, "[nativefx] %llu drawn by the renderer, %llu as packets\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(g_effectsAsPackets.load()));
        }
    }
    else
    {
        for (const std::vector<uint8_t> &packet : t_held.packets)
        {
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
        }
        static std::vector<std::string> told;
        why = std::string(rg::nativefx::name(t_held.effect)) + ": " + why;
        if (told.size() < (t_held.effect == rg::nativefx::Effect::Probe ? 64u : 8u) &&
            std::find(told.begin(), told.end(), why) == told.end())
        {
            told.push_back(why);
            std::fprintf(stderr, "[nativefx] an effect stays packets: %s\n", why.c_str());
            static const bool dump = std::getenv("RG_NATIVE_EFFECTS_TRACE") != nullptr;
            if (dump && t_held.effect != rg::nativefx::Effect::Probe)
                rg::nativefx::dumpPackets("nativefx refused", t_held.packets);
        }
        if (t_held.effect != rg::nativefx::Effect::Probe)
            ++g_effectsAsPackets;
    }
    t_held.effect = rg::nativefx::Effect::None;
    t_held.unit = 0u;
    t_held.packets.clear();
}

// RG_NATIVE_COMPOSITE: stage E's objects, each drawn into the offscreen
// buffer (its head: a clear of its box, its model) and then laid over the
// frame (its tail), drawn by the renderer into object layers of its own
// (GSNativeEffect::kRouteLayer): the head's draws into the buffer go to the
// object's layer, the tail's sprites become a kObjectTail from that layer,
// their kicks withheld, and the waiting tails are drawn together
// (kComposite) before anything else reaches the GS. Uploads and TEXFLUSHes
// between objects go on: the layers are not GS memory. So the GS's buffer is
// never drawn, and the renderer switches target twice a run of objects
// instead of twice an object.
struct CompositeBatch
{
    enum class State : uint8_t
    {
        Outside,
        Head,
        Tail,
    };
    State state = State::Outside;
    uint32_t unit = 0u;
    bool decided = false;   // the head's box known and its packets sent
    bool routed = false;    // its draws go to a layer, its tail held
    bool waiting = false;   // tails sent that no kComposite has drawn yet
    bool uploading = false; // an upload's data goes on too
    GS *gs = nullptr;
    rg::nativefx::Box box;
    std::vector<std::vector<uint8_t>> head; // the head's packets until its box is known
    std::vector<std::vector<uint8_t>> tail;
};
thread_local CompositeBatch t_batch;
std::atomic<uint64_t> g_compositeUnits{0u}, g_compositeRouted{0u}, g_compositeRuns{0u}, g_compositeRefused{0u};

// Whether what a VU1 program kicks now belongs to a composite's tail: the
// tail's packets are held and laid over from the object's layer, so a
// program there sends packets, not meshes (RG_NATIVE_VU1_MESHES).
bool inCompositeTail()
{
    if (!composites() || !t_onVif1Thread)
        return false;
    if (t_batch.state == CompositeBatch::State::Tail)
        return true;
    const uint32_t cursor = g_vif1Memory ? listAddress(g_vif1Memory->vif1Cursor()) : ~0u;
    return cursor != ~0u && rg::nativefx::placeAt(cursor).effect == rg::nativefx::Effect::CompositeTail;
}

void sendEffect(GS &gs, const GSNativeEffect &effect)
{
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    rg::nativeproducer::noteEffect(effect);
    if (rg::nativeproducer::effect(effect))
        return;
    payOwed(gs);
    if (g_stage)
        g_stage->effect(effect);
    else
        gs.submitNativeEffect(effect);
}

void sendCommand(GS &gs, uint32_t command)
{
    thread_local GSNativeEffect effect;
    effect = GSNativeEffect{};
    effect.command = command;
    sendEffect(gs, effect);
}

// The waiting tails drawn, before what comes next.
void compositeRun()
{
    CompositeBatch &batch = t_batch;
    if (!batch.waiting)
        return;
    batch.waiting = false;
    ++g_compositeRuns;
    sendCommand(*batch.gs, GSNativeEffect::kComposite);
}

// The head's box is known (or will not be): routed to a layer with it, or,
// without one, drawn as the game draws it, after the waiting tails.
void decideHead()
{
    CompositeBatch &batch = t_batch;
    batch.routed = rg::nativefx::clearBox(batch.head, batch.box);
    if (batch.routed)
    {
        thread_local GSNativeEffect route;
        route = GSNativeEffect{};
        route.command = GSNativeEffect::kRouteLayer;
        route.bufferCount = 1u;
        route.buffers[0].base = batch.box.base;
        route.buffers[0].fbw = batch.box.fbw;
        route.box[0] = batch.box.x0;
        route.box[1] = batch.box.y0;
        route.box[2] = batch.box.x1;
        route.box[3] = batch.box.y1;
        sendEffect(*batch.gs, route);
        ++g_compositeRouted;
    }
    else
    {
        compositeRun();
    }
    for (const std::vector<uint8_t> &packet : batch.head)
        sendPacket(*batch.gs, packet.data(), static_cast<uint32_t>(packet.size()));
    batch.head.clear();
    batch.decided = true;
    const uint64_t units = ++g_compositeUnits;
    if (units == 1u || units % 5000u == 0u)
        std::fprintf(stderr, "[composite] %llu objects, %llu in layers, %llu runs drawn, %llu tails refused\n",
                     static_cast<unsigned long long>(units), static_cast<unsigned long long>(g_compositeRouted.load()),
                     static_cast<unsigned long long>(g_compositeRuns.load()),
                     static_cast<unsigned long long>(g_compositeRefused.load()));
}

// A routed object's tail, whole: its sprites to the renderer, from the
// layer, and its packets to the GS with their kicks withheld. Refused, the
// object's layer goes back into the GS's buffer first (kObjectTail without
// sprites) and the packets as they are.
void finishTail()
{
    CompositeBatch &batch = t_batch;
    if (batch.tail.empty())
        return;
    GS &gs = *batch.gs;
    thread_local GSNativeEffect effect;
    thread_local std::vector<std::vector<uint8_t>> withheld;
    std::string why;
    bool ok = rg::nativefx::build(rg::nativefx::Effect::CompositeTail, batch.tail, effect, withheld, why);
    if (ok)
    {
        bool fromLayer = false;
        for (uint32_t i = 0; i < effect.bufferCount; ++i)
        {
            GSNativeEffect::Buffer &buffer = effect.buffers[i];
            if (buffer.base == batch.box.base && buffer.fbw == batch.box.fbw)
            {
                buffer.layer = 1u;
                fromLayer = true;
            }
        }
        ok = fromLayer;
        why = ok ? why : "no sprite from the object's buffer";
    }
    if (ok)
    {
        static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
        static int dumped = 0;
        bool other = false;
        for (uint32_t n = 0; n < effect.opCount; ++n)
            other = other || (effect.ops[n].source >= 0 && !(effect.ops[n].flags & GSNativeEffect::kCt24));
        if (trace && other && dumped++ < 3)
        {
            std::fprintf(stderr, "[composite] a tail reading CT32, box %d,%d-%d,%d:\n", batch.box.x0, batch.box.y0,
                         batch.box.x1, batch.box.y1);
            rg::nativefx::dumpPackets("composite", batch.tail);
        }
        for (const std::vector<uint8_t> &packet : withheld)
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
        effect.command = GSNativeEffect::kObjectTail;
        sendEffect(gs, effect);
        batch.waiting = true;
    }
    else
    {
        const bool nothing = why == "no sprites";
        static std::vector<std::string> told;
        if (!nothing && told.size() < 8u && std::find(told.begin(), told.end(), why) == told.end())
        {
            told.push_back(why);
            std::fprintf(stderr, "[composite] a tail stays packets: %s\n", why.c_str());
        }
        if (!nothing)
            ++g_compositeRefused;
        static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
        static int dumped = 0;
        if (trace && !nothing && dumped++ < 3)
        {
            std::fprintf(stderr, "[composite] refused (%s), box %d,%d-%d,%d:\n", why.c_str(), batch.box.x0, batch.box.y0,
                         batch.box.x1, batch.box.y1);
            rg::nativefx::dumpPackets("composite", batch.tail);
        }
        effect = GSNativeEffect{};
        effect.command = GSNativeEffect::kObjectTail;
        effect.bufferCount = nothing ? 0u : 1u;
        effect.buffers[0].base = batch.box.base;
        effect.buffers[0].fbw = batch.box.fbw;
        sendEffect(gs, effect);
        batch.waiting = false;
        for (const std::vector<uint8_t> &packet : batch.tail)
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
    }
    batch.tail.clear();
}

// The object being read is over (what comes next is not its).
void endObject()
{
    CompositeBatch &batch = t_batch;
    if (batch.state == CompositeBatch::State::Head && !batch.decided)
        decideHead();
    if (batch.state == CompositeBatch::State::Head && batch.routed)
        sendCommand(*batch.gs, GSNativeEffect::kRouteEnd);
    finishTail();
    batch.state = CompositeBatch::State::Outside;
    batch.routed = false;
}

// At a list's end, or before anything that is no composite's.
void flushComposites()
{
    if (!t_batch.gs)
        return;
    endObject();
    compositeRun();
    t_batch.uploading = false;
}

void compositeOther(const char *what = "?", uint32_t address = ~0u, uint32_t cursor = ~0u)
{
    static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
    static int traced = 0;
    if (trace && traced < 40 && (t_batch.waiting || t_batch.state != CompositeBatch::State::Outside))
    {
        ++traced;
        std::fprintf(stderr, "[composite]   run ended by %s @%08x cursor %08x (state %d)\n", what, address, cursor,
                     int(t_batch.state));
    }
    flushComposites();
}

bool uploadOrFlush(const uint8_t *data, uint32_t size)
{
    uint32_t first, last;
    if (rg::nativefx::uploadPages(data, size, first, last))
        return true;
    // A packet that only flushes the texture cache (TEXFLUSH: no state, no
    // drawing).
    if (size < 32u)
        return false;
    uint64_t tag, regs;
    std::memcpy(&tag, data, 8);
    std::memcpy(&regs, data + 8, 8);
    const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
    bool flushOnly = (tag >> 58) == 0x4u && (regs & 0xFu) == 0xEu && nloop != 0u && 16u + nloop * 16u <= size;
    for (uint32_t i = 0; flushOnly && i < nloop; ++i)
        flushOnly = data[16u + i * 16u + 8u] == 0x3Fu;
    return flushOnly;
}

// A packet of a composite (its place in the list's EE memory, or for what a
// VU1 program kicked, the part being run): true when it was taken.
bool compositePacket(GS &gs, const uint8_t *data, uint32_t size, uint32_t address)
{
    CompositeBatch &batch = t_batch;
    batch.gs = &gs;
    rg::nativefx::Place place;
    // What a VU1 program kicked: the part whose list holds the MSCAL.
    uint32_t cursor = ~0u;
    if (address == ~0u && g_vif1Memory)
        cursor = listAddress(g_vif1Memory->vif1Cursor());
    if (address != ~0u)
        place = rg::nativefx::placeAt(address);
    else if (cursor != ~0u)
        place = rg::nativefx::placeAt(cursor);
    // Until its tail, a head goes on through what it refers to: the model's
    // packets from the model's memory, and what its VU1 programs kick.
    // (Every object marked has a tail.)
    if (batch.state == CompositeBatch::State::Head && place.effect == rg::nativefx::Effect::None)
        place = {rg::nativefx::Effect::CompositeHead, batch.unit};
    {
        static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
        static int traced = 0;
        if (trace && traced < 400 && (place.effect != rg::nativefx::Effect::None || batch.state != CompositeBatch::State::Outside))
        {
            ++traced;
            std::fprintf(stderr, "[composite]   %s unit %u (state %d unit %u decided %d) @%08x cursor %08x %u bytes\n",
                         rg::nativefx::name(place.effect), place.unit, int(batch.state), batch.unit, int(batch.decided),
                         address, cursor, size);
        }
    }
    if (place.effect == rg::nativefx::Effect::CompositeHead)
    {
        if (batch.state != CompositeBatch::State::Head || batch.unit != place.unit)
        {
            endObject();
            batch.state = CompositeBatch::State::Head;
            batch.unit = place.unit;
            batch.decided = false;
        }
        batch.uploading = false;
        if (!batch.decided)
        {
            if (address != ~0u && rg::nativefx::placeAt(address).effect == rg::nativefx::Effect::CompositeHead)
            {
                batch.head.emplace_back(data, data + size);
                return true;
            }
            decideHead();
        }
        sendPacket(gs, data, size);
        return true;
    }
    // (Unit 0: the fixed chain every tail calls.)
    if (place.effect == rg::nativefx::Effect::CompositeTail && (place.unit == batch.unit || place.unit == 0u) &&
        (batch.state == CompositeBatch::State::Head || batch.state == CompositeBatch::State::Tail))
    {
        if (batch.state == CompositeBatch::State::Head)
        {
            if (!batch.decided)
                decideHead();
            if (batch.routed)
                sendCommand(gs, GSNativeEffect::kRouteEnd);
            batch.state = CompositeBatch::State::Tail;
        }
        if (batch.routed)
            batch.tail.emplace_back(data, data + size);
        else
            sendPacket(gs, data, size);
        return true;
    }
    // The upload's data, as an IMAGE packet.
    if (batch.uploading && size >= 16u)
    {
        uint64_t tag;
        std::memcpy(&tag, data, 8);
        if (((tag >> 58) & 3u) == 2u)
        {
            sendPacket(gs, data, size);
            return true;
        }
    }
    batch.uploading = false;
    // Between objects the caller uploads textures (CLUTs) and flushes the
    // texture cache: on ahead of the waiting tails, which read only their
    // layers (each tail's packets set every register it draws with).
    if (batch.state != CompositeBatch::State::Head && (batch.waiting || batch.state == CompositeBatch::State::Tail) &&
        uploadOrFlush(data, size))
    {
        endObject();
        uint32_t first, last;
        batch.uploading = rg::nativefx::uploadPages(data, size, first, last);
        sendPacket(gs, data, size);
        return true;
    }
    compositeOther(rg::nativefx::name(place.effect), address, cursor);
    return false;
}

void deliverPacket(GS &gs, const uint8_t *data, uint32_t size)
{
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    if (g_drawOrigins)
    {
        rg::debug::drawOriginsPacket(data, size, t_onVif1Thread ? t_vu1Tag : 0u, t_onVif1Thread ? t_vu1Name : nullptr);
    }
    if (composites() && t_onVif1Thread && compositePacket(gs, data, size, listAddress(data)))
    {
        return;
    }
    if (nativeEffects() && t_onVif1Thread)
    {
        const uint32_t address = listAddress(data);
        const rg::nativefx::Place place = address == ~0u ? rg::nativefx::Place{} : rg::nativefx::placeAt(address);
        const rg::nativefx::Effect effect = place.effect;
        const uint32_t unit =
            effect == rg::nativefx::Effect::Probe || effect == rg::nativefx::Effect::ColorFilter ? place.unit : 0u;
        if (effect != t_held.effect || unit != t_held.unit)
        {
            flushEffect();
        }
        if (effect == rg::nativefx::Effect::ShadowSetup)
        {
            rg::nativefx::observe(effect, data, size);
        }
        else if (effect != rg::nativefx::Effect::None)
        {
            t_held.effect = effect;
            t_held.unit = unit;
            t_held.gs = &gs;
            t_held.packets.emplace_back(data, data + size);
            return;
        }
    }
    // RG_NATIVE_FLIP: the flip's copy, from the game thread.
    if (!t_onVif1Thread && rg::nativefx::flipNative())
    {
        static const bool supported = gs.supportsNativeEffects();
        thread_local GSNativeEffect effect;
        thread_local std::vector<std::vector<uint8_t>> withheld;
        if (supported && rg::nativefx::flipPacket(data, size, effect, withheld))
        {
            // sendPacket drains the lists first: the effect follows them.
            for (const std::vector<uint8_t> &packet : withheld)
            {
                sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
            }
            rg::nativeproducer::noteEffect(effect);
            if (rg::nativeproducer::effect(effect))
                return;
            payOwed(gs);
            gs.submitNativeEffect(effect);
            return;
        }
    }
    sendPacket(gs, data, size);
}

void sendRaw(GS &gs, const uint8_t *data, uint32_t size);

// RG_NATIVE_VU1_MESHES (on with the native renderer).
bool vu1Meshes()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_VU1_MESHES");
        return value ? std::strcmp(value, "0") != 0 : rg::nativerenderer::enabled();
    }();
    return on;
}

// RG_NATIVE_ALL_PACKETS (on with the native renderer): every GIF packet on
// its way to the GS goes through build2D first, its primitives drawn as
// native meshes and only its register state and uploads sent on, so the GS
// never assembles or kicks a vertex. The converter carries the GS's vertex
// state from packet to packet per thread. A packet it refuses goes as it is.
bool convertAll()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_ALL_PACKETS");
        return value ? std::strcmp(value, "0") != 0 : rg::nativerenderer::enabled();
    }();
    return on;
}
std::atomic<uint64_t> g_packetsConverted{0u}, g_packetsRefused{0u}, g_meshesFromPackets{0u};

void sendPacket(GS &gs, const uint8_t *data, uint32_t size)
{
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    if (convertAll() && data && size >= 16u)
    {
        thread_local std::vector<rg::nativefx::Piece2D> pieces;
        const rg::nativefx::Span2D one{data, size};
        std::string why;
        if (rg::nativefx::build2D(&one, 1u, pieces, why))
        {
            const bool anyMesh = std::any_of(pieces.begin(), pieces.end(),
                                             [](const rg::nativefx::Piece2D &p) { return !p.isPacket(); });
            // Owned draw state: the pieces carry only what the front end
            // still needs, so they go even when nothing became a mesh.
            if (!anyMesh && !rg::nativefx::drawStateOwned())
            {
                const ConverterOutput converted; // build2D has seen it
                sendRaw(gs, data, size);
                return;
            }
            const ConverterOutput converted;
            for (const rg::nativefx::Piece2D &piece : pieces)
            {
                if (piece.isPacket())
                {
                    sendRaw(gs, piece.bytes(), piece.size());
                    continue;
                }
                ++g_meshesFromPackets;
                if (g_stage && !t_onVif1Thread)
                    g_thread->drain(); // the lists in flight first, as for the GS
                if (rg::nativeproducer::piece(piece.mesh, piece.stated ? &piece.state : nullptr))
                    continue; // step 5b: the renderer's
                if (g_stage && t_onVif1Thread)
                {
                    g_stage->mesh(piece.mesh, piece.stated ? &piece.state : nullptr);
                    continue;
                }
                if (g_stage)
                    g_thread->drain();
                if (piece.stated)
                    gs.submitNativeMesh(piece.mesh, piece.state);
                else
                    gs.submitNativeMesh(piece.mesh);
            }
            const uint64_t count = ++g_packetsConverted;
            if (count == 1u || count % 100000u == 0u)
                std::fprintf(stderr, "[nativefx] packets as meshes: %llu (%llu meshes), %llu refused\n",
                             static_cast<unsigned long long>(count),
                             static_cast<unsigned long long>(g_meshesFromPackets.load()),
                             static_cast<unsigned long long>(g_packetsRefused.load()));
            return;
        }
        ++g_packetsRefused;
        static std::vector<std::string> told;
        static std::mutex toldMutex;
        std::lock_guard lock(toldMutex);
        if (told.size() < 8u && std::find(told.begin(), told.end(), why) == told.end())
        {
            told.push_back(why);
            std::fprintf(stderr, "[nativefx] a packet stays a packet: %s\n", why.c_str());
        }
    }
    sendRaw(gs, data, size);
}

// Whether build2D's mirror of the GS state has to follow the packets that
// go to the GS as they are.
bool anyConversion()
{
    return convertAll() || vu1Meshes() || rg::nativefx::prim2DEnabled();
}

void sendToGs(GS &gs, const uint8_t *data, uint32_t size);

// The draw registers converted packets owed the front end, sent ahead of
// whatever reads its state next (drawStateOwned).
void payOwed(GS &gs)
{
    if (!rg::nativefx::drawStateOwned())
        return;
    thread_local std::vector<uint8_t> owed;
    if (rg::nativefx::takeOwedRegisters(owed))
        sendToGs(gs, owed.data(), static_cast<uint32_t>(owed.size()));
}

void sendRaw(GS &gs, const uint8_t *data, uint32_t size)
{
    // A packet as it is may draw with the front end's state: it gets what
    // it is owed first. The converter's own pieces (transfers, uploads,
    // FINISH) read none of it.
    if (!t_converterOutput)
        payOwed(gs);
    if (!t_converterOutput && anyConversion())
        rg::nativefx::observe2D(data, size);
    sendToGs(gs, data, size);
}

#if defined(RG_HAS_SDLGPU)
// docs/NATIVE-PIPELINE.md step 3b: with the native pipeline on, a named
// upload the producer already sent as a record is withheld from the GS: the
// A+D block that sets its transfer registers, and its IMAGE data, in the
// packet and in the continuations after it (withheldContinuation). Anything
// else in a packet goes on as it was: an A+D block's other registers go on
// in a block of their own. A refused texture (no record) goes to the GS as
// before.
struct UploadWithhold
{
    uint32_t transferBytes = 0; // the withheld transfer's data still to come in IMAGE tags
    uint32_t imageBytes = 0;    // an IMAGE tag's data still to come outside its packet
    bool palette = false;
};
thread_local UploadWithhold t_upload;

uint32_t transferBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u; // CT32
    case 0x01u: return 24u; // CT24
    case 0x02u: case 0x0Au: return 16u; // CT16, CT16S
    case 0x13u: return 8u;  // T8
    case 0x14u: return 4u;  // T4
    default: return 0u;
    }
}

// What of the packet goes to the GS, into `kept`; false: all of it, as it is.
bool withholdUploads(const uint8_t *data, uint32_t size, std::vector<uint8_t> &kept)
{
    namespace producer = rg::nativeproducer;
    if (!dq8::gfx::native::enabled())
        return false;
    kept.clear();
    bool changed = false;
    uint32_t pos = 0u;
    if (t_upload.imageBytes > 0u)
    {
        // A withheld image's data at the packet's start, not behind a tag.
        const uint32_t n = std::min(t_upload.imageBytes, size);
        t_upload.imageBytes -= n;
        pos = n;
        changed = true;
        producer::noteContinuedInPacket();
    }
    while (pos + 16u <= size)
    {
        uint64_t lo = 0u, hi = 0u;
        std::memcpy(&lo, data + pos, 8u);
        std::memcpy(&hi, data + pos + 8u, 8u);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        nreg = nreg ? nreg : 16u;
        const uint64_t body = flg == 0u   ? uint64_t(nloop) * nreg * 16u
                              : flg == 1u ? (uint64_t(nloop) * nreg + 1u) / 2u * 16u
                                          : uint64_t(nloop) * 16u;
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(size, pos + 16u + body));
        bool drop = false;
        if (flg == 0u && nreg == 1u && (hi & 0xFu) == 0xEu)
        {
            // A+D: a transfer's registers alone, its TRXDIR host to local?
            uint64_t bitbltbuf = 0u, trxreg = 0u, trxdir = ~0ull;
            uint32_t others = 0u;
            for (uint32_t at = pos + 16u; at + 16u <= end; at += 16u)
            {
                uint64_t value = 0u;
                std::memcpy(&value, data + at, 8u);
                const uint8_t reg = data[at + 8u];
                if (reg == 0x50u)
                    bitbltbuf = value;
                else if (reg == 0x52u)
                    trxreg = value;
                else if (reg == 0x53u)
                    trxdir = value;
                else if (reg != 0x51u)
                    ++others;
            }
            if (trxdir != ~0ull)
            {
                t_upload.transferBytes = 0u;
                const uint32_t dbp = static_cast<uint32_t>((bitbltbuf >> 32) & 0x3FFFu);
                const uint32_t dbw = std::max<uint32_t>(static_cast<uint32_t>((bitbltbuf >> 48) & 0x3Fu), 1u);
                const uint32_t dpsm = static_cast<uint32_t>((bitbltbuf >> 56) & 0x3Fu);
                const uint32_t rrw = static_cast<uint32_t>(trxreg & 0xFFFu);
                const uint32_t rrh = static_cast<uint32_t>((trxreg >> 32) & 0xFFFu);
                const uint32_t bits = transferBits(dpsm);
                bool palette = false;
                if ((trxdir & 3u) != 0u || bits == 0u)
                    ; // not an upload this takes
                else if (!producer::takeProduced(dbp, dbw, dpsm, rrw, rrh, palette))
                    producer::notePassed(producer::Passed::NotProduced);
                else
                {
                    t_upload.transferBytes = (rrw * rrh * bits / 8u + 15u) & ~15u;
                    t_upload.palette = palette;
                    drop = true;
                    if (others != 0u)
                    {
                        // The block's other registers, as a block of their own.
                        const uint64_t tag = (lo & ~uint64_t(0x7FFFu)) | others;
                        const size_t at = kept.size();
                        kept.resize(at + 16u);
                        std::memcpy(kept.data() + at, &tag, 8u);
                        std::memcpy(kept.data() + at + 8u, &hi, 8u);
                        for (uint32_t q = pos + 16u; q + 16u <= end; q += 16u)
                            if (data[q + 8u] < 0x50u || data[q + 8u] > 0x53u)
                                kept.insert(kept.end(), data + q, data + q + 16u);
                        producer::noteSplit();
                    }
                }
            }
        }
        else if (flg >= 2u && t_upload.transferBytes > 0u)
        {
            // The withheld transfer's data, as many IMAGE tags as it takes.
            drop = true;
            const uint32_t taken = static_cast<uint32_t>(std::min<uint64_t>(body, t_upload.transferBytes));
            t_upload.transferBytes -= taken;
            t_upload.imageBytes = static_cast<uint32_t>(pos + 16u + body - end);
            if (t_upload.transferBytes == 0u)
                producer::noteWithheld(t_upload.palette, taken);
        }
        if (drop)
            changed = true;
        else
            kept.insert(kept.end(), data + pos, data + end);
        pos = end;
    }
    if (changed && pos < size)
        kept.insert(kept.end(), data + pos, data + size);
    return changed;
}

// An image continuation's bytes that belong to a withheld upload; the rest,
// if any, goes on.
uint32_t withheldContinuation(uint32_t size)
{
    if (t_upload.imageBytes == 0u)
        return 0u;
    const uint32_t n = std::min(t_upload.imageBytes, size);
    t_upload.imageBytes -= n;
    return n;
}
#else
bool withholdUploads(const uint8_t *, uint32_t, std::vector<uint8_t> &) { return false; }
uint32_t withheldContinuation(uint32_t) { return 0u; }
#endif

void sendToGs(GS &gs, const uint8_t *data, uint32_t size)
{
    thread_local std::vector<uint8_t> kept;
    if (withholdUploads(data, size, kept))
    {
        if (kept.empty())
            return;
        data = kept.data();
        size = static_cast<uint32_t>(kept.size());
    }
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    if (g_stage && t_onVif1Thread)
    {
        g_stage->packet(data, size);
        return;
    }
    if (g_stage)
    {
        // The lists in flight too, not only what they handed the stage:
        // every DMA start drains them already, this keeps it so.
        g_thread->drain();
    }
    ps2_gs_detail::t_sourceTag = t_onVif1Thread ? t_vu1Tag : 0u;
    ps2_gs_detail::t_sourceCamera = t_onVif1Thread ? t_vu1Camera : 0u;
    gs.processGIFPacket(data, size);
}

void deliverMesh(GS &gs, const GSNativeMesh &mesh, const GSDrawState *given, const uint8_t *vu1Data)
{
    const gs_boundary::FromScope boundaryFrom(t_onVif1Thread ? gs_boundary::kFromVif1 : gs_boundary::kFromGame);
    if (composites() && t_onVif1Thread)
    {
        t_batch.gs = &gs;
        if (t_batch.state == CompositeBatch::State::Head)
        {
            if (!t_batch.decided)
                decideHead();
        }
        else
        {
            compositeOther("a mesh");
        }
    }
    if (nativeEffects() && t_onVif1Thread)
    {
        flushEffect();
    }
    if (g_drawOrigins)
    {
        rg::debug::drawOriginsMesh(static_cast<uint32_t>(mesh.vertices.size()), t_vu1Tag, t_vu1Name);
    }
    // Under the converter's draw state when it owns it: the front end's may
    // be owed registers.
    const bool owned = rg::nativefx::drawStateOwned();
    thread_local GSDrawState state;
    if (given)
        state = *given;
    else if (owned)
        state = rg::nativefx::mirroredDrawState();
    // Steps 4a and 4c (docs/NATIVE-PIPELINE.md): the mesh as a native mesh
    // draw, under the state the GS would get with it; made one, it does not
    // go to the GS at all.
    if (vu1Data && rg::nativeproducer::mesh(mesh, given || owned ? &state : nullptr, vu1Data, mesh.header.source))
        return;
    // Step 5b: a converter 2D piece as a window-space mesh draw (after the
    // lists in flight, from another thread, as for the GS).
    if (!vu1Data && g_stage && !t_onVif1Thread)
        g_thread->drain();
    if (!vu1Data && rg::nativeproducer::piece(mesh, given || owned ? &state : nullptr))
        return;
    if (g_stage && t_onVif1Thread)
    {
        g_stage->mesh(mesh, owned ? &state : nullptr);
        return;
    }
    if (g_stage)
    {
        g_thread->drain();
    }
    if (owned)
        gs.submitNativeMesh(mesh, state);
    else
        gs.submitNativeMesh(mesh);
}
// The thread that runs guest code, as the first hooked DMA call sees it.
std::thread::id g_gameThread;
PS2Runtime::RecompiledFunction g_dmaSend = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendN = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendI = nullptr;
PS2Runtime::RecompiledFunction g_dmaSync = nullptr;
std::vector<std::pair<uint32_t, PS2Runtime::RecompiledFunction>> g_gsWaiters;

uint32_t channelOf(R5900Context *ctx)
{
    return getRegU32(ctx, 4) & 0x1FFFFF00u;
}

// What `jr $ra` leaves. A hook that returns without the original must do
// this itself: a call through dispatchGuestBranch forgives a pc left at the
// entry, but a thread the run loop resumes at the entry (a safe point
// unwound it at that call) would be dispatched here again, forever.
void returnToCaller(R5900Context *ctx, int32_t value)
{
    setReturnS32(ctx, value);
    ctx->pc = getRegU32(ctx, 31);
}

// What the stub does right after a transfer, for the lists that have
// finished since, on the game's thread: VU1's D/T stop bits into VPU_STAT,
// and the DMAC completion handlers.
void reportFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t finished = g_thread->takeFinished();
    if (finished == 0u)
    {
        return;
    }
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) | g_thread->takeVpuStat();
    for (uint64_t n = finished; n > 0u; --n)
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 1u);
    }
}

// sceDmaSend/SendI (chain) and SendN (normal) to VIF1 go to the thread;
// anything else to the GS path first waits for the lists in flight.
void send(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original,
          bool normal)
{
    if (g_gameThread == std::thread::id())
    {
        g_gameThread = std::this_thread::get_id();
    }
    const uint32_t channel = channelOf(ctx);
    if (channel != kVif1Channel)
    {
        if (channel == kGifChannel)
        {
            g_thread->drain();
        }
        original(rdram, ctx, runtime);
        return;
    }
    PS2Memory &memory = runtime->memory();
    const uint32_t chcr = memory.readIORegister(kVif1Channel);
    const uint32_t address = toDmaPhys(getRegU32(ctx, 5));
    List list;
    list.data = g_thread->takeBuffer();
    list.dBit = (ctx->vu0_fbrst & (1u << 10)) != 0u;
    list.tBit = (ctx->vu0_fbrst & (1u << 11)) != 0u;
    if (normal)
    {
        const uint32_t qwc = getRegU32(ctx, 6) & 0xFFFFu;
        try
        {
            list.data.reserve(qwc * 16u);
            appendBytes(memory, list.data, address, qwc * 16u);
            if (g_drawOrigins || nativeEffects())
            {
                list.origins.emplace_back(0u, address);
            }
        }
        catch (const std::exception &)
        {
            list.data.clear();
        }
    }
    else
    {
        static uint32_t listNumber = 0u;
        list.number = ++listNumber;
        gatherChain(memory, address, (chcr & kChcrTte) != 0u, (chcr & kChcrTie) != 0u, list.data,
                    (g_capture || g_drawOrigins || nativeEffects()) ? &list.origins : nullptr,
                    refImages() && !g_capture && !g_drawOrigins ? &list.externals : nullptr);
    }
    reportFinished(rdram, ctx, runtime);
    g_thread->submit(std::move(list));
    returnToCaller(ctx, 0);
}

void dmaSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSend, false); }
void dmaSendI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSendI, false); }
void dmaSendN(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSendN, true); }

// sceDmaSync(chan, mode, timeout): mode 0 blocks until the channel is idle,
// any other mode only asks. For VIF1 the channel is busy while the thread
// still has lists; the stub's own model never saw them.
void dmaSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (channelOf(ctx) != kVif1Channel)
    {
        g_dmaSync(rdram, ctx, runtime);
        return;
    }
    if (getRegU32(ctx, 5) != 0u && g_thread->busy())
    {
        returnToCaller(ctx, 1);
        return;
    }
    g_thread->drain();
    reportFinished(rdram, ctx, runtime);
    returnToCaller(ctx, 0);
}

// The libgraph stubs: the lists in flight first, then the stub.
template <size_t Index>
void gsWaiter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_thread->drain();
    // The libgraph stubs write the front end's registers themselves
    // (sceGsPutDrawEnv, sceGsSwapDBuff's environments and clear): it gets
    // what it is owed first, and the converter reads them back after.
    const bool owned = rg::nativefx::drawStateOwned();
    if (owned)
        payOwed(runtime->gs());
    g_gsWaiters[Index].second(rdram, ctx, runtime);
    if (owned)
        rg::nativefx::readDrawRegisters(runtime->gs());
}

template <size_t... I>
constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> gsWaiterHooks(std::index_sequence<I...>)
{
    return {&gsWaiter<I>...};
}

bool hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
          PS2Runtime::RecompiledFunction &original)
{
    original = runtime.lookupFunction(address);
    if (!original || !runtime.replaceFunction(address, replacement))
    {
        std::fprintf(stderr, "[vif1] 0x%08X is not in the function table\n", address);
        return false;
    }
    return true;
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *mode = std::getenv("RG_VIF1_THREAD");
    if (mode && std::strcmp(mode, "0") == 0)
    {
        return;
    }
    g_vif1Memory = &runtime.memory();
    if (!runtime.lookupFunction(kSceDmaSend) || !runtime.lookupFunction(kSceDmaSendN) ||
        !runtime.lookupFunction(kSceDmaSendI) || !runtime.lookupFunction(kSceDmaSync))
    {
        std::fprintf(stderr, "[vif1] libdma entry points missing; VIF1 stays on the game's thread\n");
        return;
    }
    g_thread = new Vif1Thread(runtime);
    g_runtime = &runtime;

    // GS packets from the VIF1 thread and from the game's own GIF transfers
    // reach the GS through PS2Memory. Its GifArbiter queues them unlocked and
    // every DMA start drains it, so it cannot be shared between threads;
    // packets go straight to the GS instead, whose processGIFPacket holds the
    // GS state lock. The arbiter only ever ordered packets queued together,
    // and the VIF1 paths already drained it after each one.
    PS2Memory &memory = runtime.memory();
    memory.setGifArbiter(nullptr);
    const char *gifMode = std::getenv("RG_GIF_THREAD");
    if (!gifMode || std::strcmp(gifMode, "0") != 0)
    {
        g_stage = new GifStage(runtime.gs());
    }
    // From the VIF1 thread through the GIF stage. From any other thread (the
    // game's own GIF transfers, after the drain every DMA start does) straight
    // to the GS, behind whatever the stage still holds.
    memory.setGifPacketCallback([&gs = runtime.gs()](const uint8_t *data, uint32_t size) {
        // A program's XGKICK, while RG_VU1_REPLAY records the run.
        static const bool replay = rg::vu1replay::enabled();
        if (replay && t_onVif1Thread)
        {
            rg::vu1replay::kick(data, size);
        }
        // RG_VU1_NATIVE: a chunk done natively, whose own packets go instead.
        if (t_vu1Packets)
        {
            t_vu1Packets->emplace_back(data, data + size);
        }
        if (t_dropVu1Packets)
        {
            return;
        }
        deliverPacket(gs, data, size);
    });
    // A texture split over DIRECT packets continues in place, without the
    // copy behind a synthesized IMAGE tag.
    memory.setGifImageCallback([&gs = runtime.gs()](const uint8_t *data, uint32_t size) {
        // The rest of a named upload withheld from the GS (step 3b).
        if (const uint32_t withheld = withheldContinuation(size))
        {
            data += withheld;
            size -= withheld;
            if (size == 0u)
                return;
        }
        if (composites() && t_onVif1Thread)
        {
            t_batch.gs = &gs;
            if (t_batch.state == CompositeBatch::State::Head)
            {
                if (!t_batch.decided)
                    decideHead();
            }
            else if (!t_batch.uploading)
            {
                compositeOther("an image");
            }
        }
        if (nativeEffects() && t_onVif1Thread)
        {
            flushEffect();
        }
        if (g_stage && t_onVif1Thread)
        {
            g_stage->image(data, size);
            return;
        }
        if (g_stage)
        {
            g_thread->drain();
        }
        gs.processImageContinuation(data, size);
    });
    // DMA the game starts through the registers rather than sceDmaSend.
    // VU1 started by the lists, without the guest thread context the
    // runtime's callbacks reach into (see Vif1Thread::runVu1).
    memory.setVu1MscalCallback([](uint32_t startPC, uint32_t top, uint32_t itop) {
        g_thread->runVu1(false, startPC, top, itop);
    });
    memory.setVu1MscntCallback([](uint32_t top, uint32_t itop) { g_thread->runVu1(true, 0u, top, itop); });
    memory.setDmaStartObserver([](uint32_t channel) {
        if (channel == kGifChannel || channel == kVif1Channel)
        {
            g_thread->drain();
        }
    });

    hook(runtime, kSceDmaSend, &dmaSend, g_dmaSend);
    hook(runtime, kSceDmaSendN, &dmaSendN, g_dmaSendN);
    hook(runtime, kSceDmaSendI, &dmaSendI, g_dmaSendI);
    hook(runtime, kSceDmaSync, &dmaSync, g_dmaSync);
    constexpr size_t kWaiters = std::size(kGsWaiters);
    static constexpr auto hooks = gsWaiterHooks(std::make_index_sequence<kWaiters>{});
    g_gsWaiters.resize(kWaiters);
    for (size_t i = 0; i < kWaiters; ++i)
    {
        g_gsWaiters[i].first = kGsWaiters[i];
        hook(runtime, kGsWaiters[i], hooks[i], g_gsWaiters[i].second);
    }
    std::printf("[rg] VIF1 lists run on their own thread%s\n", g_stage ? ", their GS packets on another" : "");
}

void shutdown()
{
    if (g_thread)
    {
        g_thread->stop();
    }
    if (g_stage)
    {
        g_stage->stop();
    }
}

void submitEffectFromGame(GS &gs, const GSNativeEffect &effect)
{
    const gs_boundary::FromScope boundaryFrom(gs_boundary::kFromGame);
    rg::nativeproducer::noteEffect(effect);
    if (g_stage)
        g_thread->drain();
    if (rg::nativeproducer::effect(effect))
        return;
    payOwed(gs);
    gs.submitNativeEffect(effect);
}

std::string busyReport()
{
    static auto lastTime = std::chrono::steady_clock::now();
    static uint64_t lastBusy = 0u, lastGif = 0u;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t busy = g_busyNanos.load(std::memory_order_relaxed);
    const uint64_t gif = g_gifBusyNanos.load(std::memory_order_relaxed);
    const double wall = std::chrono::duration<double>(now - lastTime).count();
    auto share = [&](uint64_t nanos) { return wall > 0.0 ? (static_cast<double>(nanos) * 1e-9) / wall * 100.0 : 0.0; };
    char text[96];
    if (g_stage)
    {
        std::snprintf(text, sizeof(text), "vif1 busy=%.0f%% gif busy=%.0f%% of wall", share(busy - lastBusy),
                      share(gif - lastGif));
    }
    else
    {
        std::snprintf(text, sizeof(text), "vif1 busy=%.0f%% of wall", share(busy - lastBusy));
    }
    lastTime = now;
    lastBusy = busy;
    lastGif = gif;
    // VU1 pairs run on the interpreter since the last report: a program the
    // build has not compiled (several times slower), e.g. an effect's, shows
    // up here. Read unsynchronised from the VIF1 thread's counters; a stats
    // line can live with a torn count.
    if (g_runtime)
    {
        static uint64_t lastInterpreted = 0u, lastCompiled = 0u;
        const uint64_t interpreted = g_runtime->vu1().interpretedPairsExecuted();
        const uint64_t compiled = g_runtime->vu1().compiledPairsExecuted();
        const uint64_t newInterpreted = interpreted - std::min(interpreted, lastInterpreted);
        const uint64_t newCompiled = compiled - std::min(compiled, lastCompiled);
        lastInterpreted = interpreted;
        lastCompiled = compiled;
        if (newInterpreted != 0u)
        {
            // Per second, and as a share of the pairs the VU1 ran at all: the
            // chunks done natively never run it, so a high share of a small
            // count is no load (2026-11-01: 44% was 14k pairs a second).
            char more[128];
            std::snprintf(more, sizeof(more), " | vu1 interpreted %.1fk pairs/s (%.0f%% of what the VU1 ran)",
                          wall > 0.0 ? static_cast<double>(newInterpreted) / wall / 1000.0 : 0.0,
                          100.0 * static_cast<double>(newInterpreted) /
                              static_cast<double>(newInterpreted + newCompiled));
            return std::string(text) + more;
        }
    }
    return text;
}

void drainForSnapshot()
{
    // Only from the game's thread. The presenter reaches a frame boundary
    // too, through flip pacing's fallback while the game shows no flips (a
    // loading screen), and it does so holding the GS state lock, which the
    // VIF1 thread needs to finish its list: waiting for it there is a
    // deadlock (it was one, at the Story loading screen).
    if (g_thread && std::this_thread::get_id() == g_gameThread)
    {
        g_thread->drain();
    }
}

bool busy()
{
    return g_thread && g_thread->busy();
}

} // namespace rg::vif1
