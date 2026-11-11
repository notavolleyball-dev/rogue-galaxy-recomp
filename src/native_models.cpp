#include "native_models.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "draw_state_mirror.h"
#include "runtime/ps2_memory.h"
#include "vu1_native.h"

namespace rg::nativemodels
{

bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_MODEL_GAME_TIME");
        return !(value && std::strcmp(value, "0") == 0);
    }();
    return on;
}

struct StreamReader::Impl
{
    alignas(16) uint8_t data[PS2_VU1_DATA_SIZE] = {};
    alignas(16) uint8_t code[PS2_VU1_CODE_SIZE] = {};
    rg::drawstate::Tracker gs;
    // The VIF's registers this reads by.
    uint32_t base = 0u, ofst = 0u, tops = 0u, cl = 1u, wl = 1u;
    bool dbf = false;
    // The command whose data is being read: its code, the bytes still to
    // come, and those read so far.
    uint32_t command = 0u, owed = 0u;
    std::vector<uint8_t> gathered;
    // A part of a code word (the list's bytes come in pieces).
    uint8_t partial[4] = {};
    uint32_t partialSize = 0u;
    // The program the last MSCAL entered (null: not a native one), by the
    // code's generation and the entry.
    uint64_t generation = 1u;
    const rg::vu1native::Program *program = nullptr;
    std::unordered_map<uint64_t, const rg::vu1native::Program *> programs;

    void swapBuffers()
    {
        if (dbf)
            tops = base & 0x3FFu;
        else
            tops = (base + ofst) & 0x3FFu;
        dbf = !dbf;
    }

    // Words of data the command takes after its code word.
    static uint32_t dataWords(uint32_t cmd)
    {
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
        if (op == 0x20u)
            return 1u; // STMASK
        if (op == 0x30u || op == 0x31u)
            return 4u; // STROW, STCOL
        if (op == 0x4Au)
            return (num == 0u ? 256u : num) * 2u; // MPG
        if (op == 0x50u || op == 0x51u)
            return (imm == 0u ? 65536u : imm) * 4u; // DIRECT
        if ((op & 0x60u) == 0x60u)
        {
            const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, n = num == 0u ? 256u : num;
            const uint32_t bits = vl == 3u ? 16u : (32u >> vl) * (vn + 1u);
            return (n * bits + 31u) / 32u;
        }
        return 0u;
    }

    void unpack(uint32_t cmd, const uint8_t *in)
    {
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
        const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, n = num == 0u ? 256u : num;
        const bool usn = (imm >> 14) & 1u, flg = (imm >> 15) & 1u;
        uint32_t address = (imm & 0x3FFu) + (flg ? tops : 0u);
        const uint32_t lanes = vn + 1u;
        const uint32_t step = std::max(cl, 1u), write = std::max(wl, 1u);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t at = (address + (write <= step ? (i / write) * step + (i % write) : i)) & 1023u;
            uint32_t out[4];
            std::memcpy(out, data + at * 16u, sizeof(out));
            if (vl == 3u)
            {
                // V4-5: RGBA5551 a halfword each, to 8 bits a lane.
                uint16_t v;
                std::memcpy(&v, in + i * 2u, 2u);
                out[0] = (v & 31u) << 3;
                out[1] = ((v >> 5) & 31u) << 3;
                out[2] = ((v >> 10) & 31u) << 3;
                out[3] = (v & 0x8000u) ? 0x80u : 0u;
            }
            else
            {
                const uint32_t bytes = 4u >> vl;
                for (uint32_t l = 0; l < lanes; ++l)
                {
                    const uint8_t *p = in + (i * lanes + l) * bytes;
                    uint32_t v = 0u;
                    if (bytes == 4u)
                        std::memcpy(&v, p, 4u);
                    else if (bytes == 2u)
                    {
                        uint16_t h;
                        std::memcpy(&h, p, 2u);
                        v = usn ? h : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                    }
                    else
                        v = usn ? *p : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*p)));
                    out[l] = v;
                }
                if (vn == 0u)
                    out[1] = out[2] = out[3] = out[0]; // S: every lane
            }
            std::memcpy(data + at * 16u, out, sizeof(out));
        }
    }

    void run(uint32_t cmd)
    {
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu;
        switch (op)
        {
        case 0x01u:
            cl = imm & 0xFFu;
            wl = (imm >> 8) & 0xFFu;
            break;
        case 0x02u:
            ofst = imm & 0x3FFu;
            dbf = false;
            tops = base & 0x3FFu;
            break;
        case 0x03u:
            base = imm & 0x3FFu;
            break;
        case 0x14u:
        case 0x15u:
        {
            swapBuffers();
            const uint32_t startPC = imm * 8u;
            if (imm == 0u)
            {
                rg::vu1native::baseProgram(data);
                break;
            }
            const uint64_t key = (generation << 16) | imm;
            auto it = programs.find(key);
            if (it == programs.end())
            {
                if (programs.size() > 4096u)
                    programs.clear();
                it = programs.emplace(key, rg::vu1native::identify(code, startPC)).first;
            }
            program = it->second;
            break;
        }
        case 0x17u:
            swapBuffers();
            break;
        default:
            break;
        }
    }

    // A command's data, whole.
    void finish(uint32_t cmd, const uint8_t *in, uint32_t size)
    {
        const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu;
        if (op == 0x4Au)
        {
            const uint32_t at = imm * 8u;
            if (at < PS2_VU1_CODE_SIZE)
                std::memcpy(code + at, in, std::min<uint32_t>(size, PS2_VU1_CODE_SIZE - at));
            ++generation;
        }
        else if (op == 0x50u || op == 0x51u)
            gs.observe(in, size);
        else if ((op & 0x60u) == 0x60u && !((imm >> 15) & 1u))
            unpack(cmd, in); // (a TOPS-relative UNPACK is a chunk's input: not needed here)
        else if (op == 0x01u || op == 0x02u || op == 0x03u)
            run(cmd);
    }
};

