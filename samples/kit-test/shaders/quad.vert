#version 450
// A small quad per draw, placed by push constants: no vertex buffers, so a draw costs only what the
// driver spends on it (the point of the draw-cost test).
layout(push_constant) uniform Push {
    vec2 offset;
    vec2 size;
    vec4 color;
} pc;
layout(location = 0) out vec4 v_color;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 p = pc.offset + corners[gl_VertexIndex] * pc.size;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_color = pc.color;
}
