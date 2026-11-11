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
#include "gfx/native/native_records.h"
#include "vu1_native.h"
#include "draw_state_mirror.h"
#include "native_blocks.h"
#include "native_emit.h"
#include "native_effects.h"
#include "native_producer.h"
#include "gs_writes.h"
#include "guest_write_watch.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

// RG_EE_WAITLOG=1: the host waits around the VIF1 thread, every second: the
// game thread's drains (by source line), the VIF1 thread's idle and its waits
// on the GIF stage, the stage's idle.
namespace vifwait
{
const bool g_on = [] {
    const char *v = std::getenv("RG_EE_WAITLOG");
    return v && *v && *v != '0';
}();
struct Acc
{
    std::atomic<uint64_t> ns{0}, count{0};
};
constexpr int kLines = 4096;
Acc g_drain[kLines]; // the game thread in Vif1Thread::drain, by caller line
Acc g_vif1Idle, g_vif1Space, g_stageIdle, g_stageDrain;
// A list's latency: from its send to the VIF1 thread's end of it, and to the
// draw stage's (when the game's wait for it ends).
Acc g_listVif1, g_listDraw;
std::atomic<int64_t> g_listSent{0};
std::atomic<int64_t> g_windowStart{0};
int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void add(Acc &acc, int64_t start)
{
    acc.ns.fetch_add(uint64_t(nowNs() - start), std::memory_order_relaxed);
    acc.count.fetch_add(1u, std::memory_order_relaxed);
}
void report()
{
    const int64_t now = nowNs();
    int64_t start = g_windowStart.load(std::memory_order_relaxed);
    if (start == 0)
    {
        g_windowStart.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 1000000000 || !g_windowStart.compare_exchange_strong(start, now))
        return;
    const double w = double(now - start);
    auto pct = [&](Acc &a) { return 100.0 * double(a.ns.exchange(0)) / w; };
    auto cnt = [&](Acc &a) { return (unsigned long long)a.count.exchange(0); };
    std::fprintf(stderr, "[vifwait] vif1 idle %.1f%% (x%llu) waits-on-stage %.1f%% (x%llu) | stage idle %.1f%% (x%llu) | game->stage drain %.1f%% (x%llu) | game drains:",
                 pct(g_vif1Idle), cnt(g_vif1Idle), pct(g_vif1Space), cnt(g_vif1Space), pct(g_stageIdle),
                 cnt(g_stageIdle), pct(g_stageDrain), cnt(g_stageDrain));
    {
        const uint64_t lists = g_listDraw.count.load();
        const double vif1 = double(g_listVif1.ns.exchange(0)), draw = double(g_listDraw.ns.exchange(0));
        g_listVif1.count.exchange(0);
        g_listDraw.count.exchange(0);
        if (lists)
            std::fprintf(stderr, " | a list, from its send: VIF1 done %.0f us, draw stage done %.0f us (x%llu) |",
                         vif1 / double(lists) / 1000.0, draw / double(lists) / 1000.0, (unsigned long long)lists);
    }
    double total = 0.0;
    for (int line = 0; line < kLines; ++line)
    {
        const uint64_t c = g_drain[line].count.exchange(0);
        const uint64_t ns = g_drain[line].ns.exchange(0);
        if (c == 0u)
            continue;
        total += 100.0 * double(ns) / w;
        if (ns * 1000u > uint64_t(w)) // above 0.1%
            std::fprintf(stderr, " L%d %.1f%% x%llu;", line, 100.0 * double(ns) / w, (unsigned long long)c);
    }
    std::fprintf(stderr, " total %.1f%%\n", total);
}
} // namespace vifwait

namespace rg::vif1
{
namespace
{
// Nanoseconds the worker spent inside processVIF1Data, for busyReport().
std::atomic<uint64_t> g_busyNanos{0u};
// And the GIF stage in the GS, for the same line.
std::atomic<uint64_t> g_gifBusyNanos{0u};
// Set while a list runs (Vif1Thread::runList, on the game thread): what is
// delivered then comes from a list, with its place in it.
thread_local bool t_onVif1Thread = false;
thread_local bool t_listDecoder = false;
// The native program whose chunk is being made a mesh on this thread, as its
// meshes' MeshDraw::source (0: none): what the renderer's draw labels name
// (a frame capture's; debug/bug_mark.h).
thread_local uint32_t t_vu1Source = 0;
uint32_t programSource(const rg::vu1native::Program *program)
{
    const uint64_t address = reinterpret_cast<uintptr_t>(program);
    return program ? static_cast<uint32_t>(address ^ (address >> 32)) | 0x80000000u : 0u;
}
// The native pipeline's effects (src/native_effects.h); read at first use,
// after the menu's setting.
bool nativeEffects()
{
    static const bool on = rg::nativefx::enabled();
    return on;
}
// For the VIF code a VU1 program's packets came from (vif1Cursor, patch 0032).
PS2Memory *g_vif1Memory = nullptr;
// The VIF1 thread's list: its bytes and where each piece came from, for the
// EE address of a packet in it (listAddress).
thread_local const uint8_t *t_listData = nullptr;
thread_local size_t t_listSize = 0u;
thread_local const std::vector<std::pair<uint32_t, uint32_t>> *t_listOrigins = nullptr;
// The piece of t_listOrigins the last listAddress found (listAddress).
thread_local size_t t_listOriginAt = 0u;
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

// Bytes written whole before they are read: growing them leaves the new
// bytes as they are (std::vector<uint8_t> zeroes them first).
template <typename T> struct NoInit : std::allocator<T>
{
    template <typename U> struct rebind
    {
        using other = NoInit<U>;
    };
    NoInit() = default;
    template <typename U> NoInit(const NoInit<U> &) noexcept {}
    template <typename U> void construct(U *p) noexcept { ::new (static_cast<void *>(p)) U; }
    template <typename U, typename... A> void construct(U *p, A &&...a)
    {
        ::new (static_cast<void *>(p)) U(std::forward<A>(a)...);
    }
};
using Bytes = std::vector<uint8_t, NoInit<uint8_t>>;

// Model chunks kept by the renderer (M5 of docs/NATIVE-ONLY.md): a chunk
// whose inputs the VIF1 thread saw in two lists gets an id; its inputs go
// to the renderer once, and every later draw names it.
struct MeshRetain
{
    uint32_t id = 0u;    // 0: none (the model chunk is drawn once, or not a model chunk)
    bool define = false; // (unused: a model chunk's inputs go in t_model)
};
// The mesh on its way (the VIF1 thread to the draw stage, the draw stage to
// the producer).
thread_local MeshRetain t_retain;
// M5 (docs/NATIVE-ONLY.md): a model chunk drawn by the renderer's model
// vertex stage, on its way as t_retain is: its id (t_retain's), its inputs
// the first time, the program's constants and its bones for this draw.
struct ModelPayload
{
    // transient: drawn this once, its inputs with it (no id yet, or ever).
    bool active = false, define = false, list = false, transient = false;
    // An outline chunk's: its points a edge (4 or 2), its inputs edges.
    uint32_t edge = 0u;
    // A shadow volume draw's: its inputs triangles, each drawn as its volume.
    bool volume = false;
    std::vector<dq8::gfx::native::ModelInput> inputs;
    dq8::gfx::native::ModelRow row{};
    std::vector<float> palette;
};
thread_local ModelPayload t_model;
struct RetainTable
{
    struct Entry
    {
        uint32_t id;
        uint32_t sightings; // lists it was seen in, while it has no id
        uint64_t list;
    };
    // Model chunks by the hash of their inputs: seen in two lists, a chunk
    // gets an id; one whose inputs change from frame to frame (cloth) is
    // drawn once each time, its inputs with it.
    std::unordered_map<uint64_t, Entry> models;
    uint64_t list = 0u, epoch = 0u;
    uint32_t nextId = 0u; // never reused, whatever the epoch
};
thread_local RetainTable t_retained;
// At a list's start: the renderer's epoch (a new one: every id is gone), and
// now and then the meshes not seen for a while forgotten.
void retainList()
{
    RetainTable &r = t_retained;
    ++r.list;
    const uint64_t epoch = rg::nativeproducer::retainedEpoch();
    if (epoch != r.epoch)
    {
        r.epoch = epoch;
        r.models.clear();
    }
    if (r.list % 512u == 0u)
        std::erase_if(r.models, [&](const auto &e) { return e.second.id == 0u && r.list - e.second.list > 64u; });
}
// A model chunk's id (0: not yet: the CPU draws it this time), by the hash
// of its inputs; `define`: its inputs go with it.
uint32_t modelIdOf(uint64_t hash, bool &define)
{
    RetainTable &r = t_retained;
    define = false;
    const auto [it, fresh] = r.models.try_emplace(hash, RetainTable::Entry{0u, 1u, r.list});
    RetainTable::Entry &entry = it->second;
    if (fresh)
        return 0u;
    if (entry.id != 0u)
    {
        entry.list = r.list;
        return entry.id;
    }
    if (entry.list == r.list)
        return 0u;
    entry.list = r.list;
    entry.id = ++r.nextId;
    define = true;
    return entry.id;
}


// A VU1 mesh's own register writes (docs/NATIVE-BOUNDARY.md, issue 4): the
// native program's fog colour, PRIM and PRMODE ride with the mesh as
// (register, value) pairs and go straight to the draw-state mirror, not as
// packets to classify and decode again.
using RegisterWrite = std::pair<uint8_t, uint64_t>;
thread_local const RegisterWrite *t_meshWrites = nullptr;
// M6: a chunk drawn from a model call's record is drawn under the record's
// draw registers (made where the game wrote the call), not the list's.
thread_local const rg::drawstate::Registers *t_recordRegs = nullptr;
// And a captured stream's chunks under the stream's (the game thread's where
// the emitter wrote, then the stream's own writes, applied as they come).
thread_local rg::drawstate::Registers *t_streamRegs = nullptr;
thread_local uint32_t t_meshWriteCount = 0u;


// Bytes from local memory or scratchpad, wrapping as the runtime's DMA does.
void appendBytes(PS2Memory &memory, Bytes &out, uint32_t address, uint32_t bytes)
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
        if (!visit(tagAddress, data, qwc, static_cast<uint32_t>(tag)))
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
//
// With `binds`, a texture binding marker (the texture hook's, in place of a
// node's uploads: docs/NATIVE-BOUNDARY.md) is resolved in the copy: each
// entry's source becomes its record's id, now, when the PS2's DMA would
// read the pixels.
void gatherChain(PS2Memory &memory, uint32_t tadr, bool tte, bool tie, Bytes &out,
                 std::vector<std::pair<uint32_t, uint32_t>> *origins = nullptr, bool binds = false)
{
    out.clear();
    // Each piece, in order. send() runs on the game thread alone.
    struct Span
    {
        uint32_t address, bytes;
        bool bind;
    };
    thread_local std::vector<Span> spans;
    spans.clear();
    size_t total = 0u;
    walkChain(memory, tadr, tie, [&](uint32_t tagAddress, uint32_t data, uint32_t qwc, uint32_t tagLow) {
        if (tte)
        {
            // With TTE every tag supplies two VIFcodes before its payload.
            spans.push_back({tagAddress + 8u, 8u, false});
            total += 8u;
        }
        if (qwc != 0u)
        {
            // A marker: a cnt tag with the magic in its unused bits.
            const bool bind = binds && tte && qwc >= 2u && ((tagLow >> 28) & 0x7u) == 1u &&
                              ((tagLow >> 16) & 0x3FFu) == rg::nativeproducer::kBindTagMagic;
            Span span{data, qwc * 16u, bind};
            spans.push_back(span);
            total += spans.back().bytes;
        }
        return true;
    });
    try
    {
        // A garbage chain can sum to more than a buffer can hold; the list
        // then ends where the copy stops, like one that cannot be read.
        out.reserve(total);
        bool begun = false;
        for (const Span &span : spans)
        {
            if (origins)
            {
                origins->emplace_back(static_cast<uint32_t>(out.size()), span.address);
            }
            const size_t at = out.size();
            appendBytes(memory, out, span.address, span.bytes);
            if (!span.bind)
                continue;
            if (!begun)
            {
                rg::nativeproducer::bindBegin();
                begun = true;
            }
            // Past its GIF tag, each entry: its 64 bits resolved, the rest
            // of the qword the register alone.
            for (size_t q = at + 16u; q + 16u <= out.size(); q += 16u)
            {
                rg::nativeproducer::BindEntry entry;
                std::memcpy(&entry, out.data() + q, sizeof(entry));
                if (entry.reg != rg::nativeproducer::kBindRegister)
                    continue;
                const uint64_t resolved[2] = {
                    rg::nativeproducer::bindResolve(memory.getRDRAM(), PS2_RAM_SIZE, entry),
                    rg::nativeproducer::kBindRegister};
                std::memcpy(out.data() + q, resolved, sizeof(resolved));
            }
        }
    }
    catch (const std::exception &)
    {
        // As before: the list ends at the piece that could not be read.
    }
}

// A GS packet on its way, as the memory's GIF callback sends it (below).
void deliverPacket(const uint8_t *data, uint32_t size);
// Register writes to the draw state, as an A+D packet's would go (below).
void applyWrites(const std::vector<std::pair<uint8_t, uint64_t>> &writes);
// A model emit call the game did not write, drawn from its record (below).
void drawUnwritten(uint32_t unit);
// A native strip likewise, in order with the packets. `vu1Data`: the VU1
// data memory a program's mesh was built from, for the native pipeline's
// mesh draw (rg::nativeproducer::mesh); null for others.
void deliverMesh(const rg::NativeMesh &mesh, const uint8_t *vu1Data = nullptr);
// deliverPacket with the packet's place known: its EE address in the list
// (~0u for none) and, for one a VU1 program kicked, that of the VIF code
// that started the program (~0u when not looked up).
void deliverPacketAt(const uint8_t *data, uint32_t size, uint32_t address, uint32_t cursor);
// A texture continued over DIRECT packets (PS2Memory's image callback).
void deliverImage(const uint8_t *data, uint32_t size);
// A hole's image data (gatherChain), which never left guest memory.

// (M6: there is no draw stage any more. A list runs on the game thread as it
// is sent and what it makes goes straight to the producer, in the game's
// order; the producer's records go to the renderer's queue.)
// For busyReport's VU1 pair counts.
PS2Runtime *g_runtime = nullptr;

// One handed-over list, with the VU1 D/T-bit enables (VU0 FBRST bits 10 and
// 11) the game thread had when it started it.
struct List
{
    Bytes data;
    // Spans of `data` left uncopied (gatherChain), in order.
    // With the native effects: where each piece of `data` came from, as
    // (offset in data, EE address), and the list's number.
    std::vector<std::pair<uint32_t, uint32_t>> origins;
    uint32_t number = 0u;
    // M5 step 2: the REF payloads of chunks it carries (to learn), and the
    // MSCNTs standing for chunks it does not (to draw).
};

class Vif1Thread
{
public:
    explicit Vif1Thread(PS2Runtime &runtime) : m_runtime(runtime) {}

