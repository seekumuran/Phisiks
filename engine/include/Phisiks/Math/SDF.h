#pragma once
// Analytic signed distance fields (SDFs) and constructive solid geometry.
//
// An SDF returns the signed distance from point p to a surface: negative inside, zero on the
// surface, positive outside. Exact SDFs have |gradient| = 1 everywhere, which makes sphere tracing
// (ray marching) safe. Shapes are defined around the origin; to place one, transform p by the
// shape's inverse transform before evaluating.

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks::Math::SDF {

    // ---------------------------------------------------------------- primitives (exact)
    inline float Sphere(const glm::vec3& p, float radius) {
        return glm::length(p) - radius;
    }

    inline float Box(const glm::vec3& p, const glm::vec3& halfExtents) {
        const glm::vec3 q = glm::abs(p) - halfExtents;
        const float outside = glm::length(glm::max(q, glm::vec3(0.0f)));
        const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
        return outside + inside;
    }

    inline float RoundBox(const glm::vec3& p, const glm::vec3& halfExtents, float rounding) {
        return Box(p, halfExtents - glm::vec3(rounding)) - rounding;
    }

    // Segment a-b swept by a sphere.
    inline float Capsule(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float radius) {
        const glm::vec3 pa = p - a, ba = b - a;
        const float h = std::clamp(glm::dot(pa, ba) / glm::dot(ba, ba), 0.0f, 1.0f);
        return glm::length(pa - ba * h) - radius;
    }

    // Torus in the XZ plane: ring radius R, tube radius r.
    inline float Torus(const glm::vec3& p, float ringRadius, float tubeRadius) {
        const glm::vec2 q(glm::length(glm::vec2(p.x, p.z)) - ringRadius, p.y);
        return glm::length(q) - tubeRadius;
    }

    // Capped cylinder along Y: half height h, radius r.
    inline float Cylinder(const glm::vec3& p, float halfHeight, float radius) {
        const glm::vec2 d = glm::abs(glm::vec2(glm::length(glm::vec2(p.x, p.z)), p.y)) - glm::vec2(radius, halfHeight);
        return std::min(std::max(d.x, d.y), 0.0f) + glm::length(glm::max(d, glm::vec2(0.0f)));
    }

    // Half-space below the plane dot(n, p) = offset (n must be unit length).
    inline float Plane(const glm::vec3& p, const glm::vec3& normal, float offset) {
        return glm::dot(p, normal) - offset;
    }

    // ---------------------------------------------------------------- CSG
    inline float Union(float a, float b)        { return std::min(a, b); }
    inline float Intersection(float a, float b) { return std::max(a, b); }
    inline float Subtraction(float a, float b)  { return std::max(a, -b); }   // a with b removed

    // Smooth blends with radius k (polynomial smooth minimum). k -> 0 gives the sharp operation.
    // The result is a bound (not exact) inside the blend region, which is still safe to march.
    inline float SmoothUnion(float a, float b, float k) {
        if (k <= 0.0f) return Union(a, b);
        const float h = std::max(k - std::fabs(a - b), 0.0f) / k;
        return std::min(a, b) - h * h * k * 0.25f;
    }
    inline float SmoothIntersection(float a, float b, float k) { return -SmoothUnion(-a, -b, k); }
    inline float SmoothSubtraction(float a, float b, float k)  { return SmoothIntersection(a, -b, k); }

    // ---------------------------------------------------------------- field utilities
    // Gradient by central differences (unit normal on the surface of an exact SDF).
    template<class Field>
    glm::vec3 Gradient(Field field, const glm::vec3& p, float epsilon = 1e-3f) {
        const glm::vec3 ex(epsilon, 0, 0), ey(0, epsilon, 0), ez(0, 0, epsilon);
        return glm::vec3(field(p + ex) - field(p - ex), field(p + ey) - field(p - ey), field(p + ez) - field(p - ez)) / (2.0f * epsilon);
    }

    template<class Field>
    glm::vec3 Normal(Field field, const glm::vec3& p, float epsilon = 1e-3f) {
        const glm::vec3 g = Gradient(field, p, epsilon);
        const float length = glm::length(g);
        return length > 0.0f ? g / length : glm::vec3(0, 1, 0);
    }

    // Projects p onto the zero level set (exact for exact SDFs).
    template<class Field>
    glm::vec3 ClosestPoint(Field field, const glm::vec3& p) {
        return p - Normal(field, p) * field(p);
    }

    struct MarchResult {
        bool hit = false;
        float distance = 0.0f;   // along the ray
        glm::vec3 position{ 0.0f };
        glm::vec3 normal{ 0.0f };
        int steps = 0;
    };

    // Sphere tracing: advance by the field value, which can never overshoot an exact SDF.
    template<class Field>
    MarchResult Raymarch(Field field, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, int maxSteps = 256, float hitEpsilon = 1e-4f) {
        MarchResult result;
        const glm::vec3 dir = glm::normalize(direction);
        float t = 0.0f;
        for (int i = 0; i < maxSteps && t <= maxDistance; ++i) {
            const glm::vec3 p = origin + dir * t;
            const float d = field(p);
            result.steps = i + 1;
            if (d < hitEpsilon) {
                result.hit = true;
                result.distance = t;
                result.position = p;
                result.normal = Normal(field, p);
                return result;
            }
            t += d;
        }
        result.distance = t;
        return result;
    }
}
