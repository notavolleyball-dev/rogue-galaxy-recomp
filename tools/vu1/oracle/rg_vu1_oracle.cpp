// rg_vu1_oracle: replays an RG_VU1_REPLAY recording (src/debug/vu1_replay.h)
// through VU1 without the game, and checks what each candidate draws against
// what the game's own runs kicked.
//
//   rg_vu1_oracle FILE [--candidate compiled|interpreter|all] [--runs N]
//                      [--program HASH:PC] [--show RUN|diff] [--tol-xy N] [--tol-z N]
//                      [--tol-rgba N] [--tol-uv N] [--tol-stq F]
//
// --show prints one run's events beside the recorded ones, from its first
// difference; `--show diff` does so for the first differing run of each
// program.
//
// A candidate turns a run into GS events (gif_events.h): the VU1 engines run
// the recorded program from the registers and data memory the game had and
// decode what it kicks; a native replacement of a program family (phase 1 of
// docs/NATIVE-RENDERER.md) computes the events itself. Runs are replayed in
// the file's order on one VU1, since an MSCNT continues the run before it.
//
// A run's program is its code image and the entry of the MSCAL that began it;
// continuations count toward it. The table lists, per program, the runs, the
// events, how many differ beyond the tolerances and the largest differences,
// and whether the registers the run ended with match. Exit status 1 when any
// candidate differs.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <xmmintrin.h>

#include "gif_events.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "vu1_replay.h"

namespace
{

using namespace rg::oracle;
namespace replay = rg::vu1replay;

// VU1 micro memory as the EE addresses it: a write here bumps the code
// generation the VU1 caches its decoding and compiled routines by.
constexpr uint32_t kVu1CodeAddress = 0x11008000u;

struct Run
{
    replay::RunHeader header{};
    VU1State state{};
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> kicks;
    VU1State endState{};
    bool ended = false;
    // The program it belongs to: the MSCAL's code image and entry.
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
};

struct Recording
{
    std::unordered_map<uint64_t, std::vector<uint8_t>> codes;
    std::vector<Run> runs;
};

bool readRecording(const char *path, uint32_t maxRuns, Recording &out)
{
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    std::setvbuf(file, nullptr, _IOFBF, 16u << 20);
    const auto get = [&](void *bytes, size_t size) { return std::fread(bytes, 1, size, file) == size; };
    uint32_t header[5];
    if (!get(header, sizeof(header)) || header[0] != replay::kMagic || header[1] != replay::kVersion)
    {
        std::fprintf(stderr, "%s: not an RG_VU1_REPLAY recording (version %u)\n", path, replay::kVersion);
        std::fclose(file);
        return false;
    }
    if (header[2] != sizeof(VU1State) || header[3] != PS2_VU1_CODE_SIZE || header[4] != PS2_VU1_DATA_SIZE)
    {
        std::fprintf(stderr, "%s: recorded by a build with another VU1State (%u bytes, this one %zu)\n", path,
                     header[2], sizeof(VU1State));
        std::fclose(file);
        return false;
    }
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
    bool truncated = false;
    uint32_t kind;
    while (get(&kind, sizeof(kind)))
    {
        if (kind == replay::kCode)
        {
            uint64_t hash;
            std::vector<uint8_t> code(PS2_VU1_CODE_SIZE);
            truncated = !get(&hash, sizeof(hash)) || !get(code.data(), code.size());
            out.codes.emplace(hash, std::move(code));
        }
        else if (kind == replay::kRun)
        {
            if (out.runs.size() >= maxRuns)
            {
                break;
            }
            Run run;
            run.data.resize(PS2_VU1_DATA_SIZE);
            truncated = !get(&run.header, sizeof(run.header)) || !get(&run.state, sizeof(run.state)) ||
                        !get(run.data.data(), run.data.size());
            if (!(run.header.flags & replay::kRunResume))
            {
                programHash = run.header.codeHash;
                programPC = run.header.startPC;
            }
            run.programHash = programHash;
            run.programPC = programPC;
            out.runs.push_back(std::move(run));
        }
        else if (kind == replay::kKick && !out.runs.empty())
        {
            uint32_t size;
            truncated = !get(&size, sizeof(size));
            std::vector<uint8_t> packet(truncated ? 0u : size);
            truncated = truncated || !get(packet.data(), packet.size());
            out.runs.back().kicks.push_back(std::move(packet));
        }
        else if (kind == replay::kEnd && !out.runs.empty())
        {
            uint32_t flags;
            Run &run = out.runs.back();
            truncated = !get(&flags, sizeof(flags)) || !get(&run.endState, sizeof(run.endState));
            if (!truncated && (flags & replay::kEndData))
            {
                truncated = std::fseek(file, PS2_VU1_DATA_SIZE, SEEK_CUR) != 0;
            }
            run.ended = !truncated;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown record %08x after run %zu\n", path, kind, out.runs.size());
            truncated = true;
        }
        if (truncated)
        {
            break;
        }
    }
    std::fclose(file);
    if (truncated)
    {
        std::fprintf(stderr, "%s: ends inside a record; the last run is left out\n", path);
        if (!out.runs.empty())
        {
            out.runs.pop_back();
        }
    }
    return true;
}

std::vector<GifEvent> eventsOf(const std::vector<std::vector<uint8_t>> &packets)
{
    // The vertex state before the run is the same for every candidate, so
    // each starts from a blank one.
    std::vector<GifEvent> events;
    GifState state;
    for (const auto &packet : packets)
    {
        decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
    }
    return events;
}

class Candidate
{
public:
    virtual ~Candidate() = default;
    virtual const char *name() const = 0;
    // Whether it draws runs of this program (the others are not counted).
    virtual bool accepts(const Run &run) const = 0;
    // The events of `run`, every run of the file in order whether accepted or
    // not (a VU1 engine has to keep its state).
    virtual void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) = 0;
    // The registers the last run ended with, where the candidate has them.
    virtual const VU1State *endState() const { return nullptr; }
};

