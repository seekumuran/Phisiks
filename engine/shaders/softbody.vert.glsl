#version 460 core
// XPBD cloth / soft-body surface: vertex positions and normals come straight from the solver's
// buffers (gl_VertexID = particle index from the element buffer). Shaded by forward.frag with the
// material/entity of instance u_instance; SHADOW_PASS renders it into the shadow maps.
#include "common.glsl"

#ifdef PHISIKS_GL41
layout(binding = 13) uniform samplerBuffer u_positionTexels;
layout(binding = 14) uniform samplerBuffer u_normalTexels;
vec4 LoadPosition(int i) { return texelFetch(u_positionTexels, i); }
vec4 LoadNormal(int i) { return texelFetch(u_normalTexels, i); }
#else
layout(std430, binding = 4) readonly buffer Positions { vec4 u_x[]; };
layout(std430, binding = 8) readonly buffer Normals { vec4 u_normals[]; };
vec4 LoadPosition(int i) { return u_x[i]; }
vec4 LoadNormal(int i) { return u_normals[i]; }
#endif

out vec3 v_worldPos;
#ifdef SHADOW_PASS
uniform mat4 u_lightViewProjection;
#else
uniform uint u_instance;
out vec3 v_normal;
out vec3 v_patternPos;
out vec3 v_objectNormal;
out vec2 v_uv;
flat out uint v_instance;
#endif

void main() {
    vec3 p = LoadPosition(gl_VertexID).xyz;
    v_worldPos = p;
#ifdef SHADOW_PASS
    gl_Position = u_lightViewProjection * vec4(p, 1.0);
#else
    vec3 n = LoadNormal(gl_VertexID).xyz;
    v_normal = n;
    v_patternPos = p;          // world-space metres: patterns stay put while the surface deforms only locally
    v_objectNormal = n;
    v_uv = vec2(0.0);
    v_instance = u_instance;
    gl_Position = u_viewProjection * vec4(p, 1.0);
#endif
}
