// Differential test of tools/rgcpu's output against ps2_recomp's, whole call
// trees at a time.
//
//   rg_harness LIB.so NAMES.txt SEEDS [bench ITERS]
//
// LIB.so holds every ps2_recomp function of the image (the oracle) and every
// native one (NAME_nc, which calls its native callees directly). For each
// NAME: the same random guest state goes through the oracle -- its calls to
// other oracle functions are real, dispatched like the scheduler would --
// and through NAME_nc. Syscalls, host stubs and anything missing are one
// fake shared by both sides (results from the arguments and a call count).
// Compared afterwards: what a caller may read under the MIPS ABI (v0, v1,
// f0, f1, the callee-saved registers and their upper halves, pc), every
// slow-path access and boundary call in order, and all of RAM.
#include <dlfcn.h>
#include <x86intrin.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "ee_fpu.h"
#include "ps2_runtime.h"

const bool rgEeCvtRoundToNearest = false;
PS2Runtime::RecompiledFunction g_ps2DirectCallTable[1 << 18];

struct RgFunc
{
    uint32_t start, end;
    PS2Runtime::RecompiledFunction fn;
};

static std::vector<std::string> g_log;
static bool g_logOn = true;
static uint64_t g_calls;
static int64_t g_budget;
struct TooLong {};
static bool g_tooLong;
static const RgFunc *g_funcs;
static int g_funcCount;
constexpr uint32_t kRet = 0x00123450u;
constexpr uint32_t kStackTop = 0x01100000u;
constexpr uint32_t kStackBytes = 0x40000u;

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}
static uint64_t g_rng;
static uint64_t rnd() { return g_rng = mix(g_rng + 0x9E3779B97F4A7C15ULL); }

static void note(const std::string &s) { if (g_logOn) g_log.push_back(s); }
static uint32_t reg32(const R5900Context *c, int r) { return (uint32_t)_mm_cvtsi128_si64(c->r[r]); }

// RG_TRACE: every call through a register (jalr) and every native boundary
// call goes in the log with the registers it is handed.
static void traceCall(const R5900Context *c, uint32_t target)
{
    static const bool on = std::getenv("RG_TRACE") != nullptr;
    if (!on) return;
    char buf[200];
    std::snprintf(buf, sizeof buf, "jalr %x a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x t2=%x t3=%x v0=%x v1=%x sp=%x", target,
                  reg32(c, 4), reg32(c, 5), reg32(c, 6), reg32(c, 7), reg32(c, 8), reg32(c, 9), reg32(c, 10),
                  reg32(c, 11), reg32(c, 2), reg32(c, 3), reg32(c, 29));
    note(buf);
}

// The work budget: every checkpoint and call spends one. A random state can
// loop forever; past the budget both sides give up and the seed is left out.
static bool spend()
{
    if (--g_budget < 0) { g_tooLong = true; return true; }
    return false;
}

static PS2Runtime::RecompiledFunction lookup(uint32_t pc)
{
    int lo = 0, hi = g_funcCount;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (g_funcs[mid].start <= pc) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return nullptr;
    const RgFunc &f = g_funcs[lo - 1];
    return pc < f.end ? f.fn : nullptr;
}

// A call's target: only a function's entry. A garbage vtable slot can point
// into the middle of one (0x3535b6), a path the game never takes; both sides
// fake it the same way instead.
static PS2Runtime::RecompiledFunction lookupEntry(uint32_t pc)
{
    PS2Runtime::RecompiledFunction fn = lookup(pc);
    if (!fn) return nullptr;
    int lo = 0, hi = g_funcCount;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (g_funcs[mid].start <= pc) lo = mid + 1; else hi = mid;
    }
    return g_funcs[lo - 1].start == pc ? fn : nullptr;
}

// What a syscall, a host stub or a missing function does here: log the
// arguments, return results made from them.
static uint8_t *g_rdram;
static bool g_unsure;
static const uint32_t (*g_stubOut)[2];
static int g_stubOutCount;

