#version 450

// What fills the window around the game picture, which is drawn over it
// afterwards (Rogue Galaxy: added): the source scaled to cover the window,
// cut evenly at the two sides that overflow. The blurred picture is a small
// copy, read through nine taps a texel apart and darkened; the player's
// image is read as it is.
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform SideParams {
    vec4 window; // width, height, brightness, 1 to blur
    vec4 texels; // the source's width and height
} params;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 window = params.window.xy;
    vec2 size = params.texels.xy;
    vec2 uv = gl_FragCoord.xy / window;
    float windowAspect = window.x / window.y;
    float sourceAspect = size.x / size.y;
    if (sourceAspect > windowAspect)
        uv.x = 0.5 + (uv.x - 0.5) * windowAspect / sourceAspect;
    else
        uv.y = 0.5 + (uv.y - 0.5) * sourceAspect / windowAspect;
    vec3 color = vec3(0.0);
    if (params.window.w != 0.0) {
        vec2 texel = 1.0 / size;
        float total = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
                color += texture(source, uv + vec2(x, y) * texel).rgb * weight;
                total += weight;
            }
        }
        color /= total;
    } else {
        color = texture(source, uv).rgb;
    }
    outColor = vec4(color * params.window.z, 1.0);
}
