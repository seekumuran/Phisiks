// Scene colliders for GPU particles and XPBD nodes (mirrors Collide() in engine/src/physics/Xpbd.cpp).
struct Collider {
    vec4 centerType;        // xyz centre (or a point on the plane), w type (0 plane, 1 sphere, 2 box)
    vec4 normalRadius;      // xyz plane normal, w sphere radius
    vec4 halfExtents;       // xyz box half extents
    vec4 rotation;          // box rotation quaternion (x, y, z, w)
};
layout(std430, binding = 11) readonly buffer ColliderBuffer { Collider u_colliders[]; };
uniform int u_colliderCount;

vec3 QuatRotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Pushes a particle of radius r out of every collider; friction removes part of the tangential
// motion since `prev`. Returns true if any contact happened.
bool CollideAll(inout vec3 x, vec3 prev, float r, float friction) {
    bool contact = false;
    for (int i = 0; i < u_colliderCount; ++i) {
        Collider c = u_colliders[i];
        int type = int(c.centerType.w + 0.5);
        vec3 n;
        float penetration;
        if (type == 0) {
            n = normalize(c.normalRadius.xyz);
            penetration = r - dot(x - c.centerType.xyz, n);
        }
        else if (type == 1) {
            vec3 d = x - c.centerType.xyz;
            float dist = length(d);
            if (dist < 1e-9) continue;
            n = d / dist;
            penetration = c.normalRadius.w + r - dist;
        }
        else {
            vec4 inv = vec4(-c.rotation.xyz, c.rotation.w);
            vec3 local = QuatRotate(inv, x - c.centerType.xyz);
            vec3 q = abs(local) - c.halfExtents.xyz;
            vec3 outside = max(q, vec3(0.0));
            float outsideDist = length(outside);
            vec3 localNormal;
            if (outsideDist > 0.0) {
                localNormal = (local - clamp(local, -c.halfExtents.xyz, c.halfExtents.xyz)) / outsideDist;
                penetration = r - outsideDist;
            }
            else {
                int axis = 0;
                if (q.y > q[axis]) axis = 1;
                if (q.z > q[axis]) axis = 2;
                localNormal = vec3(0.0);
                localNormal[axis] = local[axis] >= 0.0 ? 1.0 : -1.0;
                penetration = r - q[axis];
            }
            n = QuatRotate(c.rotation, localNormal);
        }
        if (penetration <= 0.0) continue;
        x += n * penetration;
        vec3 motion = x - prev;
        x -= (motion - n * dot(motion, n)) * friction;
        contact = true;
    }
    return contact;
}
