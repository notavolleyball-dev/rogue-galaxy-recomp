#version 450

// The native pipeline's model chunk draw (Rogue Galaxy: added; M5 of the
// game's docs/NATIVE-ONLY.md). A model chunk stays in a vertex buffer of its
// own as the game's VU1 program reads it (the int values of its positions,
// normals, UVs and colors, its bones and weights), and this stage runs the
// program on it: B (dequantize, skin), F (UV generation, the wave and the
// swell), C (the color: the material, the vertex colors, the directional and
// point lights), as src/vu1_native.cpp's readChunk does on the CPU, product
// by product in the same order (`precise`: nothing fused). Then the rest is
// the retained meshes' rule before (deleted in M5): the program's backface rule from the triangle's
// three corners (each through the same B and F), the transform, the fog and
// the depth. The guard band's skip is not kept (the GPU clips).
//
// The draw's row is the instance index; its model row is draw.test.y, its
// bones' matrices at palette[model q37.x + 4 x + row].
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
    // 4 CT16, 5 Index8 through a CT16 palette; | 8: Index4 (packed)
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
    // xy: subtracted from the texel coordinate (Material::texelOffset).
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
layout(std430, set = 0, binding = 0) readonly buffer Draws {
    DrawRow draws[];
};
// The constants of a draw (vu1_native's modelRow), 48 vec4s:
//  0: quad 77 (position scale), w: the C piece (Color: 0 vertex colors,
//     1 material, 2 lit, 3 lit vertex colors, 4 lit point vertex colors,
//     5 lit point, 6 the projector's: the base)
//  1: quad 78 (normal scale), w: the F piece (Uv: 0 plain, 1 sphere map,
//     2 reflection, 3 swell, 4 beam, 5 ramp, 6 wave, 7 projected)
//  2: quad 79 (UV scale) xy, z: the bones (0: not skinned), w: flags (1
//     white, 2 87.y & 0x100, 4 skin the normals, 8 the inputs are floats:
//     the position, normal and UV taken as they are; 16 an outline draw,
//     outline(); 32 particle(); 64 volume(); 128 the affine D (UV linear
//     on screen); 256 the float-color D (the color input as it is))
//  3: the base color; 4-6: the lights' directions (quads 100-102, lane i
//     light i); 7-10: their colors times the material; 11: the ambient
//  12-15: the point lights' positions (w: falloff); 16-19: their colors
//  20-23: quads 60-63 (the object matrix, a column each); 24-26: 64-66
//     (its rotation); 27: quad 28 (the eye)
//  28, 29.xy: the sphere map's rows; 29.zw: the wave's a and q
//  30: the beam's shift x, y and centre x, y; 31: its scale, the swell's q,
//     far and 1 / far; 32-35: the beam's columns (quads 72-75)
//  36: the swell's lanes (quads 68-71 w); 37.x: the palette's base
//  38-41: the projector's texture matrix (C 6 and F 7: the projected
//     texture, its color the base); 42.xy: the scroll F's offset (quad
//     88.xy, F 0)
//  flag 32 (particle()): 20-23 the model, 27 the eye, nothing else.
//  flag 64 (volume()): 38-41 the light's push (the model times quads
//     30-33), 42 the direction to the light and w the lit threshold, 43
//     the color, 44-47 the camera (8-11 times 24-27, x and y times 16).
struct ModelRow {
    vec4 q[48];
};
layout(std430, set = 0, binding = 1) readonly buffer Models {
    ModelRow models[];
};
layout(std430, set = 0, binding = 2) readonly buffer Palette {
    vec4 palette[];
};
#define inDraw uint(gl_InstanceIndex)
#define draw draws[inDraw]
#define M models[draw.test.y]

invariant gl_Position;

