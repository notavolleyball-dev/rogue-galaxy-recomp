#pragma once

#include <cstdint>

class PS2Runtime;

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

// After loadELF(), with the native effects (the markers are theirs).
void install(PS2Runtime &runtime);

// The list side (game thread, as the list runs): a marker's unit (the begin
// and end of an emit call's output), each MSCAL (0: an item's base, 0x320: a
// group's A), and each model chunk drawn, with VU1 data memory as it is
// and the native program the list ran it with.
bool marker(uint32_t unit);
void mscal(uint32_t startPC);
void chunk(const uint8_t *vu1Data, uint32_t top, const void *program);

} // namespace rg::nativeemit
