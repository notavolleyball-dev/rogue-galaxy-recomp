#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

class GS;

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
// draws) and CBlurDrawMaybe (which lays the displayed frame over the new one
// by 03f80's alpha). RG_NATIVE_BLOOM (likewise): BloomExtractBlurMaybe, as the
// glare. RG_NATIVE_DOF (likewise): DofFilterDrawMaybe. RG_NATIVE_REFRACTION
// (likewise): RefractionCaptureFrame. RG_NATIVE_EFFECTS_LOG=1
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
    // DofFilterDrawMaybe: the frame to 02a00 and laid back over itself.
    Dof,
    // CBlurDrawMaybe (with RG_NATIVE_CBLUR): the displayed frame copied to
    // 02a40, its alpha from 03f80, laid over the frame a pixel or two off.
    CBlurDraw,
    // RefractionCaptureFrame (CharaDrawTranslucentPass's): the frame halved
    // into 03b80, which the refraction material samples.
    Refraction,
    // DrawListBeginPlaneShadowPass: its packets go on as they are; the
    // registers they set are where the shadow's sprites start from.
    ShadowSetup,
    // ModelDrawOffscreenComposite (stage E), an object at a time: the head
    // draws it into the offscreen buffer (its clear, its model), the tail
    // lays that over the frame (src/vif1_thread.cpp batches the tails).
    CompositeHead,
    CompositeTail,
    // RG_NATIVE_EFFECTS_PROBE=1: one of the effects not yet drawn by the
    // renderer (Place::unit: which, probeName), its packets traced and
    // what the builder makes of them logged, then sent on as they are.
    Probe,
    // GfxFlipBuffersMaybe's copy of the frame into the display buffer
    // (RG_NATIVE_FLIP): sent from the game thread, not the VIF1 lists, so
    // taken by flipPacket rather than by packet memory.
    Flip,
    // ScreenColorFilterMaybe (RG_NATIVE_COLOR_FILTER): the scene's tint,
    // built from its arguments (Place::unit: the colour), its packets sent
    // on with their kicks withheld.
    ColorFilter,
    // FocusFilterDrawMaybe (RG_NATIVE_FOCUS) and ShadowMapBlur
    // (RG_NATIVE_SHADOW_MAP): built from their sprites, as the bloom.
    Focus,
    ShadowMapBlur,
    // DrawListBeginPrim to DrawListEndPrim (RG_NATIVE_2D): a block the game
    // built through its GsPkt API (sprites, UI, 2D). Its primitives become
    // native meshes in window coordinates (build2D); only its register
    // state and uploads go on to the GS.
    Prim2D,
};
const char *probeName(uint32_t unit);

// What an EE address of the frame's packet memory holds: an effect's
// packets, and for the composites the object's number.
struct Place
{
    Effect effect = Effect::None;
    uint32_t unit = 0u;
};

bool enabled();
// RG_NATIVE_2D, once its hooks are in.
bool prim2DEnabled();
const char *name(Effect effect);
// RG_NATIVE_COMPOSITE (likewise): the VIF1 thread batches stage E's objects.
bool compositesBatched();

// After loadELF(): hooks the effects' functions.
void install(PS2Runtime &runtime);

// RG_NATIVE_FLIP (with the native renderer unless set). GfxFlipBuffersMaybe
// (RGDecomp's C) sends, from the game thread: a draw environment whose FRAME
// is the display buffer (CT24), then a packet it built once and sends every
// frame: ALPHA 0x8000000064, TEST 0x20002, PRMODE 0x110 and DECAL sprites
// of the frame, 16 pixels wide (nearest; one bilinear sprite when the
// display's height is not the frame's); then the environment back. The
// flip's hook (src/flip_pacing.cpp) marks it; the game thread's packets go
// through flipPacket, which turns the sprites into a Flip effect: one copy
// the renderer does on the GPU instead of 32 draws.
bool flipNative();
void beginFlip();
void endFlip();
// The game thread's own GIF packet: true when it was the flip's copy, with
// the effect and what to send in its place (the sprites' packet, its kicks
// withheld: the GS registers end as they would have), then the effect.
bool flipPacket(const uint8_t *data, uint32_t size, GSNativeEffect &out,
                std::vector<std::vector<uint8_t>> &withheld);

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

