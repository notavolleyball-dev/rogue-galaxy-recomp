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
// RG_NATIVE_GLARE (with the native renderer unless set): GlareFilterDrawMaybe (stage A: the frame's bright parts
// down to 192x192, 20 shifted adds of that buffer onto itself, back onto the
// frame). RG_NATIVE_SHADOW (likewise): DrawListEndPlaneShadowPass (stage F: the
// shadow volumes' mask, blurred, onto the frame). RG_NATIVE_CBLUR (likewise):
// CBlurTexBlurMaybe (03f80, a GS buffer the effect takes over while it
// draws). RG_NATIVE_BLOOM (likewise): BloomExtractBlurMaybe, as the
// glare. RG_NATIVE_EFFECTS_LOG=1
// prints the first of each effect built and why any was not.
namespace rg::nativefx
{

enum class Effect : uint8_t
{
    None,
    Glare,
    Shadow,
    // CBlurTexBlurMaybe: the 64x64 buffer at 03f80 decayed, added to and
    // its alpha taken from its green, a frame at a time.
    CBlur,
    // BloomExtractBlurMaybe: the frame's bright parts blurred in 02a00 and
    // 02c40 and added back, as the glare.
    Bloom,
    // DrawListBeginPlaneShadowPass: its packets go on as they are; the
    // registers they set are where the shadow's sprites start from.
    ShadowSetup,
    // ModelDrawOffscreenComposite (stage E), an object at a time: the head
    // draws it into the offscreen buffer (its clear, its model), the tail
    // lays that over the frame (src/vif1_thread.cpp batches the tails).
    CompositeHead,
    CompositeTail,
};

// What an EE address of the frame's packet memory holds: an effect's
// packets, and for the composites the object's number.
struct Place
{
    Effect effect = Effect::None;
    uint32_t unit = 0u;
};

bool enabled();
const char *name(Effect effect);
// RG_NATIVE_COMPOSITE (likewise): the VIF1 thread batches stage E's objects.
bool compositesBatched();

// After loadELF(): hooks the effects' functions.
void install(PS2Runtime &runtime);

// The effect whose packets the EE address holds (any of RDRAM's windows).
Effect effectAt(uint32_t address);
Place placeAt(uint32_t address);

// From the packets an effect's function wrote, in order: the effect, and
// the packets with their kicks withheld. False, with `why`, when they are
// not the sprites expected.
// A ShadowSetup packet: the registers it writes, for the next shadow.
void observe(Effect effect, const uint8_t *data, uint32_t size);

// A composite's head: the box its clear sprites cover in the offscreen
// buffer (CT32 at `base`, `fbw`), in pixels, x1 and y1 exclusive.
struct Box
{
    uint32_t base = 0u;
    uint32_t fbw = 0u;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
bool clearBox(const std::vector<std::vector<uint8_t>> &packets, Box &box);
// Debugging: the registers and primitives of `packets`, one line each.
void dumpPackets(const char *what, const std::vector<std::vector<uint8_t>> &packets);
// The 8 KiB pages of GS memory (block address / 32) a box covers.
void boxPages(const Box &box, uint32_t &first, uint32_t &last);
// When a packet starts a transfer into GS memory: the pages it may write.
bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last);

bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why);

} // namespace rg::nativefx
