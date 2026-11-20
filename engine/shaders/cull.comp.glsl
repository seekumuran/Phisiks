#version 460 core
// GPU culling for one view: frustum test, then (optionally) Hi-Z occlusion against the previous
// frame's depth pyramid. Visible objects append their instance index to their mesh's slice of the
// remap buffer and increment that mesh's DrawElementsIndirectCommand.instanceCount, so the draw
// that follows (glMultiDrawElementsIndirect) uses exactly the GPU-computed visible set.
layout(local_size_x = 64) in;

struct CullObject {
    vec4 sphere;            // xyz centre, w radius (world)
    uvec4 meshInstance;     // x command index (mesh), y instance index
};
layout(std430, binding = 4) readonly buffer Objects { CullObject u_objects[]; };

struct DrawCommand { uint count; uint instanceCount; uint firstIndex; int baseVertex; uint baseInstance; };
layout(std430, binding = 5) buffer Commands { DrawCommand u_commands[]; };
layout(std430, binding = 6) writeonly buffer Remap { uint u_remap[]; };
layout(std430, binding = 7) buffer CullStats { uint u_stats[4]; };   // visible, frustum culled, occlusion culled

uniform uint u_objectCount;
uniform uint u_commandBase;
uniform vec4 u_planes[6];
uniform int u_occlusion;
uniform mat4 u_prevViewProjection;
uniform vec4 u_hizRect;            // view rectangle (pixels) inside the pyramid's level 0
uniform int u_hizLevels;
layout(binding = 0) uniform sampler2D u_hiz;   // R32F max-depth pyramid

bool Occluded(vec3 center, float radius) {
    vec2 lo = vec2(1e9), hi = vec2(-1e9);
    float nearest = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = center + radius * vec3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0, (i & 4) != 0 ? 1.0 : -1.0);
        vec4 clip = u_prevViewProjection * vec4(corner, 1.0);
        if (clip.w <= 1e-4) return false;                     // crosses the camera plane: keep
        vec3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearest = min(nearest, ndc.z * 0.5 + 0.5);
    }
    if (nearest <= 0.0) return false;
    lo = clamp(lo, vec2(-1.0), vec2(1.0));
    hi = clamp(hi, vec2(-1.0), vec2(1.0));
    vec2 pmin = u_hizRect.xy + (lo * 0.5 + 0.5) * u_hizRect.zw;
    vec2 pmax = u_hizRect.xy + (hi * 0.5 + 0.5) * u_hizRect.zw;
    float span = max(max(pmax.x - pmin.x, pmax.y - pmin.y), 1.0);
    int level = clamp(int(ceil(log2(span))), 0, u_hizLevels - 1);
    ivec2 size = textureSize(u_hiz, level);
    ivec2 a = clamp(ivec2(floor(pmin)) >> level, ivec2(0), size - 1);
    ivec2 b = clamp(ivec2(floor(pmax)) >> level, ivec2(0), size - 1);
    float farthest = max(max(texelFetch(u_hiz, a, level).r, texelFetch(u_hiz, ivec2(b.x, a.y), level).r),
                         max(texelFetch(u_hiz, ivec2(a.x, b.y), level).r, texelFetch(u_hiz, b, level).r));
    return nearest > farthest + 1e-6;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_objectCount) return;
    CullObject o = u_objects[i];
    for (int p = 0; p < 6; ++p) {
        if (dot(u_planes[p].xyz, o.sphere.xyz) + u_planes[p].w < -o.sphere.w) {
            atomicAdd(u_stats[1], 1u);
            return;
        }
    }
    if (u_occlusion != 0 && Occluded(o.sphere.xyz, o.sphere.w)) {
        atomicAdd(u_stats[2], 1u);
        return;
    }
    uint command = u_commandBase + o.meshInstance.x;
    uint slot = atomicAdd(u_commands[command].instanceCount, 1u);
    u_remap[u_commands[command].baseInstance + slot] = o.meshInstance.y;
    atomicAdd(u_stats[0], 1u);
}
