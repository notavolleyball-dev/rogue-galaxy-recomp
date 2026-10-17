#include "movie_audio.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ps2_runtime.h"
#include "return_hook.h"

namespace rg::movie
{
namespace
{

constexpr uint32_t kPcmCallback = 0x001D4CA0u;    // AudioDecPcmCallback
constexpr uint32_t kSendToIop = 0x001D2E90u;      // AudioDecSendToIop
constexpr uint32_t kAudioDecCreate = 0x001D3210u; // AudioDecCreate
constexpr uint32_t kAudioDecReset = 0x001D3070u;  // AudioDecReset
constexpr uint32_t kAudioDec = 0x004B32D0u;       // the one AudioDec

// AudioDec fields (RGDecomp AudioDecBeginPut/EndPut). The stream's first
// 0x28 bytes, its "SShd"/"SSbd" header, go to a buffer inside the struct;
// everything after to the ring.
constexpr uint32_t kHeaderDone = 0x00u;
constexpr uint32_t kHeader = 0x04u;
constexpr uint32_t kHeaderSize = 0x28u;
constexpr uint32_t kHeaderFill = 0x2Cu;
constexpr uint32_t kRingBase = 0x30u;
constexpr uint32_t kWritePos = 0x34u;
constexpr uint32_t kBuffered = 0x38u;
constexpr uint32_t kRingSize = 0x3Cu;
constexpr uint32_t kTotal = 0x40u;

// The callback's data is the read ring, whose size is at +0x14008.
constexpr uint32_t kReadRingSize = 0x14008u;

// Bound on what the queue may hold (about 40 s of audio). Past it, whole
// 1 KB frames go from the front, which keeps the channels in step.
constexpr size_t kMaxQueued = 8u << 20;
// The queue only moves its head; once that much lies behind it the dead
// bytes are cut, or the vector would grow for as long as the ring stays full.
constexpr size_t kCompactAt = 1u << 20;

PS2Runtime::RecompiledFunction g_originalSend = nullptr;
PS2Runtime::RecompiledFunction g_originalCreate = nullptr;
PS2Runtime::RecompiledFunction g_originalReset = nullptr;

// Only the game's thread, which runs all guest code, touches it.
std::vector<uint8_t> g_queue;
size_t g_head = 0;

uint8_t *at(uint8_t *rdram, uint32_t address, uint32_t size)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    return physical + size <= PS2_RAM_SIZE ? rdram + physical : nullptr;
}

uint32_t load(uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    if (const uint8_t *p = at(rdram, address, 4u))
    {
        std::memcpy(&value, p, sizeof(value));
    }
    return value;
}

void store(uint8_t *rdram, uint32_t address, uint32_t value)
{
    if (uint8_t *p = at(rdram, address, 4u))
    {
        std::memcpy(p, &value, sizeof(value));
    }
}

void enqueue(uint8_t *rdram, uint32_t address, uint32_t size)
{
    if (const uint8_t *p = at(rdram, address, size))
    {
        g_queue.insert(g_queue.end(), p, p + size);
    }
}

void clearQueue()
{
    g_queue.clear();
    g_head = 0;
}

// What AudioDecBeginPut offers, filled, then AudioDecEndPut's accounting.
void topUp(uint8_t *rdram)
{
    const size_t queued = g_queue.size() - g_head;
    if (queued == 0u)
    {
        return;
    }
    const uint32_t ringBase = load(rdram, kAudioDec + kRingBase);
    const uint32_t ringSize = load(rdram, kAudioDec + kRingSize);
    if (ringSize == 0u)
    {
        return;
    }
    uint32_t p1, n1, p2, n2;
    if (load(rdram, kAudioDec + kHeaderDone) == 0u)
    {
        const uint32_t fill = load(rdram, kAudioDec + kHeaderFill);
        p1 = kAudioDec + kHeader + fill;
        n1 = kHeaderSize - std::min(fill, kHeaderSize);
        p2 = ringBase;
        n2 = ringSize;
    }
    else
    {
        const uint32_t writePos = load(rdram, kAudioDec + kWritePos);
        const uint32_t free = ringSize - std::min(load(rdram, kAudioDec + kBuffered), ringSize);
        p1 = ringBase + writePos;
        n1 = std::min(free, ringSize - writePos);
        p2 = ringBase;
        n2 = free - n1;
    }
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(queued, size_t{n1} + n2));
    const uint32_t first = std::min(count, n1);
    if (uint8_t *dst = at(rdram, p1, first))
    {
        std::memcpy(dst, g_queue.data() + g_head, first);
    }
    if (uint8_t *dst = at(rdram, p2, count - first))
    {
        std::memcpy(dst, g_queue.data() + g_head + first, count - first);
    }
    g_head += count;
    if (g_head == g_queue.size())
    {
        clearQueue();
    }
    else if (g_head >= kCompactAt)
    {
        g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(g_head));
        g_head = 0;
    }

    uint32_t rest = count;
    if (load(rdram, kAudioDec + kHeaderDone) == 0u)
    {
        const uint32_t fill = load(rdram, kAudioDec + kHeaderFill);
        const uint32_t take = std::min(kHeaderSize - std::min(fill, kHeaderSize), rest);
        store(rdram, kAudioDec + kHeaderFill, fill + take);
        if (fill + take >= kHeaderSize)
        {
            store(rdram, kAudioDec + kHeaderDone, 1u);
        }
        rest -= take;
    }
    store(rdram, kAudioDec + kWritePos, (load(rdram, kAudioDec + kWritePos) + rest) % ringSize);
    store(rdram, kAudioDec + kBuffered, load(rdram, kAudioDec + kBuffered) + rest);
    store(rdram, kAudioDec + kTotal, load(rdram, kAudioDec + kTotal) + rest);
}