    // (M6: lists run on the game's thread as they are sent; nothing to stop.)
    void stop() {}

    // M6 of docs/NATIVE-ONLY.md: a list runs on the caller's thread (the
    // game's, in its send), so what it makes goes to the draw stage in the
    // game's order, among the records the game's hooks make; there is no
    // VIF1 thread any more. The draw stage still consumes the records on
    // its own thread, and finishes the list.
    void runList(List list)
    {
        {
            std::lock_guard lock(m_mutex);
            ++m_submitted;
        }
        m_current = std::move(list);
        // As the VIF1 thread was: a list's decoder, at the default floating
        // point environment (the records carry it to the draw stage).
        const bool wasOn = t_onVif1Thread, wasDecoder = t_listDecoder;
        const uint32_t mxcsr = _mm_getcsr();
        _mm_setcsr(kDefaultMxcsr);
        t_onVif1Thread = true;
        t_listDecoder = true;
        m_running = true;
        runCurrent();
        m_running = false;
        t_onVif1Thread = wasOn;
        t_listDecoder = wasDecoder;
        _mm_setcsr(mxcsr);
    }

    // A buffer for the next list, with the capacity of one that has run:
    // allocating a list's megabytes afresh every time, and freeing them on
    // the other thread, was a measurable part of the game thread's send.
    Bytes takeBuffer()
    {
        std::lock_guard lock(m_mutex);
        if (m_pool.empty())
        {
            return {};
        }
        Bytes buffer = std::move(m_pool.back());
        m_pool.pop_back();
        return buffer;
    }

