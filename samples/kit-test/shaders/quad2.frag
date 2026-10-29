#version 450
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// A second fragment shader: switching to its pipeline changes shaders, not only state.
void main() {
    o_color = v_color.bgra;
}
