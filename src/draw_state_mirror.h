#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "native_types.h"


// The GS draw registers as the game sets them, kept on the game's side for
// the native pipeline (docs/NATIVE-PIPELINE.md): a VU1 mesh becomes a mesh
// draw under the material these registers give it (texture, blend, tests,
// target), read where the mesh is made, in order with the packets before it.
//
// It follows every packet the lists send (observe) and the register writes
// the libgraph stubs make (through vif1's register sink). Removed in M6 of
// docs/NATIVE-ONLY.md, with the packet path.
namespace rg::drawstate
{

// A packet as it goes to the GS: its register writes, in order. Any thread;
// the VIF1 thread and the game thread take turns (the VIF1 thread's lists
// are drained before the game thread sends).
void observe(const uint8_t *data, uint32_t size);
// The same for register writes an A+D packet would carry, without one.
void observeWrites(const std::pair<uint8_t, uint64_t> *writes, size_t count);


// The draw state of now: what a primitive drawn next would be drawn under.
rg::DrawState current();

// A mirror of its own, fed by whoever holds it (M4: the game thread's, from
// the packets the game writes, in its order). Not thread-safe: one owner.
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

private:
    struct Impl;
    Impl *m_impl;
};

} // namespace rg::drawstate
