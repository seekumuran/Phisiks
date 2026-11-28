#pragma once
// 3D rotors from geometric algebra: rotations as "a plane and an angle" instead of an axis.
//
// Basis: vectors e1 e2 e3, bivectors e12 e13 e23 (oriented planes), trivector e123.
// A rotor R = a + Bxy e12 + Bxz e13 + Byz e23 rotates a vector v by the sandwich v' = R v ~R,
// where ~R (reverse) = a - B. Rotors compose by the geometric product, never gimbal-lock, and
// generalise to any dimension. (They are isomorphic to unit quaternions; see ToQuat().)

#include <glm/gtc/quaternion.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace Phisiks::Math {

    // Oriented plane element (bivector). xy = e12 component (plane from x towards y), etc.
    struct Bivector3 {
        float xy = 0.0f, xz = 0.0f, yz = 0.0f;
        float Magnitude() const { return std::sqrt(xy * xy + xz * xz + yz * yz); }
        Bivector3 Normalized() const { const float m = Magnitude(); return m > 0.0f ? Bivector3{ xy / m, xz / m, yz / m } : Bivector3{}; }
    };

    // Outer (wedge) product: the oriented parallelogram spanned by u then v.
    inline Bivector3 Wedge(const glm::vec3& u, const glm::vec3& v) {
        return { u.x * v.y - u.y * v.x, u.x * v.z - u.z * v.x, u.y * v.z - u.z * v.y };
    }

    struct Rotor3 {
        float a = 1.0f;          // scalar part
        Bivector3 b;             // bivector part

        static Rotor3 Identity() { return {}; }

        // Rotation by `angle` (radians) inside `plane`, turning the plane's first axis towards its
        // second (e.g. plane xy: x towards y).
        static Rotor3 FromPlaneAngle(const Bivector3& plane, float angle) {
            const Bivector3 p = plane.Normalized();
            const float s = std::sin(angle * 0.5f);
            return { std::cos(angle * 0.5f), { -s * p.xy, -s * p.xz, -s * p.yz } };
        }

        // The rotation that takes unit vector `from` onto unit vector `to` (in their common plane).
        // R = normalize(1 + to*from), where to*from = to.from + to^from is the geometric product.
        static Rotor3 FromVectors(const glm::vec3& from, const glm::vec3& to) {
            Rotor3 r{ 1.0f + glm::dot(to, from), Wedge(to, from) };
            return r.Normalized();
        }

        float Norm() const { return std::sqrt(a * a + b.xy * b.xy + b.xz * b.xz + b.yz * b.yz); }
        Rotor3 Normalized() const {
            const float n = Norm();
            return n > 0.0f ? Rotor3{ a / n, { b.xy / n, b.xz / n, b.yz / n } } : Identity();
        }
        Rotor3 Reverse() const { return { a, { -b.xy, -b.xz, -b.yz } }; }

        // Geometric product: (*this) * q  ==  apply q first, then *this.
        Rotor3 operator*(const Rotor3& q) const {
            const Bivector3& B = b;
            const Bivector3& C = q.b;
            Rotor3 r;
            r.a    = a * q.a - B.xy * C.xy - B.xz * C.xz - B.yz * C.yz;
            r.b.xy = a * C.xy + B.xy * q.a - B.xz * C.yz + B.yz * C.xz;
            r.b.xz = a * C.xz + B.xz * q.a + B.xy * C.yz - B.yz * C.xy;
            r.b.yz = a * C.yz + B.yz * q.a - B.xy * C.xz + B.xz * C.xy;
            return r;
        }

        // Sandwich product v' = R v ~R, expanded by hand (the trivector terms cancel).
        glm::vec3 Rotate(const glm::vec3& v) const {
            // S = R v  (vector part S1..S3 + trivector part S123)
            const float s1 = a * v.x + b.xy * v.y + b.xz * v.z;
            const float s2 = a * v.y - b.xy * v.x + b.yz * v.z;
            const float s3 = a * v.z - b.xz * v.x - b.yz * v.y;
            const float s123 = b.xy * v.z - b.xz * v.y + b.yz * v.x;
            // v' = S ~R
            return {
                a * s1 + b.xy * s2 + b.xz * s3 + b.yz * s123,
                a * s2 - b.xy * s1 + b.yz * s3 - b.xz * s123,
                a * s3 - b.xz * s1 - b.yz * s2 + b.xy * s123
            };
        }

        glm::mat3 ToMat3() const {
            return glm::mat3(Rotate(glm::vec3(1, 0, 0)), Rotate(glm::vec3(0, 1, 0)), Rotate(glm::vec3(0, 0, 1)));
        }

        // Rotation angle and its plane.
        float Angle() const { return 2.0f * std::atan2(b.Magnitude(), a); }

        // Isomorphism to unit quaternions (axis = dual of the plane).
        glm::quat ToQuat() const { return glm::quat(a, -b.yz, b.xz, -b.xy); }
        static Rotor3 FromQuat(const glm::quat& q) { return { q.w, { -q.z, q.y, -q.x } }; }

        // Constant-angular-speed interpolation inside the rotation's own plane.
        static Rotor3 Slerp(const Rotor3& from, const Rotor3& to, float t) {
            Rotor3 delta = to * from.Reverse();
            if (delta.a < 0.0f) delta = { -delta.a, { -delta.b.xy, -delta.b.xz, -delta.b.yz } };   // shortest way
            const float angle = delta.Angle();
            if (angle < 1e-6f) return to;
            const Rotor3 step = FromPlaneAngle(Bivector3{ -delta.b.xy, -delta.b.xz, -delta.b.yz }, angle * t);
            return (step * from).Normalized();
        }
    };
}
