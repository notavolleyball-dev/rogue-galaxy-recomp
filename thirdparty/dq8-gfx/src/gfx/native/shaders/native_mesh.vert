#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md step 4b). Object space through model x view x
// projection to clip space (the game's projection: GL's -w..w in z), then
// the camera's viewport to the target's pixels. Depth is written reversed,
// 1 at the near plane and 0 at the far, as a float.
//
// Matrices are the game's row vectors, sent as they are: GLSL's column-major
// reading makes `m * v` the game's `v * m`.
// The draw's uniforms: the frame's draw table, one row per draw, found by
// the draw id each vertex carries (neighbouring draws are one GPU call).
// One draw's row; Renderer::GpuDraw is its C++ twin (std430).
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
#define draw draws[inDraw]

// One surface drawn by several meshes in several pipelines (terrain layers,
// a light laid on a wall) must get bit-identical positions and depth.
invariant gl_Position;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inFog;
layout(location = 4) in uint inDraw;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outFog;
layout(location = 3) flat out uint outDraw;

void main() {
    vec4 clip = draw.clip * vec4(inPosition, 1.0);
    vec4 window = draw.viewport * clip;
    outDraw = inDraw;
    outUv = inUv;
    outColor = inColor;
    outFog = draw.target.z != 0.0 ? inFog : clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    // Window pixels to SDL's normalized coordinates (y up), times w so the
    // GPU's divide gives them back.
    // The program caps its depth per vertex (depthMax); only in front of
    // the eye, where the cap means a depth (behind it the GPU clips).
    float depth = dot(draw.depthRow, vec4(inPosition, 1.0));
    if (draw.depthLimit.y != 0.0 && clip.w > 0.0)
        depth = min(depth, draw.depthLimit.x * clip.w);
    gl_Position = vec4(window.x * 2.0 / draw.target.x - window.w,
                       window.w - window.y * 2.0 / draw.target.y,
                       depth + draw.target.w * clip.w,
                       clip.w);
}
