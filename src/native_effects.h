#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

class PS2Runtime;

// Post effects drawn by the renderer itself instead of as the GS sprites the
// game builds for them (docs/NATIVE-RENDERER.md, phase 3).
//
// The game's function still runs, so all it does besides drawing stays as
// it was; a hook notes which of the frame's packet memory it wrote. The VIF1
// thread holds the packets it reads from there, turns their sprites into a
// GSNativeEffect (each sprite as the GS would draw it, its strips joined),
// and sends the packets on with their kicks withheld (XYZ2 as XYZ3: the GS
// registers end as they would have), then the effect. Packets that are not
// the shape expected go on as they are.
//
// RG_NATIVE_GLARE=1: GlareFilterDrawMaybe (stage A: the frame's bright parts
// down to 192x192, 20 shifted adds of that buffer onto itself, back onto the
// frame). RG_NATIVE_EFFECTS_LOG=1 prints the first effect built and why any
// was not.
namespace rg::nativefx
{

enum class Effect : uint8_t
{
    None,
    Glare,
};

bool enabled();

// After loadELF(): hooks the effects' functions.
void install(PS2Runtime &runtime);

// The effect whose packets the EE address holds (any of RDRAM's windows).
Effect effectAt(uint32_t address);

// From the packets an effect's function wrote, in order: the effect, and
// the packets with their kicks withheld. False, with `why`, when they are
// not the sprites expected.
bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why);

} // namespace rg::nativefx