// A Prim2D block, in order: register-only packets (state, uploads: sent on
// as they are) and meshes (the primitives, in window coordinates, a triangle
// list each). False when the block holds something it cannot express
// (lines, points, PRMODE attributes, UVs with no TEX0 in the block): then
// the packets go on whole.
// GSNativeMesh::Header::source's top bit on a mesh build2D made from texel
// UVs (FST): u and v are texels, as the FST draw reads them, and the
// backend bounds the texels the mesh samples by them (dq8-gfx's sdlgpu
// backend reads the same bit).
constexpr uint32_t kNativeMeshTexelUvs = 0x80000000u;

struct Piece2D
{
    std::vector<uint8_t> packet; // a packet build2D made, when not empty
    // Or a packet of the input as it is (an upload), in place: valid while
    // the input is.
    const uint8_t *view = nullptr;
    uint32_t viewSize = 0u;
    GSNativeMesh mesh;           // otherwise
    bool isPacket() const { return view != nullptr || !packet.empty(); }
    const uint8_t *bytes() const { return view ? view : packet.data(); }
    uint32_t size() const { return view ? viewSize : static_cast<uint32_t>(packet.size()); }
    // The mesh's draw state, from the converter's own copy of the GS's draw
    // registers (`stated`; drawStateOwned()).
    GSDrawState state{};
    bool stated = false;
};
bool build2D(const std::vector<std::vector<uint8_t>> &packets, std::vector<Piece2D> &out, std::string &why);
// The same over packets in place (no copy of them first).
struct Span2D
{
    const uint8_t *data;
    uint32_t size;
};
bool build2D(const Span2D *packets, size_t count, std::vector<Piece2D> &out, std::string &why);
// A packet going to the GS as it is, not through build2D, while a
// conversion is on: the state build2D mirrors (PRIM, the vertex registers,
// PRMODECONT, PRMODE) follows it, as the GS's does.
void observe2D(const uint8_t *data, uint32_t size);

// RG_NATIVE_DRAW_STATE (on with RG_NATIVE_ALL_PACKETS; docs/NATIVE-SUBMIT-PLAN.md
// phase 2): the converter keeps the GS's draw registers itself (both
// contexts, PRIM, PRMODE, PRMODECONT, TEXA, ... decoded as GS::writeRegister
// decodes them, one copy for every thread, which take turns at the GS), and
// each mesh it makes carries its draw state (Piece2D::state). The draw
// registers of converted packets do not go to the GS front end: they are
// owed to it, and sent just before anything reads its state (a packet that
// goes to it as it is, a native effect, a libgraph stub), with
// takeOwedRegisters. Transfers, TEXFLUSH, SIGNAL, FINISH and LABEL still go.
bool drawStateOwned();

// Main-memory ranges the game's texture uploads read (TexBuildUploadPkt_158610:
// the image nodes' mips and palettes), seen in the last two frames: what the
// VIF1 list copy may leave in place (src/vif1_thread.cpp).
bool textureSource(uint32_t address, uint32_t bytes);
// The draw state now (for a mesh made elsewhere, a VU1 program's).
GSDrawState mirroredDrawState();
// The owed registers as one A+D packet; false when none are owed.
bool takeOwedRegisters(std::vector<uint8_t> &packet);
// After something wrote the front end's registers directly: read them back.
void readDrawRegisters(const GS &gs);
// The 8 KiB pages of GS memory (block address / 32) a box covers.
void boxPages(const Box &box, uint32_t &first, uint32_t &last);
// When a packet starts a transfer into GS memory: the pages it may write.
bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last);

// `unit`: a probe's (Place::unit).
bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why, uint32_t unit = 0u);

} // namespace rg::nativefx
