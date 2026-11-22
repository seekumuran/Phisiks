#version 460 core
// Mesh vertex shader: per-object data comes from the instance buffer, through the remap list at
// the draw's base instance + gl_InstanceID. 4.6: indirect multi-draw, gl_BaseInstance per command;
// 4.1: one instanced draw per command with u_baseInstance.
#include "common.glsl"

#ifdef PHISIKS_GL41
layout(binding = 12) uniform usamplerBuffer u_remapTexels;
uniform uint u_baseInstance;
uint LoadRemap() { return texelFetch(u_remapTexels, int(u_baseInstance) + gl_InstanceID).r; }
#else
layout(std430, binding = 9) readonly buffer RemapBuffer { uint u_remap[]; };
uint LoadRemap() { return u_remap[uint(gl_BaseInstance + gl_InstanceID)]; }
#endif

// 0: every instance; 1: opaque materials only; 2: see-through materials only (opacity < 1).
uniform int u_alphaPass;

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
    // Draw commands index a remap list (written by CPU or GPU culling) that holds the instances.
    uint instance = LoadRemap();
    Instance inst = LoadInstance(instance);
    if (u_alphaPass != 0) {
        bool seeThrough = LoadMaterial(uint(inst.scaleMaterial.w + 0.5)).textureParams.w < 0.999;
        if (seeThrough == (u_alphaPass == 1)) {   // not drawn in this pass: every vertex outside the clip volume
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }
    vec4 world = inst.model * vec4(a_position, 1.0);
    v_worldPos = world.xyz;
#ifdef SHADOW_PASS
    gl_Position = u_lightViewProjection * world;
#else
    v_normal = mat3(inst.normalMatrix) * a_normal;
    v_patternPos = a_position * inst.scaleMaterial.xyz;
    v_objectNormal = a_normal;
    v_uv = a_uv * inst.uvScale.xy;
    v_instance = instance;
    gl_Position = u_viewProjection * world;
#endif
}
