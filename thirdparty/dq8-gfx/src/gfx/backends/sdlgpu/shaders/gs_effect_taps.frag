#version 450

// Up to four additive sprites of one effect buffer drawn in one pass (Rogue
// Galaxy: added). The glare's zoom streak is twenty sprites, each the
// buffer laid over itself shifted and weighed (TFX MODULATE, ALPHA (Cs - 0)
// * FIX + Cd with FIX 128, COLCLAMP), each a pass of its own that reads
// the one before. Here a run of four reads the buffer as the run starts and
// adds every tap, so they no longer compound within the run (the weights
// are under 0.2: the difference is their products) and four passes become
// one. The arithmetic of each tap is gs_effect.frag's, for that case.

layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 1) uniform sampler2D previous;
layout(set = 3, binding = 0) uniform TapParams {
    vec4 misc;     // x: the target's scale; yz: the source's size in GS texels; w: the taps
    vec4 rect[4];
    vec4 uv[4];
    vec4 scissor[4];
    uvec4 rgba[4];
} params;
layout(location = 0) out vec4 outColor;

uvec4 bytes(vec4 color) {
    return uvec4(round(clamp(color, 0.0, 1.0) * 255.0));
}

void main() {
    float scale = params.misc.x;
    vec2 g = (gl_FragCoord.xy - 0.5) / scale;
    uvec4 dst = bytes(texelFetch(previous, ivec2(gl_FragCoord.xy), 0));
    int count = int(params.misc.w);
    for (int j = 0; j < count; ++j) {
        vec4 rect = params.rect[j];
        vec4 scissor = params.scissor[j];
        bool inside = all(greaterThanEqual(g, rect.xy)) && all(lessThan(g, rect.zw)) &&
                      all(greaterThanEqual(g, scissor.xy)) && all(lessThan(g, scissor.zw + 1.0));
        if (!inside)
            continue;
        vec4 uvs = params.uv[j];
        vec2 uv = uvs.xy + (g - rect.xy) * (uvs.zw - uvs.xy) / (rect.zw - rect.xy);
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
            taps[i] = vec4(bytes(texelFetch(source, clamp(first + ivec2(i & 1, i >> 1), ivec2(0), bound), 0))) / 255.0;
        uvec4 t = bytes(mix(mix(taps[0], taps[1], weight.x), mix(taps[2], taps[3], weight.x), weight.y));
        uvec4 v = params.rgba[j];
        uvec3 cs = min((t.rgb * v.rgb) >> 7u, uvec3(255u));
        uint alpha = min((t.a * v.a) >> 7u, 255u);
        dst.rgb = uvec3(clamp(ivec3(cs) + ivec3(dst.rgb), ivec3(0), ivec3(255)));
        dst.a = alpha;
    }
    outColor = vec4(dst) / 255.0;
}
