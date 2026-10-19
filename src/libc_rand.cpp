#include "libc_rand.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scene_timing.h"

namespace rg::libc
{
namespace
{

constexpr uint32_t kRand = 0x00133710u;
// Rand loads the struct's address from 0x449858 (lw v0,-0x67a8(0x450000))
// and the seed from +0xA8 of it: newlib's _REENT->_new._reent._rand_next.
constexpr uint32_t kReentPointer = 0x00449858u;
constexpr uint32_t kSeedOffset = 0xA8u;
// seed = seed * 6364136223846793005 + 1; result = (seed >> 32) & 0x7FFFFFFF.
constexpr uint64_t kMultiplier = 0x5851F42D4C957F2Dull;
constexpr uint64_t kIncrement = 1u;

PS2Runtime::RecompiledFunction g_original = nullptr;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + physical, sizeof(T));
    return true;
}

void rand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    uint32_t reent = 0u;
    uint64_t seed = 0u;
    if (!readGuest(rdram, kReentPointer, reent) || reent == 0u || !readGuest(rdram, reent + kSeedOffset, seed))
    {
        g_original(rdram, ctx, runtime);
        return;
    }
    seed = seed * kMultiplier + kIncrement;
    std::memcpy(rdram + ((reent + kSeedOffset) & 0x1FFFFFFFu), &seed, sizeof(seed));
    setReturnS32(ctx, static_cast<int32_t>((seed >> 32) & 0x7FFFFFFFu));
    // A host function stands in for the guest's `jr ra` too. The scheduler
    // re-dispatches a thread parked at a safe point straight at its pc, and
    // with the pc left at this function's entry it would call it forever:
    // the game froze that way with v0 changing and nothing else.
    ctx->pc = getRegU32(ctx, 31);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_LIBC_RAND");
    if (enabled && *enabled == '0')
    {
        std::fprintf(stderr, "[rg] rand: left to the runtime's stub (RG_LIBC_RAND=0)\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kRand, &rand, g_original))
    {
        std::fprintf(stderr, "[rg] rand: the game's generator is not in place, random numbers stay at 0\n");
    }
}

} // namespace rg::libc