// In place of AudioDecPcmCallback(mpeg, cbstr, readRing): the packet's data
// after its four-byte sub-stream header, which may wrap around the read ring.
void pcmCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t cbstr = getRegU32(ctx, 5);
    const uint32_t ring = getRegU32(ctx, 6);
    const uint32_t data = load(rdram, cbstr + 8u);
    const uint32_t length = load(rdram, cbstr + 0xCu);
    const uint32_t ringEnd = ring + load(rdram, ring + kReadRingSize);
    if (length > 4u && ringEnd > ring)
    {
        uint32_t source = data + 4u;
        if (source >= ringEnd)
        {
            source -= ringEnd - ring;
        }
        const uint32_t size = length - 4u;
        const uint32_t first = std::min(size, ringEnd - source);
        enqueue(rdram, source, first);
        enqueue(rdram, ring, size - first);
    }
    if (g_queue.size() - g_head > kMaxQueued)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::fprintf(stderr, "[rg] movie audio queue over %zu bytes; dropping whole frames\n", kMaxQueued);
        }
        g_head += (g_queue.size() - g_head - kMaxQueued + 0x3FFu) & ~size_t{0x3FFu};
    }
    topUp(rdram);
    setReturnS32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}

void afterSend(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    topUp(rdram);
}

void sendToIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_originalSend, rdram, ctx, runtime, &afterSend, 0u);
}

void audioDecCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    clearQueue();
    g_originalCreate(rdram, ctx, runtime);
}

void audioDecReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    clearQueue();
    g_originalReset(rdram, ctx, runtime);
}

bool replace(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction hook,
             PS2Runtime::RecompiledFunction *original)
{
    PS2Runtime::RecompiledFunction found = runtime.lookupFunction(address);
    if (!found || !runtime.replaceFunction(address, hook))
    {
        std::fprintf(stderr, "[rg] 0x%08X is not in the function table; movie audio keeps the game's callback\n",
                     address);
        return false;
    }
    if (original)
    {
        *original = found;
    }
    return true;
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *mode = std::getenv("RG_MOVIE_AUDIO");
    if (mode && std::strcmp(mode, "game") == 0)
    {
        return;
    }
    // The queue is only safe with all four in place.
    if (!runtime.lookupFunction(kPcmCallback) || !runtime.lookupFunction(kSendToIop) ||
        !runtime.lookupFunction(kAudioDecCreate) || !runtime.lookupFunction(kAudioDecReset))
    {
        std::fprintf(stderr, "[rg] movie audio functions not in the table; the game's callback stays\n");
        return;
    }
    replace(runtime, kSendToIop, &sendToIop, &g_originalSend);
    replace(runtime, kAudioDecCreate, &audioDecCreate, &g_originalCreate);
    replace(runtime, kAudioDecReset, &audioDecReset, &g_originalReset);
    replace(runtime, kPcmCallback, &pcmCallback, nullptr);
}

} // namespace rg::movie
