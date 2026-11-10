// The native CPU's contract (docs/NATIVE-CPU.md): what tools/rgcpu's
// translated functions are compiled against.
//
// PS2Recomp's output keeps every guest register in R5900Context and hands
// that context to every runtime call, so the C++ compiler must store and
// reload registers around each memory slow path, call and checkpoint. Here:
//
// - guest GPRs are plain uint64_t locals (nc_rN, upper halves in nc_hN);
// - ctx points at a small local struct (pc, FPU registers, fcr31, hi/lo),
//   which never escapes, so the compiler promotes its fields to registers;
// - the memory slow path and checkpoints get the guest context (gctx), not
//   the locals, so they clobber nothing the function holds;
// - checkpoints never unwind: the guest thread's fiber yields in place
//   (patches 0036, 0037); the budget is read inline (0039);
// - calls out of native code go through ncBoundaryCall / ncBoundaryJump.
#pragma once

#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/ee_native_budget.h"

struct NcCtx
{
    uint32_t pc;
    bool in_delay_slot;
    uint32_t branch_pc;
    float f[32];
    uint32_t fcr31;
    uint64_t hi, lo, hi1, lo1;
    uint32_t sa;
    float f_acc;
};

// add/addi/sub overflow: the guest context gets the exception (stale
// registers; the game raises none).
[[gnu::cold, gnu::noinline]] inline void ncOverflow(R5900Context *gctx, PS2Runtime *runtime)
{
    runtime->SignalException(gctx, EXCEPTION_INTEGER_OVERFLOW);
}

#undef GPR_U32
#undef GPR_S32
#undef GPR_U64
#undef GPR_S64
#undef GPR_VEC
#undef SET_GPR_U32
#undef SET_GPR_S32
#undef SET_GPR_U64
#undef SET_GPR_S64
#undef SET_GPR_VEC

// The register index is always a literal in the generated code, so it pastes
// into a local's name. nc_r0 is a local that stays 0: the guarded writes to
// it are dead code the compiler drops.
#define NC_R(n) nc_r##n
#define NC_H(n) nc_h##n
#define GPR_U32(c, n) ((uint32_t)NC_R(n))
#define GPR_S32(c, n) ((int32_t)NC_R(n))
#define GPR_U64(c, n) ((uint64_t)NC_R(n))
#define GPR_S64(c, n) ((int64_t)NC_R(n))
#define GPR_VEC(c, n) _mm_set_epi64x((long long)NC_H(n), (long long)NC_R(n))
#define SET_GPR_U32(c, n, v) do { if ((n) != 0) NC_R(n) = (uint64_t)(int64_t)(int32_t)(v); } while (0)
#define SET_GPR_S32(c, n, v) SET_GPR_U32(c, n, v)
#define SET_GPR_U64(c, n, v) do { if ((n) != 0) NC_R(n) = (uint64_t)(v); } while (0)
#define SET_GPR_S64(c, n, v) SET_GPR_U64(c, n, v)
#define SET_GPR_VEC(c, n, v)                                                \
    do                                                                      \
    {                                                                       \
        if ((n) != 0)                                                       \
        {                                                                   \
            const __m128i _ncv = (v);                                       \
            NC_R(n) = (uint64_t)_mm_cvtsi128_si64(_ncv);                    \
            NC_H(n) = (uint64_t)_mm_extract_epi64(_ncv, 1);                 \
        }                                                                   \
    } while (0)

