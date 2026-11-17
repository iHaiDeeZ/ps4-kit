#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// The descriptor tests' fragment shader: a uniform buffer and a texture, as a translated game's draws
// bind them (set 0: binding 0 the buffer, binding 1 the texture).
layout(set = 0, binding = 0) uniform Tint {
    vec4 tint;
} u;
layout(set = 0, binding = 1) uniform sampler2D tex;

void main() {
    o_color = v_color * u.tint * texture(tex, vec2(0.5));
}