layout(location = 0) in vec3 inA;
layout(location = 1) in vec3 inB;
layout(location = 2) in vec3 inC;
layout(location = 3) in vec4 inBonesA;
layout(location = 4) in vec4 inBonesB;
layout(location = 5) in vec4 inBonesC;
layout(location = 6) in vec4 inWeightsA;
layout(location = 7) in vec4 inWeightsB;
layout(location = 8) in vec4 inWeightsC;
layout(location = 9) in vec3 inNormal;
layout(location = 10) in vec2 inUv;
layout(location = 11) in vec4 inColor;
layout(location = 12) in vec2 inCorner; // x: which corner; y: the triangle's place in the strip, +1 or -1

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outFog;
layout(location = 3) flat out uint outDraw;
layout(location = 4) out float outQ; // the UV's divisor (native_mesh.frag): w for an affine draw, else 1

// The VU's int / 32768 (ITOF15).
float itof15(float v) {
    precise float r = v * (1.0 / 32768.0);
    return r;
}

// The skinning B: the vector (w 1 for a position, 0 rows past 2 for a
// normal) scaled by each bone's weight / 255, times the bone's rows,
// summed row by row (skin(), skinNormal()).
vec3 skin(vec3 p, vec4 bones, vec4 weights, int count, bool position) {
    precise vec3 acc = vec3(0.0);
    bool first = true;
    uint base = uint(M.q[37].x);
    for (int b = 0; b < 4; ++b) {
        if (b >= count)
            break;
        precise float w = weights[b] * uintBitsToFloat(0x3b808081u);
        precise vec4 scaled = vec4(p * w, w);
        uint matrix = base + uint(bones[b]) * 4u;
        for (int row = 0; row < (position ? 4 : 3); ++row) {
            precise vec3 product = palette[matrix + uint(row)].xyz * scaled[row];
            acc = first ? product : acc + product;
            first = false;
        }
    }
    return acc;
}

// The VU's ESIN (vuSin).
float vuSin(float x) {
    precise float s = x * x;
    precise float p = 0.000002601886990;
    p = -0.000198074136279 + s * p;
    p = 0.008333025500178 + s * p;
    p = -0.166666567325592 + s * p;
    p = 1.0 + s * p;
    precise float r = x * p;
    return r;
}

// B, then the F pieces that move positions (the wave, the swell): a corner's
// object-space position.
bool floats() {
    return (uint(M.q[2].w) & 8u) != 0u;
}

vec3 decodeNormal(vec3 raw) {
    if (floats())
        return raw;
    precise vec3 n = vec3(itof15(raw.x) * M.q[1].x, itof15(raw.y) * M.q[1].y, itof15(raw.z) * M.q[1].z);
    return n;
}

vec3 position(vec3 raw, vec4 bones, vec4 weights, vec3 rawNormal) {
    precise vec3 p = floats() ? raw
                              : vec3(itof15(raw.x) * M.q[0].x, itof15(raw.y) * M.q[0].y, itof15(raw.z) * M.q[0].z);
    int count = int(M.q[2].z);
    if (count > 0)
        p = skin(p, bones, weights, count, true);
    uint uv = uint(M.q[1].w);
    if (uv == 6u) {
        // The wave: turned about (0, a) by t = q max(y - a, 0).
        float a = M.q[29].z, q = M.q[29].w;
        precise float d = p.y - a;
        precise float t = (0.0 + q) * (d > 0.0 ? d : 0.0);
        float s = vuSin(t);
        precise float c = 1.0 - (t * t) * 0.5;
        precise float nx = (0.0 - s) * d + c * p.x;
        precise float ny = ((0.0 + c) * d + s * p.x) + a;
        p.x = nx;
        p.y = ny;
    } else if (uv == 3u) {
        // The swell: pushed out along the normal, fading with depth.
        precise vec3 n = decodeNormal(rawNormal);
        if (count > 0 && (uint(M.q[2].w) & 4u) != 0u)
            n = skin(n, bones, weights, count, false);
        vec4 lane = M.q[36];
        precise float depth = ((lane.x * p.x + lane.y * p.y) + lane.z * p.z) + lane.w * 1.0;
        precise float scale = min(max((M.q[31].z - depth) * M.q[31].w, 0.2), 1.0);
        p = p + ((n * M.q[31].y) * depth) * scale;
    }
    return p;
}

