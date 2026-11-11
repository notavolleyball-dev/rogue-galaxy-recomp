#version 450

// One triangle over the whole viewport, without a vertex upload: the
// presentation passes (the sides, FSR 1) draw with it.
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
