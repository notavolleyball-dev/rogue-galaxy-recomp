#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added): the game's texture
// function, alpha test and fog, in its own units (0x80 is 1.0 for colors
// and alphas). Blending is the pipeline's: the alpha written out is the
// blend's factor, As / 0x80 (or the fixed factor's), not the stored alpha.
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 2, binding = 1) uniform sampler2D palette;
layout(set = 3, binding = 0) uniform Material {
    // x: 0 untextured, 1 RGBA, 2 Index8 through the palette
    // y: the texture function (0 modulate, 1 decal, 2 highlight, 3 highlight2)
    // z: the texel's alpha is used (TCC)
    // w: the palette's entries
    uvec4 mode;
    // x: the alpha test (0 always ... the Compare enum), y: its reference,
    // z: whether a failing pixel is discarded, w: fog on
    uvec4 test;
    // x, y: the size the uvs are normalized to (the game's 2^TW, 2^TH);
    // z, w: the texture's own, which an upload may leave smaller
    vec4 size;
    // x: the fixed blend factor / 0x80, used when y is 1; z: clamp u, w: clamp v
    vec4 blend;
    vec4 fogColor; // 0-1
} material;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inFog;
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
    // The vertex color as the GS takes it: truncated to integers 0-255.
    vec4 f = clamp(floor(inColor), 0.0, 255.0);
    vec4 c = f;
    if (material.mode.x != 0u) {
        vec4 t;
        vec2 texel = inUv * material.size.xy; // in the game's texels
        if (material.mode.x == 2u) {
            // Fetched, not filtered: an index is not a color.
            ivec2 size = ivec2(material.size.zw);
            ivec2 at = ivec2(floor(texel));
            at.x = material.blend.z != 0.0 ? clamp(at.x, 0, size.x - 1) : int(mod(float(at.x), float(size.x)));
            at.y = material.blend.w != 0.0 ? clamp(at.y, 0, size.y - 1) : int(mod(float(at.y), float(size.y)));
            int index = int(texelFetch(image, at, 0).r * 255.0 + 0.5);
            t = texelFetch(palette, ivec2(min(index, int(material.mode.w) - 1), 0), 0);
        } else {
            t = texture(image, texel / material.size.zw);
        }
        t = floor(t * 255.0 + 0.5);
        bool tcc = material.mode.z != 0u;
        uint fn = material.mode.y;
        if (fn == 1u) {
            c.rgb = t.rgb;
            c.a = tcc ? t.a : f.a;
        } else {
            c.rgb = floor(f.rgb * t.rgb / 128.0);
            if (fn >= 2u)
                c.rgb += f.a;
            if (fn == 0u)
                c.a = tcc ? floor(f.a * t.a / 128.0) : f.a;
            else if (fn == 2u)
                c.a = tcc ? t.a + f.a : f.a;
            else
                c.a = tcc ? t.a : f.a;
        }
        c = min(c, 255.0);
    }
    if (!passes(material.test.x, c.a, float(material.test.y)) && material.test.z != 0u)
        discard;
    if (material.test.w != 0u) {
        float k = clamp(floor(inFog), 0.0, 255.0) / 255.0;
        c.rgb = mix(material.fogColor.rgb * 255.0, c.rgb, k);
    }
    float alpha = material.blend.y != 0.0 ? material.blend.x : c.a / 128.0;
    outColor = vec4(c.rgb / 255.0, alpha);
}
