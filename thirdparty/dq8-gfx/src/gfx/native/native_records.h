#pragma once

// The native pipeline's records (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md, "The native interface").
//
// What producers (the game's hooks, its VU1 program starts, the packet
// converter) hand the native renderer, in the order the game built them.
// Every reference is an id the producer assigned; nothing here is a GS
// address, register, page or packet.
//
// Units (the contract v2 of the game's docs/NATIVE-ONLY.md): colors and
// alphas are floats in the draw's units, which the producer chooses: 1.0 is
// the identity of the texture functions and of an alpha as a blend factor,
// and values past it are legal (src/native_producer.h gives the game's).
// Targets keep them as they are (a color target is RGBA16F on the GPU);
// Present::scale maps them to the screen. Nothing here clamps, rounds or truncates as the PS2 did,
// apart from what a material asks for (colorClamp). This header, and the library it
// belongs to (dq8_gfx_native), must not include anything of the GS: the
// library does not link the runtime, so a GS header does not compile here.

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace dq8::gfx::native
{
using Id = uint32_t; // 0 is none

enum class PixelFormat : uint8_t
{
    Rgba8,   // 8 bits a channel, worth Texture::scale each at full
    Index8,  // palette indices, one byte each (4-bit art is widened to this)
    // The GS's CT16 as the game has it, two bytes a texel: red in bits 0-4,
    // green 5-9, blue 10-14, the alpha bit 15. The shader widens the
    // channels (by a shift, as the GS: worth Texture::scale at full) and
    // takes the alpha from the draw's TEXA (Material::texaLow, texaHigh,
    // alphaZeroOnBlack). Also a palette's entries.
    Ct16,
    // The GS's T4 as the game has it: two indices a byte, the low nibble
    // first, rows of width / 2 bytes (an even width). The shader takes the
    // nibble.
    Index4,
    Depth32, // a depth target
    // A replacement picture's blocks as its file has them (a texture pack's
    // DDS), 4x4 texels a block, a row of blocks after another.
    Bc1,
    Bc2,
    Bc3,
    Bc7,
};

// A texture's texels as the producer decoded them: shared, never copied.
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

struct Texture
{
    Id id = 0;
    PixelFormat format = PixelFormat::Rgba8;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mips = 1;
    Id palette = 0; // an Index8 texture's own palette, when the producer knows it
    Bytes texels;   // mip 0 first, rows in order
    // What a texel channel at its full 8 bits is worth in the draw's units.
    float scale = 1.0f;
    // The size the game knows it at, which its draws address it by; 0: its
    // own. A texture pack's picture of a texture is larger than the game's.
    uint16_t logicalWidth = 0;
    uint16_t logicalHeight = 0;
    // Its texels are this target's, as a colour read kept it on the GPU
    // (ColorRead::keep) at the renderer's resolution, addressed at the
    // game's size (width, height); `texels` stand in when the target is not
    // there. 0: none.
    Id fromTarget = 0;
};

struct Palette
{
    Id id = 0;
    uint16_t entries = 0; // 16 or 256, in index order
    PixelFormat format = PixelFormat::Rgba8; // or Ct16
    Bytes colors;         // `format` each
    float scale = 1.0f;   // as Texture::scale
};

enum class TargetRole : uint8_t
{
    SceneColor,
    SceneDepth,
    Display,
    Offscreen, // an effect's buffer, an object layer, a render-to-texture
};

// The renderer chooses each target's resolution: width and height are the
// game's, in its own pixels.
struct Target
{
    Id id = 0;
    TargetRole role = TargetRole::Offscreen;
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat format = PixelFormat::Rgba8;
};

// Camera ids count up, and a producer keeps at most this many of the newest
// in use (it forgets them all at that size and sends again): the renderer
// may drop one older than twice that behind the newest it was sent.
constexpr uint32_t kCameraWindow = 4096u;

// Row vectors. The projection is a standard one: to clip space with x and
// y in [-w, w] (y up) over the viewport, and z in [0, w], reversed (w is
// the near plane). The viewport is the rectangle of the target the clip
// space covers, in the target's pixels at the game's size.
struct Camera
{
    Id id = 0;
    float view[16] = {};
    float projection[16] = {};
    float viewport[4] = {}; // x, y, width, height
    float jitter[2] = {};   // in output pixels, for a temporal pass
};

// Blending: color = Cs * src + Cd * dst (or their difference, by op), with
// the alphas as they are (the source's as drawn, the target's as stored;
// past 1.0 too); Fixed is Material::blendConstant. A target's stored alpha
// is the source's, unblended.
enum class BlendFactor : uint8_t
{
    Zero,
    One,
    SrcAlpha,
    InvSrcAlpha,
    DstAlpha,
    InvDstAlpha,
    Fixed,
    InvFixed,
};

enum class BlendOp : uint8_t
{
    Add,             // Cs * src + Cd * dst
    Subtract,        // Cs * src - Cd * dst
    ReverseSubtract, // Cd * dst - Cs * src
};

enum class Compare : uint8_t
{
    Always,
    Never,
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
    NotEqual,
};

// How the texel and the vertex color combine (Cf the vertex's, Ct the
// texel's).
enum class TextureFunction : uint8_t
{
    Modulate,   // Cf * Ct
    Decal,      // Ct
    Highlight,  // Cf * Ct + Af, alpha Af + At
    Highlight2, // Cf * Ct + Af, alpha At
};

// What a pixel that fails the alpha test still writes.
enum class AlphaFail : uint8_t
{
    Nothing,
    ColorOnly, // color and alpha, not depth
    DepthOnly,
    RgbOnly,   // color without alpha, not depth
};

// A channel moved within the texel before the texture function (an effect
// reading its own target).
enum class ChannelShuffle : uint8_t
{
    None,
    // Alpha's bits 2-7 from green's, its bits 0-1 kept, a channel's value
    // times Material::shuffleScale being its byte (the game's CBlur: the
    // GS's 16-bit view of a 32-bit buffer copying each pixel's RG half onto
    // its BA half with R and G masked). Goes when M3 makes CBlur a Pass.
    GreenToAlpha,
    // Blue from red and alpha from green, whole (the same copy unmasked:
    // the shadow map's blur). A pass's only.
    RedGreenToBlueAlpha,
};

struct Material
{
    Id texture = 0; // 0: untextured
    Id palette = 0; // an Index8 texture's, chosen at the draw; 0: the texture's own
    // Instead of a texture: a target's color as drawn so far this frame (an
    // effect reading a buffer back), in the target's units.
    Id sourceTarget = 0;
    // The texel's alpha replaced by this when not negative: a buffer read
    // without its alpha (the GS's CT24 view, TEXA's TA0), and 0 where the
    // texel is black when alphaZeroOnBlack (TEXA's AEM).
    float sourceAlpha = -1.0f;
    bool alphaZeroOnBlack = false;
    // A CT16 texel's alpha (texture or palette entry, PixelFormat::Ct16):
    // texaHigh with the alpha bit, else texaLow (TEXA's TA1 and TA0), 0
    // where the texel is black when alphaZeroOnBlack (AEM).
    float texaLow = 0.0f, texaHigh = 1.0f;
    // The color after the texture function is clamped to [0, this] when
    // above 0.
    float colorClamp = 0.0f;
    // See ChannelShuffle::GreenToAlpha.
    float shuffleScale = 0.0f;
    // The size the uvs are normalized to; 0: the texture's own.
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    // Subtracted from the texel coordinate (uv times the size above) before
    // the texture is sampled: a texture cut from another (a tile of a sheet)
    // addressed as the sheet was.
    float texelOffset[2] = {0.0f, 0.0f};
    bool linear = false;
    bool clampU = true; // false: repeat
    bool clampV = true;
    TextureFunction function = TextureFunction::Modulate;
    ChannelShuffle shuffle = ChannelShuffle::None;
    bool textureAlpha = true; // the texel's alpha is used; false: the vertex's
    bool blending = false;
    BlendFactor srcFactor = BlendFactor::One;
    BlendFactor dstFactor = BlendFactor::Zero;
    BlendOp blendOp = BlendOp::Add;
    float blendConstant = 1.0f; // Fixed's
    Compare alphaTest = Compare::Always;
    float alphaReference = 0.0f;
    AlphaFail alphaFail = AlphaFail::Nothing;
    // Drawn only where the target's alpha is below 1.0 (0) or at least 1.0
    // (1), as it was before the draw; -1: everywhere.
    int8_t destinationAlphaTest = -1;
    // Depth grows toward the camera (reversed): Greater passes a nearer pixel.
    Compare depthTest = Compare::Always;
    bool depthWrite = false;
    // Added to the depth (times w): an overlay laid on a surface drawn
    // before it, pulled toward the camera so their depths do not fight.
    float decalBias = 0.0f;
    uint8_t colorWrite = 0xF; // bits R, G, B, A
    bool cullBack = false;
    bool fog = false;
    // The producer resolved every field from what the game meant, with no
    // fallback (the readiness counter R4).
    bool known = false;
};

struct Rect
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // in the target's game pixels, x1 and y1 exclusive
};