    // MSCAL/MSCNT from the list being run: the chunk drawn by the native
    // programs (M6: nothing runs the VU1 for a draw; a chunk they do not
    // know or decline is counted and not drawn).
    void runVu1(bool resume, uint32_t startPC, uint32_t top)
    {
        PS2Memory &memory = m_runtime.memory();
        // RG_VU1_NATIVE: the chunk of a program done natively, built from the
        // data memory before the VU1 changes it. The MSCAL names the program
        // (once per code image and entry), its MSCNTs run the chunks; or the
        // MSCAL is the chunk (drawsAtCall).
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
                    // For the renderer's draw labels (debug/bug_mark.h).
                    if (found->second)
                        rg::nativeproducer::nameSource(programSource(found->second), found->second->name);
                }
                m_nativeProgram = found->second;
            }
            else if (memory.getVU1CodeGeneration() != m_nativeGeneration)
            {
                // An MPG between the MSCAL and its MSCNTs: the code the
                // program was identified in is gone.
                m_nativeProgram = nullptr;
            }
            t_vu1Source = programSource(m_nativeProgram);
            if (m_nativeProgram && resume != rg::vu1native::drawsAtCall(*m_nativeProgram))
                drawChunk(*m_nativeProgram, memory.getVU1Data(), top);
            else if (!m_nativeProgram)
                m_nativeMonitor.dropped("a program identify() does not know");
            m_nativeMonitor.tick();
        }
        t_vu1Source = 0u;
    }

    // A chunk of a program the native programs know (the list's MSCNT, or a
    // record's chain), with VU1 data memory as the VU1 would find it: drawn
    // by the model vertex stage, or declined (counted, not drawn).
    void drawChunk(const rg::vu1native::Program &program, uint8_t *data, uint32_t top)
    {
        const rg::vu1native::Program *native = &program;
        // The chunks that clip (slot E) go to the GPU too: it clips in clip
        // space.
        if (const char *reason = rg::vu1native::declineChunk(program, data, top))
        {
            m_nativeMonitor.declined(program, reason);
            return;
        }
        t_model.active = false;
        m_particleMesh = false;
        m_shadowMesh = false;
        m_outlineMesh = false;
        if (native->kind == rg::vu1native::Kind::Shadow)
        {
            // M5: the shadow volumes to the model vertex stage,
            // twice (the facing ones' blend and the others').
            m_nativeWrites.clear();
            m_shadowMesh = rg::vu1native::shadowMeshes(*native, data, top,
                                                       m_shadowWrites, m_shadowMeshes, m_shadowInputs,
                                                       m_shadowRows, m_shadowPalette);
            if (!m_shadowMesh)
            {
                m_nativeMonitor.dropped("shadow: its colours not set");
                return;
            }
        }
        else if (native->kind == rg::vu1native::Kind::Outline &&
                 rg::vu1native::outlineDraw(*native, data, top, m_nativeWrites,
                                            m_nativeMesh, t_model.inputs, t_model.edge, t_model.row,
                                            t_model.palette))
        {
            // M5: the outlines to the model vertex stage, which
            // tests each edge and draws its four lines.
            t_model.active = true;
            t_model.define = false;
            t_model.list = false;
            t_model.transient = true;
            t_model.volume = false;
            m_modelId = 0u;
            m_outlineMesh = true;
        }
        else if (native->kind == rg::vu1native::Kind::Particles)
        {
            // M5: the particles to the model vertex stage, which
            // makes each billboard in world space.
            rg::vu1native::particleMesh(data, top, m_nativeWrites, m_nativeMesh,
                                        t_model.inputs, t_model.row);
            t_model.palette.clear();
            t_model.active = true;
            t_model.define = false;
            t_model.list = true;
            t_model.transient = true;
            t_model.edge = 0u;
            t_model.volume = false;
            m_modelId = 0u;
            m_particleMesh = true;
        }
        else if (rg::vu1native::modelOnGpu(*native))
        {
            // M5: the chunk goes to the model vertex stage. Seen
            // in two lists, it is kept under an id (its inputs
            // once); before that, or when its inputs change
            // from frame to frame (cloth), its inputs go with
            // this draw. No vertex is made here.
            const uint32_t modelId =
                modelIdOf(rg::vu1native::modelChunkHash(*native, data, top), t_model.define);
            t_model.transient = modelId == 0u;
            if (t_model.define || t_model.transient)
                rg::vu1native::modelChunk(*native, data, top, t_model.inputs, t_model.list);
            rg::vu1native::modelHeader(*native, data, top, m_nativeWrites, m_nativeMesh);
            rg::vu1native::modelRow(*native, data, top, t_model.row, t_model.palette);
            t_model.active = true;
            t_model.edge = 0u;
            t_model.volume = false;
            m_modelId = modelId;
        }
        else
        {
            rg::vu1native::buildChunk(*native, data, top, m_nativeWrites);
        }
        m_nativeMonitor.native(program);
        if (m_shadowMesh)
        {
            // Each mesh after its packets, the model matrix the
            // identity (its corners are in world space).
            std::memcpy(m_particleQuads, data, sizeof(m_particleQuads));
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(m_particleQuads + 60u * 16u, identity, sizeof(identity));
            for (int i = 0; i < 2; ++i)
            {
                // Its state packets' writes as the mesh's own.
                t_meshWrites = m_shadowWrites[i].data();
                t_meshWriteCount = static_cast<uint32_t>(m_shadowWrites[i].size());
                t_model.active = true;
                t_model.define = false;
                t_model.list = true;
                t_model.transient = true;
                t_model.edge = 0u;
                t_model.volume = true;
                t_model.inputs = m_shadowInputs;
                t_model.row = m_shadowRows[i];
                t_model.palette = m_shadowPalette;
                t_retain = {};
                deliverMesh(m_shadowMeshes[i], m_particleQuads);
                t_model.active = false;
                t_model.volume = false;
                t_meshWriteCount = 0u;
            }
            m_shadowMesh = false;
        }
        const bool meshed = rg::vu1native::hasMesh(*native) || m_particleMesh || m_outlineMesh;
        // A mesh's own register writes ride with it; a chunk with no mesh
        // (the base piece's FOGCOL) leaves its writes in the list's state.
        if (!meshed)
            applyWrites(m_nativeWrites);
        if (meshed)
        {
            if (t_model.active)
                t_retain = {m_modelId, false};
            t_meshWrites = m_nativeWrites.data();
            t_meshWriteCount = static_cast<uint32_t>(m_nativeWrites.size());
            if (m_particleMesh)
            {
                // The corners are in world space: the model matrix
                // the producer reads (quads 60-63) the identity.
                std::memcpy(m_particleQuads, data, sizeof(m_particleQuads));
                const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                std::memcpy(m_particleQuads + 60u * 16u, identity, sizeof(identity));
                deliverMesh(m_nativeMesh, m_particleQuads);
            }
            else
                deliverMesh(m_nativeMesh, data);
            t_retain = {};
            t_model.active = false;
            t_meshWriteCount = 0u;
        }
        // What the VU1 would have left in its memory, where it did not run.
        rg::vu1native::applyMemoryEffects(program, data, top);
    }

    // M6: a model emit call the game did not write (src/native_emit.h),
    // drawn where its marker is in the list: each item's quads put in VU1
    // memory as its packets would have put them (the model matrix, then
    // the base piece's run; the mesh header; the palette), each group's
    // material quads and texenv register writes, each chunk unpacked from
    // its span in EE memory to the item's VIF BASE and drawn as the list's
    // run draws a model chunk.
public:
    // M6: a captured stream (src/native_emit.h), replayed where its marker
    // is: the VIF runs it as it would have run it in the list here (the
    // runtime's VIF1 keeps its registers, not its position, between
    // calls), its draws under the stream's registers.
    void replayStream(uint32_t unit)
    {
        std::vector<uint8_t> bytes;
        rg::drawstate::Registers regs;
        rg::nativeemit::Globals globals;
        if (!rg::nativeemit::takeStream(unit, bytes, regs, globals) || bytes.empty())
            return;
        putGlobals(globals);
        rg::drawstate::Registers *const outer = t_streamRegs;
        t_streamRegs = &regs;
        m_runtime.memory().processVIF1Data(bytes.data(), static_cast<uint32_t>(bytes.size()));
        t_streamRegs = outer;
    }

    // The view, camera and light quads as the list had them where the
    // record was made (src/native_emit.h's Globals), in VU1 memory.
    void putGlobals(const rg::nativeemit::Globals &globals)
    {
        uint8_t *data = m_runtime.memory().getVU1Data();
        for (uint32_t q = 0; q < 60u; ++q)
            if (globals.known & (1ull << q))
                std::memcpy(data + q * 16u, globals.quads[q].data(), 16u);
    }

    void drawUnwritten(uint32_t unit)
    {
        rg::nativeemit::Call call;
        if (!rg::nativeemit::takeUnwritten(unit, call))
            return;
        putGlobals(call.globals);
        PS2Memory &memory = m_runtime.memory();
        uint8_t *data = memory.getVU1Data();
        const uint8_t *rdram = memory.getRDRAM();
        const auto put = [&](uint32_t q, const rg::nativeemit::Quad &v) {
            std::memcpy(data + (q & 1023u) * 16u, v.data(), 16u);
        };
        std::vector<RegisterWrite> baseWrites;
        rg::drawstate::Registers regs;
        for (const rg::nativeemit::Item &item : call.items)
        {
            for (uint32_t r = 0; r < 4u; ++r)
                put(60u + r, item.model[r]);
            // The base piece's packet (the object's FOGCOL): on the
            // record's registers, and the list's for what follows.
            rg::vu1native::runBaseOn(data, baseWrites);
            applyWrites(baseWrites);
            put(76u, item.q76);
            put(77u, item.q77);
            put(78u, item.q78);
            put(79u, item.q79);
            put(81u, item.q81);
            put(42u, item.q42);
            if (item.bones != 0u && item.p1C != 0u && item.paletteAt + item.bones * 4u <= 1024u)
                std::memcpy(data + item.paletteAt * 16u, rdram + ((item.p1C + 0x20u) & (PS2_RAM_SIZE - 1u)),
                            item.bones * 4u * 16u);
            for (const rg::nativeemit::Group &group : item.groups)
            {
                put(82u, group.q82);
                put(83u, rg::nativeemit::Quad{});
                put(84u, group.q84);
                put(86u, group.q86);
                put(87u, group.q87);
                put(88u, rg::nativeemit::Quad{});
                applyWrites(group.writes);
                regs = group.regs;
                regs.write(baseWrites.data(), baseWrites.size());
                t_recordRegs = &regs;
                unpackChain(memory, group.chain, item.base, *group.program);
                t_recordRegs = nullptr;
            }
        }
    }