// A corner projected as the program does (gsNativeMeshPoint): x and y
// divided, 1 / w, and whether it is outside the guard band.
void project(vec3 v, out float px, out float py, out float q, out bool outside) {
    precise vec4 clip = v.x * draw.cullMatrix[0] + v.y * draw.cullMatrix[1] + v.z * draw.cullMatrix[2] +
                        draw.cullMatrix[3];
    // The VU1's DIV: at 0 the most float, with w's sign.
    precise float r = clip.w != 0.0 ? 1.0 / clip.w
                      : ((floatBitsToUint(clip.w) >> 31) != 0u ? -3.402823466e38 : 3.402823466e38);
    q = r;
    precise float x = clip.x * r;
    precise float y = clip.y * r;
    precise float z = clip.z * r;
    px = x;
    py = y;
    precise vec3 g = vec3(x, y, z) * draw.bandScale.xyz + draw.bandOffset.xyz * draw.bandScale.xyz;
    outside = draw.cull.y != 0.0 && (any(greaterThan(g, vec3(1.0))) || any(lessThan(g, vec3(-1.0))));
}

// A vertex at p (the space draw.clip takes) with its UV and color: the
// transform, the fog and the depth; off the target when skipped.
void emit(vec3 p, vec2 uv, vec4 color, bool skip) {
    vec4 clip = draw.clip * vec4(p, 1.0);
    vec4 window = draw.viewport * clip;
    outDraw = inDraw;
    // The affine D (flag 128) leaves ST as the UV and Q 1: the GS's UV is
    // then linear on screen.
    bool affine = (uint(M.q[2].w) & 128u) != 0u;
    outUv = affine ? uv * clip.w : uv;
    outQ = affine ? clip.w : 1.0;
    outColor = min(color, draw.colorMax) * draw.vertexUnits.x;
    outFog = clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    float depth = dot(draw.depthRow, vec4(p, 1.0));
    if (draw.depthLimit.y != 0.0 && clip.w > 0.0)
        depth = min(depth, draw.depthLimit.x * clip.w);
    gl_Position = vec4(window.x, window.y, depth + draw.target.w * clip.w, clip.w);
    if (skip)
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
}

// Rows r..r+3 times (p, 1), lane by lane as vu1_native's transform().
vec4 rows4(int r, vec3 p) {
    precise vec4 v = ((M.q[r] * p.x + M.q[r + 1] * p.y) + M.q[r + 2] * p.z) + M.q[r + 3] * 1.0;
    return v;
}

// 1 / sqrt(|x|), the most float at 0 (the runtime's RSQRT).
float rsqrtVu(float x) {
    precise float root = sqrt(abs(x));
    return root != 0.0 ? 1.0 / root : 3.402823466e38;
}

// The particle program's sine (vu1_native's particleCosine): the angle's
// turns folded by the 1.5 x 2^23 trick, then an odd polynomial.
float particleCosine(float angle) {
    precise float p = abs(angle) * uintBitsToFloat(0xbe22f987u);
    float magic = uintBitsToFloat(0x4b400000u);
    precise float fraction = ((p - magic) + magic) - p;
    precise float t = abs(fraction - 0.5) - 0.25;
    precise float t2 = t * t;
    precise float t4 = t2 * t2;
    precise float t8 = t4 * t4;
    precise float a3 = t * uintBitsToFloat(0xc2255de0u) * t2;
    precise float a7 = t * uintBitsToFloat(0xc2992661u) * t2 * t4;
    precise float a5 = t * uintBitsToFloat(0x42a33457u) * t4;
    precise float a1 = t * uintBitsToFloat(0x40c90fdau);
    precise float a9 = t * uintBitsToFloat(0x421ed7b7u) * t8;
    precise float r = a3 + a7 + a5 + a1 + a9;
    return r;
}

