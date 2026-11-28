#version 460 core
// Debug lines are given in display (sRGB) colour; convert to linear HDR so they survive tonemapping
// roughly unchanged.
#include "common.glsl"

in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 linearColor = pow(v_color.rgb, vec3(2.2)) * 1.6 / max(u_skyHorizon.w, 1e-3);
    o_color = vec4(linearColor, v_color.a);
}
