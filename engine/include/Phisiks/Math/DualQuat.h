#pragma once
// Unit dual quaternions: rigid transforms (rotation + translation) as one algebraic object.
//
//   DQ = real + eps * dual,   eps^2 = 0
//   real = rotation q,  dual = 0.5 * (0, t) * q
//
// Why: composition is a single product, interpolation follows the true screw motion (ScLerp),
// and blending many transforms (skinning) stays rigid: no "candy-wrapper" volume loss that
// linear blending of matrices produces (see DualQuat::Blend).

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <algorithm>
#include <span>

namespace Phisiks::Math {

    struct ScrewParameters {
        glm::vec3 axis{ 0, 1, 0 };   // unit direction of the screw axis
        glm::vec3 moment{ 0 };       // axis moment m = p x axis (p any point on the axis)
        float angle = 0.0f;          // rotation about the axis (radians)
        float pitch = 0.0f;          // translation along the axis
    };

    struct DualQuat {
        glm::quat real{ 1, 0, 0, 0 };
        glm::quat dual{ 0, 0, 0, 0 };

        static DualQuat Identity() { return {}; }

        static DualQuat FromRotationTranslation(const glm::quat& rotation, const glm::vec3& translation) {
            const glm::quat r = glm::normalize(rotation);
            return { r, glm::quat(0.0f, translation) * r * 0.5f };
        }
        static DualQuat FromTranslation(const glm::vec3& t) { return FromRotationTranslation(glm::quat(1, 0, 0, 0), t); }
        static DualQuat FromRotation(const glm::quat& q)    { return FromRotationTranslation(q, glm::vec3(0)); }

        glm::quat Rotation() const { return real; }
        glm::vec3 Translation() const {
            const glm::quat t = dual * glm::conjugate(real) * 2.0f;
            return glm::vec3(t.x, t.y, t.z);
        }

        // Apply `other` first, then this:  (A * B) x = A (B x)
        DualQuat operator*(const DualQuat& o) const { return { real * o.real, real * o.dual + dual * o.real }; }
        DualQuat operator+(const DualQuat& o) const { return { real + o.real, dual + o.dual }; }
        DualQuat operator*(float s) const { return { real * s, dual * s }; }

        // For unit dual quaternions the inverse is the quaternion conjugate of both parts.
        DualQuat Inverse() const { return { glm::conjugate(real), glm::conjugate(dual) }; }

        // Re-normalises and removes drift between the parts (enforces real . dual = 0).
        DualQuat Normalized() const {
            const float n = glm::length(real);
            if (n < 1e-12f) return Identity();
            const glm::quat r = real / n;
            glm::quat d = dual / n;
            d = d - r * glm::dot(r, d);
            return { r, d };
        }

        glm::vec3 TransformPoint(const glm::vec3& p) const { return real * p + Translation(); }
        glm::vec3 TransformVector(const glm::vec3& v) const { return real * v; }

        glm::mat4 ToMat4() const {
            glm::mat4 m = glm::mat4_cast(real);
            m[3] = glm::vec4(Translation(), 1.0f);
            return m;
        }

        // Every rigid motion is a screw: rotate by `angle` about a line and slide `pitch` along it
        // (Chasles' theorem).
        ScrewParameters ToScrew() const {
            ScrewParameters s;
            const glm::quat r = real.w < 0.0f ? -real : real;   // shortest rotation
            const glm::quat d = real.w < 0.0f ? -dual : dual;
            const float halfSin = std::sqrt(std::max(0.0f, 1.0f - r.w * r.w));
            s.angle = 2.0f * std::acos(std::clamp(r.w, -1.0f, 1.0f));
            if (halfSin < 1e-6f) {                              // pure translation: axis along t
                const glm::vec3 t = Translation();
                const float length = glm::length(t);
                s.axis = length > 1e-9f ? t / length : glm::vec3(0, 1, 0);
                s.pitch = length;
                s.angle = 0.0f;
                return s;
            }
            s.axis = glm::vec3(r.x, r.y, r.z) / halfSin;
            s.pitch = -2.0f * d.w / halfSin;
            const float halfCos = r.w;
            s.moment = (glm::vec3(d.x, d.y, d.z) - s.axis * (s.pitch * 0.5f * halfCos)) / halfSin;
            return s;
        }

        static DualQuat FromScrew(const ScrewParameters& s) {
            const float sh = std::sin(s.angle * 0.5f), ch = std::cos(s.angle * 0.5f);
            DualQuat q;
            q.real = glm::quat(ch, s.axis * sh);
            q.dual = glm::quat(-s.pitch * 0.5f * sh, s.moment * sh + s.axis * (s.pitch * 0.5f * ch));
            return q;
        }

        // Fractional power: the same screw axis, scaled angle and pitch.
        DualQuat Pow(float t) const {
            ScrewParameters s = ToScrew();
            s.angle *= t;
            s.pitch *= t;
            return FromScrew(s);
        }

        // Screw linear interpolation: constant-speed motion along the screw from a to b.
        static DualQuat ScLerp(const DualQuat& a, DualQuat b, float t) {
            if (glm::dot(a.real, b.real) < 0.0f) b = b * -1.0f;   // shortest path (same transform)
            return (a * (a.Inverse() * b).Pow(t)).Normalized();
        }

        // Dual quaternion linear blending (DLB) for skinning: weighted sum aligned to the first
        // transform's hemisphere, then normalised. The result is always a rigid transform.
        static DualQuat Blend(std::span<const DualQuat> transforms, std::span<const float> weights) {
            DualQuat sum{ glm::quat(0, 0, 0, 0), glm::quat(0, 0, 0, 0) };
            if (transforms.empty()) return Identity();
            const glm::quat pivot = transforms[0].real;
            for (size_t i = 0; i < transforms.size() && i < weights.size(); ++i) {
                const float sign = glm::dot(pivot, transforms[i].real) < 0.0f ? -1.0f : 1.0f;
                sum = sum + transforms[i] * (weights[i] * sign);
            }
            return sum.Normalized();
        }
    };
}