struct Vertex
{
    float position[3] = {}; // object space
    float uv[2] = {};
    float color[4] = {};    // in the draw's units
    float normal[3] = {};   // when MeshDraw::hasNormals
    // A window-space draw's fog factor, per vertex (1 none, toward the fog
    // color as it falls to 0); a draw with a model has MeshDraw::fog instead.
    float fog = 1.0f;
};

// A model chunk's vertex as the game's VU1 program reads it, before the
// program (M5 of the game's docs/NATIVE-ONLY.md): the int values of its
// position, normal, UV and color, its bones' palette slots (0-35) and their
// weights (0-255; 0: none). native_model.vert runs the program on it.
struct ModelInput
{
    float position[3];
    float bones[4];
    float weights[4];
    float normal[3];
    float uv[2];
    float color[4];
};
static_assert(sizeof(ModelInput) == 80u, "tightly packed");

// What the model vertex stage reads for one draw, made per draw from the
// VU1 constants (vu1_native's modelRow; the layout is native_model.vert's).
constexpr uint32_t kModelRowQuads = 48u;
struct ModelRow
{
    float q[kModelRowQuads][4];
};

// With hasModel, vertices are in object space, through the model and the
// camera. Without it, they are the target's pixels (x, y) and the game's
// window depth (z), which the camera, when there is one, maps to the
// renderer's depth: a 2D draw that tests against the scene's 3D draws.
struct MeshDraw
{
    Id target = 0;
    Id depthTarget = 0;
    Id camera = 0;      // 0: no camera (a screen-space draw)
    bool hasModel = false;
    // Object to world, row vectors, when hasModel. With no camera, object
    // straight to the target's pixels, homogeneous (divide by w).
    float model[16] = {};
    uint64_t object = 0;  // stable across frames for the same object; 0 unknown
    Material material;
    Rect scissor;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangles; empty: vertices are a strip
    bool hasNormals = false;
    // With material.fog: the fog factor F = clamp(fog[0] + fog[3] * w, at
    // most fog[1], at least fog[2]), w the clip-space w; 1 is no fog,
    // toward fogColor (in the draw's units) as F falls to 0.
    float fog[4] = {};
    float fogColor[3] = {};
    uint32_t source = 0; // what made it (a VU1 program, a hook), for profiles
    // The model the same object had in the frame before (motion vectors,
    // TAA), when the producer knows it.
    bool hasPreviousModel = false;
    float previousModel[16] = {};
    // With hasModel: the depth is the camera's d (clip z / w), made
    // depthScale d + depthBias, then capped at depthCap per vertex (in
    // front of the eye). The game draws decals (lights on walls, layers on
    // the ground) slightly in front of the surface they lie on this way.
    float depthScale = 1.0f;
    float depthBias = 0.0f;
    float depthCap = 3.402823466e38f;
    // A model chunk's (hasModelRow): the model stage caps each vertex's
    // color at modelColorMax, then times modelColorScale it is in the
    // draw's units.
    float modelColorMax[4] = {};
    float modelColorScale = 1.0f;
    // A retained mesh (the game's docs/NATIVE-BOUNDARY.md, issue 2): a
    // chunk the producer saw unchanged from one frame to the next, kept by
    // the renderer in a vertex buffer of its own under this id (a
    // RetainedMesh record sent before its first draw). The program's cull is
    // then judged on the GPU, from these: the program's matrix to GS window
    // coordinates (row vectors), its guard band, its winding sign, and
    // which tests it makes. An id the renderer no longer holds is not
    // drawn (retainedEpoch tells the producer to start again).
    Id retained = 0;
    float cullMatrix[16] = {};
    float bandScale[4] = {}, bandOffset[4] = {};
    float winding = 1.0f;
    bool guardBand = false, backface = false, clips = false;
    // A model chunk's draw (its RetainedMesh has `model`): the program's
    // constants, and its bones' matrices (4 rows of 4 floats a slot, row
    // vectors, slot x at palette[16 x]).
    bool hasModelRow = false;
    ModelRow modelRow{};
    std::vector<float> palette;
    // A model chunk drawn this once (with hasModelRow, no `retained`): its
    // inputs ride with the draw (a strip, or a list with modelList).
    std::vector<ModelInput> modelInputs;
    bool modelList = false;
    // An outline chunk's (its row's flag 16): its inputs are edges of this
    // many points (4: the far corner of the first triangle, the edge's two
    // ends, the second's far corner; 2: the edge alone), each drawn as four
    // lines (native_model.vert's outline()).
    uint32_t modelEdge = 0u;
    // A shadow volume draw's (its row's flag 64): its inputs a list of
    // triangles, each drawn as its volume's five fans (native_model.vert's
    // volume()).
    bool modelVolume = false;
};