private:
    // A group's chain, as the VIF would run it: STCYCL and UNPACK into VU1
    // data memory (TOP the item's BASE), each MSCNT a chunk drawn.
    void unpackChain(PS2Memory &memory, uint32_t chain, uint32_t top, const rg::vu1native::Program &program)
    {
        const uint8_t *rdram = memory.getRDRAM();
        uint8_t *data = memory.getVU1Data();
        const auto word = [&](uint32_t address) {
            uint32_t v;
            std::memcpy(&v, rdram + (address & (PS2_RAM_SIZE - 1u)), 4u);
            return v;
        };
        uint32_t cl = 1u, wl = 1u;
        for (int tags = 0; tags < 4096; ++tags, chain += 16u)
        {
            const uint32_t tag = word(chain);
            if (((tag >> 28) & 7u) != 3u)
                return; // the RET (or not a REF: nothing more)
            const uint32_t start = word(chain + 4u) & 0x0FFFFFFFu, end = start + (tag & 0xFFFFu) * 16u;
            uint32_t at = start;
            while (at + 4u <= end)
            {
                const uint32_t cmd = word(at);
                at += 4u;
                const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
                if (op == 0x01u)
                {
                    cl = imm & 0xFFu;
                    wl = (imm >> 8) & 0xFFu;
                }
                else if (op == 0x17u)
                {
                    t_vu1Source = programSource(&program);
                    drawChunk(program, data, top);
                    t_vu1Source = 0u;
                }
                else if ((op & 0x60u) == 0x60u)
                {
                    const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, n = num == 0u ? 256u : num;
                    const bool usn = (imm >> 14) & 1u, flg = (imm >> 15) & 1u;
                    const uint32_t address = (imm & 0x3FFu) + (flg ? top : 0u);
                    const uint32_t lanes = vn + 1u, step = std::max(cl, 1u), write = std::max(wl, 1u);
                    const uint32_t bits = vl == 3u ? 16u : (32u >> vl) * lanes;
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const uint32_t q = (address + (write <= step ? (i / write) * step + (i % write) : i)) & 1023u;
                        uint32_t out[4];
                        std::memcpy(out, data + q * 16u, sizeof(out));
                        const uint32_t bytes = 4u >> vl;
                        for (uint32_t l = 0; l < lanes && vl != 3u; ++l)
                        {
                            const uint32_t from = at + (i * lanes + l) * bytes;
                            uint32_t v = word(from & ~3u) >> ((from & 3u) * 8u);
                            if (bytes == 2u)
                                v = usn ? (v & 0xFFFFu) : uint32_t(int32_t(int16_t(v & 0xFFFFu)));
                            else if (bytes == 1u)
                                v = usn ? (v & 0xFFu) : uint32_t(int32_t(int8_t(v & 0xFFu)));
                            out[l] = v;
                        }
                        if (vn == 0u && vl != 3u)
                            out[1] = out[2] = out[3] = out[0];
                        std::memcpy(data + q * 16u, out, sizeof(out));
                    }
                    at += ((n * bits + 31u) / 32u) * 4u;
                }
                else
                {
                    // Data words of the others (MPG, DIRECT, STMASK, ...): skipped.
                    if (op == 0x20u)
                        at += 4u;
                    else if (op == 0x30u || op == 0x31u)
                        at += 16u;
                    else if (op == 0x4Au)
                        at += (num == 0u ? 256u : num) * 8u;
                    else if (op == 0x50u || op == 0x51u)
                        at += (imm == 0u ? 65536u : imm) * 16u;
                }
            }
        }
    }

public:
    // The tag of the program starting at pair `startPC`: the entry and a hash
    // of the 1 KiB of microcode from it, which names the program whatever else
    // the code memory holds. Kept until the code memory is next written.


    // Waits until every list handed over has run, and the GS packets they
    // made with them. A no-op on the VIF1 thread itself, which never starts
    // DMA but could reach a hook, and on the GIF stage's.
    void drain(int line = __builtin_LINE())
    {
        if (m_running)
        {
            return;
        }
        const int64_t start = vifwait::g_on ? vifwait::nowNs() : 0;
        {
            std::unique_lock lock(m_mutex);
            m_idle.wait(lock, [&] { return m_completed == m_submitted; });
        }
        if (vifwait::g_on)
        {
            vifwait::add(vifwait::g_drain[line & (vifwait::kLines - 1)], start);
            vifwait::report();
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
        return false;
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
    void runCurrent()
    {
        PS2Memory &memory = m_runtime.memory();
        const auto started = std::chrono::steady_clock::now();
        try
        {
            t_listData = m_current.data.data();
            t_listSize = m_current.data.size();
            t_listOrigins = &m_current.origins;
            t_listOriginAt = 0u;
            retainList();
            if (!m_current.data.empty())
            {
                memory.processVIF1Data(m_current.data.data(), static_cast<uint32_t>(m_current.data.size()));
            }
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "[vif1] list failed: %s\n", error.what());
        }
        if (vifwait::g_on)
            vifwait::add(vifwait::g_listVif1, vifwait::g_listSent.load());
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
        m_runtime.eeScheduler().postEvent(EeEvent{EeEventType::Dmac, 1u, 0u});
    }

    static constexpr uint32_t kDefaultMxcsr = 0x1F80u;
    PS2Runtime &m_runtime;
    std::mutex m_mutex;
    std::condition_variable m_idle;
    List m_current; // the list running
    bool m_running = false; // a list is running (on the game's thread)
    static constexpr size_t kPoolSize = 8u;
    std::vector<Bytes> m_pool; // guarded by m_mutex
    // RG_VU1_NATIVE: the program the last MSCAL started, for its MSCNTs, the
    // code image it was found in, and the packets of a chunk.
    const rg::vu1native::Program *m_nativeProgram = nullptr;
    uint64_t m_nativeGeneration = ~0ull;
    uint32_t m_nativePC = 0u;
    // The programs identified, by code image (its hash) and entry.
    std::unordered_map<uint64_t, const rg::vu1native::Program *> m_identified;
    uint64_t m_codeHash = 0u;

    // The 16 KiB of micro memory: a hash per 256-byte block, made again only
    // for the blocks whose bytes changed since the last (against a copy of
    // them), folded into one. Rehashing the whole image a word at a time
    // on every code upload was ~15% of this thread (2026-11-09): the game
    // uploads pieces of its programs per object.
    static constexpr uint32_t kCodeBlock = 256u, kCodeBlocks = PS2_VU1_CODE_SIZE / kCodeBlock;
    static_assert(PS2_VU1_CODE_SIZE % kCodeBlock == 0u);
    uint64_t imageHash(const uint8_t *code)
    {
        for (uint32_t b = 0; b < kCodeBlocks; ++b)
        {
            const uint8_t *at = code + b * kCodeBlock;
            uint8_t *seen = m_codeSeen.data() + b * kCodeBlock;
            if (m_codeSeenValid && std::memcmp(seen, at, kCodeBlock) == 0)
                continue;
            std::memcpy(seen, at, kCodeBlock);
            uint64_t hash = 0x84222325CBF29CE4ull ^ (uint64_t(b) * 0x9E3779B97F4A7C15ull);
            for (uint32_t i = 0; i < kCodeBlock; i += 8u)
            {
                uint64_t word;
                std::memcpy(&word, at + i, sizeof(word));
                hash = (hash ^ word) * 0x100000001B3ull;
                hash ^= hash >> 29;
            }
            m_blockHash[b] = hash;
        }
        m_codeSeenValid = true;
        uint64_t hash = 0x84222325CBF29CE4ull;
        for (uint32_t b = 0; b < kCodeBlocks; ++b)
        {
            hash = (hash ^ m_blockHash[b]) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }
    std::vector<uint8_t> m_codeSeen = std::vector<uint8_t>(PS2_VU1_CODE_SIZE);
    std::array<uint64_t, kCodeBlocks> m_blockHash{};
    bool m_codeSeenValid = false;
    rg::vu1native::Monitor m_nativeMonitor;
    std::vector<RegisterWrite> m_nativeWrites; // a mesh's, in their place
    rg::NativeMesh m_nativeMesh;
    uint32_t m_modelId = 0u;              // the model chunk being drawn (t_model)
    bool m_outlineMesh = false;           // the chunk is outlines for the model stage (outlineDraw)
    bool m_particleMesh = false;          // the chunk is particles as a mesh (particleMesh)
    bool m_shadowMesh = false;            // the chunk is shadow volumes as two meshes (shadowMeshes)
    std::vector<RegisterWrite> m_shadowWrites[2];
    rg::NativeMesh m_shadowMeshes[2];
    std::vector<dq8::gfx::native::ModelInput> m_shadowInputs;
    dq8::gfx::native::ModelRow m_shadowRows[2]{};
    std::vector<float> m_shadowPalette;
    alignas(16) uint8_t m_particleQuads[64u * 16u] = {}; // its VU1 quads, the model the identity
    uint64_t m_submitted = 0u;
    uint64_t m_completed = 0u;
    uint64_t m_reported = 0u;
};

Vif1Thread *g_thread = nullptr;

void drawUnwritten(uint32_t unit)
{
    g_thread->drawUnwritten(unit);
}

// Steps 3b and 6 of docs/NATIVE-PIPELINE.md: uploads the producer made
// records of are withheld from the GS, a record whose memory the game
// changed after the texture hook is made again from what was sent, and host
// uploads nobody took and local copies are handed to the producer (which
// keeps them as images) on their way to the GS. Always, now that the native
// pipeline is the only one; the withholding itself goes in M6
// (docs/NATIVE-ONLY.md).
constexpr bool uploadsTaken()
{
    return true;
}

// The transfer registers as the packets set them (this thread's).
thread_local uint64_t t_bitbltbuf = 0u, t_trxpos = 0u, t_trxreg = 0u;

// M6's census of the uploads the list still carries, by path and the draw
// task that wrote them (below, with the packets' census).
enum class UploadPath : uint8_t
{
    Host,       // a host upload nobody made a record of (captured)
    LocalCopy,  // a local-to-local copy (mirrored)
    Taken,      // a take marker's record (no transfer in the list)
    HostMarker, // an upload by reference made a host upload at its marker
};
void countUpload(UploadPath path);

// Bits a transfer sends a pixel of, by DPSM; 0: not one taken.
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

// M6: the texture hook's and the upload builders' markers in the list
// (src/native_producer.h): an A+D block of writes to kBindRegister (bound
// records), kTakeRegister (records the hook made, published here) or
// kHostRegister/kHostSourceRegister (an upload by reference, its record or
// a host upload of its source as it is now), applied here in the stream's
// order. A block of markers alone is dropped (into `kept`, true); anything
// else (a transfer the game wrote some other way) goes on as it is.
bool takeMarkers(const uint8_t *data, uint32_t size, std::vector<uint8_t> &kept)
{
    namespace producer = rg::nativeproducer;
    kept.clear();
    bool changed = false;
    uint32_t pos = 0u;
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
            thread_local std::vector<uint64_t> binds, takes, hosts, sources;
            binds.clear();
            takes.clear();
            hosts.clear();
            sources.clear();
            uint32_t others = 0u;
            for (uint32_t at = pos + 16u; at + 16u <= end; at += 16u)
            {
                uint64_t value = 0u;
                std::memcpy(&value, data + at, 8u);
                const uint8_t reg = data[at + 8u];
                if (reg == producer::kBindRegister)
                    binds.push_back(value);
                else if (reg == producer::kTakeRegister)
                    takes.push_back(value);
                else if (reg == producer::kHostRegister)
                    hosts.push_back(value);
                else if (reg == producer::kHostSourceRegister)
                    sources.push_back(value);
                else
                    ++others;
            }
            if (!binds.empty())
                producer::bound(binds.data(), binds.size());
            const auto shape = [](uint64_t v, uint32_t &dbp, uint32_t &dbw, uint32_t &dpsm, uint32_t &w, uint32_t &h) {
                dbp = uint32_t(v & 0x3FFFu);
                dbw = uint32_t((v >> 14) & 0x3Fu);
                dpsm = uint32_t((v >> 20) & 0x3Fu);
                w = uint32_t((v >> 26) & 0xFFFu);
                h = uint32_t((v >> 38) & 0xFFFu);
            };
            for (const uint64_t v : takes)
            {
                uint32_t dbp, dbw, dpsm, w, h;
                shape(v, dbp, dbw, dpsm, w, h);
                bool palette = false;
                uint64_t hash = 0u;
                uint32_t hashed = 0u;
                if (producer::takeProduced(dbp, dbw, dpsm, w, h, palette, hash, hashed))
                    countUpload(UploadPath::Taken);
                else
                    producer::notePassed(producer::Passed::NotProduced);
            }
            if (hosts.size() == sources.size())
            {
                uint8_t *ram = g_runtime->memory().getRDRAM();
                for (size_t i = 0; i < hosts.size(); ++i)
                {
                    uint32_t dbp, dbw, dpsm, w, h;
                    shape(hosts[i], dbp, dbw, dpsm, w, h);
                    const uint64_t src = sources[i];
                    bool palette = false;
                    uint64_t hash = 0u;
                    uint32_t hashed = 0u;
                    if (producer::takeProduced(dbp, dbw, dpsm, w, h, palette, hash, hashed))
                    {
                        countUpload(UploadPath::Taken);
                        continue;
                    }
                    const uint32_t address = uint32_t(src) & (PS2_RAM_SIZE - 1u);
                    const uint64_t bytes = (uint64_t(w) * h * transferBits(dpsm) + 7u) / 8u;
                    if ((src & producer::kHostSourceBlocks) != 0u && dpsm == 0u && bytes != 0u &&
                        address + bytes <= PS2_RAM_SIZE && (w & 15u) == 0u && (h & 15u) == 0u)
                    {
                        // The movie's macroblocks, 16x16 CT32 column by
                        // column: as rows of the frame.
                        thread_local std::vector<uint8_t> frame;
                        frame.resize(size_t(w) * h * 4u);
                        const uint32_t nh = h / 16u;
                        for (uint32_t i = 0; i < w / 16u; ++i)
                            for (uint32_t j = 0; j < nh; ++j)
                            {
                                const uint8_t *block = ram + address + (size_t(i) * nh + j) * 0x400u;
                                for (uint32_t r = 0; r < 16u; ++r)
                                    std::memcpy(frame.data() + ((size_t(j) * 16u + r) * w + i * 16u) * 4u,
                                                block + r * 64u, 64u);
                            }
                        producer::hostUpload(dbp, dbw, dpsm, 0u, 0u, w, h, frame.data(),
                                             static_cast<uint32_t>(frame.size()));
                        countUpload(UploadPath::HostMarker);
                    }
                    else if (bytes != 0u && address + bytes <= PS2_RAM_SIZE)
                    {
                        producer::hostUpload(dbp, dbw, dpsm, uint32_t((src >> 32) & 0x7FFu),
                                             uint32_t((src >> 43) & 0x7FFu), w, h, ram + address,
                                             static_cast<uint32_t>(bytes));
                        countUpload(UploadPath::HostMarker);
                    }
                }
            }
            drop = others == 0u && (!binds.empty() || !takes.empty() || !hosts.empty());
        }
        if (drop && !changed)
        {
            kept.assign(data, data + pos);
            changed = true;
        }
        else if (!drop && changed)
            kept.insert(kept.end(), data + pos, data + end);
        pos = end;
    }
    if (changed && pos < size)
        kept.insert(kept.end(), data + pos, data + size);
    return changed;
}

