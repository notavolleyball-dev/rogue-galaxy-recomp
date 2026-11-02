#version 450

// Expands an 8-bit indexed texture from the CT32 target that drew it, sparing a
// readback. A page is the same 8 KiB in both formats; `placement` maps a texel
// of a page to its target pixel and byte, built from the CPU swizzle tables.

layout(set = 2, binding = 0) uniform sampler2D target;
layout(set = 2, binding = 1) uniform sampler2D palette;
// 128x192: rows 0-63 one texel per PSMT8 texel of a page, rows 64-191 one
// per PSMT4 texel (Rogue Galaxy: added): the CT32 pixel holding it in R (x)
// and G (y), which of that pixel's bytes in B, and for PSMT4 which nibble
// in A.
layout(set = 2, binding = 2) uniform sampler2D placement;
// The target holding the palette when the palette is in a target (bit 24
// below); the texels' target when it is the same one.
layout(set = 2, binding = 3) uniform sampler2D paletteTarget;
layout(set = 3, binding = 0) uniform Index8Params {
    // x: texture base page, y: texture pages per row | mode << 16 (0 T8,
    // 1 T4, 2 T8H, 3 T4HL, 4 T4HH),
    // z: target base page, w: target pages per row | resolution scale << 16
    uvec4 pages;
} params;
layout(location = 0) out vec4 outColor;

uvec4 bytesAt(sampler2D source, ivec2 position) {
    return uvec4(round(texelFetch(source, position, 0) * 255.0));
}

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint mode = (params.pages.y >> 16u) & 255u;
    // Bit 24: the palette holds each entry's place in the target (x low 16
    // bits, y high), not its colour (a CT32 CLUT inside the same target).
    bool paletteInTarget = (params.pages.y & (1u << 24u)) != 0u;
    uint texturePagesPerRow = params.pages.y & 65535u;
    // T8 pages are 128x64 texels, T4 128x128; the alpha formats are CT32
    // pages, 64x32, a texel to a pixel.
    uvec2 pageShift = mode == 0u ? uvec2(7u, 6u) : (mode == 1u ? uvec2(7u, 7u) : uvec2(6u, 5u));
    uint page = (params.pages.x + (p.y >> pageShift.y) * texturePagesPerRow + (p.x >> pageShift.x)) & 511u;
    uint relative = (page + 512u - params.pages.z) & 511u;
    uint pagesPerRow = params.pages.w & 65535u;
    // An upscaled target is read at each GS pixel's sample point, the first
    // physical pixel of its block.
    uint scale = max(params.pages.w >> 16u, 1u);
    uvec2 origin = uvec2((relative % pagesPerRow) * 64u, (relative / pagesPerRow) * 32u);
    uint index;
    if (mode >= 2u) {
        uvec4 pixel = bytesAt(target, ivec2((origin + (p & uvec2(63u, 31u))) * scale));
        index = mode == 2u ? pixel.a : (mode == 3u ? (pixel.a & 15u) : (pixel.a >> 4u));
    } else if (mode == 1u) {
        uvec4 place = bytesAt(placement, ivec2(p & uvec2(127u, 127u)) + ivec2(0, 64));
        uvec4 pixel = bytesAt(target, ivec2((origin + place.xy) * scale));
        index = (pixel[place.z] >> (place.w * 4u)) & 15u;
    } else {
        uvec4 place = bytesAt(placement, ivec2(p & uvec2(127u, 63u)));
        uvec4 pixel = bytesAt(target, ivec2((origin + place.xy) * scale));
        index = pixel[place.z];
    }
    if (paletteInTarget) {
        uvec4 at = bytesAt(palette, ivec2(index, 0));
        uvec2 place = uvec2(at.x | (at.y << 8u), at.z | (at.w << 8u));
        // Bits 25-28: the palette target's resolution scale.
        uint paletteScale = max((params.pages.y >> 25u) & 15u, 1u);
        outColor = texelFetch(paletteTarget, ivec2(place * paletteScale), 0);
    } else {
        outColor = texelFetch(palette, ivec2(index, 0), 0);
    }
}
