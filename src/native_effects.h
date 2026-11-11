#pragma once

#include <cstdint>
#include <string>
#include <vector>


class PS2Runtime;

// The game's post effects as the native pipeline's own draws (M3 of
// docs/NATIVE-ONLY.md), instead of the GS sprites the game builds for them.
//
// Each hooked function's arguments become a pass request
// (rg::nativeproducer::PassRequest) kept under a unit, and a marker for the
// unit is written into the draw list where the call's output is (M6: the
// only thing of the call's the list carries for drawing; its 2D blocks are
// not written, src/native_blocks.cpp). A function with no side effect but
// its packets is not run. The list's run, reaching the marker, sends the
// Pass in the stream's order; the renderer expands it into its own draws
// (native_passes.cpp). Each function hooked has a note in
// docs/NATIVE-ONLY-NOTES.md.
//
// RG_NATIVE_EFFECTS_LOG=1 logs the effects' first passes.
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
    // ModelDrawOffscreenComposite (stage E), an object at a time: the head
    // draws it into the offscreen buffer (its clear, its model), the tail
    // lays that over the frame. Drawn into an object layer of the
    // renderer's own, between the head's marker and the tail's
    // (compositeBegin, the composite's after-callback), the tail's passes
    // from there.
    CompositeTail,
    // GfxFlipBuffersMaybe's copy of the frame into the display buffer: sent
    // from the game thread, not the VIF1 lists, so taken by flipPacket
    // rather than by packet memory.
    Flip,
    // ScreenColorFilterMaybe: the scene's tint, built from its arguments.
    ColorFilter,
    // FocusFilterDrawMaybe and ShadowMapBlur: built from their arguments
    // and the frames they set, as the bloom.
    Focus,
    ShadowMapBlur,
};


// Whether any effect is on (the native pipeline is, and not every effect's
// variable is 0).
bool enabled();
const char *name(Effect effect);

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

// M4 (src/native_blocks.cpp): a block's marker, and the layer of the draw
// task running now.
// A marker for `unit` at the draw list's write cursor (a cnt tag, NOP and
// DIRECT 2, an A+D write of the unit to kPassRegister): the draw stage
// sends what is kept under it there. False: no list being built.
bool writeMarker(uint32_t unit);
// A pass effect's hooked call is in progress (game thread): a 2D block
// drawn inside one is its pass's.
bool inEffectCall();
// ModelDrawOffscreenComposite's call is in progress; and past its head (a
// part emitted: its blocks are then the tail's, which its passes draw).
bool inCompositeCall();
bool compositeTail();
uint8_t currentLayer();

// M4: the layer (native::Layer's value) of the draw task whose packets the
// EE address holds; 0 (Scene) for none.
uint8_t layerAt(uint32_t address);
// M4's census: the address of the draw task (TaskDraw*) whose packets the
// EE address holds; 0 for none.
uint32_t taskAt(uint32_t address);



// A composite's head: the box its clear sprites cover in the offscreen
// buffer (CT32 at `base`, `fbw`), in pixels, x1 and y1 exclusive.
struct Box
{
    uint32_t base = 0u;
    uint32_t fbw = 0u;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// Game thread, at the head's clear (src/native_blocks.cpp): its layer
// begun there, a marker written with the box (once per composite; false:
// none in progress, or already begun). The list's side takes the box at
// the marker (by its unit).
bool compositeBegin(const Box &box);
bool takeCompositeBox(uint32_t unit, Box &box);


// M3: an effect sent as a Pass record (native_records.h) in place of its
// packets: whether it is one, and the pass of the call whose packet range
// carries `unit`, sent (false: not found, or its buffers unknown).
bool isPass(Effect effect);
// Game thread: DrawEnvSetFrameFromDesc_144fe0 was called with `desc`
// (native_capture.cpp's hook); an effect's call in progress may record it.
void noteFrameDesc(uint32_t desc);
bool emitPass(Effect effect, uint32_t unit, bool fromLayer = false);
// M3 step 2: a pass marker's (writePassMarker) register, and its pass sent.
constexpr uint8_t kPassRegister = 0x7Du;
bool emitPassUnit(uint32_t unit);
// The packet with every drawing kick made a plain vertex write: what is left
// of a pass's packets (uploads, registers) still goes on.
void withholdKicks(std::vector<uint8_t> &bytes);

} // namespace rg::nativefx
