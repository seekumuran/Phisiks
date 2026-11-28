#version 460 core
// Synthetic-vision G-buffer (export only): world normal, linear depth (metres along the view axis;
// orthographic: same) and entity id, aligned pixel for pixel with the internal-resolution colour.
#include "common.glsl"

in vec3 v_worldPos;
in vec3 v_normal;
flat in uint v_instance;   // mesh.vert (remap) or softbody.vert (u_instance)

layout(location = 0) out vec4 o_normal;
layout(location = 1) out float o_depth;
layout(location = 2) out uint o_entity;

void main() {
    vec3 N = normalize(v_normal);
    if (!gl_FrontFacing) N = -N;
    Instance inst = LoadInstance(v_instance);
    o_normal = vec4(N, 1.0);
    o_depth = -(u_view * vec4(v_worldPos, 1.0)).z;
    o_entity = inst.ids.x;
}
