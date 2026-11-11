#pragma once

#include <cstdint>
#include <string>
#include <vector>


class PS2Runtime;

// The game's post effects as the native pipeline's own draws (M3 of
// docs/NATIVE-ONLY.md), instead of the GS sprites the game builds for them.
//
// The game's function still runs, so all it does besides drawing stays as
// it was. Its hook reads the function's arguments into a pass request
// (rg::nativeproducer::PassRequest) kept under the unit of the packet range
// the call wrote. The VIF1 thread, reaching that range, sends the Pass
// (emitPass) in the stream's order and the packets on with their kicks
// withheld (XYZ2 as XYZ3: uploads and registers go on, no draws). The
// renderer expands the pass into its own draws (native_passes.cpp). Each
// function hooked has a note in docs/NATIVE-ONLY-NOTES.md.
//
// RG_NATIVE_EFFECTS_LOG=1 logs the effects' first passes;
// RG_NATIVE_EFFECTS_TRACE=1 dumps the packets of an effect whose pass was
// not sent.
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
    // DofFilterDrawMaybe: the frame to 02a00 and laid back over itself.
    Dof,
    // CBlurDrawMaybe: the displayed frame copied to 02a40, its alpha from
    // 03f80, laid over the frame a pixel or two off.
    CBlurDraw,
    // RefractionCaptureFrame (CharaDrawTranslucentPass's): the frame halved
    // into 03b80, which the refraction material samples.
    Refraction,
    // DrawListBeginPlaneShadowPass: its packets go on as they are; the
    // registers they set are where the shadow's sprites start from.
    ShadowSetup,
    // ModelDrawOffscreenComposite (stage E), an object at a time: the head
    // draws it into the offscreen buffer (its clear, its model), the tail
    // lays that over the frame. The VIF1 thread draws each object into an
    // object layer of the renderer's own and its tail from there
    // (src/vif1_thread.cpp batches them). Place::unit: the object.
    CompositeHead,
    CompositeTail,
    // GfxFlipBuffersMaybe's copy of the frame into the display buffer: sent
    // from the game thread, not the VIF1 lists, so taken by flipPacket
    // rather than by packet memory.
    Flip,
    // ScreenColorFilterMaybe: the scene's tint, built from its arguments
    // (Place::unit: the colour), its packets sent on with their kicks
    // withheld.
    ColorFilter,
    // FocusFilterDrawMaybe and ShadowMapBlur: built from their sprites, as
    // the bloom.
    Focus,
    ShadowMapBlur,
};

// What an EE address of the frame's packet memory holds: an effect's
// packets, and for the colour filter its colour, for a composite its
// object's number.
struct Place
{
    Effect effect = Effect::None;
    uint32_t unit = 0u;
};

// Whether any effect is on (the native pipeline is, and not every effect's
// variable is 0).
bool enabled();
const char *name(Effect effect);
// Stage E's objects as object layers (false only when its hooks failed).
bool compositesBatched();

// After loadELF(): hooks the effects' functions.
void install(PS2Runtime &runtime);

// GfxFlipBuffersMaybe (RGDecomp's C) sends, from the game thread: a draw
// environment whose FRAME is the display buffer (CT24), then a packet it
// built once and sends every frame: ALPHA 0x8000000064, TEST 0x20002, PRMODE
// 0x110 and DECAL sprites of the frame, 16 pixels wide (nearest; one
// bilinear sprite when the display's height is not the frame's); then the
// environment back. The flip's hook (src/flip_pacing.cpp) marks it; the
// game thread's packets go through flipPacket, which turns the sprites into
// a Capture pass: one copy instead of 32 draws.
bool flipNative();
void beginFlip();
void endFlip();
// The game thread's own GIF packet: true when it was the flip's copy, with
// the buffer it reads (the frame) and the one it draws into (the display
// buffer), as GS bases.
bool flipPacket(const uint8_t *data, uint32_t size, uint32_t &source, uint32_t &target);

// The effect whose packets the EE address holds (any of RDRAM's windows).
Effect effectAt(uint32_t address);
Place placeAt(uint32_t address);


// A composite's head: the box its clear sprites cover in the offscreen
// buffer (CT32 at `base`, `fbw`), in pixels, x1 and y1 exclusive.
struct Box
{
    uint32_t base = 0u;
    uint32_t fbw = 0u;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
bool clearBox(const std::vector<std::vector<uint8_t>> &packets, Box &box);
// When a packet starts a transfer into GS memory: the pages it may write.
bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last);

// Debugging: the registers and primitives of `packets`, one line each.
void dumpPackets(const char *what, const std::vector<std::vector<uint8_t>> &packets);

// M3: an effect sent as a Pass record (native_records.h) in place of its
// packets: whether it is one, and the pass of the call whose packet range
// carries `unit`, sent (false: not found, or its buffers unknown).
bool isPass(Effect effect);
// Game thread: DrawEnvSetFrameFromDesc_144fe0 was called with `desc`
// (native_capture.cpp's hook); an effect's call in progress may record it.
void noteFrameDesc(uint32_t desc);
bool emitPass(Effect effect, uint32_t unit, bool fromLayer = false);
// The packet with every drawing kick made a plain vertex write: what is left
// of a pass's packets (uploads, registers) still goes on.
void withholdKicks(std::vector<uint8_t> &bytes);

} // namespace rg::nativefx