// The recorded program on PS2Recomp's VU1: its compiled routines, or its
// interpreter alone.
class Vu1Engine final : public Candidate
{
public:
    explicit Vu1Engine(bool compiled) : m_compiled(compiled)
    {
        if (!m_memory.initialize())
        {
            std::fprintf(stderr, "PS2Memory::initialize failed\n");
            std::exit(2);
        }
        m_vram.resize(PS2_GS_VRAM_SIZE);
        m_gs.init(m_vram.data(), static_cast<uint32_t>(m_vram.size()));
        m_memory.setGifArbiter(nullptr);
        m_memory.setGifPacketCallback([this](const uint8_t *data, uint32_t size) {
            if (m_packets)
            {
                m_packets->emplace_back(data, data + size);
            }
        });
        m_vu.setCompiledExecutionEnabled(compiled);
    }

    const char *name() const override { return m_compiled ? "compiled" : "interpreter"; }
    bool accepts(const Run &) const override { return true; }
    const VU1State *endState() const override { return &m_vu.state(); }

    void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) override
    {
        if (!m_loaded || m_codeHash != run.header.codeHash)
        {
            const auto found = recording.codes.find(run.header.codeHash);
            if (found == recording.codes.end())
            {
                std::fprintf(stderr, "run %u: code image %016" PRIx64 " missing from the file\n", run.header.index,
                             run.header.codeHash);
                std::exit(2);
            }
            std::memcpy(m_memory.getVU1Code(), found->second.data(), PS2_VU1_CODE_SIZE);
            const uint64_t before = m_memory.getVU1CodeGeneration();
            uint32_t first;
            std::memcpy(&first, found->second.data(), sizeof(first));
            m_memory.write32(kVu1CodeAddress, first);
            if (m_memory.getVU1CodeGeneration() == before)
            {
                std::fprintf(stderr, "a write to 0x%08x did not reach VU1 micro memory\n", kVu1CodeAddress);
                std::exit(2);
            }
            m_codeHash = run.header.codeHash;
            m_loaded = true;
        }
        std::memcpy(m_memory.getVU1Data(), run.data.data(), PS2_VU1_DATA_SIZE);
        m_vu.state() = run.state;
        std::vector<std::vector<uint8_t>> packets;
        m_packets = &packets;
        const uint32_t mxcsr = _mm_getcsr();
        _mm_setcsr(run.header.mxcsr);
        if (run.header.flags & replay::kRunResume)
        {
            m_vu.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs,
                        &m_memory, run.header.top, run.header.itop, 65536);
        }
        else
        {
            m_vu.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs,
                         &m_memory, run.header.startPC, run.header.top, run.header.itop, 65536);
        }
        _mm_setcsr(mxcsr);
        m_packets = nullptr;
        events = eventsOf(packets);
    }

