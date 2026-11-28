// Scene colliders for GPU particles and XPBD nodes (mirrors Collide() in engine/src/physics/Xpbd.cpp).
// Two-way coupling: a collider of a dynamic rigid body (halfExtents.w = 1) receives the reaction
// impulse of every push-out, J = -m * (resolved displacement) / h, accumulated in fixed point
// (integer atomics: order-independent, so deterministic) and applied to the body by the engine.
struct Collider {
    vec4 centerType;        // xyz centre (or a point on the plane), w type (0 plane, 1 sphere, 2 box)
    vec4 normalRadius;      // xyz plane normal, w sphere radius
    vec4 halfExtents;       // xyz box half extents, w 1 = dynamic body (receives impulses)
    vec4 rotation;          // box rotation quaternion (x, y, z, w)
};
layout(std430, binding = 11) readonly buffer ColliderBuffer { Collider u_colliders[]; };
layout(std430, binding = 12) buffer ColliderImpulses { int u_impulses[]; };   // 8 per collider: linear xyz, angular xyz (about the centre)
uniform int u_colliderCount;
uniform int u_coupling;          // 1: accumulate reaction impulses
uniform float u_impulseScale;    // fixed-point units per N*s

vec3 QuatRotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Pushes a particle of radius r out of every collider; friction removes part of the tangential
// motion since `prev`. Returns true if any contact happened.
// massOverH: the particle's mass / time step (reaction impulse per metre of push-out).
bool CollideAll(inout vec3 x, vec3 prev, float r, float friction, float massOverH) {
    bool contact = false;
    for (int i = 0; i < u_colliderCount; ++i) {
        Collider c = u_colliders[i];
        vec3 before = x;
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
        if (u_coupling != 0 && c.halfExtents.w > 0.5) {
            vec3 J = -(x - before) * massOverH;
            vec3 L = cross(x - c.centerType.xyz, J);
            int base = i * 8;
            atomicAdd(u_impulses[base + 0], int(round(J.x * u_impulseScale)));
            atomicAdd(u_impulses[base + 1], int(round(J.y * u_impulseScale)));
            atomicAdd(u_impulses[base + 2], int(round(J.z * u_impulseScale)));
            atomicAdd(u_impulses[base + 4], int(round(L.x * u_impulseScale)));
            atomicAdd(u_impulses[base + 5], int(round(L.y * u_impulseScale)));
            atomicAdd(u_impulses[base + 6], int(round(L.z * u_impulseScale)));
        }
    }
    return contact;
}