// A host upload nobody took (no record withheld it), its pixels gathered
// from the IMAGE data that follows (in the packet or continuing past it)
// for the producer, which keeps what it leaves in GS memory
// (rg::nativeproducer::hostUpload). The upload still goes to the GS.
struct HostCapture
{
    bool active = false;
    uint64_t bitbltbuf = 0u, trxpos = 0u, trxreg = 0u;
    uint32_t need = 0u;
    std::vector<uint8_t> data;
};
thread_local HostCapture t_capture;

uint32_t captureBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u;                     // CT32
    case 0x02u: case 0x0Au: return 16u;         // CT16, CT16S
    case 0x13u: case 0x1Bu: return 8u;          // T8, T8H
    case 0x14u: case 0x24u: case 0x2Cu: return 4u; // T4, T4HL, T4HH
    default: return 0u;
    }
}

void startCapture()
{
    HostCapture &c = t_capture;
    c.active = false;
    c.data.clear();
    const uint32_t bits = captureBits(static_cast<uint32_t>(t_bitbltbuf >> 56));
    const uint64_t need = (uint64_t(t_trxreg & 0xFFFu) * ((t_trxreg >> 32) & 0xFFFu) * bits + 7u) / 8u;
    if (bits == 0u || need == 0u || need > (16u << 20))
        return;
    c.active = true;
    countUpload(UploadPath::Host);
    c.bitbltbuf = t_bitbltbuf;
    c.trxpos = t_trxpos;
    c.trxreg = t_trxreg;
    c.need = static_cast<uint32_t>(need);
    c.data.reserve(c.need);
}

// IMAGE bytes of the upload being captured; whatever is past it is not.
void feedCapture(const uint8_t *data, uint32_t size)
{
    HostCapture &c = t_capture;
    if (!c.active)
        return;
    const uint32_t n = std::min<uint32_t>(size, c.need - static_cast<uint32_t>(c.data.size()));
    c.data.insert(c.data.end(), data, data + n);
    if (c.data.size() < c.need)
        return;
    c.active = false;
    const uint64_t b = c.bitbltbuf;
    rg::nativeproducer::hostUpload(
        static_cast<uint32_t>((b >> 32) & 0x3FFFu), static_cast<uint32_t>((b >> 48) & 0x3Fu),
        static_cast<uint32_t>((b >> 56) & 0x3Fu), static_cast<uint32_t>((c.trxpos >> 32) & 0x7FFu),
        static_cast<uint32_t>((c.trxpos >> 48) & 0x7FFu), static_cast<uint32_t>(c.trxreg & 0xFFFu),
        static_cast<uint32_t>((c.trxreg >> 32) & 0xFFFu), c.data.data(), static_cast<uint32_t>(c.data.size()));
    c.data.clear();
}

void transferRegister(uint8_t reg, uint64_t value)
{
    switch (reg)
    {
    case 0x50u: t_bitbltbuf = value; break;
    case 0x51u: t_trxpos = value; break;
    case 0x52u: t_trxreg = value; break;
    case 0x53u: // TRXDIR
        if ((value & 3u) == 0u)
            startCapture();
        else
            t_capture.active = false;
        if ((value & 3u) == 2u)
        {
            countUpload(UploadPath::LocalCopy);
            const uint64_t b = t_bitbltbuf;
            rg::nativeproducer::noteLocalCopy(
                unsigned(b & 0x3FFFu), unsigned((b >> 16) & 0x3Fu), unsigned((b >> 24) & 0x3Fu),
                unsigned(t_trxpos & 0x7FFu), unsigned((t_trxpos >> 16) & 0x7FFu), unsigned((b >> 32) & 0x3FFFu),
                unsigned((b >> 48) & 0x3Fu), unsigned((b >> 56) & 0x3Fu), unsigned((t_trxpos >> 32) & 0x7FFu),
                unsigned((t_trxpos >> 48) & 0x7FFu), unsigned(t_trxreg & 0xFFFu), unsigned((t_trxreg >> 32) & 0xFFFu));
        }
        break;
    default:
        break;
    }
}

void absorbRegister(uint8_t reg, uint64_t value);
std::atomic<uint64_t> g_absorbedPackets{0u};

// A packet on its way to the GS: its transfers, for the producer; with
// `absorbToo`, its SIGNAL, FINISH and LABEL as well (absorb's, in the same
// walk).
void watchTransfers(const uint8_t *data, uint32_t size, bool absorbToo = false)
{
    if (absorbToo)
        g_absorbedPackets.fetch_add(1u, std::memory_order_relaxed);
    uint32_t pos = 0u;
    while (pos + 16u <= size)
    {
        uint64_t lo = 0u, hi = 0u;
        std::memcpy(&lo, data + pos, 8u);
        std::memcpy(&hi, data + pos + 8u, 8u);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        nreg = nreg ? nreg : 16u;
        pos += 16u;
        if (flg == 0u)
        {
            // PACKED: a qword a register; only A+D reaches the transfer ones.
            for (uint32_t i = 0; i < nloop * nreg && pos + 16u <= size; ++i, pos += 16u)
                if (((hi >> ((i % nreg) * 4u)) & 0xFu) == 0xEu)
                {
                    uint64_t value = 0u;
                    std::memcpy(&value, data + pos, 8u);
                    transferRegister(data[pos + 8u], value);
                    if (absorbToo)
                        absorbRegister(data[pos + 8u], value);
                }
        }
        else if (flg == 1u)
        {
            pos += ((nloop * nreg * 8u) + 15u) & ~15u; // REGLIST: no A+D
        }
        else
        {
            const uint32_t bytes = std::min<uint32_t>(nloop * 16u, size - std::min(size, pos));
            feedCapture(data + pos, bytes);
            pos += bytes;
        }
    }
}

