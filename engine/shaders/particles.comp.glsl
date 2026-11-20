#version 460 core
// GPU particles. Each particle is two vec4s: (position, radius) and (velocity, mass).
//   NBODY            softened pairwise gravity, tiled through shared memory (O(N^2), N <= a few 10k)
//   PARTICLES_STEP   fields + gravity + n-body, neighbour separation through the spatial hash,
//                    collisions with scene colliders; reads `in`, writes `out` (ping-pong)
layout(local_size_x = 256) in;

layout(std430, binding = 4) readonly buffer ParticlesIn { vec4 u_in[]; };
uniform uint u_count;

#ifdef NBODY
layout(std430, binding = 8) writeonly buffer Acceleration { vec4 u_accel[]; };
uniform float u_G;
uniform float u_softening2;
shared vec4 s_tile[256];

void main() {
    uint i = gl_GlobalInvocationID.x;
    vec3 p = i < u_count ? u_in[i * 2u].xyz : vec3(0.0);
    vec3 a = vec3(0.0);
    for (uint base = 0u; base < u_count; base += 256u) {
        uint j = base + gl_LocalInvocationID.x;
        s_tile[gl_LocalInvocationID.x] = j < u_count ? vec4(u_in[j * 2u].xyz, u_in[j * 2u + 1u].w) : vec4(0.0);
        barrier();
        uint tileCount = min(256u, u_count - base);
        for (uint k = 0u; k < tileCount; ++k) {
            if (base + k == i) continue;
            vec3 d = s_tile[k].xyz - p;
            float r2 = dot(d, d) + u_softening2;
            a += d * (s_tile[k].w * inversesqrt(r2 * r2 * r2));
        }
        barrier();
    }
    if (i < u_count) u_accel[i] = vec4(a * u_G, 0.0);
}
#endif

#ifdef PARTICLES_STEP
#include "fields.glsl"
#include "colliders.glsl"
#include "spatial_hash.glsl"
layout(std430, binding = 6) readonly buffer CellStart { uint u_cellStart[]; };
layout(std430, binding = 7) readonly buffer Sorted { uint u_sorted[]; };
layout(std430, binding = 8) readonly buffer Acceleration { vec4 u_accel[]; };
layout(std430, binding = 13) writeonly buffer ParticlesOut { vec4 u_out[]; };
uniform float u_dt;
uniform vec3 u_gravity;
uniform int u_useNBody;
uniform float u_friction;
uniform float u_damping;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_count) return;
    vec3 x = u_in[i * 2u].xyz;
    float r = u_in[i * 2u].w;
    vec3 v = u_in[i * 2u + 1u].xyz;
    float mass = u_in[i * 2u + 1u].w;

    vec3 a = u_gravity + FieldAcceleration(x, 2u);
    if (u_useNBody != 0) a += u_accel[i].xyz;

    // Neighbour separation: overlapping particles are pushed apart (position-based) and their
    // approaching relative velocity is removed (inelastic contact).
    vec3 correction = vec3(0.0);
    ivec3 c = HashCell(x);
    uint visited[27];
    int visitedCount = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint h = HashIndex(c + ivec3(dx, dy, dz));
                bool seen = false;
                for (int k = 0; k < visitedCount; ++k) seen = seen || visited[k] == h;
                if (seen) continue;
                visited[visitedCount++] = h;
                for (uint s = u_cellStart[h]; s < u_cellStart[h + 1u]; ++s) {
                    uint j = u_sorted[s];
                    if (j == i) continue;
                    vec3 d = x - u_in[j * 2u].xyz;
                    float rsum = r + u_in[j * 2u].w;
                    float d2 = dot(d, d);
                    if (d2 >= rsum * rsum) continue;
                    float dist = sqrt(d2);
                    vec3 n = dist > 1e-6 ? d / dist : normalize(vec3(float(i & 7u) - 3.5, 1.0, float(j & 7u) - 3.5));
                    correction += n * (rsum - dist) * 0.5;
                    float approach = dot(v - u_in[j * 2u + 1u].xyz, n);
                    if (approach < 0.0) v -= n * approach * 0.5;
                }
            }

    v += a * u_dt;
    v *= max(0.0, 1.0 - u_damping * u_dt);
    vec3 next = x + v * u_dt + correction;
    if (CollideAll(next, x, r, u_friction)) v = (next - x) / u_dt;   // contact: velocity follows the resolved motion
    u_out[i * 2u] = vec4(next, r);
    u_out[i * 2u + 1u] = vec4(v, mass);
}
#endif
