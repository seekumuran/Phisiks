#pragma once
// Phisiks::Physics::Advanced: GPU simulations on OpenGL 4.6 compute (needs the renderer).
//
// Scene components (attach to an entity; the entity's world position places the system):
//   ForceField      (Phisiks/Physics/Fields.h) uniform / planetary / vortex / n-body fields. They act
//                   on rigid bodies (CPU, every fixed step), GPU particles and XPBD nodes.
//   ParticleSystem  GPU particles with a spatial hash grid for neighbour collisions, scene
//                   colliders, force fields and (with an NBody field) O(N^2) mutual gravity.
//   SoftBody        GPU XPBD rope, cloth or soft box colliding with the scene's colliders.
// Colliders: every Collider component with a box or sphere shape, plus the ground plane y = 0
// when the scene has an entity named "Ground".

#include "Phisiks/Physics/Xpbd.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks {
    class Entity;
    class Scene;
}

namespace Phisiks::Physics::Advanced {
    struct ParticleSystem {
        uint32_t count = 4096;
        glm::vec3 spawnSize{ 2.0f, 2.0f, 2.0f };   // particles start on a jittered grid in this box (around the entity)
        glm::vec3 initialVelocity{ 0.0f };
        float radius = 0.04f;
        float mass = 0.01f;                        // per particle (kg), used by n-body fields
        float friction = 0.3f;
        float damping = 0.05f;                     // 1/s
        bool useGravity = true;
        glm::vec3 color{ 0.25f, 0.65f, 1.0f };     // linear albedo
        glm::vec3 fastColor{ 1.0f, 0.55f, 0.2f };  // tint at fastSpeed
        float fastSpeed = 6.0f;
        uint32_t seed = 1;
    };

    enum class SoftBodyKind { Rope, Cloth, Box };
    struct SoftBody {
        SoftBodyKind kind = SoftBodyKind::Cloth;
        glm::vec3 size{ 2.0f, 2.0f, 2.0f };   // rope: length along local -Y (size.y); cloth: width x (size.x) by height along -Y (size.y); box: size
        glm::ivec3 resolution{ 24, 24, 4 };   // rope: x segments; cloth: x by y particles; box: x cells per side
        float mass = 1.0f;
        float compliance = 0.0f;              // distance constraints (m/N); 0 = rigid
        float bendCompliance = 1e-3f;         // cloth bending
        float volumeCompliance = 0.0f;        // soft box tetrahedra
        bool pinned = true;                   // rope: first node; cloth: the two top corners
        int substeps = 12;
        float damping = 0.5f;
        float friction = 0.4f;
        std::string material;                 // cloth / box surface material
    };

    struct Stats {
        uint32_t particleSystems = 0;
        uint32_t particles = 0;
        uint32_t softBodies = 0;
        uint32_t softBodyNodes = 0;
        uint32_t constraints = 0;
        uint32_t colorsMax = 0;               // largest number of constraint colours (parallel passes)
        uint32_t fields = 0;
        uint32_t colliders = 0;
        uint64_t steps = 0;
        uint32_t dispatchesLastStep = 0;   // compute dispatches of the last Step (fused solver: ~2 per soft body)
        float cpuMsLastStep = 0.0f;        // CPU time to record the last Step (driver overhead; GPU work is async)
        bool gpuAvailable = false;
    };
    Stats GetStats();

    // Verification / tooling readbacks (synchronise with the GPU; not for per-frame use).
    bool ReadSoftBody(Entity entity, std::vector<glm::vec4>& positions);   // xyz, w inverse mass
    bool ReadParticles(Entity entity, std::vector<glm::vec4>& positionRadius);
    // Builds the spatial hash for `positions` on the GPU and returns, per point, the number of other
    // points within `radius` (radius <= cellSize).
    bool GpuNeighbourCounts(const std::vector<glm::vec3>& positions, float cellSize, uint32_t tableSize, float radius, std::vector<uint32_t>& counts);
    // Steps a model on the GPU exactly like Xpbd::Step (gravity only, ground plane at y = 0 if `ground`).
    // singleWorkgroup: the fused one-dispatch solver (used for bodies that fit one workgroup); false
    // forces the multi-pass path (one dispatch per colour per sub-step) that large bodies use.
    bool GpuStepModel(Xpbd::Model& model, float dt, int substeps, int steps, bool ground, bool singleWorkgroup = true);
    // GPU n-body accelerations (tiled kernel) for verification against FieldMath::NBodyAccelerations.
    bool GpuNBody(const std::vector<glm::vec3>& positions, const std::vector<float>& masses, float G, float softening, std::vector<glm::vec3>& accelerations);
    // Samples the current scene's force fields on the GPU at grid points (as the visualiser does).
    bool SampleFields(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& resolution, std::vector<glm::vec4>& samples);
}
