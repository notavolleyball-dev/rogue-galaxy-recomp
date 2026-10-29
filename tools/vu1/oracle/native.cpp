// The native programs of src/vu1_native.h as an oracle candidate: every run of
// a program it recognizes, MSCAL (which kicks nothing) and the MSCNTs it does
// not decline, compared with the recording.
#include "oracle.h"
#include "vu1_native.h"

namespace rg::oracle
{
namespace
{

class Native final : public Candidate
{
public:
    const char *name() const override { return "native (src/vu1_native.cpp)"; }

    bool accepts(const Run &run) const override
    {
        const rg::vu1native::Program *program = programOf(run);
        if (!program)
        {
            return false;
        }
        if (!(run.header.flags & rg::vu1replay::kRunResume))
        {
            return true;
        }
        const char *reason = rg::vu1native::declineChunk(*program, run.data.data(), run.header.top);
        if (reason)
        {
            ++m_declined[std::string("left to the VU1, ") + program->name + ": " + reason];
            return false;
        }
        ++m_taken[program->name];
        return true;
    }

    std::map<std::string, uint32_t> declined() const override
    {
        std::map<std::string, uint32_t> lines = m_declined;
        for (const auto &[name, count] : m_taken)
        {
            lines[std::string("done natively, ") + name] = count;
        }
        return lines;
    }

    void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) override
    {
        events.clear();
        const auto key = std::make_pair(run.programHash, run.programPC);
        if (!m_programs.count(key) && run.programHash == run.header.codeHash)
        {
            const auto code = recording.codes.find(run.programHash);
            m_programs[key] =
                code == recording.codes.end() ? nullptr : rg::vu1native::identify(code->second.data(), run.programPC);
        }
        const rg::vu1native::Program *program = programOf(run);
        if (!program || !(run.header.flags & rg::vu1replay::kRunResume) ||
            rg::vu1native::declineChunk(*program, run.data.data(), run.header.top))
        {
            return;
        }
        rg::vu1native::buildChunk(*program, run.data.data(), run.header.top, m_packets);
        GifState state;
        for (const auto &packet : m_packets)
        {
            decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
        }
    }

private:
    const rg::vu1native::Program *programOf(const Run &run) const
    {
        const auto found = m_programs.find(std::make_pair(run.programHash, run.programPC));
        return found == m_programs.end() ? nullptr : found->second;
    }

    // Per code image and entry (the MSCAL's), what it is, if anything.
    std::map<std::pair<uint64_t, uint32_t>, const rg::vu1native::Program *> m_programs;
    std::vector<std::vector<uint8_t>> m_packets;
    mutable std::map<std::string, uint32_t> m_declined;
    mutable std::map<std::string, uint32_t> m_taken;
};

} // namespace

std::unique_ptr<Candidate> makeNative()
{
    return std::make_unique<Native>();
}

} // namespace rg::oracle
