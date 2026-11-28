#version 460 core
// HDR bloom through a mip chain (each level half the previous).
//   BLOOM_PREFILTER : exposure, soft-knee threshold and firefly clamp, then the 13-tap downsample
//   BLOOM_DOWN      : 13-tap downsample (a 4x4 box made of overlapping 2x2 boxes: no aliasing / flicker)
//   BLOOM_UP        : 3x3 tent filter of the smaller level, added (ONE, ONE) onto the larger one
#include "common.glsl"

layout(binding = 0) uniform sampler2D u_source;
uniform vec2 u_texel;              // 1 / source size
uniform vec4 u_threshold;          // threshold, knee, exposure, radius (up)
layout(location = 0) out vec4 o_color;
in vec2 v_uv;

vec3 Down13(vec2 uv) {
    vec3 a = texture(u_source, uv + u_texel * vec2(-2, -2)).rgb, b = texture(u_source, uv + u_texel * vec2(0, -2)).rgb, c = texture(u_source, uv + u_texel * vec2(2, -2)).rgb;
    vec3 d = texture(u_source, uv + u_texel * vec2(-2, 0)).rgb, e = texture(u_source, uv).rgb, f = texture(u_source, uv + u_texel * vec2(2, 0)).rgb;
    vec3 g = texture(u_source, uv + u_texel * vec2(-2, 2)).rgb, h = texture(u_source, uv + u_texel * vec2(0, 2)).rgb, i = texture(u_source, uv + u_texel * vec2(2, 2)).rgb;
    vec3 j = texture(u_source, uv + u_texel * vec2(-1, -1)).rgb, k = texture(u_source, uv + u_texel * vec2(1, -1)).rgb;
    vec3 l = texture(u_source, uv + u_texel * vec2(-1, 1)).rgb, m = texture(u_source, uv + u_texel * vec2(1, 1)).rgb;
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

void main() {
#ifdef BLOOM_UP
    vec2 r = u_texel * u_threshold.w;
    vec3 sum = texture(u_source, v_uv + vec2(-r.x, -r.y)).rgb + 2.0 * texture(u_source, v_uv + vec2(0.0, -r.y)).rgb + texture(u_source, v_uv + vec2(r.x, -r.y)).rgb
             + 2.0 * texture(u_source, v_uv + vec2(-r.x, 0.0)).rgb + 4.0 * texture(u_source, v_uv).rgb + 2.0 * texture(u_source, v_uv + vec2(r.x, 0.0)).rgb
             + texture(u_source, v_uv + vec2(-r.x, r.y)).rgb + 2.0 * texture(u_source, v_uv + vec2(0.0, r.y)).rgb + texture(u_source, v_uv + vec2(r.x, r.y)).rgb;
    o_color = vec4(sum / 16.0, 1.0);
#else
    vec3 color = Down13(v_uv);
#ifdef BLOOM_PREFILTER
    color = min(color * u_threshold.z, vec3(64.0));
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(u_threshold.y, 1e-4);
    float soft = clamp(brightness - u_threshold.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    color *= max(soft, brightness - u_threshold.x) / max(brightness, 1e-4);
#endif
    o_color = vec4(color, 1.0);
#endif
}
