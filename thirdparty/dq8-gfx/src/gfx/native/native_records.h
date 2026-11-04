#pragma once

// The native pipeline's records (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md, "The native interface").
//
// What producers (the game's hooks, its VU1 program starts, the packet
// converter) hand the native renderer, in the order the game built them.
// Every reference is an id the producer assigned; nothing here is a GS
// address, register, page or packet. This header, and the library it
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
    Rgba8,   // 8 bits a channel; alpha as the game stores it, 0x80 is opaque
    Index8,  // palette indices, one byte each (4-bit art is widened to this)
    Depth32, // a depth target
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
};

struct Palette
{
    Id id = 0;
    uint16_t entries = 0; // 16 or 256, in index order
    Bytes colors;         // Rgba8 each, alpha as the game stores it
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

// Row vectors, as the game's VU1 data memory holds them.
struct Camera
{
    Id id = 0;
    float view[16] = {};
    float projection[16] = {};
    float viewport[16] = {}; // the game's projection-to-window transform
    float jitter[2] = {};    // in output pixels, for a temporal pass
};

enum class Blend : uint8_t
{
    Opaque,
    Alpha,      // Cs*As + Cd*(1-As)
    Add,        // Cs*As + Cd
    Subtract,   // Cd - Cs*As
    Multiply,   // Cs*Cd
    FixedAlpha, // Cs*F + Cd*(1-F), F in Material::blendFactor
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
// texel's, 0x80 is 1.0 in both).
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

struct Material
{
    Id texture = 0; // 0: untextured
    Id palette = 0; // an Index8 texture's, chosen at the draw; 0: the texture's own
    // The size the uvs are normalized to; 0: the texture's own.
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    bool linear = false;
    bool clampU = true;
    bool clampV = true;
    TextureFunction function = TextureFunction::Modulate;
    bool textureAlpha = true; // the texel's alpha is used; false: the vertex's
    Blend blend = Blend::Opaque;
    uint8_t blendFactor = 0x80; // FixedAlpha's, 0x80 is 1.0
    Compare alphaTest = Compare::Always;
    uint8_t alphaRef = 0;
    AlphaFail alphaFail = AlphaFail::Nothing;
    // Depth grows toward the camera (reversed): Greater passes a nearer pixel.
    Compare depthTest = Compare::Always;
    bool depthWrite = false;
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
    float color[4] = {};    // 0-255, before any clamp
    float normal[3] = {};   // when MeshDraw::hasNormals
};

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
    bool floatDepth = false; // depth written as float (linear or reversed), not the game's integers
    // With material.fog: the fog factor F = clamp(fog[0] + fog[3] * w, at
    // most fog[1], at least fog[2]), w the clip-space w; 255 is no fog,
    // toward fogColor as F falls.
    float fog[4] = {};
    uint8_t fogColor[3] = {};
    uint32_t pass = 0;   // the game's pass, for grouping
    uint32_t source = 0; // what made it (a VU1 program, a hook), for profiles
};

struct SpriteDraw
{
    Id target = 0;
    bool ui = false; // drawn at output resolution
    float rect[4] = {};
    float uv[4] = {};
    float color[4] = {};
    Material material;
    Rect scissor;
    uint32_t source = 0;
};

enum class EffectKind : uint8_t
{
    Glare,
    Bloom,
    DepthOfField,
    PlaneShadow,
    ObjectLayer,
    CBlur,
    ColorFilter,
    Fade,
    RefractionCapture,
    TexAnime,
};

struct Effect
{
    EffectKind kind = EffectKind::Glare;
    Id targets[4] = {};     // what it reads and writes, by its own convention
    float params[16] = {};  // the hooked function's parameters
};

struct FrameBegin
{
    uint64_t frame = 0;
};

struct Present
{
    Id target = 0;
    Rect rect; // what of the target is shown
    uint64_t frame = 0;
};

struct Readback
{
    uint32_t request = 0; // the producer's, completed through its own mailbox
    Id target = 0;
    Rect rect;
};

struct DepthQuery
{
    uint32_t request = 0;
    Id target = 0;
    int32_t x = 0, y = 0;
};

using Record = std::variant<Texture, Palette, Target, Camera, MeshDraw, SpriteDraw, Effect, FrameBegin, Present,
                            Readback, DepthQuery>;
} // namespace dq8::gfx::native
