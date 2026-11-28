#version 460 core
// Samples the force fields on a regular grid (vector-field visualiser; also checked against the CPU).
layout(local_size_x = 64) in;
#include "fields.glsl"

layout(std430, binding = 14) writeonly buffer Samples { vec4 u_samples[]; };   // xyz acceleration, w magnitude
uniform ivec3 u_resolution;
uniform vec3 u_boundsMin;
uniform vec3 u_boundsMax;

vec3 GridPoint(uint index) {
    ivec3 c = ivec3(int(index) % u_resolution.x, (int(index) / u_resolution.x) % u_resolution.y, int(index) / (u_resolution.x * u_resolution.y));
    vec3 t = (vec3(c) + 0.5) / vec3(u_resolution);
    return mix(u_boundsMin, u_boundsMax, t);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(u_resolution.x * u_resolution.y * u_resolution.z)) return;
    vec3 a = FieldAcceleration(GridPoint(i), 7u);
    u_samples[i] = vec4(a, length(a));
}
