#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added): the texture
// function, alpha test and fog, in float, in the draw's units (1.0 the
// identity of a texture function and of an alpha as a blend factor).
// Blending is the pipeline's: the alpha written out is the blend's source
// alpha and what the target (RGBA16F) stores, past 1.0 too.
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 2, binding = 1) uniform sampler2D palette;
// The draw's uniforms, from the frame's draw table (see the vertex shader).
// One draw's row; Renderer::GpuDraw is its C++ twin (std430).
struct DrawRow {
    // The vertex stage's.
    mat4 clip;     // model x view x projection (standard), or the target's pixels for a 2D draw
    mat4 viewport; // clip space over the camera's rectangle to clip space over the whole target
    vec4 target;   // the target's width and height in game pixels; z: fog per vertex (a window-space draw);
                   // w: added to the depth times w (Material::decalBias)
    vec4 fog;      // F = clamp(fog.x + fog.w * w, at most fog.y, at least fog.z)
    vec4 depthRow; // the depth times w from the position, made in double (scaled and biased)
    vec4 depthLimit; // x: MeshDraw::depthCap; y: 1 when it applies
    // The fragment stage's.
    // mode x: 0 untextured, 1 RGBA, 2 Index8 through the palette, 3 a target,
    // 4 CT16, 5 Index8 through a CT16 palette; | 8: the indices packed two
    // a byte (Index4, the low nibble first)
    // y: the texture function (0 modulate, 1 decal, 2 highlight, 3 highlight2)
    // z: the texel's alpha is used (TCC)
    // w: the palette's entries
    uvec4 mode;
    // x: the alpha test (0 always ... the Compare enum), y unused,
    // z: whether a failing pixel is discarded, w: fog on
    uvec4 test;
    // x, y: the size the uvs are normalized to (the game's 2^TW, 2^TH);
    // z, w: the texture's own, which an upload may leave smaller
    vec4 size;
    // x: a channel shuffle (1: alpha's bits 2-7 from green's), z: clamp u,
    // w: clamp v (y unused)
    vec4 blend;
    vec4 fogColor; // in the draw's units
    // x: the texel's alpha replaced by this when not negative (a target
    // read without its alpha); y: then 0 where the texel is black; z: the
    // destination alpha test, 0 none, 1 drawn where the target's alpha is
    // below 1, 2 where at least (read from `palette`, the target's copy)
    vec4 texa;
    // xy: subtracted from the texel coordinate (Material::texelOffset);
    // zw: a CT16 texel's alpha without and with its alpha bit (TEXA's TA0
    // and TA1).
    vec4 offset;
    // A retained mesh's cull (native_mesh_static.vert): the program's matrix
    // to GS window coordinates, its guard band, x: the winding sign, y: the
    // guard band test on, z: the backface test on, w: the chunk is clipped;
    // and the cap of its vertex colors.
    mat4 cullMatrix;
    vec4 bandScale;
    vec4 bandOffset;
    vec4 cull;
    vec4 colorMax;
    // x: what a texel channel's 1.0 is worth (the texture's or palette's
    // scale; 1 for a target), y: the color's clamp (0 none), z: the alpha
    // test's reference, w: the shuffle's scale.
    vec4 units;
    // x: what a raw vertex color is worth once capped (MeshDraw's
    // modelColorScale).
    vec4 vertexUnits;
};
layout(std430, set = 2, binding = 2) readonly buffer Draws {
    DrawRow draws[];
};
#define material draws[inDraw]

int wrapAxis(int a, int size, bool clamped) {
    return clamped ? clamp(a, 0, size - 1) : int(mod(float(a), float(size)));
}

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inFog;
layout(location = 3) flat in uint inDraw;
// The UV's divisor: 1, or w for an affine draw (its UV times w, so that the
// GPU's perspective-correct interpolation of both, divided, is the GS's
// screen-linear ST with Q = 1).
layout(location = 4) in float inQ;
layout(location = 0) out vec4 outColor;

