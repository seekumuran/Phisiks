#version 460 core
// Sphere sprites for GPU particles and XPBD rope nodes, read straight from the simulation buffer
// (u_stride vec4s per element: position in .xyz, radius in .w unless u_radius > 0, velocity in the
// next vec4 when u_stride > 1).
#include "common.glsl"

#ifdef PHISIKS_GL41
layout(binding = 13) uniform samplerBuffer u_elementTexels;
vec4 LoadElement(uint i) { return texelFetch(u_elementTexels, int(i)); }
#else
layout(std430, binding = 4) readonly buffer Elements { vec4 u_elements[]; };
vec4 LoadElement(uint i) { return u_elements[i]; }
#endif
uniform uint u_stride;
uniform float u_radius;
uniform float u_pointScale;   // pixels per metre at distance 1 (perspective) or per metre (orthographic)

out vec3 v_center;
out float v_radius;
out float v_speed;

void main() {
    uint base = uint(gl_VertexID) * u_stride;
    vec4 e = LoadElement(base);
    v_center = e.xyz;
    v_radius = u_radius > 0.0 ? u_radius : e.w;
    v_speed = u_stride > 1u ? length(LoadElement(base + 1u).xyz) : 0.0;
    gl_Position = u_viewProjection * vec4(e.xyz, 1.0);
    gl_PointSize = clamp(2.0 * v_radius * u_pointScale / gl_Position.w, 1.0, 256.0);
}