// What a 2D draw belongs to, for passes that treat the layers apart (the
// HUD at output resolution, the menus over a post effect). Until M4 of the
// game's docs/NATIVE-ONLY.md tags each draw at its source, every one is
// Scene.
enum class Layer : uint8_t
{
    Scene,   // part of the picture: effects' sprites, sprites in the world
    Hud,
    Menu,
    Text,
    Fade,
    Overlay,
};

// A draw in the target's own space: positions are the target's pixels at
// the game's size (640x448 for the scene). With a depth target, each
// vertex's z is a depth in the scene's convention (reversed: 1 near), and
// `camera` the camera whose depth that is.
struct Draw2D
{
    Layer layer = Layer::Scene;
    Id target = 0;
    Id depthTarget = 0;
    Id camera = 0;
    Material material;
    Rect scissor;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangles; empty: vertices are a strip
    // With material.fog: the color each vertex's fog factor (Vertex::fog)
    // goes toward, in the draw's units.
    float fogColor[3] = {};
    uint32_t source = 0;
};

// A post effect the renderer draws itself (M3 of the game's
// docs/NATIVE-ONLY.md), from the hooked function's arguments: `frame` is
// the target it reads and draws onto (the scene), `work` the game's own
// buffers it uses (by the kind's convention), `params` its parameters.
// native_passes.cpp has what each kind draws.
enum class PassKind : uint8_t
{
    // GlareFilterDrawMaybe: work[0] the frame's bright parts, work[1] their
    // streaks. params: 0 the alpha a pixel must be over to glow, 1 the
    // streaks' strength, 2 their spread in sixteenths of a pixel, 3 the
    // strength laid over the frame, 4 the color clamp (what the game's
    // blends clamp to).
    Glare,
    // BloomExtractBlurMaybe: work[0] the frame's alpha under 1 as grey, then
    // blurred, work[1] its partner. params: 0 the blur's offset in
    // sixteenths of a pixel, 1-3 the color it is added onto the frame at,
    // 4 its alpha.
    Bloom,
    // DofFilterDrawMaybe: work[0] the levels (each a blurred copy of the
    // one before, side by side). params: 0 the levels' count (at most 7),
    // 1 the alpha they are laid over the frame at, then two depths a level
    // (the renderer's): where the scene is behind them, the level shows.
    DepthOfField,
    // DrawListEndPlaneShadowPass: work[0] the shadow volumes' mask,
    // work[1] its blurred copy. params: 0 the shadow's strength, 1-3 its
    // color, 4 flags (1 blurred, 2 its red and green made its blue and
    // alpha, 4 colored), 5 the color clamp, 6-17 the blur's six offsets
    // (x, y) in pixels.
    PlaneShadow,
    // CBlurTexBlurMaybe: the trail work[0] (P), its history work[1] (Q).
    // params: 0 the feedback blur (else the history's), 1 the history's
    // strength, 2 the color clamp, 3-10 the feedback's four taps (u, v) in
    // texels.
    CBlur,
    // ScreenColorFilterMaybe: the frame's alpha rebuilt from its green, its
    // color then params 0-2 times that alpha. params 3: the green-to-alpha
    // shuffle's scale (ChannelShuffle::GreenToAlpha).
    ColorFilter,
    Fade,
    // The frame copied whole into work[0], scaled to its size (the
    // refraction's picture).
    Capture,
    TexAnime,
    // FocusFilterDrawMaybe: work A, A', C, C' (two pairs of partners).
    // params: 0-3 four depths (the renderer's), 4-7 the alpha the frame
    // gets where the scene is behind each, 8 the first blur's lerp, 9 the
    // mask's color, 10 the composite's alpha, 11-12 the blurs' offsets in
    // pixels.
    Focus,
    // ShadowMapBlur: work[0] the shadow map, work[1] its partner. params: 0
    // the first copy's color, 1 the blur's, 2-9 the four offsets (x, y) in
    // pixels.
    ShadowMap,
    Flare,
    // CBlurDrawMaybe: the trail work[0] (P), the sprite work[1] (S).
    // params: 0-1 the shift (dx, dy), 2 the blend preset (the game's table
    // at 0x449EF0), 3 S is small (a color, not the frame's copy), 4-7 the
    // color, 8-9 the draw environment's size, 10-11 its frame's.
    CBlurDraw,
    // ModelDrawOffscreenComposite's tail, a draw each (its overlay calls,
    // its blur mask): work[0] the object's layer (or its buffer), work[1]
    // the character blur's trail. params: 1 into work[1] alpha only (else
    // the frame), 2-5 the rectangle in the target's pixels, 6-9 its texels,
    // 10-13 the color, 14 the blend (0 none, 1 (Cs - Cd) As + Cd, 2 the same
    // by FIX), 15 FIX, 16 the alpha test (5 GEQUAL, 6 GREATER), 17 its
    // reference, 18 the source read as CT24 with black clear, 19 how many
    // offsets, 20-27 the offsets (x, y) in pixels.
    ObjectLayer,
};

