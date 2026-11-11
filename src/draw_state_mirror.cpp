#include "draw_state_mirror.h"

#include <algorithm>
#include <vector>

#include "gs_writes.h"


namespace rg::drawstate
{
namespace
{

rg::PrimReg decodePrim(uint64_t value)
{
    rg::PrimReg prim{};
    prim.type = static_cast<rg::PrimType>(value & 0x7u);
    prim.iip = ((value >> 3) & 1u) != 0u;
    prim.tme = ((value >> 4) & 1u) != 0u;
    prim.fge = ((value >> 5) & 1u) != 0u;
    prim.abe = ((value >> 6) & 1u) != 0u;
    prim.aa1 = ((value >> 7) & 1u) != 0u;
    prim.fst = ((value >> 8) & 1u) != 0u;
    prim.ctxt = ((value >> 9) & 1u) != 0u;
    prim.fix = ((value >> 10) & 1u) != 0u;
    return prim;
}

void decodeTex0(rg::Tex0Reg &t, uint64_t value)
{
    t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
    t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
    t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
    t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
    t.th = static_cast<uint8_t>((value >> 30) & 0xF);
    t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
    t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
    t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
    t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
    t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
    t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
    t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
}

// One register write, as the front end takes it.
void writeTo(DrawRegisters &r, rg::PrimReg &prim, uint8_t reg, uint64_t value)
{
    const int ci = (reg & 1u) ? 1 : 0;
    switch (reg)
    {
    case 0x00:
        r.primRegister = decodePrim(value);
        if (r.prmodecont)
            prim = r.primRegister;
        else
            prim.type = r.primRegister.type;
        break;
    case 0x06: case 0x07:
        decodeTex0(r.ctx[ci].tex0, value);
        break;
    case 0x08: case 0x09: r.ctx[ci].clamp = value; break;
    case 0x14: case 0x15: r.ctx[ci].tex1 = value; break;
    case 0x16: case 0x17: {
        // TEX2: part of TEX0.
        auto &t = r.ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case 0x18: case 0x19:
        r.ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        r.ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    case 0x1A: {
        r.prmodecont = (value & 1) != 0;
        const rg::PrimType type = r.primRegister.type;
        prim = r.prmodecont ? r.primRegister : r.prmodeRegister;
        prim.type = type;
        break;
    }
    case 0x1B:
        r.prmodeRegister = decodePrim(value);
        if (!r.prmodecont)
        {
            const rg::PrimType type = r.primRegister.type;
            prim = r.prmodeRegister;
            prim.type = type;
        }
        break;
    case 0x1C:
        r.texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        r.texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        r.texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case 0x22: r.scanmsk = value; break;
    case 0x34: case 0x35: r.ctx[ci].miptbp1 = value; break;
    case 0x36: case 0x37: r.ctx[ci].miptbp2 = value; break;
    case 0x3B:
        r.texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        r.texa.aem = ((value >> 15) & 0x1u) != 0u;
        r.texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        break;
    case 0x3D:
        r.fogR = static_cast<uint8_t>(value & 0xFFu);
        r.fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        r.fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case 0x40: case 0x41:
        r.ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        r.ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        r.ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        r.ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    case 0x42: case 0x43: r.ctx[ci].alpha = value; break;
    case 0x44: r.dimx = value; break;
    case 0x45: r.dthe = value; break;
    case 0x46: r.colclamp = value; break;
    case 0x47: r.ctx[0].test = value; break;
    case 0x48: r.ctx[1].test = value; break;
    case 0x49: r.pabe = (value & 1u) != 0u; break;
    case 0x4A: case 0x4B: r.ctx[ci].fba = value; break;
    case 0x4C: case 0x4D:
        r.ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        r.ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        r.ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        r.ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    case 0x4E: case 0x4F:
        r.ctx[ci].zbuf.zbp = value & 0x1FF;
        r.ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        r.ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    default:
        break;
    }
}
} // namespace


void Registers::write(uint8_t reg, uint64_t value)
{
    writeTo(r, prim, reg, value);
}

void Registers::write(const std::pair<uint8_t, uint64_t> *writes, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        writeTo(r, prim, writes[i].first, writes[i].second);
}

rg::DrawState Registers::state() const
{
    rg::DrawState state{};
    state.context = r.ctx[prim.ctxt ? 1 : 0];
    state.prim = prim;
    state.texa = r.texa;
    state.texclut = r.texclut;
    state.pabe = r.pabe;
    state.scanmsk = r.scanmsk;
    state.dimx = r.dimx;
    state.dthe = r.dthe;
    state.colclamp = r.colclamp;
    state.fogR = r.fogR;
    state.fogG = r.fogG;
    state.fogB = r.fogB;
    state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(state.context.tex0.tw, 10u));
    state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(state.context.tex0.th, 10u));
    const uint64_t tex1 = state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return state;
}

struct Tracker::Impl
{
    Registers regs;
    std::vector<rg::gswrites::Write> writes;
};

Tracker::Tracker() : m_impl(new Impl) {}
Tracker::~Tracker()
{
    delete m_impl;
}

void Tracker::observe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 16u)
        return;
    Impl &t = *m_impl;
    t.writes.clear();
    rg::gswrites::read(data, size, t.writes);
    t.regs.write(t.writes.data(), t.writes.size());
}

void Tracker::observeWrites(const std::pair<uint8_t, uint64_t> *writes, size_t count)
{
    m_impl->regs.write(writes, count);
}

rg::DrawState Tracker::current() const
{
    return m_impl->regs.state();
}

const Registers &Tracker::registers() const
{
    return m_impl->regs;
}

Tracker &game()
{
    static Tracker tracker;
    return tracker;
}

} // namespace rg::drawstate