// Bytes a host stub writes through a stack pointer argument (check_trees.sh,
// by name); 0 for anything that is not a known stub.
static uint32_t stubOut(uint32_t target)
{
    for (int i = 0; i < g_stubOutCount; ++i)
        if (g_stubOut[i][0] == target) return g_stubOut[i][1];
    return 0;
}

static void fakeCall(R5900Context *ctx, const char *what, uint32_t target)
{
    ++g_calls;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s %x a0=%x a1=%x a2=%x a3=%x sp=%x", what, target, reg32(ctx, 4), reg32(ctx, 5),
                  reg32(ctx, 6), reg32(ctx, 7), reg32(ctx, 29));
    note(buf);
    const uint64_t h = mix(g_calls * 77 + reg32(ctx, 4) * 3 + reg32(ctx, 5));
    ctx->r[2] = _mm_set_epi64x(0, (long long)(int64_t)(int32_t)(h & 0xFF));
    ctx->r[3] = _mm_set_epi64x(0, (long long)(int64_t)(int32_t)((h >> 8) & 0xFF));
    // A float result too, as a real callee would give: f0/f1 are dead across
    // a call, so the native side need not keep what was there before.
    ctx->f[0] = (float)(int32_t)((h >> 16) & 0xFFFF) / 7.0f;
    ctx->f[1] = (float)(int32_t)((h >> 32) & 0xFFFF) / 7.0f;
    // A pointer into the stack: likely an out-parameter (RemPio2f's y[2],
    // sceVu0TransposeMatrix's dst) the fake does not write, so the caller
    // reads whatever its side's frames left in guest memory there, and the
    // two sides' frames differ by design. Write 8 bytes (RemPio2f's y[2]
    // exactly) and mark the run: a mismatch in it is reported as UNSURE.
    for (int r = 4; r < 8; ++r)
    {
        const uint32_t p = reg32(ctx, r);
        if (p >= kStackTop - kStackBytes && p < kStackTop)
        {
            g_unsure = true;
            const uint32_t n = std::min<uint32_t>(stubOut(target), 8);
            if (n && p + n <= kStackTop && (p & 3) == 0)
                for (uint32_t k = 0; k < n; k += 4)
                {
                    const float v = (float)(int32_t)(mix(h + k * 131 + r) & 0xFFFF) / 5.0f;
                    std::memcpy(g_rdram + p + k, &v, 4);
                }
        }
    }
}

// Runs guest code from ctx->pc until it comes back to `ret` with sp where it
// was: the scheduler's dispatch loop, in miniature.
static void dispatchUntil(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t ret)
{
    const uint32_t sp = reg32(ctx, 29);
    for (int guard = 0; !(ctx->pc == ret && reg32(ctx, 29) == sp); ++guard)
    {
        if (g_tooLong || spend()) return;
        PS2Runtime::RecompiledFunction fn = lookup(ctx->pc);
        if (!fn)
        {
            fakeCall(ctx, "unknown", ctx->pc);
            ctx->pc = reg32(ctx, 31);
            continue;
        }
        fn(rdram, ctx, rt);
    }
}

// A host stub an oracle function calls directly: what the boundary does
// with an address it has no function for.
void rgHarnessStub(R5900Context *ctx, uint32_t addr)
{
    fakeCall(ctx, "unknown", addr);
    ctx->pc = reg32(ctx, 31);
}

bool PS2Runtime::eeCheckpointDue(uint32_t) noexcept { return spend(); }
void ncCheckpoint(PS2Runtime *) { if (spend()) throw TooLong{}; }