private:
    bool m_compiled;
    PS2Memory m_memory;
    std::vector<uint8_t> m_vram;
    GS m_gs;
    VU1Interpreter m_vu{VU1Interpreter::Unit::VU1};
    std::vector<std::vector<uint8_t>> *m_packets = nullptr;
    uint64_t m_codeHash = 0u;
    bool m_loaded = false;
};

// Whether the registers a run ended with are the recorded ones: the vector
// and integer registers, ACC, Q, P and I, bit for bit.
bool sameRegisters(const VU1State &a, const VU1State &b)
{
    return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
           std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, sizeof(a.q)) == 0 &&
           std::memcmp(&a.p, &b.p, sizeof(a.p)) == 0 && std::memcmp(&a.i, &b.i, sizeof(a.i)) == 0;
}

struct ProgramResult
{
    uint32_t runs = 0;
    uint32_t resumes = 0;
    uint32_t identical = 0; // runs whose events all match within the tolerance
    uint32_t registers = 0; // runs whose end registers differ
    uint64_t kicks = 0;
    Comparison comparison;
};

constexpr int64_t kShowFirstDifferences = -2;

struct Options
{
    const char *path = nullptr;
    std::string candidate = "all";
    uint32_t maxRuns = UINT32_MAX;
    bool filtered = false;
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
    int64_t show = -1;
    Tolerance tolerance;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: rg_vu1_oracle FILE [--candidate compiled|interpreter|all] [--runs N]\n"
                 "                      [--program HASH:PC] [--show RUN|diff] [--tol-xy N] [--tol-z N]\n"
                 "                      [--tol-rgba N] [--tol-uv N] [--tol-stq F]\n");
}

bool parse(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--candidate" && hasValue)
            options.candidate = argv[++i];
        else if (arg == "--runs" && hasValue)
            options.maxRuns = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--program" && hasValue)
        {
            char *end = nullptr;
            options.programHash = std::strtoull(argv[++i], &end, 16);
            if (!end || *end != ':')
                return false;
            options.programPC = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
            options.filtered = true;
        }
        else if (arg == "--show" && hasValue)
        {
            const std::string value = argv[++i];
            options.show = value == "diff" ? kShowFirstDifferences : std::strtoll(value.c_str(), nullptr, 0);
        }
        else if (arg == "--tol-xy" && hasValue)
            options.tolerance.xy = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-z" && hasValue)
            options.tolerance.z = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-rgba" && hasValue)
            options.tolerance.rgba = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-uv" && hasValue)
            options.tolerance.uv = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-stq" && hasValue)
            options.tolerance.stq = std::strtof(argv[++i], nullptr);
        else if (!options.path && arg[0] != '-')
            options.path = argv[i];
        else
            return false;
    }
    return options.path != nullptr;
}

