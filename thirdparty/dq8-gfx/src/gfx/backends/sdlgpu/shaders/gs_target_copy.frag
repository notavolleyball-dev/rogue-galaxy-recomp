#version 450

// A CT32 texture's texels from the CT32 target that drew them, sparing a
// readback and a CPU expansion (Rogue Galaxy: added; as gs_index8.frag, for
// the formats whose texel is the target's pixel). A page is 64x32 pixels in
// both, so a texture pixel is the target's pixel in the same page less the
// target's base.

layout(set = 2, binding = 0) uniform sampler2D target;
layout(set = 3, binding = 0) uniform CopyParams {
    // x: texture base page, y: texture pages per row,
    // z: target base page, w: target pages per row | resolution scale << 16
    uvec4 pages;
} params;
layout(location = 0) out vec4 outColor;

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint page = (params.pages.x + (p.y >> 5u) * params.pages.y + (p.x >> 6u)) & 511u;
    uint relative = (page + 512u - params.pages.z) & 511u;
    uint pagesPerRow = params.pages.w & 65535u;
    // An upscaled target is read at each GS pixel's sample point, the first
    // physical pixel of its block.
    uint scale = max(params.pages.w >> 16u, 1u);
    uvec2 origin = uvec2((relative % pagesPerRow) * 64u, (relative / pagesPerRow) * 32u);
    outColor = texelFetch(target, ivec2((origin + (p & uvec2(63u, 31u))) * scale), 0);
}
