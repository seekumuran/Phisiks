#pragma once
// Programmable force fields (header-only, no dependencies beyond glm). The same formulas run in
// engine/shaders/fields.glsl for GPU particles, XPBD nodes and the vector-field visualiser.
//
//   Uniform           acceleration `vector` everywhere (e.g. a gravity override or wind volume)
//   SphericalGravity  planet of mass parameter GM = `strength` (m^3/s^2) and radius `radius`:
//                     |a| = GM / r^2 outside, GM r / R^3 inside (uniform density), towards the origin
//   Vortex            swirl around `axis` through the origin: tangential |a| = strength * r / radius
//                     inside the core, strength * (radius / r)^falloff outside
//   NBody             mutual softened gravity between the bodies/particles it affects:
//                     a_i = G * sum_j m_j (p_j - p_i) / (|p_j - p_i|^2 + eps^2)^(3/2),  G = `strength`
// `influenceRadius` > 0 limits a field to a sphere around its origin (fades out over the last 10%).

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Phisiks::Physics {
    enum class FieldType : uint32_t { Uniform = 0, SphericalGravity = 1, Vortex = 2, NBody = 3 };

    // Scene component (Phisiks::Scene): the origin is the entity's world position and `axis` is
    // rotated by its world rotation.
    struct ForceField {
        FieldType type = FieldType::Uniform;
        glm::vec3 vector{ 0.0f, -9.81f, 0.0f };   // Uniform: acceleration (m/s^2)
        float strength = 1.0f;                     // SphericalGravity: GM; Vortex: tangential accel at the core radius; NBody: G
        float radius = 1.0f;                       // SphericalGravity: planet radius; Vortex: core radius (m)
        float falloff = 1.0f;                      // Vortex: exponent outside the core
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };        // Vortex axis (local)
        float influenceRadius = 0.0f;   // 0 = unbounded. NBody (rigid bodies): only bodies within this radius of the entity take part              // 0 = unbounded
        float softening = 0.05f;                   // NBody softening length (m)
        bool affectsRigidBodies = true;
        bool affectsParticles = true;
        bool affectsSoftBodies = true;
    };

    namespace FieldMath {
        inline float InfluenceWeight(float distance, float influenceRadius) {
            if (influenceRadius <= 0.0f) return 1.0f;
            const float t = (influenceRadius - distance) / (0.1f * influenceRadius);
            const float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return c * c * (3.0f - 2.0f * c);
        }

        // Acceleration at `p` from one field placed at `origin` (world `axis`). NBody contributes 0 here.
        inline glm::vec3 Acceleration(const ForceField& f, const glm::vec3& origin, const glm::vec3& axis, const glm::vec3& p) {
            const glm::vec3 d = p - origin;
            const float dist = glm::length(d);
            const float w = InfluenceWeight(dist, f.influenceRadius);
            if (w <= 0.0f) return glm::vec3(0.0f);
            switch (f.type) {
                case FieldType::Uniform:
                    return f.vector * w;
                case FieldType::SphericalGravity: {
                    if (dist < 1e-6f) return glm::vec3(0.0f);
                    const float R = f.radius > 1e-6f ? f.radius : 1e-6f;
                    const float magnitude = dist >= R ? f.strength / (dist * dist) : f.strength * dist / (R * R * R);
                    return -d / dist * magnitude * w;
                }
                case FieldType::Vortex: {
                    const glm::vec3 a = glm::normalize(axis);
                    const glm::vec3 radial = d - a * glm::dot(d, a);
                    const float r = glm::length(radial);
                    if (r < 1e-6f) return glm::vec3(0.0f);
                    const float core = f.radius > 1e-6f ? f.radius : 1e-6f;
                    const float magnitude = r <= core ? f.strength * r / core : f.strength * std::pow(core / r, f.falloff);
                    return glm::cross(a, radial / r) * magnitude * w;
                }
                case FieldType::NBody:
                    return glm::vec3(0.0f);
            }
            return glm::vec3(0.0f);
        }

        // Where to sample a field for a body about to take a fixed step of `dt` split into `substeps`
        // sub-steps. The force is held for the whole step and the velocity kicks happen at the start of
        // each sub-step, so the mean kick time is dt*(substeps-1)/(2*substeps): sampling there instead of
        // at the current position keeps a 60 Hz circular orbit within ~0.4% of its radius (vs ~7%).
        inline glm::vec3 SamplePosition(const glm::vec3& position, const glm::vec3& velocity, float dt, uint32_t substeps) {
            const float n = float(substeps < 1u ? 1u : substeps);
            return position + velocity * (dt * (n - 1.0f) / (2.0f * n));
        }

        // Softened pairwise gravity (O(n^2) reference; the GPU uses a tiled kernel).
        inline void NBodyAccelerations(const glm::vec3* positions, const float* masses, size_t n, float G, float softening, glm::vec3* out) {
            const float eps2 = softening * softening;
            for (size_t i = 0; i < n; ++i) {
                glm::vec3 a(0.0f);
                for (size_t j = 0; j < n; ++j) {
                    if (i == j) continue;
                    const glm::vec3 d = positions[j] - positions[i];
                    const float r2 = glm::dot(d, d) + eps2;
                    a += d * (masses[j] / (r2 * std::sqrt(r2)));
                }
                out[i] = a * G;
            }
        }
    }
}