void showRun(const Run &run, const std::vector<GifEvent> &reference, const std::vector<GifEvent> &candidate,
             const char *name, int64_t from)
{
    std::printf("run %u (%s of %016" PRIx64 ":%x, list %u, %zu packets): recorded, then %s\n", run.header.index,
                (run.header.flags & replay::kRunResume) ? "MSCNT" : "MSCAL", run.programHash, run.programPC,
                run.header.list, run.kicks.size(), name);
    const size_t first = from > 4 ? static_cast<size_t>(from - 4) : 0u;
    const size_t last = std::min(std::max(reference.size(), candidate.size()), first + 24u);
    for (size_t i = first; i < last; ++i)
    {
        const std::string a = i < reference.size() ? describe(reference[i]) : "-";
        const std::string b = i < candidate.size() ? describe(candidate[i]) : "-";
        std::printf("%6zu %s %s\n%6s %s\n", i, a == b ? " " : "*", a.c_str(), "", b.c_str());
    }
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, options))
    {
        usage();
        return 2;
    }
    Recording recording;
    if (!readRecording(options.path, options.maxRuns, recording))
    {
        return 2;
    }
    uint32_t resumes = 0u;
    uint64_t kicks = 0u;
    for (const Run &run : recording.runs)
    {
        resumes += (run.header.flags & replay::kRunResume) ? 1u : 0u;
        kicks += run.kicks.size();
    }
    std::printf("%s: %zu runs (%u continuations), %" PRIu64 " packets, %zu code images, lists %u-%u\n",
                options.path, recording.runs.size(), resumes, kicks, recording.codes.size(),
                recording.runs.empty() ? 0u : recording.runs.front().header.list,
                recording.runs.empty() ? 0u : recording.runs.back().header.list);

    std::vector<std::unique_ptr<Candidate>> candidates;
    if (options.candidate == "compiled" || options.candidate == "all")
        candidates.push_back(std::make_unique<Vu1Engine>(true));
    if (options.candidate == "interpreter" || options.candidate == "all")
        candidates.push_back(std::make_unique<Vu1Engine>(false));
    if (candidates.empty())
    {
        usage();
        return 2;
    }

    bool allMatch = true;
    for (const auto &candidate : candidates)
    {
        std::map<std::pair<uint64_t, uint32_t>, ProgramResult> programs;
        for (const Run &run : recording.runs)
        {
            std::vector<GifEvent> events;
            candidate->run(recording, run, events);
            const bool wanted = !options.filtered ||
                                (run.programHash == options.programHash && run.programPC == options.programPC);
            if (!wanted || !candidate->accepts(run))
            {
                continue;
            }
            const std::vector<GifEvent> reference = eventsOf(run.kicks);
            const Comparison comparison = compare(reference, events, options.tolerance);
            ProgramResult &result = programs[{run.programHash, run.programPC}];
            ++result.runs;
            result.resumes += (run.header.flags & replay::kRunResume) ? 1u : 0u;
            result.kicks += run.kicks.size();
            result.comparison.add(comparison);
            const bool same = comparison.mismatched == 0u && comparison.countDifference == 0;
            result.identical += same ? 1u : 0u;
            const VU1State *end = candidate->endState();
            const bool registersDiffer = end && run.ended && !sameRegisters(*end, run.endState);
            result.registers += registersDiffer ? 1u : 0u;
            if (options.show == static_cast<int64_t>(run.header.index) ||
                (options.show == kShowFirstDifferences && !same && result.runs - result.identical == 1u))
            {
                showRun(run, reference, events, candidate->name(), comparison.firstMismatch);
            }
        }

        ProgramResult total;
        std::printf("\n%s\n%-24s %7s %6s %8s %9s %9s %9s %6s %8s %6s %5s %9s %8s\n", candidate->name(), "program",
                    "runs", "mscnt", "packets", "vertices", "same", "differ", "regs", "max xy", "max z", "rgba",
                    "uv/16", "stq");
        for (const auto &[key, result] : programs)
        {
            const Comparison &c = result.comparison;
            std::printf("%016" PRIx64 ":%-6x %7u %6u %8" PRIu64 " %9" PRIu64 " %9u %9u %6u %8.4f %6u %5u %9u %8.2g\n",
                        key.first, key.second, result.runs, result.resumes, result.kicks, c.vertices,
                        result.identical, result.runs - result.identical, result.registers, c.maxXy / 16.0, c.maxZ,
                        c.maxRgba, c.maxUv, c.maxStq);
            total.runs += result.runs;
            total.resumes += result.resumes;
            total.identical += result.identical;
            total.registers += result.registers;
            total.kicks += result.kicks;
            total.comparison.add(result.comparison);
        }
        std::printf("%-24s %7u %6u %8" PRIu64 " %9" PRIu64 " %9u %9u %6u %8.4f %6u %5u %9u %8.2g\n", "all",
                    total.runs, total.resumes, total.kicks, total.comparison.vertices, total.identical,
                    total.runs - total.identical, total.registers, total.comparison.maxXy / 16.0,
                    total.comparison.maxZ, total.comparison.maxRgba, total.comparison.maxUv,
                    total.comparison.maxStq);
        allMatch = allMatch && total.identical == total.runs;
    }
    return allMatch ? 0 : 1;
}
