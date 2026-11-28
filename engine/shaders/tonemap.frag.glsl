#version 460 core
// HDR -> display: exposure, + bloom (already exposure-scaled), filmic curve (rational fit of the
// ACES reference curve), sRGB encode.
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_hdr;
layout(binding = 1) uniform sampler2D u_bloom;
uniform float u_bloomIntensity;
in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 Filmic(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 LinearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

#ifdef ENTITY_ID_COLORS
// Instance-segmentation preview (Camera::entityIdColors): the resolved entity id as a false colour,
// identical to Render::EntityIdColor on the CPU.
layout(binding = 2) uniform usampler2D u_ids;
void main() {
    uint id = texelFetch(u_ids, ivec2(gl_FragCoord.xy), 0).r;
    if (id == 0u) { o_color = vec4(13.0, 13.0, 15.0, 255.0) / 255.0; return; }
    uint h = id * 2654435761u;
    vec3 c = vec3(64u + ((h >> 8u) & 255u) * 191u / 255u, 64u + ((h >> 16u) & 255u) * 191u / 255u, 64u + ((h >> 24u) & 255u) * 191u / 255u);
    o_color = vec4(c / 255.0, 1.0);
}
#else
void main() {
    vec3 hdr = texture(u_hdr, v_uv).rgb * u_skyHorizon.w;
    if (u_bloomIntensity > 0.0) hdr += texture(u_bloom, v_uv).rgb * u_bloomIntensity;
    o_color = vec4(LinearToSrgb(Filmic(hdr)), 1.0);
}
#endif
