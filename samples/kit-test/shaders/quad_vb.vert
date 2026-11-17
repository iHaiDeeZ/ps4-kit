#version 450
// quad.vert with its corners from a vertex buffer (the vertex/index buffer test).
layout(push_constant) uniform Push {
    vec2 offset;
    vec2 size;
    vec4 color;
} pc;
layout(location = 0) in vec2 corner;
layout(location = 0) out vec4 v_color;

void main() {
    vec2 p = pc.offset + corner * pc.size;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_color = pc.color;
}
