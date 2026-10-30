#version 450

// One sprite of a GSNativeEffect (Rogue Galaxy: added): what the GS would
// draw for it, with its integer colour arithmetic. Into an effect's own
// buffer the pass covers the whole buffer: outside the sprite, or where the
// alpha test keeps the buffer, it copies `previous` (the buffer before the
// sprite), and the blend reads its destination there, so a sprite may read
// the buffer it draws. Into an external target (the frame) it covers the
// sprite alone and gives the blend's Cs C term (A - B being Cs, -Cs or
// Cs - Cd), which the pipeline adds, subtracts or lays over Cd; for the last
// the alpha it gives is C.

layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 1) uniform sampler2D previous;
layout(set = 3, binding = 0) uniform EffectParams {
    vec4 rect;    // x0, y0, x1, y1 in GS pixels
    vec4 uv;      // u0, v0, u1, v1 in texels of the source
    vec4 scissor; // x0, y0, x1, y1, inclusive
    vec4 misc;    // x: the target's scale; yz: the source's size in GS texels; w: TEXA's TA0 + 256 AEM
    uvec4 rgba;   // the vertex colour
    // x: flags (GSNativeEffect's, and 256 for an external target, 512 for
    // a textured sprite); y: ALPHA's A, B, C, D (2 bits each); z: its FIX;
    // w: TEST's ATE, ATST, AREF, AFAIL in their register positions.
    uvec4 control;
} params;
layout(location = 0) out vec4 outColor;

const uint kBlend = 1u;
const uint kLinear = 2u;
const uint kDecal = 4u;
const uint kColClamp = 8u;
const uint kTextureAlpha = 16u;
const uint kCt24 = 32u;
const uint kHighlight = 64u;
const uint kHighlight2 = 128u;
const uint kGreenToAlpha = 1024u;
const uint kKeepAlpha = 2048u;
const uint kExternal = 256u;
const uint kTextured = 512u;
// The alpha pass after a lay over: the alpha the GS stores, As (the backend's).
const uint kAlphaOnly = 8192u;
// The source is an object layer (the backend's).
const uint kLayerSource = 16384u;

uvec4 bytes(vec4 color) {
    return uvec4(round(clamp(color, 0.0, 1.0) * 255.0));
}

bool alphaPasses(uint alpha, uint test) {
    if ((test & 1u) == 0u)
        return true;
    uint ref = (test >> 4u) & 255u;
    switch ((test >> 1u) & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return alpha < ref;
    case 3u: return alpha <= ref;
    case 4u: return alpha == ref;
    case 5u: return alpha >= ref;
    case 6u: return alpha > ref;
    default: return alpha != ref;
    }
}

// A source texel as the sprite reads it: CT24's alpha from TEXA (TA0, or
// 0 for black under AEM).
uvec4 texel(vec4 color, uint flags, uint texa) {
    uvec4 raw = bytes(color);
    if ((flags & kCt24) != 0u)
        raw.a = (texa >= 256u && all(equal(raw.rgb, uvec3(0u)))) ? 0u : (texa & 255u);
    return raw;
}

ivec3 colorTerm(uint select, ivec3 cs, ivec3 cd) {
    return select == 0u ? cs : (select == 1u ? cd : ivec3(0));
}

