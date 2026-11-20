#version 460 core
// HDR -> display: exposure, filmic curve (rational fit of the ACES reference curve), sRGB encode.
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_hdr;
in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 Filmic(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 LinearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec3 hdr = texture(u_hdr, v_uv).rgb * u_skyHorizon.w;
    o_color = vec4(LinearToSrgb(Filmic(hdr)), 1.0);
}
