#include "vif1_thread.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "ThreadNaming.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

namespace rg::vif1
{
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

// The bytes a VIF1 source-chain transfer hands the VIF, gathered the way
// PS2Memory::writeIORegister gathers them: the tag's upper half first when
// TTE is set, then its payload, following refe/cnt/next/ref/refs/call/ret/end
// and stopping at end, at an empty return stack or at a TIE interrupt.
std::vector<uint8_t> gatherChain(PS2Memory &memory, uint32_t tadr, bool tte, bool tie)
{
    std::vector<uint8_t> out;
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
        try
        {
            if (tte)
            {
                // With TTE every tag supplies two VIFcodes before its payload.
                appendBytes(memory, out, tagAddress + 8u, 8u);
            }
            if (qwc != 0u)
            {
                appendBytes(memory, out, data, qwc * 16u);
            }
        }
        catch (const std::exception &)
        {
            break;
        }
        if (end || (irq && tie))
        {
            break;
        }
    }
    return out;
}

class Vif1Thread
{
public:
    explicit Vif1Thread(PS2Runtime &runtime) : m_runtime(runtime)
    {
        m_thread = std::thread([this] { run(); });
        m_workerId = m_thread.get_id();
        m_thread.detach();
    }

    void submit(std::vector<uint8_t> list)
    {
        {
            std::lock_guard lock(m_mutex);
            m_lists.push_back(std::move(list));
            ++m_submitted;
        }
        m_work.notify_one();
    }

    // Waits until every list handed over has run. A no-op on the VIF1
    // thread itself, which never starts DMA but could reach a hook.
    void drain()
    {
        if (std::this_thread::get_id() == m_workerId)
        {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [&] { return m_completed == m_submitted; });
    }

    bool busy()
    {
        std::lock_guard lock(m_mutex);
        return m_completed != m_submitted;
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
    void run()
    {
        ThreadNaming::SetCurrentThreadName("VIF1");
        PS2Memory &memory = m_runtime.memory();
        for (;;)
        {
            std::vector<uint8_t> list;
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return !m_lists.empty(); });
                list = std::move(m_lists.front());
                m_lists.pop_front();
            }
            try
            {
                if (!list.empty())
                {
                    memory.processVIF1Data(list.data(), static_cast<uint32_t>(list.size()));
                }
            }
            catch (const std::exception &error)
            {
                std::fprintf(stderr, "[vif1] list failed: %s\n", error.what());
            }
            {
                std::lock_guard lock(m_mutex);
                ++m_completed;
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
    std::deque<std::vector<uint8_t>> m_lists;
    uint64_t m_submitted = 0u;
    uint64_t m_completed = 0u;
    uint64_t m_reported = 0u;
};

Vif1Thread *g_thread = nullptr;
PS2Runtime::RecompiledFunction g_dmaSend = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendN = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendI = nullptr;
PS2Runtime::RecompiledFunction g_dmaSync = nullptr;
std::vector<std::pair<uint32_t, PS2Runtime::RecompiledFunction>> g_gsWaiters;

uint32_t channelOf(R5900Context *ctx)
{
    return getRegU32(ctx, 4) & 0x1FFFFF00u;
}

// The DMAC completion the stub dispatches right after a transfer, for the
// lists that have finished since; on the game's thread, where handlers run.
void reportFinished(uint8_t *rdram, PS2Runtime *runtime)
{
    for (uint64_t n = g_thread->takeFinished(); n > 0u; --n)
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 1u);
    }
}

// sceDmaSend/SendI (chain) and SendN (normal) to VIF1 go to the thread;
// anything else to the GS path first waits for the lists in flight.
void send(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original,
          bool normal)
{
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
    std::vector<uint8_t> list;
    if (normal)
    {
        const uint32_t qwc = getRegU32(ctx, 6) & 0xFFFFu;
        try
        {
            appendBytes(memory, list, address, qwc * 16u);
        }
        catch (const std::exception &)
        {
            list.clear();
        }
    }
    else
    {
        list = gatherChain(memory, address, (chcr & kChcrTte) != 0u, (chcr & kChcrTie) != 0u);
    }
    reportFinished(rdram, runtime);
    g_thread->submit(std::move(list));
    setReturnS32(ctx, 0);
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
        setReturnS32(ctx, 1);
        return;
    }
    g_thread->drain();
    reportFinished(rdram, runtime);
    setReturnS32(ctx, 0);
}

// The libgraph stubs: the lists in flight first, then the stub.
template <size_t Index>
void gsWaiter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_thread->drain();
    g_gsWaiters[Index].second(rdram, ctx, runtime);
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
    if (!runtime.lookupFunction(kSceDmaSend) || !runtime.lookupFunction(kSceDmaSendN) ||
        !runtime.lookupFunction(kSceDmaSendI) || !runtime.lookupFunction(kSceDmaSync))
    {
        std::fprintf(stderr, "[vif1] libdma entry points missing; VIF1 stays on the game's thread\n");
        return;
    }
    g_thread = new Vif1Thread(runtime);

    // GS packets from the VIF1 thread and from the game's own GIF transfers
    // reach the GS through PS2Memory. Its GifArbiter queues them unlocked and
    // every DMA start drains it, so it cannot be shared between threads;
    // packets go straight to the GS instead, whose processGIFPacket holds the
    // GS state lock. The arbiter only ever ordered packets queued together,
    // and the VIF1 paths already drained it after each one.
    PS2Memory &memory = runtime.memory();
    memory.setGifArbiter(nullptr);
    memory.setGifPacketCallback([&gs = runtime.gs()](const uint8_t *data, uint32_t size) {
        gs.processGIFPacket(data, size);
    });
    // DMA the game starts through the registers rather than sceDmaSend.
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
    std::printf("[rg] VIF1 lists run on their own thread\n");
}

} // namespace rg::vif1
