#version 460 core
// GPU XPBD (mirrors Phisiks::Physics::Xpbd::Step, engine/src/physics/Xpbd.cpp). Constraints are
// solved one colour at a time (no two constraints of a colour share a particle).
//   XPBD_PREDICT   prev = x; v += (gravity + fields) h; x += v h
//   XPBD_SOLVE     distance / tet-volume constraints [u_first, u_first + u_constraintCount)
//   XPBD_FINISH    collisions with scene colliders, v = (x - prev) / h (damped)
//   XPBD_FUSED     all three for u_substeps sub-steps inside ONE workgroup, colours separated by
//                  barrier(): one dispatch per body per fixed step instead of substeps * (colours + 2).
//                  Used for bodies that fit; large bodies use the three passes above.
//   XPBD_NORMALS   per-vertex normals of the render surface (sum of adjacent face normals)
#ifdef XPBD_FUSED
layout(local_size_x = 256) in;
#define XPBD_COHERENT coherent
#else
layout(local_size_x = 128) in;
#define XPBD_COHERENT
#endif

layout(std430, binding = 4) XPBD_COHERENT buffer Positions { vec4 u_x[]; };   // xyz, w inverse mass
uniform uint u_count;
uniform float u_h;

#ifndef XPBD_NORMALS
#include "fields.glsl"
#include "colliders.glsl"
layout(std430, binding = 5) XPBD_COHERENT buffer Velocities { vec4 u_v[]; };
layout(std430, binding = 6) XPBD_COHERENT buffer Previous { vec4 u_prev[]; };
struct Constraint { uvec4 idx; vec4 params; };   // params: x type (0 distance, 1 volume), y rest, z compliance
layout(std430, binding = 7) readonly buffer Constraints { Constraint u_constraints[]; };
uniform vec3 u_gravity;
uniform float u_radius;
uniform float u_friction;
uniform float u_damping;

void PredictNode(uint i) {
    vec4 x = u_x[i];
    u_prev[i] = x;
    if (x.w == 0.0) return;
    vec3 v = u_v[i].xyz + (u_gravity + FieldAcceleration(x.xyz, 4u)) * u_h;
    u_v[i] = vec4(v, 0.0);
    u_x[i] = vec4(x.xyz + v * u_h, x.w);
}

void SolveConstraint(uint index) {
    Constraint c = u_constraints[index];
    float h2 = u_h * u_h;
    if (c.params.x < 0.5) {
        vec4 a = u_x[c.idx.x], b = u_x[c.idx.y];
        vec3 d = b.xyz - a.xyz;
        float len = length(d);
        float wsum = a.w + b.w + c.params.z / h2;
        if (len < 1e-9 || wsum <= 0.0) return;
        vec3 n = d / len;
        float dl = -(len - c.params.y) / wsum;
        u_x[c.idx.x] = vec4(a.xyz - n * (a.w * dl), a.w);
        u_x[c.idx.y] = vec4(b.xyz + n * (b.w * dl), b.w);
    }
    else {
        vec4 x0 = u_x[c.idx.x], x1 = u_x[c.idx.y], x2 = u_x[c.idx.z], x3 = u_x[c.idx.w];
        vec3 g1 = cross(x2.xyz - x0.xyz, x3.xyz - x0.xyz) / 6.0;
        vec3 g2 = cross(x3.xyz - x0.xyz, x1.xyz - x0.xyz) / 6.0;
        vec3 g3 = cross(x1.xyz - x0.xyz, x2.xyz - x0.xyz) / 6.0;
        vec3 g0 = -(g1 + g2 + g3);
        float wsum = c.params.z / h2 + x0.w * dot(g0, g0) + x1.w * dot(g1, g1) + x2.w * dot(g2, g2) + x3.w * dot(g3, g3);
        if (wsum <= 1e-12) return;
        float volume = dot(cross(x1.xyz - x0.xyz, x2.xyz - x0.xyz), x3.xyz - x0.xyz) / 6.0;
        float dl = -(volume - c.params.y) / wsum;
        u_x[c.idx.x] = vec4(x0.xyz + g0 * (x0.w * dl), x0.w);
        u_x[c.idx.y] = vec4(x1.xyz + g1 * (x1.w * dl), x1.w);
        u_x[c.idx.z] = vec4(x2.xyz + g2 * (x2.w * dl), x2.w);
        u_x[c.idx.w] = vec4(x3.xyz + g3 * (x3.w * dl), x3.w);
    }
}

void FinishNode(uint i) {
    vec4 x = u_x[i];
    if (x.w == 0.0) return;
    vec3 prev = u_prev[i].xyz;
    vec3 p = x.xyz;
    CollideAll(p, prev, u_radius, u_friction, 1.0 / (x.w * u_h));
    u_x[i] = vec4(p, x.w);
    u_v[i] = vec4((p - prev) / u_h * max(0.0, 1.0 - u_damping * u_h), 0.0);
}
#endif

#ifdef XPBD_PREDICT
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < u_count) PredictNode(i);
}
#endif

#ifdef XPBD_SOLVE
uniform uint u_first;
uniform uint u_constraintCount;
void main() {
    uint k = gl_GlobalInvocationID.x;
    if (k < u_constraintCount) SolveConstraint(u_first + k);
}
#endif

#ifdef XPBD_FINISH
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < u_count) FinishNode(i);
}
#endif

#ifdef XPBD_FUSED
layout(std430, binding = 8) readonly buffer ColorStarts { uint u_colorStart[]; };   // colourCount + 1 entries
uniform uint u_colorCount;
uniform int u_substeps;
void main() {
    uint lane = gl_LocalInvocationID.x, lanes = gl_WorkGroupSize.x;
    for (int sub = 0; sub < u_substeps; ++sub) {   // uniform control flow: barrier() is legal here
        for (uint i = lane; i < u_count; i += lanes) PredictNode(i);
        memoryBarrierBuffer();
        barrier();
        for (uint k = 0u; k < u_colorCount; ++k) {
            uint first = u_colorStart[k], last = u_colorStart[k + 1u];
            for (uint c = first + lane; c < last; c += lanes) SolveConstraint(c);
            memoryBarrierBuffer();
            barrier();
        }
        for (uint i = lane; i < u_count; i += lanes) FinishNode(i);
        memoryBarrierBuffer();
        barrier();
    }
}
#endif

#ifdef XPBD_NORMALS
layout(std430, binding = 8) writeonly buffer Normals { vec4 u_normals[]; };
layout(std430, binding = 13) readonly buffer AdjacencyStart { uint u_adjStart[]; };
layout(std430, binding = 14) readonly buffer AdjacencyTriangles { uint u_adjTris[]; };
layout(std430, binding = 15) readonly buffer Triangles { uint u_tris[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_count) return;
    vec3 n = vec3(0.0);
    for (uint k = u_adjStart[i]; k < u_adjStart[i + 1u]; ++k) {
        uint t = u_adjTris[k] * 3u;
        vec3 a = u_x[u_tris[t]].xyz, b = u_x[u_tris[t + 1u]].xyz, c = u_x[u_tris[t + 2u]].xyz;
        n += cross(b - a, c - a);
    }
    float len = length(n);
    u_normals[i] = vec4(len > 1e-12 ? n / len : vec3(0.0, 1.0, 0.0), 0.0);
}
#endif
