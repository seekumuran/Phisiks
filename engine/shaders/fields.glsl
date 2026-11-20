// Force fields (mirrors Phisiks::Physics::FieldMath in engine/include/Phisiks/Physics/Fields.h).
// Affect mask bits: 1 rigid bodies, 2 particles, 4 soft bodies.
struct Field {
    vec4 originType;        // xyz origin, w type (0 uniform, 1 spherical gravity, 2 vortex, 3 n-body)
    vec4 vectorStrength;    // xyz uniform acceleration, w strength
    vec4 axisRadius;        // xyz world axis, w radius
    vec4 params;            // x falloff, y influence radius, z softening, w affect mask
};
layout(std430, binding = 10) readonly buffer FieldBuffer { Field u_fields[]; };
uniform int u_fieldCount;

float FieldInfluence(float distance, float influenceRadius) {
    if (influenceRadius <= 0.0) return 1.0;
    float c = clamp((influenceRadius - distance) / (0.1 * influenceRadius), 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

vec3 FieldAcceleration(vec3 p, uint mask) {
    vec3 total = vec3(0.0);
    for (int i = 0; i < u_fieldCount; ++i) {
        Field f = u_fields[i];
        if ((uint(f.params.w + 0.5) & mask) == 0u) continue;
        int type = int(f.originType.w + 0.5);
        vec3 d = p - f.originType.xyz;
        float dist = length(d);
        float w = FieldInfluence(dist, f.params.y);
        if (w <= 0.0) continue;
        if (type == 0) {
            total += f.vectorStrength.xyz * w;
        }
        else if (type == 1) {
            if (dist < 1e-6) continue;
            float R = max(f.axisRadius.w, 1e-6);
            float magnitude = dist >= R ? f.vectorStrength.w / (dist * dist) : f.vectorStrength.w * dist / (R * R * R);
            total += -d / dist * magnitude * w;
        }
        else if (type == 2) {
            vec3 axis = normalize(f.axisRadius.xyz);
            vec3 radial = d - axis * dot(d, axis);
            float r = length(radial);
            if (r < 1e-6) continue;
            float core = max(f.axisRadius.w, 1e-6);
            float magnitude = r <= core ? f.vectorStrength.w * r / core : f.vectorStrength.w * pow(core / r, f.params.x);
            total += cross(axis, radial / r) * magnitude * w;
        }
    }
    return total;
}
