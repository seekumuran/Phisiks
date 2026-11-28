#version 460 core
// Hi-Z pyramid (max depth). HIZ_INIT copies the resolved depth into level 0; HIZ_DOWNSAMPLE builds
// level n+1 from level n (odd sizes fold the extra row/column into the last texel).
layout(local_size_x = 8, local_size_y = 8) in;

#ifdef HIZ_INIT
layout(binding = 0) uniform sampler2D u_depth;
layout(r32f, binding = 1) writeonly uniform image2D u_dst;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(u_dst);
    if (any(greaterThanEqual(p, size))) return;
    imageStore(u_dst, p, vec4(texelFetch(u_depth, p, 0).r));
}
#endif

#ifdef HIZ_DOWNSAMPLE
layout(r32f, binding = 0) readonly uniform image2D u_src;
layout(r32f, binding = 1) writeonly uniform image2D u_dst;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(u_dst);
    if (any(greaterThanEqual(p, dstSize))) return;
    ivec2 srcSize = imageSize(u_src);
    ivec2 base = p * 2;
    ivec2 extent = ivec2(2);
    if (p.x == dstSize.x - 1 && (srcSize.x & 1) != 0) extent.x = 3;
    if (p.y == dstSize.y - 1 && (srcSize.y & 1) != 0) extent.y = 3;
    float m = 0.0;
    for (int y = 0; y < extent.y; ++y)
        for (int x = 0; x < extent.x; ++x)
            m = max(m, imageLoad(u_src, min(base + ivec2(x, y), srcSize - 1)).r);
    imageStore(u_dst, p, vec4(m));
}
#endif
