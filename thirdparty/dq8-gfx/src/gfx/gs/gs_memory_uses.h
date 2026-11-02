#pragma once

#include <cstdint>
#include <string>

// What still uses GS local memory on the native path (Rogue Galaxy: added).
//
// With DQ8_GFX_NATIVE_BYPASS the goal is that nothing does, so that local
// memory need not be kept at all. Each place that still reads or writes it
// notes a use here, by kind; DQ8_GFX_WARN_MEMORY=1 prints the first of each
// distinct shape, and the [native-memory] line carries the counts.
namespace dq8::gfx::memoryuses {

enum class Kind : uint8_t {
    TextureFromMemory, // a texture no name, target or GPU path served: decoded from memory
    LocalCopy,         // a local-to-local copy made in memory
    UploadToMemory,    // an upload written into memory (not bypassed)
    HostRead,          // a GS-to-host read answered from memory
    Resolve,           // a render target's pixels written back into memory
    Count,
};

// `shape` names what it was (formats, addresses, sizes): printed once per
// distinct shape and kind when DQ8_GFX_WARN_MEMORY=1.
void note(Kind kind, const char *shape);
uint64_t count(Kind kind);
std::string summary(); // "texture N, copy N, upload N, read N, resolve N"

} // namespace dq8::gfx::memoryuses