// What the game still sees of the GS: SIGNAL, FINISH and LABEL in the
// privileged registers (CSR, SIGLBLID), set here as the GS would. Nothing
// else of a packet goes to the GS (there is none behind the pipeline).
void absorbRegister(uint8_t reg, uint64_t value)
{
    GSRegisters *regs = g_runtime ? &g_runtime->memory().gs() : nullptr;
    if (!regs)
        return;
    switch (reg)
    {
    case 0x60u: // SIGNAL
    {
        const uint32_t id = static_cast<uint32_t>(value), mask = static_cast<uint32_t>(value >> 32);
        uint32_t lo = static_cast<uint32_t>(regs->siglblid);
        lo = (lo & ~mask) | (id & mask);
        regs->siglblid = (regs->siglblid & 0xFFFFFFFF00000000ull) | lo;
        regs->csr.fetch_or(0x1u);
        break;
    }
    case 0x61u: // FINISH
        regs->csr.fetch_or(0x2u);
        break;
    case 0x62u: // LABEL
    {
        const uint32_t id = static_cast<uint32_t>(value), mask = static_cast<uint32_t>(value >> 32);
        uint32_t hi = static_cast<uint32_t>(regs->siglblid >> 32);
        hi = (hi & ~mask) | (id & mask);
        regs->siglblid = (uint64_t(hi) << 32) | (regs->siglblid & 0xFFFFFFFFull);
        break;
    }
    default:
        break;
    }
}

// A packet to the GS as it is.
void sendRaw(const uint8_t *data, uint32_t size)
{
    if (uploadsTaken())
    {
        thread_local std::vector<uint8_t> kept;
        if (takeMarkers(data, size, kept))
        {
            if (kept.empty())
                return;
            data = kept.data();
            size = static_cast<uint32_t>(kept.size());
        }
        watchTransfers(data, size, true);
    }
}

// Register writes, as an A+D packet of them would be taken: the draw-state
// mirror, then SIGNAL, FINISH and LABEL.
void applyWrites(const std::vector<std::pair<uint8_t, uint64_t>> &writes)
{
    if (t_streamRegs)
        t_streamRegs->write(writes.data(), writes.size());
    g_absorbedPackets.fetch_add(1u, std::memory_order_relaxed);
    for (const auto &[reg, value] : writes)
        absorbRegister(reg, value);
}


// The draw task of the packet being delivered (deliverPacketAt), for the census.
thread_local uint32_t t_task = 0u;

// M6: a packet the list still carries is register state and uploads; the
// game's 2D comes from its blocks' records (src/native_blocks.h), its models
// from theirs. A packet that still draws (a block the shadow could not
// follow, 2D written outside the GsPkt builder) is not drawn: counted by the
// draw task that wrote it, a line every 2000 (what is left to move).
std::atomic<uint64_t> g_packetsState{0u};
void countUndrawn()
{
    static std::mutex mutex;
    static std::unordered_map<uint32_t, uint64_t> byTask;
    static uint64_t total = 0u, nextReport = 1u;
    std::lock_guard lock(mutex);
    byTask[t_task] += 1u;
    total += 1u;
    if (total < nextReport)
        return;
    nextReport = total + 2000u;
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto &[task, count] : byTask)
        top.emplace_back(count, task);
    std::sort(top.rbegin(), top.rend());
    std::string line;
    for (size_t i = 0; i < top.size() && i < 12u; ++i)
    {
        char item[48];
        std::snprintf(item, sizeof(item), " %s%06x=%llu", top[i].second ? "TaskDraw@" : "no task@", top[i].second,
                      (unsigned long long)top[i].first);
        line += item;
    }
    std::fprintf(stderr, "[gs-packets] packets that draw, not drawn, by draw task (%llu; state-only %llu):%s\n",
                 (unsigned long long)total, (unsigned long long)g_packetsState.load(), line.c_str());
}

void countUpload(UploadPath path)
{
    static std::mutex mutex;
    static std::unordered_map<uint32_t, uint64_t> byTask[4];
    static uint64_t total = 0u, nextReport = 1u;
    std::lock_guard lock(mutex);
    byTask[static_cast<int>(path)][t_task] += 1u;
    if (++total < nextReport)
        return;
    nextReport = total + 2000u;
    static const char *const names[4] = {"host (no record)", "local copies", "taken at a marker (no transfer)",
                                         "host at a marker (no transfer)"};
    std::string line;
    for (int k = 0; k < 4; ++k)
    {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (const auto &[task, count] : byTask[k])
            top.emplace_back(count, task);
        std::sort(top.rbegin(), top.rend());
        line += std::string(" | ") + names[k] + ":";
        for (size_t i = 0; i < top.size() && i < 6u; ++i)
        {
            char item[48];
            std::snprintf(item, sizeof(item), " %s%06x=%llu", top[i].second ? "TaskDraw@" : "no task@",
                          top[i].second, (unsigned long long)top[i].first);
            line += item;
        }
    }
    std::fprintf(stderr, "[uploads] the list's uploads by path and draw task (%llu)%s\n", (unsigned long long)total,
                 line.c_str());
}

// A packet on: its register writes to a captured stream's state (replaying
// one) and SIGNAL, FINISH, LABEL; one with a transfer's registers or image
// data to the upload path as it is.
void sendPacket(const uint8_t *data, uint32_t size)
{
    if (!data || size < 16u)
        return;
    thread_local std::vector<rg::gswrites::Write> writes;
    writes.clear();
    const rg::gswrites::Read read = rg::gswrites::read(data, size, writes);
    if (read.kicks)
        countUndrawn();
    bool transfer = read.image || !read.whole;
    for (const auto &[reg, value] : writes)
        transfer |= (reg >= 0x50u && reg <= 0x54u) || reg == rg::nativeproducer::kBindRegister ||
                    reg == rg::nativeproducer::kTakeRegister || reg == rg::nativeproducer::kHostRegister;
    if (transfer || t_capture.active)
    {
        sendRaw(data, size);
        return;
    }
    ++g_packetsState;
    applyWrites(writes);
}

std::atomic<uint64_t> g_effectsNative{0u};
std::atomic<uint64_t> g_effectsAsPackets{0u};

// The EE address of a packet in the VIF1 thread's list, or ~0u.
uint32_t listAddress(const uint8_t *data)
{
    if (!t_listData || !t_listOrigins || data < t_listData || data >= t_listData + t_listSize)
    {
        return ~0u;
    }
    const uint32_t offset = static_cast<uint32_t>(data - t_listData);
    const auto &pieces = *t_listOrigins;
    // The list is decoded front to back, so the piece is the last one found
    // or one soon after it: stepped to from there. A search of the whole
    // table for every packet (twice for a VU1 program's: its bytes, then the
    // VIF cursor) was 8% of the VIF1 thread, in cache misses (2026-11-09).
    size_t at = t_listOriginAt;
    if (at < pieces.size() && pieces[at].first <= offset)
    {
        while (at + 1u < pieces.size() && pieces[at + 1u].first <= offset)
        {
            ++at;
        }
    }
    else
    {
        auto it = std::upper_bound(pieces.begin(), pieces.end(), offset,
                                   [](uint32_t value, const auto &piece) { return value < piece.first; });
        if (it == pieces.begin())
        {
            return ~0u;
        }
        at = static_cast<size_t>(it - pieces.begin()) - 1u;
    }
    t_listOriginAt = at;
    return pieces[at].second + (offset - pieces[at].first);
}

