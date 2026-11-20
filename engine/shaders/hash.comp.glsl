#version 460 core
// Spatial hash grid passes over a particle buffer (stride of vec4s per particle, position in .xyz).
//   HASH_COUNT    count[h]++ per particle
//   HASH_SCATTER  sorted[cellStart[h] + cursor[h]++] = particle
//   HASH_QUERY    number of other particles within u_queryRadius (verification against the CPU)
layout(local_size_x = 256) in;
#include "spatial_hash.glsl"

layout(std430, binding = 4) readonly buffer Particles { vec4 u_particles[]; };
layout(std430, binding = 5) buffer CellCounters { uint u_counters[]; };
layout(std430, binding = 6) readonly buffer CellStart { uint u_cellStart[]; };
layout(std430, binding = 7) buffer Sorted { uint u_sorted[]; };
layout(std430, binding = 8) writeonly buffer QueryOut { uint u_query[]; };

uniform uint u_count;
uniform uint u_stride;
uniform float u_queryRadius;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_count) return;
    vec3 p = u_particles[i * u_stride].xyz;
#ifdef HASH_COUNT
    atomicAdd(u_counters[HashIndex(HashCell(p))], 1u);
#endif
#ifdef HASH_SCATTER
    uint h = HashIndex(HashCell(p));
    uint slot = atomicAdd(u_counters[h], 1u);
    u_sorted[u_cellStart[h] + slot] = i;
#endif
#ifdef HASH_QUERY
    ivec3 c = HashCell(p);
    uint visited[27];
    int visitedCount = 0;
    uint n = 0u;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                uint h = HashIndex(c + ivec3(x, y, z));
                bool seen = false;
                for (int k = 0; k < visitedCount; ++k) seen = seen || visited[k] == h;
                if (seen) continue;
                visited[visitedCount++] = h;
                for (uint s = u_cellStart[h]; s < u_cellStart[h + 1u]; ++s) {
                    uint j = u_sorted[s];
                    if (j != i && WithinRadius(u_particles[j * u_stride].xyz, p, u_queryRadius)) ++n;
                }
            }
    u_query[i] = n;
#endif
}