bool passes(uint compare, float a, float ref) {
    switch (compare) {
    case 0u: return true;
    case 1u: return false;
    case 2u: return a < ref;
    case 3u: return a <= ref;
    case 4u: return a == ref;
    case 5u: return a >= ref;
    case 6u: return a > ref;
    default: return a != ref;
    }
}

void main() {
    if (material.texa.z > 0.0) {
        bool high = texelFetch(palette, ivec2(gl_FragCoord.xy), 0).a >= 1.0;
        if (high != (material.texa.z > 1.5))
            discard;
    }
    vec4 f = inColor;
    vec4 c = f;
    if (material.mode.x != 0u) {
        vec4 t;
        vec2 texel = (inUv / inQ) * material.size.xy - material.offset.xy; // in texels
        uint kind = material.mode.x & 7u;
        bool indexed = kind == 2u || kind == 5u;
        bool ct16 = kind == 4u || kind == 5u;
        if (indexed) {
            // Fetched, not filtered: an index is not a color.
            ivec2 size = ivec2(material.size.zw);
            ivec2 at = ivec2(floor(texel));
            at.x = wrapAxis(at.x, size.x, material.blend.z != 0.0);
            at.y = wrapAxis(at.y, size.y, material.blend.w != 0.0);
            int index;
            if ((material.mode.x & 8u) != 0u) {
                int packed = int(texelFetch(image, ivec2(at.x >> 1, at.y), 0).r * 255.0 + 0.5);
                index = (at.x & 1) != 0 ? (packed >> 4) : (packed & 15);
            } else {
                index = int(texelFetch(image, at, 0).r * 255.0 + 0.5);
            }
            t = texelFetch(palette, ivec2(min(index, int(material.mode.w) - 1), 0), 0);
        } else {
            t = texture(image, texel / material.size.zw);
        }
        if (ct16) {
            // The GS's CT16 as B5G5R5A1 reads it: its red in blue. The
            // channels widened by a shift (units.x), the alpha TEXA's: TA1
            // with the bit, TA0 without, 0 for black with AEM (filtered
            // between them as the GS filters the expanded texels).
            float bit = t.a;
            t.rgb = t.bgr * material.units.x;
            float low = (material.texa.y != 0.0 && t.rgb == vec3(0.0)) ? 0.0 : material.offset.z;
            t.a = mix(low, material.offset.w, bit);
        } else {
            t *= material.units.x;
            if (material.texa.x >= 0.0)
                t.a = (material.texa.y != 0.0 && t.rgb == vec3(0.0)) ? 0.0 : material.texa.x;
        }
        if (material.blend.x == 1.0) {
            // As bytes of `units.w` a unit: alpha's bits 2-7 from green's.
            float k = material.units.w;
            t.a = float((uint(t.g * k + 0.5) & 0xFCu) | (uint(t.a * k + 0.5) & 3u)) / k;
        } else if (material.blend.x == 2.0) {
            t.b = t.r;
            t.a = t.g;
        }
        bool tcc = material.mode.z != 0u;
        uint fn = material.mode.y;
        if (fn == 1u) {
            c.rgb = t.rgb;
            c.a = tcc ? t.a : f.a;
        } else {
            c.rgb = f.rgb * t.rgb;
            if (fn >= 2u)
                c.rgb += f.a;
            if (fn == 0u)
                c.a = tcc ? f.a * t.a : f.a;
            else if (fn == 2u)
                c.a = tcc ? t.a + f.a : f.a;
            else
                c.a = tcc ? t.a : f.a;
        }
        if (material.units.y > 0.0)
            c = clamp(c, 0.0, material.units.y);
    }
    if (!passes(material.test.x, c.a, material.units.z) && material.test.z != 0u)
        discard;
    if (material.test.w != 0u)
        c.rgb = mix(material.fogColor.rgb, c.rgb, clamp(inFog, 0.0, 1.0));
    outColor = c;
}