// A pass sent at its marker (src/native_effects.h), or not (`why`): counted.
void countEffect(bool drawn, rg::nativefx::Effect kind, const std::string &why)
{
    if (drawn)
    {
        const uint64_t count = ++g_effectsNative;
        if (count == 1u || count % 3600u == 0u)
        {
            std::fprintf(stderr, "[nativefx] %llu passes drawn by the renderer, %llu not\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(g_effectsAsPackets.load()));
        }
        return;
    }
    ++g_effectsAsPackets;
    static std::mutex toldMutex;
    static std::vector<std::string> told;
    const std::string line = std::string(rg::nativefx::name(kind)) + ": " + why;
    std::lock_guard lock(toldMutex);
    if (told.size() < 16u && std::find(told.begin(), told.end(), line) == told.end())
    {
        told.push_back(line);
        std::fprintf(stderr, "[nativefx] a pass not drawn: %s\n", line.c_str());
    }
}

// M6: stage E's objects (ModelDrawOffscreenComposite), each drawn into an
// object layer of the renderer's own: its head's draws into the offscreen
// buffer go to the layer between its begin marker (the box its clear
// covers, src/native_blocks.cpp) and its tail marker, where the tail's
// passes read the layer (ObjectLayer, M3). The game wrote none of their
// blocks (the head's from their records, the tail's left to its passes).
thread_local bool t_layerRouted = false;
std::atomic<uint64_t> g_compositeUnits{0u}, g_compositeRouted{0u}, g_compositeRefused{0u};

void compositeBegin(uint32_t unit)
{
    rg::nativefx::Box box;
    t_layerRouted = rg::nativefx::takeCompositeBox(unit, box);
    if (t_layerRouted)
    {
        const int32_t rect[4] = {box.x0, box.y0, box.x1, box.y1};
        rg::nativeproducer::routeLayer(box.base, box.fbw, rect);
        ++g_compositeRouted;
    }
}

void compositeTail(uint32_t unit)
{
    const bool routed = t_layerRouted;
    if (routed)
        rg::nativeproducer::routeEnd();
    t_layerRouted = false;
    if (!rg::nativefx::emitPass(rg::nativefx::Effect::CompositeTail, unit, routed))
        ++g_compositeRefused;
    const uint64_t units = ++g_compositeUnits;
    if (units == 1u || units % 5000u == 0u)
        std::fprintf(stderr,
                     "[composite] %llu objects, %llu in layers (heads that found a clear box: %llu), %llu tails not "
                     "drawn\n",
                     static_cast<unsigned long long>(units), static_cast<unsigned long long>(g_compositeRouted.load()),
                     static_cast<unsigned long long>(rg::nativefx::compositesBegun()),
                     static_cast<unsigned long long>(g_compositeRefused.load()));
}

// The VIF code that started the running VU1 program, for a packet with no
// list address of its own (compositePacket).
uint32_t packetCursor(uint32_t address)
{
    return address == ~0u && g_vif1Memory ? listAddress(g_vif1Memory->vif1Cursor()) : ~0u;
}

void deliverPacket(const uint8_t *data, uint32_t size)
{
    const uint32_t address = t_onVif1Thread ? listAddress(data) : ~0u;
    const uint32_t cursor = t_onVif1Thread ? packetCursor(address) : ~0u;
    deliverPacketAt(data, size, address, cursor);
}

void deliverPacketAt(const uint8_t *data, uint32_t size, uint32_t address, uint32_t cursor)
{
    // M3 step 2: a pass marker, where a function the hook did not run would
    // have put its packets: the pass, in the stream's order.
    if (data && size >= 32u && (data[8] & 0xFu) == 0xEu && data[24] == rg::nativefx::kPassRegister)
    {
        uint64_t unit = 0u;
        std::memcpy(&unit, data + 16u, sizeof(unit));
        if ((unit >> 24) == 5u)
        {
            // M6: a model draw call the game did not write, drawn here from
            // its record (src/native_emit.h).
            bool unwritten = false;
            rg::nativeemit::marker(static_cast<uint32_t>(unit), unwritten);
            if (unwritten)
                drawUnwritten(static_cast<uint32_t>(unit));
            return;
        }
        if ((unit >> 24) == 8u)
        {
            // M6: an emitter's captured output (src/native_emit.h).
            g_thread->replayStream(static_cast<uint32_t>(unit));
            return;
        }
        if ((unit >> 24) == 6u || (unit >> 24) == 7u)
        {
            if ((unit >> 24) == 6u)
                compositeBegin(static_cast<uint32_t>(unit));
            else
                compositeTail(static_cast<uint32_t>(unit));
            return;
        }
        if ((unit >> 24) == 2u)
        {
            // M4: a block the game did not write (src/native_gspkt.h).
            rg::nativeblocks::emit(static_cast<uint32_t>(unit));
            return;
        }
        countEffect(rg::nativefx::emitPassUnit(static_cast<uint32_t>(unit)), rg::nativefx::Effect::Glare,
                    "its pass was not kept");
        return;
    }
    // M4: the layer of the draw task that wrote it (what a VU1 program
    // kicked: the task whose list holds the MSCAL), for its 2D.
    t_task = rg::nativefx::taskAt(address != ~0u ? address : cursor != ~0u ? cursor : 0u);
    // The flip's copy, from the game thread: after the lists in flight, as
    // the GS would get it.
    if (!t_onVif1Thread && rg::nativefx::flipNative())
    {
        uint32_t source = 0u, target = 0u;
        if (rg::nativefx::flipPacket(data, size, source, target))
        {
            g_thread->drain();
            // M3: the flip's copy as a pass, the frame copied whole into the
            // display buffer (32 sprites before).
            rg::nativeproducer::PassRequest request;
            request.kind = 7u; // native::PassKind::Capture
            request.frameBase = source;
            request.workBases[0] = target;
            const bool drawn = rg::nativeproducer::pass(request);
            if (drawn)
            {
                std::vector<uint8_t> packet(data, data + size);
                rg::nativefx::withholdKicks(packet);
                sendPacket(packet.data(), static_cast<uint32_t>(packet.size()));
            }
            else
            {
                sendPacket(data, size);
            }
            countEffect(drawn, rg::nativefx::Effect::Flip, "the producer cannot draw it");
            return;
        }
    }
    sendPacket(data, size);
}

void deliverMesh(const rg::NativeMesh &mesh, const uint8_t *vu1Data)
{
    // The mesh's own register writes ride with it (on its record's or its
    // stream's state, below); as a packet only when image data is due first.
    if (t_meshWriteCount != 0u)
    {
        if (!t_capture.active)
            g_absorbedPackets.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            const std::vector<RegisterWrite> writes(t_meshWrites, t_meshWrites + t_meshWriteCount);
            const std::vector<uint8_t> packet = rg::gswrites::packet(writes);
            const uint32_t count = t_meshWriteCount;
            t_meshWriteCount = 0u;
            sendPacket(packet.data(), static_cast<uint32_t>(packet.size()));
            t_meshWriteCount = count;
        }
    }
    // Steps 4a and 4c of docs/NATIVE-PIPELINE.md: a VU1 program's mesh as a
    // native mesh draw, under the draw state the GS would draw it with; made
    // one, it does not go to the GS at all.
    if (vu1Data)
    {
        if (!t_onVif1Thread)
            g_thread->drain();
        rg::DrawState state;
        if (t_recordRegs)
        {
            rg::drawstate::Registers regs = *t_recordRegs;
            regs.write(t_meshWrites, t_meshWriteCount);
            state = regs.state();
        }
        else if (t_streamRegs)
        {
            t_streamRegs->write(t_meshWrites, t_meshWriteCount);
            state = t_streamRegs->state();
        }
        else
        {
            // M6: every model emitter's chunks come from a record or a
            // captured stream; one from neither (a capture that failed, a
            // writer not known) has no draw state: not drawn.
            static std::atomic<uint64_t> outside{0u};
            const uint64_t n = ++outside;
            if (n == 1u || n % 10000u == 0u)
                std::fprintf(stderr, "[vu1native] chunks in the list outside a record or a captured stream, not "
                                     "drawn: %llu\n",
                             (unsigned long long)n);
            return;
        }
        rg::nativeproducer::ModelDraw model;
        if (t_model.active)
        {
            model.define = t_model.define;
            model.list = t_model.list;
            model.inputs = &t_model.inputs;
            model.row = &t_model.row;
            model.palette = &t_model.palette;
            model.edge = t_model.edge;
            model.volume = t_model.volume;
        }
        if (rg::nativeproducer::mesh(mesh, &state, vu1Data, t_vu1Source, t_retain.id,
                                     t_model.active ? &model : nullptr))
            return;
        // Refused here only for a buffer the game never named (counted as a
        // target miss): with no GS behind the pipeline nothing can draw it.
    }
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
    ctx->vu0_vpu_stat &= ~0x0600u; // (no VU1 run: never stopped by D or T)
    for (uint64_t n = finished; n > 0u; --n)
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 1u);
    }
}

} // namespace
// Whether the texture hook writes binding markers in place of uploads
// (docs/NATIVE-BOUNDARY.md): whenever the lists are sent through here (the
// send resolves the markers).
bool bindingTextures()
{
    return g_thread != nullptr; // (RG_TEXTURE_BIND went in M6)
}
namespace
{

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
    if (normal)
    {
        const uint32_t qwc = getRegU32(ctx, 6) & 0xFFFFu;
        try
        {
            list.data.reserve(qwc * 16u);
            appendBytes(memory, list.data, address, qwc * 16u);
            if (nativeEffects())
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
                    nativeEffects() ? &list.origins : nullptr,
                    bindingTextures());
    }
    reportFinished(rdram, ctx, runtime);
    if (vifwait::g_on)
        vifwait::g_listSent.store(vifwait::nowNs());
    g_thread->runList(std::move(list));
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

// M4: a draw environment's register writes (libgraph's: its stubs' through
// the register sink, sceGsPutDrawEnv's packet) taken without the converter:
// the state's writes go to the draw state in order, and each clear sprite
// (PRIM an untextured sprite, its RGBAQ, its two corners) becomes the
// producer's Clear of the frame under the state before it. False, nothing
// done: they hold something else (a textured sprite, UV or ST, a transfer).
std::atomic<uint64_t> g_envWritesTaken{0u}, g_envClears{0u}, g_envWritesLeft{0u};
bool takeEnvironment(const std::vector<std::pair<uint8_t, uint64_t>> &in)
{
    for (const auto &[reg, value] : in)
    {
        if ((reg == 0x00u && (value & 0x10u) != 0u) || reg == 0x02u || reg == 0x03u ||
            (reg >= 0x50u && reg <= 0x54u) || reg == rg::nativeproducer::kBindRegister)
        {
            ++g_envWritesLeft;
            return false;
        }
    }
    std::vector<std::pair<uint8_t, uint64_t>> pending;
    pending.reserve(in.size());
    bool sprite = false;
    uint8_t rgba[4] = {};
    int corners = 0;
    float xy[2][2] = {};
    uint32_t z = 0u;
    for (const auto &[reg, value] : in)
    {
        switch (reg)
        {
        case 0x00u: // PRIM
            sprite = (value & 7u) == 6u;
            corners = 0;
            pending.emplace_back(reg, value);
            break;
        case 0x01u: // RGBAQ
            for (int c = 0; c < 4; ++c)
                rgba[c] = static_cast<uint8_t>(value >> (8 * c));
            break;
        case 0x05u: // XYZ2
        case 0x0Du: // XYZ3
            if (!sprite)
                break; // (a vertex of no sprite: nothing the GS would fill here)
            xy[corners][0] = static_cast<float>(value & 0xFFFFu) / 16.0f;
            xy[corners][1] = static_cast<float>((value >> 16) & 0xFFFFu) / 16.0f;
            z = static_cast<uint32_t>(value >> 32);
            if (++corners == 2)
            {
                applyWrites(pending);
                rg::drawstate::game().observeWrites(pending.data(), pending.size());
                pending.clear();
                const rg::DrawState state = rg::drawstate::game().current();
                if (rg::nativeproducer::clear(state, xy[0][0], xy[0][1], xy[1][0], xy[1][1], rgba, z == 0u))
                    ++g_envClears;
                corners = 0;
            }
            break;
        default:
            pending.emplace_back(reg, value);
            break;
        }
    }
    applyWrites(pending);
    rg::drawstate::game().observeWrites(pending.data(), pending.size());
    if (++g_envWritesTaken == 1u)
        std::fprintf(stderr, "[native-env] libgraph's environments taken as state and clears (no converter)\n");
    return true;
}

// sceGsSwapDBuff's address (kGsWaiters): its draw environment taken from
// its arguments.
constexpr uint32_t kSceGsSwapDBuff = 0x00108F38u;

// The libgraph stubs: the lists in flight first, then the stub. The stubs
// keep what they do to the privileged registers (the display) and the
// game's memory; the draw state they set is taken from their arguments
// (sceGsPutDrawEnv's packet, below; sceGsSwapDBuff's buffer, here), and
// their own writes into the runtime's GS front end, which draws nothing,
// are not read (M6: no register sink, patch 0034).
template <size_t Index>
void gsWaiter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_thread->drain();
    if (g_gsWaiters[Index].first == kSceGsSwapDBuff)
    {
        // sceGsSwapDBuff(db, which): its sceGsDBuff's draw environment
        // `which` (eight register pairs at +96 or +336, after the two
        // display environments and a GIF tag; the buffer's clear is
        // sceGsSwapDBuffDc's, not this one's).
        const uint32_t at = (getRegU32(ctx, 4) + ((getRegU32(ctx, 5) & 1u) ? 336u : 96u)) & (PS2_RAM_SIZE - 1u);
        if (uint64_t(at) + 8u * 16u <= PS2_RAM_SIZE)
        {
            std::vector<std::pair<uint8_t, uint64_t>> pairs;
            for (uint32_t i = 0; i < 8u; ++i)
            {
                uint64_t q[2];
                std::memcpy(q, rdram + at + i * 16u, sizeof(q));
                pairs.emplace_back(static_cast<uint8_t>(q[1] & 0xFFu), q[0]);
            }
            if (!takeEnvironment(pairs))
            {
                const std::vector<uint8_t> packet = rg::gswrites::packet(pairs);
                sendPacket(packet.data(), static_cast<uint32_t>(packet.size()));
            }
        }
    }
    g_gsWaiters[Index].second(rdram, ctx, runtime);
}

