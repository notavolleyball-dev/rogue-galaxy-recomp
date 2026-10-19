#include "vu0_flags.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scene_timing.h"

namespace rg::vu0
{
namespace
{

// Each takes vec4 pointers in a0..a3 (the game's boxes are stored as
// max, min), does `ctc2 zero, Status`, two `vsub.xyz vf25, A, B` and
// returns (Status & 0x80) == 0: 1 unless a component of either difference
// was negative.
constexpr uint32_t kAabbContains = 0x0013E090u;      // (a2 - a0, a1 - a3)
constexpr uint32_t kAabbOverlap = 0x0013E0E0u;       // (a0 - a3, a2 - a1)
constexpr uint32_t kAabbContainsPoint = 0x0013E130u; // (a1 - a0, a0 - a2)
// VU0 status register bits: Z, S current; ZS, SS sticky.
constexpr uint16_t kZero = 0x0001u;
constexpr uint16_t kSign = 0x0002u;
constexpr uint16_t kZeroSticky = 0x0040u;
constexpr uint16_t kSignSticky = 0x0080u;

PS2Runtime::RecompiledFunction g_contains = nullptr;
PS2Runtime::RecompiledFunction g_overlap = nullptr;
PS2Runtime::RecompiledFunction g_containsPoint = nullptr;

bool readVec4(const uint8_t *rdram, uint32_t address, float out[4])
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + 16u > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(out, rdram + physical, 16u);
    return true;
}

// The flags of one vsub.xyz: the VU sets the sign flag from the result's
// sign bit (a - a is +0, never negative) and the zero flag on an exact
// zero, for the fields the destination mask names.
uint16_t subtractFlags(const float a[4], const float b[4], float result[4])
{
    uint16_t flags = 0u;
    for (int i = 0; i < 3; ++i)
    {
        result[i] = a[i] - b[i];
        if (std::signbit(result[i]))
        {
            flags |= kSign | kSignSticky;
        }
        if (result[i] == 0.0f)
        {
            flags |= kZero | kZeroSticky;
        }
    }
    return flags;
}

void setVf(R5900Context *ctx, int reg, const float value[4])
{
    std::memcpy(&ctx->vu0_vf[reg], value, 16u);
}

// Runs the pattern: loads the four vectors into the registers the game
// used, the two subtractions into vf25 (xyz) with their flags, the result
// into v0, and returns to the caller.
void test(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original,
          const int loadRegs[4], const uint32_t loadArgs[4], int loads, int firstA, int firstB, int secondA,
          int secondB)
{
    float v[4][4];
    for (int i = 0; i < loads; ++i)
    {
        if (!readVec4(rdram, getRegU32(ctx, 4 + loadArgs[i]), v[i]))
        {
            original(rdram, ctx, runtime);
            return;
        }
        setVf(ctx, loadRegs[i], v[i]);
    }
    float result[4];
    std::memcpy(result, &ctx->vu0_vf[25], 16u);
    uint16_t status = subtractFlags(v[firstA], v[firstB], result);
    // The current flags are the last operation's; the sticky bits keep both.
    const uint16_t sticky = status & (kZeroSticky | kSignSticky);
    status = static_cast<uint16_t>(subtractFlags(v[secondA], v[secondB], result) | sticky);
    setVf(ctx, 25, result);
    ctx->vu0_status = status;
    setReturnS32(ctx, (status & kSignSticky) ? 0 : 1);
    ctx->pc = getRegU32(ctx, 31);
}

void aabbContains(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // vf10 = a0, vf11 = a1, vf1 = a2, vf2 = a3; vf1 - vf10, vf11 - vf2.
    static const int regs[4] = {10, 11, 1, 2};
    static const uint32_t args[4] = {0, 1, 2, 3};
    test(rdram, ctx, runtime, g_contains, regs, args, 4, 2, 0, 1, 3);
}

void aabbOverlap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // vf10 = a0, vf11 = a1, vf1 = a2, vf2 = a3; vf10 - vf2, vf1 - vf11.
    static const int regs[4] = {10, 11, 1, 2};
    static const uint32_t args[4] = {0, 1, 2, 3};
    test(rdram, ctx, runtime, g_overlap, regs, args, 4, 0, 3, 2, 1);
}

void aabbContainsPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // vf1 = a0, vf10 = a1, vf11 = a2; vf10 - vf1, vf1 - vf11.
    static const int regs[4] = {1, 10, 11, 0};
    static const uint32_t args[4] = {0, 1, 2, 0};
    test(rdram, ctx, runtime, g_containsPoint, regs, args, 3, 1, 0, 0, 2);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_VU0_FLAGS");
    if (enabled && *enabled == '0')
    {
        std::fprintf(stderr, "[rg] VU0 flags: the generated box tests stay (RG_VU0_FLAGS=0)\n");
        return;
    }
    using rg::timing::hookFunction;
    bool complete = hookFunction(runtime, kAabbContains, &aabbContains, g_contains);
    complete &= hookFunction(runtime, kAabbOverlap, &aabbOverlap, g_overlap);
    complete &= hookFunction(runtime, kAabbContainsPoint, &aabbContainsPoint, g_containsPoint);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] VU0 flags: a box test is not in the function table, collision stays coarse\n");
    }
}

} // namespace rg::vu0