bool PS2Runtime::directGuestCall(uint8_t *rdram, R5900Context *ctx, RecompiledFunction fn, uint32_t target, uint32_t,
                                 uint32_t fall)
{
    if (spend()) return false;
    { static const bool all = std::getenv("RG_TRACE") && std::strcmp(std::getenv("RG_TRACE"), "2") == 0;
      if (all) traceCall(ctx, target); }
    ctx->pc = target;
    if (!fn || !lookup(target))
    {
        fakeCall(ctx, "unknown", target);
        ctx->pc = fall;
        return true;
    }
    fn(rdram, ctx, this);
    if (g_tooLong) return false;
    // Not back at the fallthrough: something below jumped out (a tail
    // jump); finish it here, as the scheduler would after the unwind.
    if (ctx->pc != fall) dispatchUntil(rdram, ctx, this, fall);
    return !g_tooLong;
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram, R5900Context *ctx, uint32_t target, uint32_t, uint32_t fall,
                                     GuestBranchKind kind, const char *)
{
    if (spend()) return false;
    ctx->pc = target;
    if (kind == GuestBranchKind::IndirectCall && lookupEntry(target)) traceCall(ctx, target);
    if (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall)
    {
        if (!lookupEntry(target))
        {
            fakeCall(ctx, "unknown", target);
            ctx->pc = fall;
            return true;
        }
        dispatchUntil(rdram, ctx, this, fall);
        return !g_tooLong;
    }
    return false; // a jump: unwind, the caller's loop goes on from pc
}

void ncBoundaryCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t target, uint32_t ret)
{
    if (lookupEntry(target)) traceCall(ctx, target);
    ctx->pc = target;
    if (!lookupEntry(target))
    {
        fakeCall(ctx, "unknown", target);
        ctx->pc = ret;
        return;
    }
    dispatchUntil(rdram, ctx, rt, ret);
    if (g_tooLong) throw TooLong{};
}

void ncBoundaryJump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t target, uint32_t ret)
{
    ctx->pc = target;
    if (!lookup(target))
    {
        fakeCall(ctx, "unknown", target);
        ctx->pc = ret;
        return;
    }
    dispatchUntil(rdram, ctx, rt, ret);
    if (g_tooLong) throw TooLong{};
}

void PS2Runtime::handleSyscall(uint8_t *, R5900Context *ctx, uint32_t id) { fakeCall(ctx, "syscall", id); }
void PS2Runtime::handleSyscall(uint8_t *, R5900Context *ctx) { fakeCall(ctx, "syscall", 0); }
void PS2Runtime::handleBreak(uint8_t *, R5900Context *ctx) { note("break " + std::to_string(ctx->pc)); }
void PS2Runtime::clearLLBit(R5900Context *) {}
void PS2Runtime::SignalException(R5900Context *, PS2Exception e) { note("exc " + std::to_string((int)e)); }
void PS2Runtime::executeVU0Microprogram(uint8_t *, R5900Context *, uint32_t a) { note("vu0 " + std::to_string(a)); }

static uint64_t loadVal(uint32_t a) { return mix(a * 31ULL + 7); }
#define LOAD(bits, T)                                                                 \
    T PS2Runtime::Load##bits(uint8_t *, R5900Context *, uint32_t a)                   \
    {                                                                                 \
        note("load" #bits " " + std::to_string(a));                                   \
        return (T)loadVal(a);                                                         \
    }
LOAD(8, uint8_t) LOAD(16, uint16_t) LOAD(32, uint32_t) LOAD(64, uint64_t)
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t a)
{
    note("load128 " + std::to_string(a));
    return _mm_set_epi64x((long long)loadVal(a + 1), (long long)loadVal(a));
}
#define STORE(bits, T)                                                                \
    void PS2Runtime::Store##bits(uint8_t *, R5900Context *, uint32_t a, T v)          \
    {                                                                                 \
        note("store" #bits " " + std::to_string(a) + " " + std::to_string((uint64_t)v)); \
    }
STORE(8, uint8_t) STORE(16, uint16_t) STORE(32, uint32_t) STORE(64, uint64_t)
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t a, __m128i v)
{
    note("store128 " + std::to_string(a) + " " + std::to_string(_mm_cvtsi128_si64(v)));
}

using Fn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