// The particles (flag 32; vu1_native's particleMesh, buildParticles'
// arithmetic): the particle's position through the model, turned to face
// the eye, its corner at the offsets along the turned axes, in world space.
// A: the position; bones A: its shape (x, y, the angle); normal: this
// corner's offsets; uv: its texel; color: the particle's (times 128,
// clamped to 0-128, truncated).
void particle() {
    vec4 world = rows4(20, inA);
    vec3 eye = M.q[27].xyz;
    precise float ex = eye.x - world.x;
    precise float ey = eye.y - world.y;
    precise float ez = eye.z - world.z;
    precise float rx = 0.0 + ez;
    float ry = 0.0;
    precise float rz = 0.0 - ex;
    precise float rr = (rx * rx + ry * ry) + rz * rz;
    precise float ux = ey * rz - ry * ez;
    precise float uy = ez * rx - rz * ex;
    precise float uz = ex * ry - rx * ey;
    precise float uu = (ux * ux + uy * uy) + uz * uz;
    float rq = rsqrtVu(rr), uq = rsqrtVu(uu);
    vec3 shape = inBonesA.xyz;
    precise vec3 rs = (vec3(rx, ry, rz) * rq) * shape.x;
    precise vec3 us = (vec3(ux, uy, uz) * uq) * shape.y;
    float c = 0.0 - particleCosine(shape.z - uintBitsToFloat(0x3fc90fd8u));
    float s = particleCosine(shape.z);
    precise vec3 a = rs * s - us * c;
    precise vec3 b = rs * c + us * s;
    precise vec3 corner = (b * inNormal.y + world.xyz) + a * inNormal.x;
    vec4 color = floor(clamp(inColor * 128.0, 0.0, 128.0));
    emit(corner, inUv, color, false);
}

// Whether a point is outside its own w (vu1_native's clipsOwnW).
bool clipsOwnW(vec4 v) {
    float w = abs(v.w);
    return v.x > w || v.x < -w || v.y > w || v.y < -w || v.z > w || v.z < -w;
}

// The shadow volumes (flag 64; vu1_native's buildShadow): the triangle's
// corners in world space and pushed from the light; none when it is not lit
// (its world normal's dot with row 42 at most 42.w) or a corner is outside
// its own w through rows 44-47. Five fans of two triangles over the six
// (corner.x: fan x 6 + which of 0 1 2 0 2 3), each judged by the backface
// rule from its own projected corners; the color row 43.
const int kFans[20] = int[](0, 3, 4, 1, 1, 4, 5, 2, 2, 5, 3, 0, 0, 1, 2, 2, 5, 4, 3, 3);
const int kFanCorner[6] = int[](0, 1, 2, 0, 2, 3);
void volume() {
    vec3 o[3];
    o[0] = position(inA, inBonesA, inWeightsA, vec3(0.0));
    o[1] = position(inB, inBonesB, inWeightsB, vec3(0.0));
    o[2] = position(inC, inBonesC, inWeightsC, vec3(0.0));
    vec3 corners[6];
    bool skip = false;
    for (int j = 0; j < 3; ++j) {
        corners[j] = rows4(20, o[j]).xyz;
        corners[3 + j] = rows4(38, o[j]).xyz;
    }
    for (int j = 0; j < 6; ++j)
        skip = skip || clipsOwnW(rows4(44, corners[j]));
    precise vec3 ea = corners[1] - corners[0];
    precise vec3 eb = corners[2] - corners[0];
    precise float nx = ea.y * eb.z - eb.y * ea.z;
    precise float ny = ea.z * eb.x - eb.z * ea.x;
    precise float nz = ea.x * eb.y - eb.x * ea.y;
    vec4 toLight = M.q[42];
    precise float lit = (toLight.x * nx + toLight.y * ny) + toLight.z * nz;
    skip = skip || !(lit > toLight.w);

    int k = int(inCorner.x + 0.5);
    int fan = k / 6;
    int which = k - fan * 6;
    int t0 = kFans[fan * 4 + 0];
    int t1 = kFans[fan * 4 + (which < 3 ? 1 : 2)];
    int t2 = kFans[fan * 4 + (which < 3 ? 2 : 3)];
    if (draw.cull.z != 0.0) {
        float ax, ay, aq, bx, by, bq, cx, cy, cq;
        bool ao, bo, co;
        project(corners[t0], ax, ay, aq, ao);
        project(corners[t1], bx, by, bq, bo);
        project(corners[t2], cx, cy, cq, co);
        precise float cross = (cx - bx) * (by - ay) - (bx - ax) * (cy - by);
        precise float facing = cross * (draw.cull.x * inCorner.y);
        skip = skip || facing < 0.0;
    }
    vec3 p = corners[kFans[fan * 4 + kFanCorner[which]]];
    emit(p, vec2(0.0), M.q[43], skip);
}

