#pragma once

// EE FPU semantics that the runtime's macros get wrong, put right for the
// generated code. It reaches every generated translation unit through their
// precompiled header, after ps2_runtime_macros.h, whose include guard then
// keeps the generated files' own #include from undoing it.

#include <cstdint>
#include <cstring>

#include <ps2_runtime_macros.h>

// CVT.W.S on the R5900 truncates toward zero: its FPU has no rounding modes.
// Anything outside int32 saturates by sign; with no Inf or NaN on the EE,
// that covers every exponent from 2^31 up. The runtime rounds to nearest
// instead (nearbyintf, and the game thread runs in the default mode), which
// pushes every fraction of .5 or more to the next integer. Rogue Galaxy's
// animation clock (AnimKeyFindSpan, the cvt.w.s at 0x0015600C) then picks the
// next key with a negative blend that clamps to zero, so characters snap from
// key to key and animate at half their key rate.
static inline int32_t rgEeCvtWS(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & 0x7F800000u) < 0x4F000000u)
    {
        return static_cast<int32_t>(value);
    }
    return (bits & 0x80000000u) != 0u ? INT32_MIN : INT32_MAX;
}

#undef FPU_CVT_W_S
#define FPU_CVT_W_S(a) rgEeCvtWS((float)(a))