static uint8_t *g_clean;
alignas(64) static uint8_t g_fakeRuntime[sizeof(PS2Runtime)];
static PS2Runtime *runtimePtr() { return reinterpret_cast<PS2Runtime *>(g_fakeRuntime); }
constexpr uint32_t kWindow = 0x00100000u;
constexpr uint32_t kWindowBytes = 0x10000u;
// Data pointers land above the code, in a window of their own.
constexpr uint32_t kData = 0x01000000u;
static uint32_t dataPtr(uint64_t v) { return kData + (uint32_t)((v >> 8) % kWindowBytes) & ~15u; }

static void seedState(R5900Context &ctx, uint64_t seed, uint32_t entry)
{
    g_rng = seed;
    std::memset(&ctx, 0, sizeof ctx);
    for (int i = 1; i < 32; ++i)
    {
        uint64_t lo = rnd(), hi = rnd();
        const uint64_t kind = rnd() % 4;
        if (kind == 0) lo = dataPtr(lo);
        else if (kind == 1) lo = (uint64_t)(int64_t)(int32_t)(lo % 64);
        else if (kind == 2) lo = (uint64_t)(int64_t)(int32_t)lo;
        ctx.r[i] = _mm_set_epi64x((long long)hi, (long long)lo);
    }
    ctx.r[29] = _mm_set_epi64x(0, kStackTop);
    ctx.r[31] = _mm_set_epi64x(0, kRet);
    for (int i = 0; i < 32; ++i) ctx.f[i] = (float)(int32_t)(rnd() & 0xFFFF) / 7.0f;
    ctx.fcr31 = (uint32_t)rnd() & 0x00800000u;
    ctx.hi = rnd();
    ctx.lo = rnd();
    ctx.pc = entry;
}

// The stack below the entry sp is the native side's own business: its frames
// live in locals, the guest stack memory there is not part of the contract.



// RG_VERBOSE: the 8-byte words a run changed, for the diff.
static std::vector<std::pair<uint32_t, uint64_t>> *g_dirty;

static uint64_t memHashAndRestore()
{
    uint64_t h = 0;
    for (size_t p = 0; p < PS2_RAM_SIZE; p += 4096)
    {
        if (p >= kStackTop - kStackBytes && p < kStackTop)
        {
            std::memcpy(g_rdram + p, g_clean + p, 4096);
            continue;
        }
        if (std::memcmp(g_rdram + p, g_clean + p, 4096) == 0) continue;
        for (size_t i = p; i < p + 4096; i += 8)
        {
            uint64_t v;
            std::memcpy(&v, g_rdram + i, 8);
            h = mix(h ^ v ^ i);
            if (g_dirty && std::memcmp(g_rdram + i, g_clean + i, 8) != 0) g_dirty->push_back({(uint32_t)i, v});
        }
        std::memcpy(g_rdram + p, g_clean + p, 4096);
    }
    return h;
}

struct Result
{
    std::vector<std::string> log;
    uint64_t mem = 0;
    bool unsure = false;
    std::vector<std::pair<uint32_t, uint64_t>> dirty;
    R5900Context ctx;
    bool tooLong = false;
};

constexpr int64_t kBudget = 20000;

using FillFn = void (*)(PS2Runtime::RecompiledFunction *);
static FillFn g_fillOld, g_fillNew;
static FillFn g_filled;
// The old run calls through the oracle's functions, the new one through the
// NAME_nc entries, as the game does under RG_NATIVE_CPU.
// RG_ORACLE=FILE: functions whose slots keep the oracle in the new run too,
// to find which native function a tree's mismatch comes from.
static std::vector<std::pair<PS2Runtime::RecompiledFunction, PS2Runtime::RecompiledFunction>> g_keepOracle;

static void useTable(FillFn f)
{
    if (g_filled == f) return;
    f(g_ps2DirectCallTable);
    g_filled = f;
    if (f == g_fillNew)
        for (size_t i = 0; i < sizeof g_ps2DirectCallTable / sizeof g_ps2DirectCallTable[0]; ++i)
            for (const auto &[nc, oracle] : g_keepOracle)
                if (g_ps2DirectCallTable[i] == nc) g_ps2DirectCallTable[i] = oracle;
}

