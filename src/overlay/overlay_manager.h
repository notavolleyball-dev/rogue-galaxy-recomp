#pragma once

#include <cstdint>

class PS2Runtime;

// Dispatch for the MWo3 code overlays (config/SCUS_974.90/overlays.toml).
//
// All five code overlays are linked for the same arena, so an arena address
// means different code depending on which one is resident. The game's loader
// calls a `jr ra` stub with a0 = the overlay header after every read and
// before .bss and the constructors; the manager replaces that stub, checks
// the header against the one each tree was generated from, and points the
// runtime's FunctionRegion resolver at that overlay's table.
namespace rg::overlay
{

constexpr uint32_t kArenaStart = 0x004E9E80u;
constexpr uint32_t kArenaEnd = 0x00528200u;
constexpr uint32_t kHeaderSize = 0x40u;
constexpr uint32_t kImageLoadedHook = 0x00100400u; // OverlayLoadNotifyStubMaybe_100400

// Call after PS2Runtime::loadELF(), which fills the table the hook lives in.
bool install(PS2Runtime &runtime);

// Name of the resident overlay ("none" before the first load).
const char *residentName();

// Overlay loads seen so far.
uint32_t loadCount();

} // namespace rg::overlay
