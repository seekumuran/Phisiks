// Spatial hash grid (mirrors engine/src/compute/SpatialHash.h exactly: multiplication by the inverse
// cell size and squared distances, which GLSL rounds correctly).
uniform float u_invCellSize;
uniform uint u_tableSize;

ivec3 HashCell(vec3 p) {
    return ivec3(floor(p * u_invCellSize));
}

uint HashIndex(ivec3 c) {
    uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u);
    return h % u_tableSize;
}

bool WithinRadius(vec3 a, vec3 b, float radius) {
    vec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z < radius * radius;
}