static Result runOld(uint32_t entry, uint64_t seed)
{
    Result r;
    useTable(g_fillOld);
    seedState(r.ctx, seed, entry);
    g_log.clear();
    g_calls = 0;
    g_budget = kBudget;
    g_tooLong = false;
    g_unsure = false;
    dispatchUntil(g_rdram, &r.ctx, runtimePtr(), kRet);
    r.unsure = g_unsure;
    r.tooLong = g_tooLong;
    r.log = g_log;
    g_dirty = std::getenv("RG_VERBOSE") ? &r.dirty : nullptr;
    r.mem = memHashAndRestore();
    g_dirty = nullptr;
    return r;
}

static Result runNew(Fn fn, uint32_t entry, uint64_t seed)
{
    Result r;
    useTable(g_fillNew);
    seedState(r.ctx, seed, entry);
    g_log.clear();
    g_calls = 0;
    g_budget = kBudget;
    g_tooLong = false;
    g_unsure = false;
    try { fn(g_rdram, &r.ctx, runtimePtr()); }
    catch (const TooLong &) { r.tooLong = true; }
    r.unsure = g_unsure;
    r.tooLong |= g_tooLong;
    r.log = g_log;
    g_dirty = std::getenv("RG_VERBOSE") ? &r.dirty : nullptr;
    r.mem = memHashAndRestore();
    g_dirty = nullptr;
    return r;
}