struct Pass
{
    PassKind kind = PassKind::Glare;
    Id frame = 0;
    Id work[4] = {};
    Id depth = 0; // the frame's depth, for a pass that tests against it
    float params[32] = {};
};

// A target, or a rectangle of it, set to a color, a depth or both, in order
// with the draws.
struct Clear
{
    Id target = 0;
    Rect rect;                 // empty: the whole target
    bool color = false;
    float rgba[4] = {};        // in the target's units
    bool depth = false;
    float depthValue = 0.0f;   // reversed: 0 is the far plane
};

struct FrameBegin
{
    uint64_t frame = 0;
};

// The end of a frame: `target`'s `rect` is shown.
struct Present
{
    Id target = 0;
    Rect rect; // what of the target is shown
    uint64_t frame = 0;
    // The target's color times this is shown (clamped to [0, 1]).
    float scale = 1.0f;
};

// A retained mesh's vertices (MeshDraw::retained), sent once: a strip, or
// with `list` a triangle list.
// A model chunk the renderer keeps (M5): its vertices as the program reads
// them (a strip, or a list with `list`), drawn through native_model.vert
// from MeshDraw::modelRow and its palette.
struct RetainedMesh
{
    Id id = 0;
    bool list = false;
    std::vector<ModelInput> model;
};

using Record = std::variant<Texture, Palette, Target, Camera, MeshDraw, Draw2D, Pass, Clear, FrameBegin, Present,
                            RetainedMesh>;
} // namespace dq8::gfx::native