template <size_t... I>
constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> gsWaiterHooks(std::index_sequence<I...>)
{
    return {&gsWaiter<I>...};
}

// M4: libgraph's sceGsPutDrawEnv(packet), the draw environment the game
// sets between frames and at a double buffer's swap: a GIF tag and its A+D
// writes (a sceGsDrawEnv1; with sceGsSetDefDrawEnv's clear, a sprite over
// the frame after them), which it sends by GIF DMA started from the
// registers. Taken here instead: the state's writes go to the draw state in
// order (behind the lists before them), the sprite becomes the producer's
// Clear of the frame; no DMA, no packet for the converter. A packet of
// another shape goes as the game sends it.
constexpr uint32_t kSceGsPutDrawEnv = 0x00108BB8u;
PS2Runtime::RecompiledFunction g_putDrawEnv = nullptr;
std::atomic<uint64_t> g_drawEnvsLeft{0u};
void putDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t at = getRegU32(ctx, 4) & (PS2_RAM_SIZE - 1u) & ~15u;
    uint64_t tag[2] = {};
    if (uint64_t(at) + 16u <= PS2_RAM_SIZE)
        std::memcpy(tag, rdram + at, sizeof(tag));
    const uint32_t nloop = static_cast<uint32_t>(tag[0] & 0x7FFFu);
    const uint32_t flg = static_cast<uint32_t>((tag[0] >> 58) & 3u);
    const uint32_t nreg = static_cast<uint32_t>((tag[0] >> 60) & 0xFu);
    const bool ad = flg == 0u && nreg == 1u && (tag[1] & 0xFu) == 0xEu;
    if (!ad || nloop == 0u || nloop > 64u || uint64_t(at) + 16u + nloop * 16u > PS2_RAM_SIZE)
    {
        // Not an A+D packet: read by its tags (as far as the stub reads,
        // a tag and eight quadwords), then the stub.
        ++g_drawEnvsLeft;
        g_thread->drain();
        if (uint64_t(at) + 9u * 16u <= PS2_RAM_SIZE)
            sendPacket(rdram + at, 9u * 16u);
        g_putDrawEnv(rdram, ctx, runtime);
        return;
    }
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    writes.reserve(nloop);
    for (uint32_t i = 0; i < nloop; ++i)
    {
        uint64_t q[2];
        std::memcpy(q, rdram + at + 16u + i * 16u, sizeof(q));
        writes.emplace_back(static_cast<uint8_t>(q[1] & 0xFFu), q[0]);
    }
    g_thread->drain();
    if (!takeEnvironment(writes))
    {
        // Writes of another shape (a textured sprite, a transfer): as a
        // packet of them, then the stub.
        ++g_drawEnvsLeft;
        sendPacket(rdram + at, 16u + nloop * 16u);
        g_putDrawEnv(rdram, ctx, runtime);
        return;
    }
    returnToCaller(ctx, 0);
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

namespace
{
// A texture split over DIRECT packets continues in place, without the copy
// behind a synthesized IMAGE tag.
void deliverImage(const uint8_t *data, uint32_t size)
{
    if (uploadsTaken())
    {
        // A host upload's data, gathered for the producer on its way.
        feedCapture(data, size);
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (!runtime.lookupFunction(kSceDmaSend) || !runtime.lookupFunction(kSceDmaSendN) ||
        !runtime.lookupFunction(kSceDmaSendI) || !runtime.lookupFunction(kSceDmaSync))
    {
        std::fprintf(stderr, "[vif1] libdma entry points missing; VIF1 stays on the game's thread\n");
        return;
    }
    g_thread = new Vif1Thread(runtime);
    g_runtime = &runtime;
    // Unpaced, a frame waiting on these lists gets no vsyncs past its own.
    runtime.eeScheduler().setHostWorkPending([] { return busy(); });
    g_vif1Memory = &runtime.memory();

    // GS packets from the VIF1 thread and from the game's own GIF transfers
    // reach the GS through PS2Memory. Its GifArbiter queues them unlocked and
    // every DMA start drains it, so it cannot be shared between threads;
    // packets go straight to the GS instead, whose processGIFPacket holds the
    // GS state lock. The arbiter only ever ordered packets queued together,
    // and the VIF1 paths already drained it after each one.
    PS2Memory &memory = runtime.memory();
    memory.setGifArbiter(nullptr);
    // From the VIF1 thread through the GIF stage. From any other thread (the
    // game's own GIF transfers, after the drain every DMA start does) straight
    // to the GS, behind whatever the stage still holds.
    memory.setGifPacketCallback([](const uint8_t *data, uint32_t size) {
        deliverPacket(data, size);
    });
    // A texture split over DIRECT packets continues in place, without the
    // copy behind a synthesized IMAGE tag.
    memory.setGifImageCallback([](const uint8_t *data, uint32_t size) {
        deliverImage(data, size);
    });
    // DMA the game starts through the registers rather than sceDmaSend.
    // VU1 started by the lists, without the guest thread context the
    // runtime's callbacks reach into (see Vif1Thread::runVu1).
    memory.setVu1MscalCallback([](uint32_t startPC, uint32_t top, uint32_t itop) {
        (void)itop;
        g_thread->runVu1(false, startPC, top);
    });
    memory.setVu1MscntCallback([](uint32_t top, uint32_t) { g_thread->runVu1(true, 0u, top); });
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
    hook(runtime, kSceGsPutDrawEnv, &putDrawEnv, g_putDrawEnv);
    constexpr size_t kWaiters = std::size(kGsWaiters);
    static constexpr auto hooks = gsWaiterHooks(std::make_index_sequence<kWaiters>{});
    g_gsWaiters.resize(kWaiters);
    for (size_t i = 0; i < kWaiters; ++i)
    {
        g_gsWaiters[i].first = kGsWaiters[i];
        hook(runtime, kGsWaiters[i], hooks[i], g_gsWaiters[i].second);
    }
    std::printf("[rg] VIF1 lists run on their own thread\n");
}

void shutdown()
{
    if (g_thread)
    {
        g_thread->stop();
    }
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
    char text[128];
    std::snprintf(text, sizeof(text), "lists busy=%.0f%% of wall", share(busy - lastBusy));
    lastTime = now;
    lastBusy = busy;
    lastGif = gif;
    return text;
}

bool busy()
{
    return g_thread && g_thread->busy();
}

} // namespace rg::vif1