// What a caller may read after a call (analysis.py ABI_LIVE) and pc.
static std::string diffState(const R5900Context &a, const R5900Context &b)
{
    static const int lows[] = {2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    static const int uppers[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    std::string d;
    for (int r : lows)
        if (_mm_cvtsi128_si64(a.r[r]) != _mm_cvtsi128_si64(b.r[r])) d += " r" + std::to_string(r);
    for (int r : uppers)
        if (_mm_extract_epi64(a.r[r], 1) != _mm_extract_epi64(b.r[r], 1)) d += " h" + std::to_string(r);
    for (int f : {0, 1, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31})
        if (std::memcmp(&a.f[f], &b.f[f], 4) != 0) d += " f" + std::to_string(f);
    if (a.pc != b.pc) d += " pc";
    return d;
}

static double benchOne(bool old, Fn fn, uint32_t entry, const std::vector<int> &seeds, int iters)
{
    static R5900Context start[64];
    static R5900Context ctx;
    const size_t n = std::min<size_t>(seeds.size(), 64);
    for (size_t s = 0; s < n; ++s) seedState(start[s], 1000 + seeds[s], entry);
    useTable(old ? g_fillOld : g_fillNew);
    g_logOn = false;
    uint64_t best = ~0ull;
    for (int rep = 0; rep < 3; ++rep)
    {
        const uint64_t t0 = __rdtsc();
        for (int it = 0; it < iters; ++it)
        {
            std::memcpy(&ctx, &start[it % n], sizeof ctx);
            g_budget = kBudget;
            g_tooLong = false;
            g_calls = 0;
            try
            {
                if (old) dispatchUntil(g_rdram, &ctx, runtimePtr(), kRet);
                else fn(g_rdram, &ctx, runtimePtr());
            }
            catch (const TooLong &) {}
            if (g_tooLong) { g_logOn = true; memHashAndRestore(); return -1; }
        }
        best = std::min<uint64_t>(best, __rdtsc() - t0);
        memHashAndRestore();
    }
    g_logOn = true;
    return (double)best / iters;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 4) { std::fprintf(stderr, "usage: rg_harness LIB.so NAMES.txt SEEDS [bench ITERS]\n"); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const int seeds = std::atoi(argv[3]);
    const bool bench = argc >= 6 && std::strcmp(argv[4], "bench") == 0;
    const int iters = bench ? std::atoi(argv[5]) : 0;
    g_fillOld = reinterpret_cast<FillFn>(dlsym(lib, "rgFillTable"));
    g_fillNew = reinterpret_cast<FillFn>(dlsym(lib, "rgFillTableNc"));
    g_stubOut = reinterpret_cast<const uint32_t (*)[2]>(dlsym(lib, "rgStubOut"));
    if (const void *c = dlsym(lib, "rgStubOutCount")) g_stubOutCount = *static_cast<const int *>(c);
    g_funcs = reinterpret_cast<const RgFunc *>(dlsym(lib, "rgFuncs"));
    g_funcCount = *reinterpret_cast<const int *>(dlsym(lib, "rgFuncCount"));

    g_rdram = static_cast<uint8_t *>(std::aligned_alloc(64, PS2_RAM_SIZE));
    g_clean = static_cast<uint8_t *>(std::aligned_alloc(64, PS2_RAM_SIZE));
    for (size_t i = 0; i < PS2_RAM_SIZE; i += 8)
    {
        uint64_t v = mix(1000 ^ i);
        if ((v & 3) == 0) v = dataPtr(v >> 2);
        // No NaN/Inf float patterns: the PS2 has none, and which NaN x86
        // keeps depends on operand order the compiler may swap.
        for (int sh : {23, 55})
            if (((v >> sh) & 0xFF) == 0xFF) v &= ~(1ull << (sh + 7));
        std::memcpy(g_clean + i, &v, 8);
    }
    // The ELF's own bytes over that (RG_ELF): jump tables, vtables and
    // constants are the game's, so a random table entry does not send both
    // sides down a path the game cannot take. .bss stays random.
    if (const char *elf = std::getenv("RG_ELF"))
    {
        std::ifstream f(elf, std::ios::binary);
        std::vector<char> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, img.data() + o, 4); return v; };
        auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, img.data() + o, 2); return v; };
        const uint32_t phoff = u32(28), phentsize = u16(42), phnum = u16(44);
        for (uint32_t i = 0; i < phnum; ++i)
        {
            const size_t ph = phoff + i * phentsize;
            if (u32(ph) != 1) continue; // PT_LOAD
            const uint32_t off = u32(ph + 4), vaddr = u32(ph + 8) & 0x1FFFFFFF, filesz = u32(ph + 16);
            if (vaddr + filesz <= PS2_RAM_SIZE && off + filesz <= img.size())
                std::memcpy(g_clean + vaddr, img.data() + off, filesz);
        }
    }
    std::memcpy(g_rdram, g_clean, PS2_RAM_SIZE);

    std::ifstream names(argv[2]);
    std::string name;
    int tested = 0, bad = 0, skipped = 0, unsure = 0;
    double sumOld = 0, sumNew = 0;
    auto mangled = [](const std::string &n) {
        return "_Z" + std::to_string(n.size()) + n + "PhP12R5900ContextP10PS2Runtime";
    };
    if (const char *keep = std::getenv("RG_ORACLE"))
    {
        std::ifstream k(keep);
        for (std::string n; std::getline(k, n);)
        {
            auto nc = reinterpret_cast<PS2Runtime::RecompiledFunction>(dlsym(lib, mangled(n + "_nc").c_str()));
            auto oracle = reinterpret_cast<PS2Runtime::RecompiledFunction>(dlsym(lib, mangled(n).c_str()));
            if (nc && oracle) g_keepOracle.push_back({nc, oracle});
        }
    }
    while (std::getline(names, name))
    {
        Fn fn = reinterpret_cast<Fn>(dlsym(lib, mangled(name + "_nc").c_str()));
        const size_t at = name.rfind("_0x");
        if (!fn || at == std::string::npos) { std::printf("missing %s\n", name.c_str()); continue; }
        const uint32_t entry = (uint32_t)std::stoul(name.substr(at + 3), nullptr, 16);
        ++tested;
        std::vector<int> ok;
        for (int s = 0; s < seeds; ++s)
        {
            Result a = runOld(entry, 1000 + s);
            Result b = runNew(fn, entry, 1000 + s);
            if (a.tooLong || b.tooLong)
            {
                if (a.tooLong != b.tooLong && !(a.tooLong && b.tooLong))
                    ; // one side ran out first: the work counts differ by design
                continue;
            }
            const std::string d = diffState(a.ctx, b.ctx);
            if ((!d.empty() || a.log != b.log || a.mem != b.mem) && (a.unsure || b.unsure))
            {
                ++unsure;
                std::printf("UNSURE %s seed %d:%s%s%s (a fake was handed a stack pointer)\n", name.c_str(), s, d.c_str(),
                            a.log != b.log ? " log" : "", a.mem != b.mem ? " ram" : "");
                break;
            }
            if (!d.empty() || a.log != b.log || a.mem != b.mem)
            {
                ++bad;
                size_t firstLog = 0;
                while (firstLog < a.log.size() && firstLog < b.log.size() && a.log[firstLog] == b.log[firstLog]) ++firstLog;
                std::printf("MISMATCH %s seed %d:%s%s%s", name.c_str(), s, d.c_str(), a.log != b.log ? " log" : "",
                            a.mem != b.mem ? " ram" : "");
                if (a.log != b.log)
                    std::printf(" (entry %zu: old '%s' new '%s')", firstLog,
                                firstLog < a.log.size() ? a.log[firstLog].c_str() : "-",
                                firstLog < b.log.size() ? b.log[firstLog].c_str() : "-");
                std::printf("\n");
                if (std::getenv("RG_VERBOSE"))
                {
                    std::printf("  old pc %x sp %x ra %x log %zu | new pc %x sp %x ra %x log %zu\n", a.ctx.pc, reg32(&a.ctx, 29),
                                reg32(&a.ctx, 31), a.log.size(), b.ctx.pc, reg32(&b.ctx, 29), reg32(&b.ctx, 31), b.log.size());
                    for (size_t i = firstLog > 8 ? firstLog - 8 : 0; i < std::max(a.log.size(), b.log.size()) && i < firstLog + 12; ++i)
                        std::printf("  %-50s | %s\n", i < a.log.size() ? a.log[i].c_str() : "", i < b.log.size() ? b.log[i].c_str() : "");
                    if (const char *dump = std::getenv("RG_DUMP"))
                    {
                        std::ofstream o(std::string(dump) + ".old"), n(std::string(dump) + ".new");
                        for (const auto &l : a.log) o << l << "\n";
                        for (const auto &l : b.log) n << l << "\n";
                    }
                    for (int f = 0; f < 32; ++f)
                        if (std::memcmp(&a.ctx.f[f], &b.ctx.f[f], 4) != 0)
                            std::printf("  f%d old %g new %g\n", f, a.ctx.f[f], b.ctx.f[f]);
                    std::unordered_map<uint32_t, uint64_t> ma(a.dirty.begin(), a.dirty.end()), mb(b.dirty.begin(), b.dirty.end());
                    int shown = 0;
                    for (const auto &[at, v] : a.dirty)
                        if (shown < 12 && (!mb.count(at) || mb[at] != v))
                            std::printf("  ram %08x old %016llx new %s\n", at, (unsigned long long)v,
                                        mb.count(at) ? std::to_string(mb[at]).c_str() : "(unchanged)"), ++shown;
                    for (const auto &[at, v] : b.dirty)
                        if (shown < 12 && !ma.count(at))
                            std::printf("  ram %08x old (unchanged) new %016llx\n", at, (unsigned long long)v), ++shown;
                }
                break;
            }
            ok.push_back(s);
        }
        if (ok.empty()) { ++skipped; continue; }
        if (bench)
        {
            const double co = benchOne(true, fn, entry, ok, iters);
            const double cn = benchOne(false, fn, entry, ok, iters);
            if (co < 0 || cn < 0) continue;
            sumOld += co;
            sumNew += cn;
            std::printf("%-48s %10.1f %10.1f %6.2fx  (%zu seeds)\n", name.c_str(), co, cn, co / cn, ok.size());
        }
    }
    if (bench)
        std::printf("%-48s %10.1f %10.1f %6.2fx  (sum over functions)\n", "TOTAL", sumOld, sumNew, sumOld / sumNew);
    std::printf("%d functions, %d mismatches, %d unsure, %d with no seed that finished\n", tested, bad, unsure, skipped);
    return bad ? 1 : 0;
}
