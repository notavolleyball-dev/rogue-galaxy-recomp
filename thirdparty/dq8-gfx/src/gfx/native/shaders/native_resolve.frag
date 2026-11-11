#version 450

// The native pipeline's presentation (Rogue Galaxy: added): what a Present
// record shows, a rectangle of a target in the draw's units, times its scale
// and clamped, into the 8-bit picture the presenter fits into the window.
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform Resolve {
    ivec4 origin; // x, y: the rectangle's top left in the source
    vec4 scale;   // x: Present::scale
} params;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy) + params.origin.xy, 0);
    outColor = vec4(clamp(c.rgb * params.scale.x, 0.0, 1.0), 1.0);
}
