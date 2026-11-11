#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "native_types.h"


// The GS draw registers as the game sets them, on the game's side, for the
// native pipeline: a draw is made under the material these registers give
// it (texture, blend, tests, target).
//
// M6 of docs/NATIVE-ONLY.md: there is no mirror of the list's state any
// more. The game thread's tracker (game()) follows the draw environments'
// packets, the blocks, libgraph's environments and the model calls, at the
// time the game writes them; each record (a block's pieces, a model call's
// groups, a captured stream) keeps the registers it is drawn under.
namespace rg::drawstate
{

// The draw registers (both contexts, PRIM and PRMODE apart, the rest) and
// the effective PRIM (PRMODECONT applied), as the GS decodes them: a value
// a record can keep (the draw state it is drawn under, made where the game
// wrote it).
struct DrawRegisters
{
    rg::DrawContext ctx[2]{};
    rg::PrimReg primRegister{};
    rg::PrimReg prmodeRegister{};
    bool prmodecont = true;
    rg::TexaReg texa{};
    rg::TexClutReg texclut{};
    bool pabe = false;
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint8_t fogR = 0, fogG = 0, fogB = 0;
};
struct Registers
{
    DrawRegisters r{};
    rg::PrimReg prim{};
    void write(uint8_t reg, uint64_t value);
    void write(const std::pair<uint8_t, uint64_t> *writes, size_t count);
    rg::DrawState state() const;
};


// A tracker of the registers, fed by whoever holds it (the game thread's,
// game(): the packets and writes the game makes, in its order). Not
// thread-safe: one owner.
class Tracker
{
public:
    Tracker();
    ~Tracker();
    Tracker(const Tracker &) = delete;
    Tracker &operator=(const Tracker &) = delete;
    void observe(const uint8_t *data, uint32_t size);
    void observeWrites(const std::pair<uint8_t, uint64_t> *writes, size_t count);
    rg::DrawState current() const;
    const Registers &registers() const;

private:
    struct Impl;
    Impl *m_impl;
};

// The game thread's (M6): the draw state as the game writes it, at the time
// it writes it (the draw environments' packets, the blocks, the model calls
// drawn from records), for the records made there.
Tracker &game();

} // namespace rg::drawstate