StreamReader::StreamReader() : m_impl(new Impl) {}
StreamReader::~StreamReader()
{
    delete m_impl;
}

void StreamReader::read(const uint8_t *in, uint32_t size)
{
    Impl &r = *m_impl;
    uint32_t at = 0u;
    while (at < size)
    {
        if (r.owed != 0u)
        {
            // The command's data.
            const uint32_t take = std::min(r.owed, size - at);
            r.gathered.insert(r.gathered.end(), in + at, in + at + take);
            at += take;
            r.owed -= take;
            if (r.owed == 0u)
                r.finish(r.command, r.gathered.data(), static_cast<uint32_t>(r.gathered.size()));
            continue;
        }
        // A code word, perhaps across pieces.
        while (r.partialSize < 4u && at < size)
            r.partial[r.partialSize++] = in[at++];
        if (r.partialSize < 4u)
            break;
        r.partialSize = 0u;
        uint32_t cmd;
        std::memcpy(&cmd, r.partial, 4u);
        const uint32_t words = Impl::dataWords(cmd);
        if (words == 0u)
        {
            r.run(cmd);
            continue;
        }
        r.command = cmd;
        r.owed = words * 4u;
        r.gathered.clear();
    }
}

void StreamReader::skip(uint32_t size)
{
    Impl &r = *m_impl;
    if (r.owed == 0u)
        return; // (a hole is always a DIRECT's data)
    if (size >= r.owed)
    {
        // An upload's data: nothing of the state; the DIRECT is dropped.
        r.owed = 0u;
        r.gathered.clear();
    }
    else
    {
        r.owed -= size;
        r.gathered.resize(r.gathered.size() + size, 0u);
    }
}

std::shared_ptr<GameDraw> StreamReader::chunk(const uint8_t header[32], uint32_t slots, uint32_t id)
{
    Impl &r = *m_impl;
    const uint32_t top = r.tops & 0x3FFu;
    std::shared_ptr<GameDraw> draw;
    if (r.program && rg::vu1native::modelOnGpu(*r.program))
    {
        std::memcpy(r.data + top * 16u, header, 32u);
        draw = std::make_shared<GameDraw>();
        draw->id = id;
        rg::vu1native::modelHeader(*r.program, r.data, top, draw->packets, draw->mesh);
        rg::vu1native::modelRow(*r.program, r.data, top, draw->row, draw->palette, slots);
        for (const std::vector<uint8_t> &packet : draw->packets)
            r.gs.observe(packet.data(), static_cast<uint32_t>(packet.size()));
        std::memcpy(draw->vu.data(), r.data, draw->vu.size());
    }
    r.swapBuffers(); // the MSCNT
    return draw;
}

StreamReader &reader()
{
    static StreamReader *r = new StreamReader();
    return *r;
}

} // namespace rg::nativemodels
