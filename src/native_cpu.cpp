#include "native_cpu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

// The runtime half of the native contract (tools/rgcpu/nc_contract.h).
// The cycle charges are the generated code's: 32 per back edge, 8 per call,
// so the scheduler's clock runs as it does for ps2_recomp's code.
// The inline checkpoints' call out: the charge is in the budget already.
[[gnu::noinline, gnu::cold]] void ncCheckpoint(PS2Runtime *runtime)
{
    static_assert(EeScheduler::kGeneratedCheckpointCycles == 32u && EeScheduler::kGuestDispatchCycles == 8u,
                  "NC_BACKEDGE and NC_CALLCHECK charge these (nc_contract.h)");
    runtime->nativeCheckpoint(0u);
}

void ncBoundaryCall(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t target, uint32_t ret)
{
    runtime->nativeBoundary(rdram, gctx, target, ret, true);
}

void ncBoundaryJump(uint8_t *rdram, R5900Context *gctx, PS2Runtime *runtime, uint32_t target, uint32_t ret)
{
    runtime->nativeBoundary(rdram, gctx, target, ret, false);
}

#if RG_HAVE_NATIVE_CPU
struct RgNativeEntry
{
    uint32_t address;
    PS2Runtime::RecompiledFunction oracle, native;
    bool hooked;
};
extern const RgNativeEntry g_rgNativeEntries[];
extern const uint32_t g_rgNativeEntryCount;
#endif

namespace rg::nativecpu
{
void install(PS2Runtime &runtime)
{
    if (!EeScheduler::s_nativeCpu)
    {
        return;
    }
#if RG_HAVE_NATIVE_CPU
    uint32_t swapped = 0, hooked = 0, kept = 0;
    // Bisecting a native/oracle difference in the game (the harness's
    // RG_ORACLE): RG_NATIVE_CPU_ONLY=LO-HI (hex) makes only functions starting
    // in [LO, HI) native; RG_NATIVE_CPU_ORACLE=FILE keeps the listed entry
    // addresses (hex, one per line) on the oracle. Native callers of a kept
    // slot see it is not theirs and call it as a boundary.
    uint32_t onlyLo = 0, onlyHi = 0xFFFFFFFFu;
    if (const char *only = std::getenv("RG_NATIVE_CPU_ONLY"))
    {
        char *end = nullptr;
        onlyLo = uint32_t(std::strtoul(only, &end, 16));
        if (end && *end == '-')
            onlyHi = uint32_t(std::strtoul(end + 1, nullptr, 16));
    }
    std::unordered_set<uint32_t> oracleKept;
    if (const char *path = std::getenv("RG_NATIVE_CPU_ORACLE"))
    {
        if (std::FILE *f = std::fopen(path, "r"))
        {
            char line[64];
            while (std::fgets(line, sizeof(line), f))
            {
                char *end = nullptr;
                const unsigned long a = std::strtoul(line, &end, 16);
                if (end != line)
                    oracleKept.insert(uint32_t(a));
            }
            std::fclose(f);
        }
        else
        {
            std::fprintf(stderr, "[rg] native CPU: cannot read RG_NATIVE_CPU_ORACLE=%s\n", path);
        }
    }
    // RG_NATIVE_CPU_HOOKS=FILE: the hooked slots, for native-hooks.txt.
    const char *dumpPath = std::getenv("RG_NATIVE_CPU_HOOKS");
    std::FILE *dump = dumpPath ? std::fopen(dumpPath, "w") : nullptr;
    for (uint32_t i = 0; i < g_rgNativeEntryCount; ++i)
    {
        const RgNativeEntry &e = g_rgNativeEntries[i];
        if (runtime.lookupFunction(e.address) != e.oracle)
        {
            // A hook is host code: it may read any argument register, so the
            // translator must call it as a boundary (config/SCUS_974.90/
            // native-hooks.txt). One it was not told about gets the native
            // caller's stale registers.
            if (!e.hooked)
                std::fprintf(stderr, "[rg] native CPU: 0x%08X is hooked but not in native-hooks.txt; "
                                     "its native callers pass only what the MIPS code reads\n", e.address);
            if (dump)
                std::fprintf(dump, "%08x\n", e.address);
            ++hooked;
            continue;
        }
        if (e.address < onlyLo || e.address >= onlyHi || oracleKept.count(e.address))
        {
            ++kept;
            continue;
        }
        runtime.replaceFunction(e.address, e.native);
        ++swapped;
    }
    if (dump)
        std::fclose(dump);
    std::fprintf(stderr, "[rg] native CPU: %u functions native, %u slots kept by hooks, %u kept on the oracle\n",
                 swapped, hooked, kept);
#else
    std::fprintf(stderr, "[rg] RG_NATIVE_CPU=1 but this build has no native CPU code "
                         "(tools/rgcpu output in build/generated/native)\n");
#endif
}
}
