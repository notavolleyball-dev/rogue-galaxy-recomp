#version 450

// A block of a depth target as the GS's Z values (Rogue Galaxy: added), for
// the depth reads src/gfx/gs/gs_depth_probe.h serves. Draws store Z divided
// by the depth format's largest value (gs_draw.vert, gs_native.vert), so
// this multiplies back and rounds; for PSMZ32 the float holds 24 bits of it,
// as the draw that stored it did. Each GS pixel is read at its sample point,
// the first physical pixel of its block, as gs_target_copy.frag reads.

layout(set = 2, binding = 0) uniform sampler2D depth;
layout(set = 3, binding = 0) uniform ProbeParams {
    // x, y: the block's origin in GS pixels; z: resolution scale;
    // w: the depth format's largest value.
    uvec4 block;
    // x, y: the target's size in GS pixels (outside it reads 0).
    uvec4 bounds;
} params;
layout(location = 0) out uint outZ;

void main() {
    uvec2 pixel = params.block.xy + uvec2(gl_FragCoord.xy);
    if (pixel.x >= params.bounds.x || pixel.y >= params.bounds.y) {
        outZ = 0u;
        return;
    }
    float d = clamp(texelFetch(depth, ivec2(pixel * params.block.z), 0).r, 0.0, 1.0);
    // 4294967040 is the largest float below 2^32.
    float z = min(d * float(params.block.w) + 0.5, 4294967040.0);
    outZ = min(uint(z), params.block.w);
}
