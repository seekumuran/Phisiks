#version 460 core
// Exclusive prefix sum (two levels, 512 elements per workgroup, up to 512 x 512 elements).
//   SCAN_LOCAL   out[i] = exclusive scan within the block; blockSums[block] = block total
//   SCAN_BLOCKS  exclusive scan of blockSums in place (one workgroup)
//   SCAN_ADD     out[i] += blockSums[block]
layout(local_size_x = 512) in;

layout(std430, binding = 5) readonly buffer Input { uint u_in[]; };
layout(std430, binding = 6) buffer Output { uint u_out[]; };
layout(std430, binding = 13) buffer BlockSums { uint u_blockSums[]; };
uniform uint u_n;

shared uint s_data[512];

void BlockScan(uint value) {
    uint l = gl_LocalInvocationID.x;
    s_data[l] = value;
    barrier();
    for (uint offset = 1u; offset < 512u; offset <<= 1u) {
        uint t = l >= offset ? s_data[l - offset] : 0u;
        barrier();
        s_data[l] += t;
        barrier();
    }
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint l = gl_LocalInvocationID.x;
#ifdef SCAN_LOCAL
    uint v = i < u_n ? u_in[i] : 0u;
    BlockScan(v);
    if (i < u_n) u_out[i] = s_data[l] - v;
    if (l == 511u) u_blockSums[gl_WorkGroupID.x] = s_data[511];
#endif
#ifdef SCAN_BLOCKS
    uint v = l < u_n ? u_blockSums[l] : 0u;
    BlockScan(v);
    if (l < u_n) u_blockSums[l] = s_data[l] - v;
#endif
#ifdef SCAN_ADD
    if (i < u_n) u_out[i] += u_blockSums[gl_WorkGroupID.x];
#endif
}
