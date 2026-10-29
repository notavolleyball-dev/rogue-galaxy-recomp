#include "vu1_replay.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include <xmmintrin.h>

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

namespace rg::vu1replay
{
namespace
{

const char *const g_path = std::getenv("RG_VU1_REPLAY");

uint32_t envNumber(const char *name, uint32_t fallback)
{
    const char *value = std::getenv(name);
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : fallback;
}

uint64_t fnv(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

// Only the thread that runs VU1 calls in, one at a time.
struct Recorder
{
    std::FILE *file = nullptr;
    uint32_t from = envNumber("RG_VU1_REPLAY_FROM", 1u);
    uint32_t lists = envNumber("RG_VU1_REPLAY_LISTS", 30u);
    bool after = envNumber("RG_VU1_REPLAY_AFTER", 0u) != 0u;
    uint32_t firstList = 0u;
    uint32_t lastList = 0u;
    bool finished = false;
    // A run being recorded, between begin and end.
    bool recording = false;
    uint32_t runs = 0u;
    uint64_t kicks = 0u;
    uint64_t generation = ~0ull;
    uint64_t codeHash = 0u;
    std::unordered_set<uint64_t> written;

    // A run that ends inside the window keeps what it wrote.
    ~Recorder() { close(); }

    void put(const void *bytes, size_t size) { std::fwrite(bytes, 1, size, file); }
    void put32(uint32_t value) { put(&value, sizeof(value)); }

    bool open()
    {
        file = std::fopen(g_path, "wb");
        if (!file)
        {
            std::fprintf(stderr, "[vu1replay] cannot write %s\n", g_path);
            finished = true;
            return false;
        }
        std::setvbuf(file, nullptr, _IOFBF, 16u << 20);
        put32(kMagic);
        put32(kVersion);
        put32(sizeof(VU1State));
        put32(PS2_VU1_CODE_SIZE);
        put32(PS2_VU1_DATA_SIZE);
        return true;
    }

    void close()
    {
        finished = true;
        recording = false;
        if (file)
        {
            std::fclose(file);
            file = nullptr;
            std::fprintf(stderr, "[vu1replay] %u runs, %llu kicks, %zu code images from lists %u-%u in %s\n", runs,
                         static_cast<unsigned long long>(kicks), written.size(), firstList, lastList, g_path);
        }
    }
};

Recorder &recorder()
{
    static Recorder instance;
    return instance;
}

} // namespace

bool enabled()
{
    return g_path != nullptr;
}

void begin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
           uint64_t codeGeneration, const uint8_t *data, const VU1State &state)
{
    Recorder &r = recorder();
    r.recording = false;
    if (r.finished)
    {
        return;
    }
    // Lists sent in normal mode carry no number: they belong to the one
    // around them.
    if (list != 0u)
    {
        if (!r.file)
        {
            // Whole programs only: the file starts with an MSCAL.
            if (list < r.from || resume || !r.open())
            {
                return;
            }
            r.firstList = list;
        }
        if (list - r.firstList >= r.lists)
        {
            r.close();
            return;
        }
        if (list != r.lastList)
        {
            r.lastList = list;
            std::fflush(r.file);
        }
    }
    else if (!r.file)
    {
        return;
    }
    if (codeGeneration != r.generation)
    {
        r.generation = codeGeneration;
        r.codeHash = fnv(code, PS2_VU1_CODE_SIZE);
    }
    if (r.written.insert(r.codeHash).second)
    {
        r.put32(kCode);
        r.put(&r.codeHash, sizeof(r.codeHash));
        r.put(code, PS2_VU1_CODE_SIZE);
    }
    RunHeader header{};
    header.flags = (resume ? kRunResume : 0u) | (state.dBitEnabled ? kRunDBit : 0u) |
                   (state.tBitEnabled ? kRunTBit : 0u);
    header.startPC = startPC;
    header.top = top;
    header.itop = itop;
    header.list = list;
    header.index = r.runs++;
    header.mxcsr = _mm_getcsr();
    header.codeHash = r.codeHash;
    r.put32(kRun);
    r.put(&header, sizeof(header));
    r.put(&state, sizeof(state));
    r.put(data, PS2_VU1_DATA_SIZE);
    r.recording = true;
}

void kick(const uint8_t *data, uint32_t size)
{
    Recorder &r = recorder();
    if (!r.recording)
    {
        return;
    }
    r.put32(kKick);
    r.put32(size);
    r.put(data, size);
    ++r.kicks;
}

void end(const VU1State &state, const uint8_t *data)
{
    Recorder &r = recorder();
    if (!r.recording)
    {
        return;
    }
    r.recording = false;
    r.put32(kEnd);
    r.put32(r.after ? kEndData : 0u);
    r.put(&state, sizeof(state));
    if (r.after)
    {
        r.put(data, PS2_VU1_DATA_SIZE);
    }
}

} // namespace rg::vu1replay
