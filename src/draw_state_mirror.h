#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "runtime/gs/gs_types.h"

class GS;

// The GS draw registers as the game sets them, kept on the game's side for
// the native pipeline (docs/NATIVE-PIPELINE.md): a VU1 mesh becomes a mesh
// draw under the material these registers give it (texture, blend, tests,
// target), read where the mesh is made, in order with the packets before it.
//
// It follows every packet the VIF1 thread sends to the GS (observe), and
// reads the front end's registers back after a libgraph stub has written
// them directly (readBack, patch 0031). Removed in M6 of
// docs/NATIVE-ONLY.md, with the packet path.
namespace rg::drawstate
{

// A packet as it goes to the GS: its register writes, in order. Any thread;
// the VIF1 thread and the game thread take turns (the VIF1 thread's lists
// are drained before the game thread sends).
void observe(const uint8_t *data, uint32_t size);
// The same for register writes an A+D packet would carry, without one.
void observeWrites(const std::pair<uint8_t, uint64_t> *writes, size_t count);

// After something wrote the front end's registers without a packet (the
// libgraph stubs): the mirror takes them from the front end.
void readBack(const GS &gs);

// The draw state of now: what a primitive drawn next would be drawn under.
GSDrawState current();

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
    GSDrawState current() const;

private:
    struct Impl;
    Impl *m_impl;
};

} // namespace rg::drawstate
