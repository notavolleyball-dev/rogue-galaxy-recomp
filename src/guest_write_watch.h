#pragma once

#include <cstddef>
#include <cstdint>

// Writes to guest RAM, watched a page at a time: a watched page is kept
// read-only, and the first write to it (from any thread) faults, marks the
// page written and makes it writable again. A range hashed once can then be
// known unchanged without reading it (the texture uploads the game sends
// every frame, nativeproducer::texture). Kernel writes into guest RAM go
// through g_beforeHostWrite (runtime/guest_host_writes.h), which unwatches
// their destination first.
//
// Only whole pages inside guest RAM are watched; a range touching another
// is never "unchanged". Linux only; elsewhere, and with RG_WRITE_WATCH=0,
// nothing is watched.
namespace rg::writewatch
{

// Once, with guest RAM: installs the fault handler and the host-write hook.
void init(uint8_t *ram, size_t size);
bool enabled();

// Watches the range's pages; the stamp to give `unchanged` later (taken
// before the caller reads the range), 0 when it cannot be watched.
uint64_t watch(const uint8_t *at, size_t size);

// Whether no byte of the range was written since `stamp` (watch's).
bool unchanged(const uint8_t *at, size_t size, uint64_t stamp);

// Makes the range writable without a fault (a host write coming), as a
// write to it would.
void unwatch(void *at, size_t size);

} // namespace rg::writewatch
