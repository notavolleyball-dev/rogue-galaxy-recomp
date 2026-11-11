#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md step 4b). Object space through model x view x
// projection to the camera's standard clip space (z the reversed depth: 1
// at the near plane, 0 at the far), then over the whole target. A 2D draw's
// positions are the target's pixels and its z the depth already.
//
// Matrices are row vectors, sent as they are: GLSL's column-major reading
// makes `m * v` the row vector's `v * m`.
// The draw's uniforms: the frame's draw table, one row per draw, found by
// the draw id each vertex carries (neighbouring draws are one GPU call).
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
layout(location = 4) out float outQ; // the UV's divisor (native_model.vert's affine draws)

void main() {
    vec4 clip = draw.clip * vec4(inPosition, 1.0);
    vec4 window = draw.viewport * clip;
    outDraw = inDraw;
    outUv = inUv;
    outQ = 1.0;
    outColor = inColor;
    outFog = draw.target.z != 0.0 ? inFog : clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    // The program caps its depth per vertex (depthMax); only in front of
    // the eye, where the cap means a depth (behind it the GPU clips).
    float depth = dot(draw.depthRow, vec4(inPosition, 1.0));
    if (draw.depthLimit.y != 0.0 && clip.w > 0.0)
        depth = min(depth, draw.depthLimit.x * clip.w);
    gl_Position = vec4(window.x, window.y, depth + draw.target.w * clip.w, clip.w);
}