void main() {
    uint flags = params.control.x;
    bool toFrame = (flags & kExternal) != 0u;
    float scale = params.misc.x;
    // A GS pixel samples at its top-left corner; each of the scale x scale
    // pixels of an upscaled target at its own.
    vec2 g = (gl_FragCoord.xy - 0.5) / scale;
    uvec4 dst = toFrame ? uvec4(0u) : bytes(texelFetch(previous, ivec2(gl_FragCoord.xy), 0));
    bool inside = all(greaterThanEqual(g, params.rect.xy)) && all(lessThan(g, params.rect.zw)) &&
                  all(greaterThanEqual(g, params.scissor.xy)) && all(lessThan(g, params.scissor.zw + 1.0));
    if (!inside) {
        if (toFrame)
            discard;
        outColor = vec4(dst) / 255.0;
        return;
    }

    if ((flags & kGreenToAlpha) != 0u) {
        outColor = vec4(vec3(dst.rgb), float((dst.g & 0xFCu) | (dst.a & 3u))) / 255.0;
        return;
    }

    uvec4 v = params.rgba;
    uvec4 c = v;
    if ((flags & kTextured) != 0u) {
        vec2 uv = params.uv.xy + (g - params.rect.xy) * (params.uv.zw - params.uv.xy) / (params.rect.zw - params.rect.xy);
        uvec4 t;
        uint texa = uint(params.misc.w);
        if ((flags & kLinear) != 0u && (flags & (kCt24 | kLayerSource)) != 0u) {
            // Each texel's alpha (from TEXA) before the four
            // are weighed, as the GS (and the GS path's sampleTexture)
            // filters: under AEM black ones count as transparent, so a
            // silhouette's alpha falls off.
            int sourceScale = max(int(round(float(textureSize(source, 0).x) / params.misc.y)), 1);
            ivec2 bound = textureSize(source, 0) - 1;
            vec2 base;
            vec2 weight;
            ivec2 first;
            if (sourceScale > 1) {
                base = uv * float(sourceScale) - 0.5;
                first = ivec2(floor(base));
                weight = base - vec2(first);
            } else {
                base = floor((uv - 0.5) * 16.0 + 0.002) / 16.0;
                first = ivec2(floor(base));
                weight = fract(base);
            }
            vec4 taps[4];
            for (int i = 0; i < 4; ++i)
                taps[i] = vec4(texel(texelFetch(source, clamp(first + ivec2(i & 1, i >> 1), ivec2(0), bound), 0),
                                     flags, texa)) / 255.0;
            t = bytes(mix(mix(taps[0], taps[1], weight.x), mix(taps[2], taps[3], weight.x), weight.y));
        } else {
            t = texel(texture(source, uv / params.misc.yz), flags, texa);
        }
        if ((flags & kDecal) != 0u) {
            c.rgb = t.rgb;
            c.a = (flags & kTextureAlpha) != 0u ? t.a : v.a;
        } else if ((flags & (kHighlight | kHighlight2)) != 0u) {
            c.rgb = min(((t.rgb * v.rgb) >> 7u) + v.a, uvec3(255u));
            c.a = (flags & kTextureAlpha) == 0u ? v.a
                : ((flags & kHighlight) != 0u ? min(t.a + v.a, 255u) : t.a);
        } else {
            c.rgb = min((t.rgb * v.rgb) >> 7u, uvec3(255u));
            c.a = (flags & kTextureAlpha) != 0u ? min((t.a * v.a) >> 7u, 255u) : v.a;
        }
    }

    uint test = params.control.w;
    bool writeAlpha = true;
    if (!alphaPasses(c.a, test)) {
        uint fail = (test >> 12u) & 3u;
        // KEEP and ZB_ONLY leave the colour; FB_ONLY writes it; RGB_ONLY
        // writes all but the alpha.
        if (fail == 0u || fail == 2u) {
            if (toFrame)
                discard;
            outColor = vec4(dst) / 255.0;
            return;
        }
        writeAlpha = fail == 1u;
    }

    ivec3 rgb = ivec3(c.rgb);
    uint alpha = params.control.y;
    uint selectC = (alpha >> 4u) & 3u;
    int factor = selectC == 0u ? int(c.a) : (selectC == 1u ? int(dst.a) : int(params.control.z));
    if (toFrame && (flags & kBlend) != 0u) {
        // The pipeline adds this to Cd, takes it from Cd, or (A - B being
        // Cs - Cd) adds it to Cd (1 - C).
        rgb = clamp((ivec3(c.rgb) * factor) >> 7, ivec3(0), ivec3(255));
        bool over = (alpha & 0xFu) == 0x4u && (flags & kAlphaOnly) == 0u;
        outColor = vec4(vec3(rgb) / 255.0, over ? min(float(factor) / 128.0, 1.0) : float(c.a) / 255.0);
        return;
    }
    if ((flags & kBlend) != 0u) {
        ivec3 cs = ivec3(c.rgb);
        ivec3 cd = ivec3(dst.rgb);
        ivec3 a = colorTerm(alpha & 3u, cs, cd);
        ivec3 b = colorTerm((alpha >> 2u) & 3u, cs, cd);
        ivec3 d = colorTerm((alpha >> 6u) & 3u, cs, cd);
        rgb = (((a - b) * factor) >> 7) + d;
    }
    rgb = (flags & kColClamp) != 0u ? clamp(rgb, ivec3(0), ivec3(255)) : (rgb & ivec3(255));
    // Into an external target the pipeline masks the alpha.
    uint outAlpha = writeAlpha && (flags & kKeepAlpha) == 0u ? c.a : dst.a;
    outColor = vec4(vec3(rgb), float(outAlpha)) / 255.0;
}
