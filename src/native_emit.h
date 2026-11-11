#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "draw_state_mirror.h"

class PS2Runtime;
namespace rg::vu1native
{
struct Program;
}

// M6 of docs/NATIVE-ONLY.md: model draws from the game's emit call
// (ModelEmitDrawPacketsMaybe_14ec20; NATIVE-ONLY-NOTES.md, "Building model
// draws at Emit time"). At the call, each item's and group's inputs are read
// from the game's structures (the model matrix and the mesh header the item
// packets carry, the material's packet), a marker on each side of the call's
// list output says which call the list's draws belong to, and each model
// chunk the list draws compares its VU1 memory with them: the same,
// differing (the first in full). Step one: the draws still come from the
// list. Step two, once the count says they agree: the draws from these, the
// list carrying no model draw.
namespace rg::nativeemit
{

using Quad = std::array<uint32_t, 4>;
// What a group's chunks should find in VU1 memory: the material packet's
// quads 82, 84, 86, 87.
struct Group
{
    Quad q82{}, q84{}, q86{}, q87{};
    // The program the microcode table names (null: a slot left as it was,
    // or pieces identify() does not know; not compared).
    const rg::vu1native::Program *program = nullptr;
    bool programKnown = false;
    // The GS state its texenv sets: the material's GsEnv (t+0x14: ALPHA,
    // TEST, ZBUF's mask, FBA), TEX0 from its texture node, TEX1 and CLAMP
    // from its packet (frozen at load).
    uint64_t alpha = 0u, test = 0u, fba = 0u, tex0 = 0u, tex1 = 0u, clamp = 0u;
    bool zmask = false, textured = false, stateKnown = false;
    // Its chunks' two header quads (TOP, TOP+1), from its chain's spans in
    // EE memory, in the order they are drawn; and how many the list drew.
    std::vector<std::array<Quad, 2>> chunks;
    bool chunksKnown = false;
    uint32_t drawn = 0u;
    // For a group drawn from these (no packet in the list): its chain (REF
    // tags to its chunks' spans) and its texenv's register writes in the
    // list's order (the GsEnv's five with the draw environment's frame and
    // depth base, then TEX1 and CLAMP, then TEX0).
    uint32_t chain = 0u;
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    // The draw registers its chunks are drawn under: the game thread's
    // where the call's marker went (rg::drawstate::game()), its writes
    // applied.
    rg::drawstate::Registers regs{};
};
// An item's: the model matrix (60-63) and the mesh header (76-79, 81, 42),
// from its packets p14 and p18; its groups, in the order they are drawn.
struct Item
{
    std::array<Quad, 4> model{};
    Quad q76{}, q77{}, q78{}, q79{}, q81{}, q42{};
    std::vector<Group> groups;
    uint32_t mesh = 0u, p14 = 0u;
    // Its VIF BASE (the chunks' TOP, mesh+0x00) and its palette (p1C's
    // quads: `bones` x 4 from p1C+0x20 to quad `paletteAt`).
    uint32_t base = 0u, p1C = 0u, paletteAt = 0u, bones = 0u;
};
// M6: the VU1 state every model program reads besides its own call's (the
// view, camera and light packets: quads 0-59), as the list had put it where
// a record's or a stream's marker goes: the quads set so far (`known`, a bit
// a quad) and their values. Put in VU1 memory before the record draws.
struct Globals
{
    std::array<Quad, 60> quads{};
    uint64_t known = 0u;
};
struct Call
{
    uint32_t unit = 0u;
    Globals globals;
    std::vector<Item> items;
    // One node of the sorted path (one item, one group): its item's
    // packets may or may not be CALLed before it (they are only when they
    // changed), so its MSCALs do not count items.
    bool single = false;
};

// A call the game did not write (its marker reached): its record.
bool takeUnwritten(uint32_t unit, Call &out);
// A captured stream's marker (kind 8) reached: what the emitter wrote, as
// the VIF stream the list would have carried there, and the draw registers
// the game thread had where it wrote it.
bool takeStream(uint32_t unit, std::vector<uint8_t> &bytes, rg::drawstate::Registers &regs, Globals &globals);
// DrawListFrameBegin (game thread, src/native_effects.cpp): the frame's list
// begins at `start` (the VU1 state goes on from the last frame's).
void listBegun(uint32_t start);

// After loadELF(), with the native effects (the markers are theirs).
void install(PS2Runtime &runtime);

// The list side (game thread, as the list runs): a marker's unit (the begin
// and end of an emit call's output), each MSCAL (0: an item's base, 0x320: a
// group's A), and each model chunk drawn, with VU1 data memory as it is
// and the native program the list ran it with.
// True: a marker of this module's; `unwritten` set for one of a call the
// game did not write (takeUnwritten then has its record).
bool marker(uint32_t unit, bool &unwritten);

} // namespace rg::nativeemit
