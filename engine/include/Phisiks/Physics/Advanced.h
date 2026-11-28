#pragma once
// Phisiks::Physics::Advanced: particle and XPBD simulations. Backends (same results within float
// tolerance, same API):
//   GPU  OpenGL 4.6 compute (the renderer's 4.6 tier)
//   CPU  worker thread pool (OpenGL 4.1 tier, e.g. macOS, older iGPUs; headless runs)
//
// Scene components (attach to an entity; the entity's world position places the system):
//   ForceField      (Phisiks/Physics/Fields.h) uniform / planetary / vortex / n-body fields. They act
//                   on rigid bodies (CPU, every fixed step), GPU particles and XPBD nodes.
//   ParticleSystem  particles with a spatial hash grid for neighbour collisions, scene colliders,
//                   force fields and (with an NBody field) O(N^2) mutual gravity. Either a block
//                   spawned at once, or an emitter (emissionRate > 0) with optional lifetime.
//   SoftBody        XPBD rope, cloth or soft box colliding with the scene's colliders.
// Two-way coupling: particles and soft-body nodes that collide with the collider of a dynamic
// rigid body push it (reaction impulse = -m * push-out / dt, applied to the body; one fixed step
// later on the CPU backend, up to three on the GPU backend, whose results are read without stalling).
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
    enum class ParticleSpawn { Box, Disk, Fill };
    enum class ParticleRender { Sprites, Fluid };      // Fluid: screen-space fluid surface (Render::FluidSettings)
    enum class ParticleColor { Speed, Doppler };       // Doppler: relativistic blue / red shift + beaming (engine/shaders/doppler.glsl)

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
        // Emitter: `count` is the pool size. emissionRate > 0 spawns particles over time inside
        // spawnSize around the entity, moving along the entity's local emitDirection at emitSpeed
        // (+ initialVelocity) within a cone of emitSpread radians; lifetime > 0 retires them (else
        // the oldest are recycled when the pool is full).
        float emissionRate = 0.0f;                 // particles / s (0 = the whole pool at once)
        float lifetime = 0.0f;                     // s (0 = unlimited)
        glm::vec3 emitDirection{ 0.0f, 1.0f, 0.0f };
        float emitSpeed = 0.0f;
        float emitSpread = 0.25f;
        bool pushRigidBodies = true;               // two-way coupling with dynamic rigid bodies
        // Whole-pool spawn shape. Box: a jittered n x n x n grid scaled to spawnSize (the original
        // behaviour). Fill: an even lattice (spacing max(cbrt(volume / count), 2 radius)) filling
        // spawnSize from the bottom up, e.g. a pool of liquid. Disk: an annulus around the entity in the world XZ plane (uniform
        // per area, `diskThickness` tall); orbitalGM > 0 starts every particle at the circular-orbit
        // speed sqrt(GM / r) about the world Y axis (counter-clockwise seen from above).
        ParticleSpawn spawnShape = ParticleSpawn::Box;
        float diskInnerRadius = 1.0f, diskOuterRadius = 4.0f, diskThickness = 0.1f;
        float orbitalGM = 0.0f;
        // Rendering
        ParticleRender render = ParticleRender::Sprites;
        ParticleColor colorMode = ParticleColor::Speed;
        float glow = 0.0f;                         // emissive multiplier on the colour (HDR: > 1 blooms)
        float dopplerSpeed = 10.0f;                // Doppler colouring: the "speed of light" (m/s)
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
        bool pushRigidBodies = true;          // two-way coupling with dynamic rigid bodies
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
        float cpuMsLastStep = 0.0f;        // CPU time of the last Step (GPU: recording; CPU backend: the simulation)
        bool gpuAvailable = false;         // GPU compute backend in use
        bool cpuBackend = false;           // CPU worker-pool backend in use
        uint32_t particlesAlive = 0;       // emitters: live particles
        uint64_t particlesEmitted = 0;
        uint32_t coupledBodies = 0;        // dynamic bodies that received an impulse in the last applied step
        float coupledImpulse = 0.0f;       // sum of |J| (N*s) applied in the last applied step
        uint64_t coupledImpulsesApplied = 0;
    };
    Stats GetStats();
    // Destroys every particle system and soft body simulation; the scene systems rebuild them from
    // their components on the next step (the editor's play-mode Stop: back to the authored state).
    void ResetSimulations();

    // Verification / tooling readbacks (synchronise with the GPU; not for per-frame use). Work on
    // both backends; the Gpu* reference helpers below need the GPU backend.
    bool ReadSoftBody(Entity entity, std::vector<glm::vec4>& positions);   // xyz, w inverse mass
    bool ReadParticles(Entity entity, std::vector<glm::vec4>& positionRadius);
    // Non-blocking variant for per-frame gameplay use: returns the most recent completed copy (a
    // frame or two old on the GPU backend; false until the first one arrives) and starts the next.
    bool ReadParticlesAsync(Entity entity, std::vector<glm::vec4>& positionRadius);
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
