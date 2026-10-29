#version 450

// A strip a VU1 program would have drawn, from the program's own inputs
// (GSNativeMesh, Rogue Galaxy: added): the program's transform, done here, to
// the GS window pixels, depth, Q, S/T, colour and fog gs_draw.vert passes on,
// so gs_draw.frag draws it as it draws the program's triangles. The position
// goes out homogeneous, so the GPU clips the strip where the program would
// have dropped a triangle at its guard band or clipped it in slot E.
//
// The vertex is a GsGpuVertex slot read another way: x, y and z the object-
// space position, q and s the texture coordinates before the divide, the
// colour before its clamp (0-255).

layout(location = 0) in vec2 inPositionXY;
layout(location = 1) in vec2 inPositionZU;
layout(location = 2) in vec2 inV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in float inUnused;

layout(set = 1, binding = 0) uniform VertexParams {
    // As gs_draw.vert's.
    vec4 targetSize;
    vec4 adjust;
    // Rows: clip = x * matrix[0] + y * matrix[1] + z * matrix[2] + matrix[3],
    // in GS window coordinates (XYOFFSET included) once divided by w.
    vec4 matrix[4];
    // F = clamp(fog.x + fog.w * w, at most fog.y, at least fog.z).
    vec4 fog;
    vec4 colorMax;
    // xy: XYOFFSET in pixels. z: 1 / the depth format's largest value.
    vec4 misc;
} params;

layout(location = 0) noperspective out vec2 vTexCoord;
layout(location = 1) noperspective out float vQ;
layout(location = 2) noperspective out vec4 vColor;
layout(location = 3) noperspective out float vFog;

void main() {
    vec4 clip = inPositionXY.x * params.matrix[0] + inPositionXY.y * params.matrix[1] +
                inPositionZU.x * params.matrix[2] + params.matrix[3];
    float q = 1.0 / clip.w;
    vec3 projected = clip.xyz * q;
    // The program's FTOI4: x and y in 12.4 fixed point, Z in whole units.
    vec2 pixel = trunc(projected.xy * 16.0) / 16.0 - params.misc.xy;
    float depth = clamp(trunc(projected.z) * params.misc.z, 0.0, 1.0);

    vec2 normalised = (pixel + params.adjust.xy) * params.targetSize.zw;
    vec2 ndc = normalised * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y * params.adjust.z, depth, 1.0) * clip.w;

    vTexCoord = vec2(inPositionZU.y, inV.x) * q;
    vQ = q;
    vColor = trunc(min(inColor, params.colorMax)) / 255.0;
    // In the program's order: the upper bound first.
    float f = max(min(params.fog.x + params.fog.w * clip.w, params.fog.y), params.fog.z);
    vFog = trunc(f) / 255.0;
}
