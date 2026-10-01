#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 0) out vec3 out_color;

void main() {
    // The acceptance source is Cartographer's XZ plane. Project it into the
    // native headless clip-space target without introducing a camera contract.
    gl_Position = vec4(in_position.x, in_position.z, 0.0, 1.0);
    out_color = in_color;
}