// The outline C (src/vu1_native.cpp's buildOutline), flag 16: an edge drawn
// as four lines, moved by +-h in x, then in y, h = alpha / 2, alpha = clamp(
// 10/9 - w / 360, 0, 1) of its first end (w through the program's matrix),
// colored (16, 16, 16, alpha * 64); none when an end is outside the guard
// band or, with four points, the edge's two triangles face apart on screen.
// Each line is a quad half a game pixel either side of it (as the 2D path
// widens the GS's lines): this vertex is corner.x's of the edge's 24.
void outline() {
    vec3 e0 = position(inB, inBonesB, inWeightsB, vec3(0.0));
    vec3 e1 = position(inC, inBonesC, inWeightsC, vec3(0.0));
    float x0, y0, q0, x1, y1, q1;
    bool o0, o1;
    project(e0, x0, y0, q0, o0);
    project(e1, x1, y1, q1, o1);
    bool hidden = o0 || o1;
    if (inCorner.y > 3.0) {
        vec3 f0 = position(inA, inBonesA, inWeightsA, vec3(0.0));
        vec4 bonesD = vec4(mod(inUv.x, 256.0), floor(inUv.x / 256.0), mod(inUv.y, 256.0), floor(inUv.y / 256.0));
        vec3 f1 = position(inNormal, bonesD, inColor, vec3(0.0));
        float xa, ya, qa, xd, yd, qd;
        bool oa, od;
        project(f0, xa, ya, qa, oa);
        project(f1, xd, yd, qd, od);
        precise float nearCross = (x0 - xa) * (y1 - ya) - (x1 - xa) * (y0 - ya);
        precise float farCross = (x1 - x0) * (yd - y0) - (xd - x0) * (y1 - y0);
        hidden = hidden || nearCross * farCross < 0.0;
    }
    precise float w0 = e0.x * draw.cullMatrix[0].w + e0.y * draw.cullMatrix[1].w + e0.z * draw.cullMatrix[2].w +
                       draw.cullMatrix[3].w;
    precise float alpha = clamp(uintBitsToFloat(0xbb360b61u) * w0 + uintBitsToFloat(0x3f8e38e4u), 0.0, 1.0);
    float h = 0.5 * alpha;

    int k = int(inCorner.x + 0.5);
    int lineAt = k / 6;
    int which = k - lineAt * 6;
    // The quad's six: (end, side) as native_2d's quad(v0, v1, v3, v2).
    int end = (which == 2 || which == 4 || which == 5) ? 1 : 0;
    float side = (which == 1 || which == 5) ? -1.0 : 1.0;
    vec2 shift = lineAt == 0 ? vec2(h, 0.0) : (lineAt == 1 ? vec2(-h, 0.0) : (lineAt == 2 ? vec2(0.0, -h) : vec2(0.0, h)));

    vec4 clip0 = draw.clip * vec4(e0, 1.0), clip1 = draw.clip * vec4(e1, 1.0);
    vec4 window0 = draw.viewport * clip0, window1 = draw.viewport * clip1;
    // In the target's game pixels: the line's normal, half a pixel.
    vec2 halfSize = draw.target.xy * 0.5;
    vec2 d = (window1.xy / clip1.w - window0.xy / clip0.w) * halfSize;
    float span = sqrt(dot(d, d));
    vec2 n = span > 1e-6 ? vec2(-d.y, d.x) / span * 0.5 : vec2(0.5, 0.0);
    vec2 pixels = shift + n * side;

    vec3 p = end == 0 ? e0 : e1;
    vec4 clip = end == 0 ? clip0 : clip1;
    vec4 window = end == 0 ? window0 : window1;
    outDraw = inDraw;
    outUv = vec2(0.0);
    outQ = 1.0;
    outColor = min(vec4(16.0, 16.0, 16.0, floor(alpha * 64.0)), draw.colorMax) * draw.vertexUnits.x;
    outFog = clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    float depth = dot(draw.depthRow, vec4(p, 1.0));
    if (draw.depthLimit.y != 0.0 && clip.w > 0.0)
        depth = min(depth, draw.depthLimit.x * clip.w);
    gl_Position = vec4(window.xy + pixels / halfSize * clip.w, depth + draw.target.w * clip.w, clip.w);
    if (hidden)
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
}

