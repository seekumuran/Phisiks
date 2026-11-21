// Signed distance primitives and smooth CSG (mirrors Phisiks::Math::SDF, engine/include/Phisiks/Math/SDF.h).
float SdSphere(vec3 p, float r) { return length(p) - r; }
float SdBox(vec3 p, vec3 h) {
    vec3 q = abs(p) - h;
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float SdRoundBox(vec3 p, vec3 h, float rounding) { return SdBox(p, h - vec3(rounding)) - rounding; }
float SdCapsule(vec3 p, vec3 a, vec3 b, float r) {
    vec3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}
float SdTorus(vec3 p, float ringRadius, float tubeRadius) {
    vec2 q = vec2(length(p.xz) - ringRadius, p.y);
    return length(q) - tubeRadius;
}
float SdCylinder(vec3 p, float halfHeight, float radius) {
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(radius, halfHeight);
    return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0)));
}
float OpSmoothUnion(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}
float OpSmoothIntersection(float a, float b, float k) { return -OpSmoothUnion(-a, -b, k); }
float OpSmoothSubtraction(float a, float b, float k) { return OpSmoothIntersection(a, -b, k); }

// Scene: a list of primitives combined in order (the first one starts the shape).
struct SdfPrimitive {
    vec4 positionShape;     // xyz centre, w shape (0 sphere, 1 box, 2 round box, 3 torus, 4 capsule, 5 cylinder)
    vec4 rotation;          // quaternion (x, y, z, w)
    vec4 params;            // sphere: x radius; box: xyz half extents; round box: xyz half, w rounding;
                            // torus: x ring, y tube; capsule: x half length (local Y), y radius; cylinder: x half height, y radius
    vec4 op;                // x operation (0 union, 1 subtraction, 2 intersection), y smoothness k
};
#ifdef PHISIKS_GL41
layout(binding = 13) uniform samplerBuffer u_sdfTexels;   // 4 texels per primitive
SdfPrimitive LoadSdf(int i) {
    SdfPrimitive s;
    s.positionShape = texelFetch(u_sdfTexels, i * 4);
    s.rotation = texelFetch(u_sdfTexels, i * 4 + 1);
    s.params = texelFetch(u_sdfTexels, i * 4 + 2);
    s.op = texelFetch(u_sdfTexels, i * 4 + 3);
    return s;
}
#else
layout(std430, binding = 12) readonly buffer SdfBuffer { SdfPrimitive u_sdf[]; };
SdfPrimitive LoadSdf(int i) { return u_sdf[i]; }
#endif
uniform int u_sdfCount;

vec3 SdfRotateInverse(vec4 q, vec3 v) {
    vec3 u = -q.xyz;
    vec3 t = 2.0 * cross(u, v);
    return v + q.w * t + cross(u, t);
}

float SdfPrimitiveDistance(SdfPrimitive s, vec3 p) {
    vec3 local = SdfRotateInverse(s.rotation, p - s.positionShape.xyz);
    int shape = int(s.positionShape.w + 0.5);
    if (shape == 0) return SdSphere(local, s.params.x);
    if (shape == 1) return SdBox(local, s.params.xyz);
    if (shape == 2) return SdRoundBox(local, s.params.xyz, s.params.w);
    if (shape == 3) return SdTorus(local, s.params.x, s.params.y);
    if (shape == 4) return SdCapsule(local, vec3(0.0, -s.params.x, 0.0), vec3(0.0, s.params.x, 0.0), s.params.y);
    return SdCylinder(local, s.params.x, s.params.y);
}

float SceneSdf(vec3 p) {
    float d = 1e9;
    for (int i = 0; i < u_sdfCount; ++i) {
        SdfPrimitive s = LoadSdf(i);
        float di = SdfPrimitiveDistance(s, p);
        int op = int(s.op.x + 0.5);
        if (i == 0) d = di;
        else if (op == 0) d = OpSmoothUnion(d, di, s.op.y);
        else if (op == 1) d = OpSmoothSubtraction(d, di, s.op.y);
        else d = OpSmoothIntersection(d, di, s.op.y);
    }
    return d;
}