// Memory: the plain-RAM compare stays inline, and so does the uncached RAM
// mirror (0x20000000-0x3FFFFFFF, the same RAM at addr & PS2_RAM_MASK): the
// game builds its texture upload packets through 0x30xxxxxx
// (GsBuildImageUploadRefPkt, ~50,000 stores a frame in the field, each a call
// with every caller-saved register spilled around it). An access that would
// wrap past the end of RAM, and everything else (kseg0/1, scratchpad,
// hardware: a handful a frame), is one cold, out-of-line call that sees the
// guest context but not the locals.
#define NC_RAM_MIRROR(addr) __builtin_expect(((addr) - 0x20000000u) < 0x20000000u, 1)
template <typename T>
[[gnu::cold, gnu::noinline]] static T ncSlowRead(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t a)
{
    R5900Context *const ctx = gctx;
    if constexpr (sizeof(T) == 1) return PS2Runtime::isSpecialAddress(a) ? runtime->Load8(rdram, ctx, a) : FAST_READ8(a);
    else if constexpr (sizeof(T) == 2) return PS2Runtime::isSpecialAddress(a) ? runtime->Load16(rdram, ctx, a) : FAST_READ16(a);
    else if constexpr (sizeof(T) == 4) return PS2Runtime::isSpecialAddress(a) ? runtime->Load32(rdram, ctx, a) : FAST_READ32(a);
    else if constexpr (sizeof(T) == 8) return PS2Runtime::isSpecialAddress(a) ? runtime->Load64(rdram, ctx, a) : FAST_READ64(a);
    else return PS2Runtime::isSpecialAddress(a) ? runtime->Load128(rdram, ctx, a) : FAST_READ128(a);
}
template <typename T>
[[gnu::cold, gnu::noinline]] static void ncSlowWrite(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t a, T v)
{
    R5900Context *const ctx = gctx;
    if constexpr (sizeof(T) == 1) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store8(rdram, ctx, a, v); else FAST_WRITE8(a, v); }
    else if constexpr (sizeof(T) == 2) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store16(rdram, ctx, a, v); else FAST_WRITE16(a, v); }
    else if constexpr (sizeof(T) == 4) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store32(rdram, ctx, a, v); else FAST_WRITE32(a, v); }
    else if constexpr (sizeof(T) == 8) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store64(rdram, ctx, a, v); else FAST_WRITE64(a, v); }
    else { if (PS2Runtime::isSpecialAddress(a)) runtime->Store128(rdram, ctx, a, v); else FAST_WRITE128(a, v); }
}
template <typename T>
static inline T ncRead(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t a)
{
    if (PS2_PLAIN_RAM(a, sizeof(T))) return Ps2PlainRead<T>(rdram, a);
    if (NC_RAM_MIRROR(a) && PS2_PLAIN_RAM(a & PS2_RAM_MASK, sizeof(T))) return Ps2PlainRead<T>(rdram, a & PS2_RAM_MASK);
    return ncSlowRead<T>(rdram, gctx, runtime, a);
}
template <typename T>
static inline void ncWrite(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t a, T v)
{
    if (PS2_PLAIN_RAM(a, sizeof(T))) Ps2PlainWrite<T>(rdram, a, v);
    else if (NC_RAM_MIRROR(a) && PS2_PLAIN_RAM(a & PS2_RAM_MASK, sizeof(T))) Ps2PlainWrite<T>(rdram, a & PS2_RAM_MASK, v);
    else ncSlowWrite<T>(rdram, gctx, runtime, a, v);
}

#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128
#define READ8(addr) ncRead<uint8_t>(rdram, gctx, runtime, (uint32_t)(addr))
#define READ16(addr) ncRead<uint16_t>(rdram, gctx, runtime, (uint32_t)(addr))
#define READ32(addr) ncRead<uint32_t>(rdram, gctx, runtime, (uint32_t)(addr))
#define READ64(addr) ncRead<uint64_t>(rdram, gctx, runtime, (uint32_t)(addr))
#define READ128(addr) ncRead<__m128i>(rdram, gctx, runtime, (uint32_t)(addr))
#define WRITE8(addr, val) ncWrite<uint8_t>(rdram, gctx, runtime, (uint32_t)(addr), (uint8_t)(val))
#define WRITE16(addr, val) ncWrite<uint16_t>(rdram, gctx, runtime, (uint32_t)(addr), (uint16_t)(val))
#define WRITE32(addr, val) ncWrite<uint32_t>(rdram, gctx, runtime, (uint32_t)(addr), (uint32_t)(val))
#define WRITE64(addr, val) ncWrite<uint64_t>(rdram, gctx, runtime, (uint32_t)(addr), (uint64_t)(val))
#define WRITE128(addr, val) ncWrite<__m128i>(rdram, gctx, runtime, (uint32_t)(addr), (__m128i)(val))

// The function's own stack frame, as a local byte array: constant indexes,
// never escaping, so the compiler keeps the slots in registers or drops them.
template <typename T>
static inline T ncFrameRead(const uint8_t *frame, int at)
{
    T v;
    std::memcpy(&v, frame + at, sizeof v);
    return v;
}
template <typename T>
static inline void ncFrameWrite(uint8_t *frame, int at, T v)
{
    std::memcpy(frame + at, &v, sizeof v);
}

// A back edge's checkpoint, and every guest call's (charged as
// directGuestCall charges): the charge comes off the scheduler's budget
// inline (runtime/ee_native_budget.h), and only when it runs out, or the
// scheduler has work pending, does the call out ask it, which may yield the
// thread there (in place: nothing unwinds, so no guest state has to be in
// gctx). Out of line and cold, it clobbers no locals on the fast path.
[[gnu::noinline, gnu::cold]] void ncCheckpoint(PS2Runtime *runtime);
#define NC_CHECK(charge)                                                                        \
    do                                                                                          \
    {                                                                                           \
        if (__builtin_expect((g_eeNativeBudget -= (charge)) <= 0 ||                             \
                                 g_eeCheckpointPending.load(std::memory_order_relaxed),          \
                             0))                                                                \
            ncCheckpoint(runtime);                                                              \
    } while (0)
#define NC_BACKEDGE() NC_CHECK(32) // EeScheduler::kGeneratedCheckpointCycles
#define NC_CALLCHECK() NC_CHECK(8) // EeScheduler::kGuestDispatchCycles

// A call into the guest context's world (a function not on the native
// contract, a host stub, a jalr): the caller has stored its registers into
// gctx and set ra; this runs the guest from `target` until it returns to
// `ret` (tail jumps included) and the caller reloads.
void ncBoundaryCall(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t target, uint32_t ret);
// The same for a jump out of the function (a tail jump, jr through a
// register): its target may be any address code can go on from, not only a
// function's entry.
void ncBoundaryJump(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t target, uint32_t ret);