void main() {
    uint mode = uint(M.q[2].w);
    if ((mode & 16u) != 0u) {
        outline();
        return;
    }
    if ((mode & 32u) != 0u) {
        particle();
        return;
    }
    if ((mode & 64u) != 0u) {
        volume();
        return;
    }
    vec3 a = position(inA, inBonesA, inWeightsA, inNormal);
    vec3 b = position(inB, inBonesB, inWeightsB, inNormal);
    vec3 c = position(inC, inBonesC, inWeightsC, inNormal);
    float ax, ay, aq, bx, by, bq, cx, cy, cq;
    bool ao, bo, co;
    project(a, ax, ay, aq, ao);
    project(b, bx, by, bq, bo);
    project(c, cx, cy, cq, co);
    // The guard band's skip is the PS2's (the GPU clips): only the facing.
    bool skip = false;
    if (draw.cull.z != 0.0) {
        precise float cross = (cx - bx) * (by - ay) - (bx - ax) * (cy - by);
        precise float winding = draw.cull.x * inCorner.y;
        precise float facing = cross * winding;
        precise float wSign = aq * bq * cq;
        if (draw.cull.w != 0.0 && wSign < 0.0) {
            precise float turned = cross * -winding;
            skip = turned < 0.0;
        } else {
            skip = facing < 0.0;
        }
    }
    int corner = inCorner.x < 0.5 ? 0 : (inCorner.x < 1.5 ? 1 : 2);
    vec3 p = corner == 0 ? a : (corner == 1 ? b : c);
    vec4 bones = corner == 0 ? inBonesA : (corner == 1 ? inBonesB : inBonesC);
    vec4 weights = corner == 0 ? inWeightsA : (corner == 1 ? inWeightsB : inWeightsC);
    int count = int(M.q[2].z);
    uint uvKind = uint(M.q[1].w);
    uint colorKind = uint(M.q[0].w);
    uint flags = uint(M.q[2].w);

    // The UV: the scaled int, or what F makes of it.
    precise vec2 uv = floats() ? inUv : vec2(itof15(inUv.x) * M.q[2].x, itof15(inUv.y) * M.q[2].y);
    // The scroll F (Uv 0): moved by quad 88.xy (row 42; 0 when it is off).
    if (uvKind == 0u)
        uv = uv + M.q[42].xy;
    // The color: the base, times the vertex color for C 0.
    vec4 base = M.q[3];
    precise vec4 cv = vec4(itof15(inColor.x), itof15(inColor.y), itof15(inColor.z), itof15(inColor.w)) * 256.0;
    precise vec4 color = colorKind == 0u ? base * cv : base;
    // 70362be4 with 87.y & 0x100 (flag 2): the vertex color added (the raw
    // int16, cv * 128), its alpha still multiplied.
    if (colorKind == 0u && (uint(M.q[2].w) & 2u) != 0u) {
        precise vec3 added = base.rgb + cv.rgb * 128.0;
        precise float alpha = base.a * cv.a;
        color = vec4(added, alpha);
    }
    if (uvKind == 4u) {
        // The beam: the position projected through quads 72-75.
        vec4 c0 = M.q[32], c1 = M.q[33], c2 = M.q[34], c3 = M.q[35];
        precise float x = ((c0.x * p.x + c1.x * p.y) + c2.x * p.z) + c3.x * 1.0;
        precise float y = ((c0.y * p.x + c1.y * p.y) + c2.y * p.z) + c3.y * 1.0;
        precise float w = ((c0.w * p.x + c1.w * p.y) + c2.w * p.z) + c3.w * 1.0;
        precise float r = w != 0.0 ? 1.0 / w : ((floatBitsToUint(w) >> 31) != 0u ? -3.402823466e38 : 3.402823466e38);
        precise float dx = x * r - M.q[30].z;
        precise float dy = (0.0 - y * r) - M.q[30].w;
        uv = vec2(((dx * M.q[31].x + M.q[30].x) + 1.0) * 0.5, ((dy * M.q[31].x + M.q[30].y) + 1.0) * 0.5);
    }
    if (uvKind == 7u) {
        // The projector: the position through the texture matrix, divided.
        vec4 t0 = M.q[38], t1 = M.q[39], t2 = M.q[40], t3 = M.q[41];
        precise vec4 t = t0 * p.x + t1 * p.y + t2 * p.z + t3 * 1.0;
        precise float tq = t.w != 0.0 ? 1.0 / t.w : ((floatBitsToUint(t.w) >> 31) != 0u ? -3.402823466e38 : 3.402823466e38);
        uv = vec2(t.x * tq, t.y * tq);
    }
    // The float-color D (flag 256): the colors section's floats as they are.
    if ((flags & 256u) != 0u)
        color = inColor;
    if ((flags & 1u) != 0u)
        color.rgb = vec3(128.0);
    bool lit = colorKind >= 2u && colorKind <= 5u;
    bool normals = lit || uvKind == 1u || uvKind == 2u || uvKind == 5u || uvKind == 3u;
    if (normals) {
        precise vec3 n = decodeNormal(inNormal);
        if (count > 0 && (flags & 4u) != 0u)
            n = skin(n, bones, weights, count, false);
        if (lit) {
            // Four directional lights (lane i of quads 100-102), clamped at
            // 0, their colors, and the ambient.
            vec4 d0 = M.q[4], d1 = M.q[5], d2 = M.q[6];
            precise vec4 d = max(d0 * n.x + d1 * n.y + d2 * n.z, vec4(0.0));
            vec4 amb = M.q[11];
            precise vec3 rgb = amb.rgb * 1.0 + M.q[7].rgb * d.x + M.q[8].rgb * d.y + M.q[9].rgb * d.z +
                               M.q[10].rgb * d.w;
            precise vec4 lc = vec4(rgb, amb.w);
            color = lc;
            if (colorKind == 4u || colorKind == 5u) {
                // Four point lights: d = light - W, f = max(s / |d| - s w, 0).
                vec4 m0 = M.q[20], m1 = M.q[21], m2 = M.q[22], m3 = M.q[23];
                precise vec3 W = vec3(((m0.x * p.x + m1.x * p.y) + m2.x * p.z) + m3.x * 1.0,
                                      ((m0.y * p.x + m1.y * p.y) + m2.y * p.z) + m3.y * 1.0,
                                      ((m0.z * p.x + m1.z * p.y) + m2.z * p.z) + m3.z * 1.0);
                vec4 r0 = M.q[24], r1 = M.q[25], r2 = M.q[26];
                precise vec3 N = vec3((r0.x * n.x + r1.x * n.y) + r2.x * n.z, (r0.y * n.x + r1.y * n.y) + r2.y * n.z,
                                      (r0.z * n.x + r1.z * n.y) + r2.z * n.z);
                precise vec3 sum = vec3(0.0);
                for (int i = 0; i < 4; ++i) {
                    vec4 L = M.q[12 + i];
                    precise vec3 dl = L.xyz - W;
                    precise float s = max((dl.x * N.x + dl.y * N.y) + dl.z * N.z, 0.0);
                    precise float root = sqrt(abs((dl.x * dl.x + dl.y * dl.y) + (dl.z * dl.z) * 1.0));
                    precise float inverse = root != 0.0 ? 1.0 / root : 3.402823466e38;
                    precise float f = max(s * inverse - s * L.w, 0.0);
                    sum = i == 0 ? M.q[16 + i].rgb * f : sum + M.q[16 + i].rgb * f;
                }
                color.rgb = color.rgb + sum;
            }
            if (colorKind == 4u) {
                float sign = (flags & 2u) != 0u ? -1.0 : 1.0;
                precise vec3 l = color.rgb;
                color.rgb = max((l * cv.rgb) * sign, (l + cv.rgb * 128.0) * -sign);
                color.a = lc.a * cv.a;
            }
            if (colorKind == 3u) {
                // 2460e528: times the vertex color, or with 87.y & 0x100
                // (flag 2) plus it (the raw int16), the alpha times it.
                if ((flags & 2u) != 0u) {
                    precise vec3 added = lc.rgb + cv.rgb * 128.0;
                    precise float alpha = lc.a * cv.a;
                    color = vec4(added, alpha);
                } else {
                    color = lc * cv;
                }
            }
        }
        if (uvKind == 1u) {
            // The sphere map: the normal through the view's rotation, (n + 1) / 2.
            vec4 s01 = M.q[28];
            vec2 s2 = M.q[29].xy;
            uv = vec2((s01.x * n.x + s01.z * n.y + s2.x * n.z + 1.0) * 0.5,
                      (s01.y * n.x + s01.w * n.y + s2.y * n.z + 1.0) * 0.5);
        } else if (uvKind == 2u) {
            // The reflection: the view vector reflected in the normal.
            vec4 m0 = M.q[20], m1 = M.q[21], m2 = M.q[22], m3 = M.q[23];
            precise vec3 W = vec3(((m0.x * p.x + m1.x * p.y) + m2.x * p.z) + m3.x * 1.0,
                                  ((m0.y * p.x + m1.y * p.y) + m2.y * p.z) + m3.y * 1.0,
                                  ((m0.z * p.x + m1.z * p.y) + m2.z * p.z) + m3.z * 1.0);
            precise vec3 dv = W - M.q[27].xyz;
            precise float len = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
            precise float inv = len != 0.0 ? 1.0 / len : len;
            precise vec3 V = dv * inv;
            vec4 r0 = M.q[24], r1 = M.q[25], r2 = M.q[26];
            precise vec3 N = vec3((r0.x * n.x + r1.x * n.y) + r2.x * n.z, (r0.y * n.x + r1.y * n.y) + r2.y * n.z,
                                  (r0.z * n.x + r1.z * n.y) + r2.z * n.z);
            precise float dotVN = (V.x * N.x + V.y * N.y) + V.z * N.z;
            precise float twice = dotVN + dotVN;
            uv = vec2(((V.x - N.x * twice) + 1.0) * 0.5, ((V.z - N.z * twice) + 1.0) * 0.5);
        } else if (uvKind == 5u) {
            // The ramp: (n . (quads 100-102).x + 1) / 2, 0.
            uv = vec2(((M.q[4].x * n.x + M.q[5].x * n.y) + M.q[6].x * n.z + 1.0) * 0.5, 0.0);
        }
    }

    emit(p, uv, color, skip);
}
