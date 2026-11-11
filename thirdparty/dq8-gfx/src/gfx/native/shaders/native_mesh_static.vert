#version 450

// The native pipeline's retained mesh draw (Rogue Galaxy: added; the game's
// docs/NATIVE-BOUNDARY.md, issue 2). A mesh the game sends unchanged frame
// after frame stays in a vertex buffer of its own; each triangle is three
// vertices that all carry the triangle's three corners, so the VU1 program's
// own rule for dropping a triangle (a corner outside the guard band, or
// facing away) is judged here, by the arithmetic the producer's cullMesh
// uses on the CPU for a mesh sent whole (rg::nativeproducer, gsNativeMeshSkips):
// separate multiplies and adds in the same order (`precise`: nothing fused or
// reordered). A dropped triangle's corners all leave the clip volume at one
// point. The rest is native_mesh.vert's.
//
// The draw's row is the instance index (the draw call's first instance).
struct DrawRow {
    // The vertex stage's.
    mat4 clip;     // model x view x projection
    mat4 viewport; // clip to the target's pixels (w kept)
    vec4 target;   // the target's width and height in game pixels; z: fog per vertex (a window-space draw);
                   // w: added to the depth (an overlay pulled toward the camera)
    vec4 fog;      // F = clamp(fog.x + fog.w * w, at most fog.y, at least fog.z)
    vec4 depthRow; // reversed depth (w - z) / 2 from the position, made in double, with the program's depthScale
    vec4 depthLimit; // x: the program's depthMax as this depth; y: 1 when it applies
    // The fragment stage's.
    // mode x: 0 untextured, 1 RGBA, 2 Index8 through the palette, 3 a target
    //    (its alpha stored as A / 0x80, the blend's factor)
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
    // x: a channel shuffle (1: alpha's bits 2-7 from green's), z: clamp u,
    // w: clamp v (y unused)
    vec4 blend;
    vec4 fogColor; // 0-1
    // x: the texel's alpha replaced by this (0-255) when not negative (a
    // CT24 read); y: then 0 where the texel is black (AEM); z: the
    // destination alpha test, 0 none, 1 drawn where the target's alpha is
    // below 0x80, 2 where at least (read from `palette`, the target's copy)
    vec4 texa;
    // The GS's CLAMP: x, y the u region (min, max), z, w the v region;
    // wrap.x, .y the modes (0 repeat, 1 clamp, 2 region clamp, 3 region
    // repeat).
    uvec4 region;
    uvec4 wrap;
    // A retained mesh's cull (native_mesh_static.vert): the program's matrix
    // to GS window coordinates, its guard band, x: the winding sign, y: the
    // guard band test on, z: the backface test on, w: the chunk is clipped;
    // and the cap of its vertex colors.
    mat4 cullMatrix;
    vec4 bandScale;
    vec4 bandOffset;
    vec4 cull;
    vec4 colorMax;
};
layout(std430, set = 0, binding = 0) readonly buffer Draws {
    DrawRow draws[];
};
#define inDraw uint(gl_InstanceIndex)
#define draw draws[inDraw]

invariant gl_Position;

layout(location = 0) in vec3 inA;
layout(location = 1) in vec3 inB;
layout(location = 2) in vec3 inC;
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inColor;
layout(location = 5) in vec2 inCorner; // x: which corner this vertex is; y: the triangle's place in the strip, +1 or -1

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outFog;
layout(location = 3) flat out uint outDraw;

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

void main() {
    float ax, ay, aq, bx, by, bq, cx, cy, cq;
    bool ao, bo, co;
    project(inA, ax, ay, aq, ao);
    project(inB, bx, by, bq, bo);
    project(inC, cx, cy, cq, co);
    bool skip = ao || bo || co;
    if (draw.cull.z != 0.0) {
        precise float cross = (cx - bx) * (by - ay) - (bx - ax) * (cy - by);
        precise float winding = draw.cull.x * inCorner.y;
        precise float facing = cross * winding;
        // A clipped chunk's triangle with a corner behind the eye: its facing
        // alone, with the sign turned (cullMesh).
        precise float wSign = aq * bq * cq;
        if (draw.cull.w != 0.0 && wSign < 0.0) {
            precise float turned = cross * -winding;
            skip = turned < 0.0;
        } else {
            skip = skip || facing < 0.0;
        }
    }

    vec3 inPosition = inCorner.x < 0.5 ? inA : (inCorner.x < 1.5 ? inB : inC);
    vec4 clip = draw.clip * vec4(inPosition, 1.0);
    vec4 window = draw.viewport * clip;
    outDraw = inDraw;
    outUv = inUv;
    outColor = min(inColor, draw.colorMax);
    outFog = clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    float depth = dot(draw.depthRow, vec4(inPosition, 1.0));
    if (draw.depthLimit.y != 0.0 && clip.w > 0.0)
        depth = min(depth, draw.depthLimit.x * clip.w);
    gl_Position = vec4(window.x * 2.0 / draw.target.x - window.w,
                       window.w - window.y * 2.0 / draw.target.y,
                       depth + draw.target.w * clip.w,
                       clip.w);
    if (skip)
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
}
