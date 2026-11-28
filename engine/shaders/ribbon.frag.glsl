#version 460 core
// Additive ribbon: soft edges across the strip, fog-attenuated like the scene.
#include "common.glsl"
#include "fog.glsl"

in vec4 v_color;
in float v_side;
layout(location = 0) out vec4 o_color;

void main() {
    float edge = 1.0 - v_side * v_side;            // 1 at the centre line, 0 at the edges
    float core = edge * edge;
    o_color = vec4(v_color.rgb * v_color.a * (0.35 * edge + 0.65 * core), 0.0);
}
