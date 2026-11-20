#version 460 core
// Mesh vertex shader for indirect multi-draw: per-object data comes from the instance buffer,
// indexed by gl_BaseInstance (set per draw command) + gl_InstanceID.
#include "common.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

out vec3 v_worldPos;
#ifdef SHADOW_PASS
uniform mat4 u_lightViewProjection;
#else
out vec3 v_normal;
out vec3 v_patternPos;
out vec3 v_objectNormal;
out vec2 v_uv;
flat out uint v_instance;
#endif

void main() {
    uint instance = uint(gl_BaseInstance + gl_InstanceID);
    vec4 world = u_instances[instance].model * vec4(a_position, 1.0);
    v_worldPos = world.xyz;
#ifdef SHADOW_PASS
    gl_Position = u_lightViewProjection * world;
#else
    v_normal = mat3(u_instances[instance].normalMatrix) * a_normal;
    v_patternPos = a_position * u_instances[instance].scaleMaterial.xyz;
    v_objectNormal = a_normal;
    v_uv = a_uv * u_instances[instance].uvScale.xy;
    v_instance = instance;
    gl_Position = u_viewProjection * world;
#endif
}
