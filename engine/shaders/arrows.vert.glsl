#version 460 core
// Vector-field arrows: 6 line vertices per grid sample (shaft + two head strokes), length and
// colour from the GPU-sampled field (field_sample.comp). Drawn with lines.frag.
#include "common.glsl"

layout(std430, binding = 14) readonly buffer Samples { vec4 u_samples[]; };
uniform ivec3 u_resolution;
uniform vec3 u_boundsMin;
uniform vec3 u_boundsMax;
uniform float u_arrowScale;     // metres per (m/s^2)
uniform float u_maxLength;      // metres
out vec4 v_color;

void main() {
    int arrow = gl_VertexID / 6;
    int k = gl_VertexID % 6;
    ivec3 c = ivec3(arrow % u_resolution.x, (arrow / u_resolution.x) % u_resolution.y, arrow / (u_resolution.x * u_resolution.y));
    vec3 base = mix(u_boundsMin, u_boundsMax, (vec3(c) + 0.5) / vec3(u_resolution));
    vec4 s = u_samples[arrow];
    float magnitude = s.w;
    vec3 dir = magnitude > 1e-6 ? s.xyz / magnitude : vec3(0.0, 1.0, 0.0);
    float len = magnitude > 1e-6 ? min(magnitude * u_arrowScale, u_maxLength) : 0.0;
    vec3 tip = base + dir * len;
    vec3 side = normalize(abs(dir.y) < 0.9 ? cross(dir, vec3(0, 1, 0)) : cross(dir, vec3(1, 0, 0)));
    vec3 back = tip - dir * len * 0.3;
    vec3 p = k == 0 ? base : k == 1 ? tip : k == 2 ? tip : k == 3 ? back + side * len * 0.15 : k == 4 ? tip : back - side * len * 0.15;
    // Colour: log-scaled magnitude, cool (weak) -> hot (strong).
    float t = clamp(log2(1.0 + magnitude) / 5.0, 0.0, 1.0);
    v_color = vec4(mix(vec3(0.2, 0.6, 1.0), vec3(1.0, 0.35, 0.15), t), 1.0);
    gl_Position = u_viewProjection * vec4(p, 1.0);
}
