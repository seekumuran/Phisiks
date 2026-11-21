// Particle systems, XPBD soft bodies, force-field and collider data, on one of two backends:
//   GPU  OpenGL 4.6 compute (renderer on the 4.6 tier)
//   CPU  the worker thread pool (renderer on the 4.1 tier, or no renderer at all: headless). The CPU
//        path runs the same algorithms (Xpbd::Step is the reference the GPU solver mirrors; particles
//        mirror particles.comp) and uploads positions for drawing when a renderer exists.
// Also: particle emitters (pool slots spawned / retired on the CPU, both backends) and two-way
// coupling (reaction impulses of particle / node contacts on dynamic rigid-body colliders).
#include "compute/AdvancedWorld.h"
#include "compute/SpatialHash.h"
#include "compute/ThreadPool.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"
#include "render/RendererInternal.h"
#include "render/ShaderPrograms.h"
#include "render/gl/GLObjects.h"

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>

namespace Phisiks { namespace Shaders = Render::Shaders; }

namespace {
    uint64_t g_dispatches = 0;   // compute dispatches issued by the GPU physics (cost / regression metric)
    void CountedDispatch(GLuint x, GLuint y, GLuint z) { glDispatchCompute(x, y, z); ++g_dispatches; }
}

namespace Phisiks::Advanced {
    namespace {
        using namespace Physics;
        using namespace Physics::Advanced;
        using Shaders::ProgramId;

        struct FieldGpu { glm::vec4 originType, vectorStrength, axisRadius, params; };
        struct ColliderGpu { glm::vec4 centerType, normalRadius, halfExtents, rotation; };
        struct ConstraintGpu { glm::uvec4 idx; glm::vec4 params; };

        constexpr int IMPULSE_SLOTS = 3;
        constexpr uint32_t MAX_COLLIDERS = 128;
        constexpr float IMPULSE_SCALE = 1.0e4f;   // fixed-point units per N*s (GPU atomics)
        const glm::vec4 DEAD_POSITION(0.0f, -1.0e4f, 0.0f, 0.0f);   // radius 0 = dead emitter slot

        // ---- GL helpers (tier-aware buffers; compute helpers are 4.6 only)
        GLuint MakeBuffer(size_t bytes, const void* data = nullptr) { return Render::GL::CreateBufferObject(std::max<size_t>(bytes, 16), data); }
        void FreeBuffer(GLuint& b) { if (b) glDeleteBuffers(1, &b); b = 0; }
        void Upload(GLuint& b, size_t& capacity, const void* data, size_t bytes) {
            if (!b || bytes > capacity) {
                FreeBuffer(b);
                capacity = std::max<size_t>(bytes + bytes / 2, 256);
                b = MakeBuffer(capacity);
            }
            if (bytes) Render::GL::BufferSubData(b, 0, bytes, data);
        }

        GLuint Use(ProgramId id) {
            const GLuint p = Render::Internal::ProgramHandle(id);
            glUseProgram(p);
            return p;
        }
        void U1u(GLuint p, const char* n, uint32_t v) { glProgramUniform1ui(p, glGetUniformLocation(p, n), v); }
        void U1i(GLuint p, const char* n, int v) { glProgramUniform1i(p, glGetUniformLocation(p, n), v); }
        void U1f(GLuint p, const char* n, float v) { glProgramUniform1f(p, glGetUniformLocation(p, n), v); }
        void U3f(GLuint p, const char* n, const glm::vec3& v) { glProgramUniform3f(p, glGetUniformLocation(p, n), v.x, v.y, v.z); }
        void Bind(uint32_t binding, GLuint buffer) { glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer); }
        void Barrier() { glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); }
        uint32_t Groups(uint32_t n, uint32_t size) { return (n + size - 1) / size; }
        uint32_t NextPow2(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

        // ------------------------------------------------------------------ spatial hash (GPU)
        struct HashGrid {
            GLuint counters = 0, cellStart = 0, sorted = 0, blockSums = 0;
            uint32_t tableSize = 0, capacity = 0;
            void Ensure(uint32_t table, uint32_t count) {
                if (table == tableSize && count <= capacity) return;
                Release();
                tableSize = table;
                capacity = count;
                counters = MakeBuffer((table + 1) * 4ull);
                cellStart = MakeBuffer((table + 1) * 4ull);
                sorted = MakeBuffer(std::max(count, 1u) * 4ull);
                blockSums = MakeBuffer(512 * 4);
            }
            void Release() { FreeBuffer(counters); FreeBuffer(cellStart); FreeBuffer(sorted); FreeBuffer(blockSums); tableSize = capacity = 0; }

            // count -> exclusive scan -> scatter (counting sort of the elements into hash buckets)
            void Build(GLuint elements, uint32_t count, uint32_t stride, float cellSize, bool skipDead) {
                const uint32_t zero = 0;
                glClearNamedBufferData(counters, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                GLuint p = Use(Shaders::HASH_COUNT);
                U1u(p, "u_count", count); U1u(p, "u_stride", stride); U1f(p, "u_invCellSize", 1.0f / cellSize); U1u(p, "u_tableSize", tableSize);
                U1i(p, "u_skipDead", skipDead ? 1 : 0);
                Bind(4, elements); Bind(5, counters);
                CountedDispatch(Groups(count, 256), 1, 1);
                Barrier();

                const uint32_t n = tableSize + 1, blocks = Groups(n, 512);
                p = Use(Shaders::SCAN_LOCAL);
                U1u(p, "u_n", n);
                Bind(5, counters); Bind(6, cellStart); Bind(13, blockSums);
                CountedDispatch(blocks, 1, 1);
                Barrier();
                p = Use(Shaders::SCAN_BLOCKS);
                U1u(p, "u_n", blocks);
                CountedDispatch(1, 1, 1);
                Barrier();
                p = Use(Shaders::SCAN_ADD);
                U1u(p, "u_n", n);
                CountedDispatch(blocks, 1, 1);
                Barrier();

                glClearNamedBufferData(counters, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                p = Use(Shaders::HASH_SCATTER);
                U1u(p, "u_count", count); U1u(p, "u_stride", stride); U1f(p, "u_invCellSize", 1.0f / cellSize); U1u(p, "u_tableSize", tableSize);
                U1i(p, "u_skipDead", skipDead ? 1 : 0);
                Bind(4, elements); Bind(5, counters); Bind(6, cellStart); Bind(7, sorted);
                CountedDispatch(Groups(count, 256), 1, 1);
                Barrier();
            }
        };

        uint32_t TableSizeFor(uint32_t count) { return std::clamp(NextPow2(count * 2), 1024u, 512u * 512u - 1u); }

        // ------------------------------------------------------------------ systems
        struct Particles {
            ParticleSystem params;
            uint32_t entity = 0;
            uint32_t count = 0;
            float cellSize = 0.1f;
            // GPU
            GLuint state[2] = {};
            int current = 0;
            HashGrid hash;
            GLuint accel = 0;
            // CPU
            std::vector<glm::vec4> cpu[2];
            GLuint renderBuffer = 0;
            // emitter
            bool emitter = false;
            std::vector<float> birth;              // spawn time per slot (< 0 = dead)
            uint32_t cursor = 0;
            float accumulator = 0.0f, time = 0.0f;
            uint32_t alive = 0;
            uint64_t emitted = 0;
            uint32_t rng = 1;
            void Release() { FreeBuffer(state[0]); FreeBuffer(state[1]); FreeBuffer(accel); FreeBuffer(renderBuffer); hash.Release(); }
        };

        struct Soft {
            SoftBody params;
            uint32_t entity = 0;
            Xpbd::Model model;   // CPU backend: the live state; GPU: colour ranges, triangles, radius
            GLuint x = 0, v = 0, prev = 0, constraints = 0, colorStarts = 0, normals = 0, adjStart = 0, adjTris = 0, tris = 0, vao = 0;
            uint32_t count = 0;
            std::vector<glm::vec4> cpuNormals;
            void Release() {
                for (GLuint* b : { &x, &v, &prev, &constraints, &colorStarts, &normals, &adjStart, &adjTris, &tris }) FreeBuffer(*b);
                if (vao) glDeleteVertexArrays(1, &vao);
                vao = 0;
            }
        };

        struct ColliderInfo { BodyId body; bool dynamic = false; };

        struct State {
            bool cpu = false;          // CPU worker-pool backend
            bool render = false;       // a renderer exists (GL buffers for drawing)
            const Scene* scene = nullptr;
            std::map<uint32_t, std::unique_ptr<Particles>> particles;   // ordered by entity id: deterministic
            std::map<uint32_t, std::unique_ptr<Soft>> soft;
            GLuint fields = 0, colliders = 0, samples = 0;
            uint32_t lastDispatches = 0;
            float lastCpuMs = 0.0f;
            size_t fieldsCap = 0, collidersCap = 0, samplesCap = 0;
            uint32_t fieldCount = 0, colliderCount = 0;
            bool particleNBody = false;
            float nbodyG = 0.0f, nbodySoftening = 0.05f;
            uint64_t steps = 0;
            // CPU copies of the scene data
            std::vector<Xpbd::PlacedField> placedFields;
            std::vector<uint32_t> fieldMasks;
            std::vector<Xpbd::Collider> cpuColliders;
            std::vector<ColliderInfo> colliderInfo;
            // coupling
            GLuint impulseBuffers[IMPULSE_SLOTS] = {};
            GLsync impulseFences[IMPULSE_SLOTS] = {};
            std::vector<ColliderInfo> slotInfo[IMPULSE_SLOTS];
            float slotDt[IMPULSE_SLOTS] = {};
            uint32_t coupledBodies = 0;
            float coupledImpulse = 0.0f;
            uint64_t coupledApplied = 0;
        };
        std::unique_ptr<State> g;

        void BindShared() { Bind(10, g->fields); Bind(11, g->colliders); }

        glm::vec3 RandomUnit(uint32_t& seed) {
            auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
            return glm::vec3(rnd(), rnd(), rnd());
        }

        // ------------------------------------------------------------------ CPU field / collider math
        glm::vec3 FieldAccelerationCpu(const glm::vec3& p, uint32_t mask) {
            glm::vec3 a(0.0f);
            for (size_t i = 0; i < g->placedFields.size(); ++i) {
                if ((g->fieldMasks[i] & mask) == 0) continue;
                const Xpbd::PlacedField& f = g->placedFields[i];
                a += FieldMath::Acceleration(f.field, f.origin, f.axis, p);
            }
            return a;
        }

        // Mirrors CollideAll() in colliders.glsl (impulses: 2 per collider, linear + angular).
        bool CollideAllCpu(glm::vec3& x, const glm::vec3& prev, float r, float friction, float massOverH, glm::vec3* impulses) {
            bool contact = false;
            for (size_t k = 0; k < g->cpuColliders.size(); ++k) {
                const glm::vec3 before = x;
                if (!Xpbd::CollideParticle(x, prev, r, friction, g->cpuColliders[k])) continue;
                contact = true;
                if (impulses && g->colliderInfo[k].dynamic) {
                    const glm::vec3 J = -(x - before) * massOverH;
                    impulses[k * 2] += J;
                    impulses[k * 2 + 1] += glm::cross(x - g->cpuColliders[k].center, J);
                }
            }
            return contact;
        }

        void ApplyImpulses(const glm::vec3* impulses, const std::vector<ColliderInfo>& infos, float dt) {
            g->coupledBodies = 0;
            g->coupledImpulse = 0.0f;
            for (size_t k = 0; k < infos.size(); ++k) {
                if (!infos[k].dynamic || !Physics::IsValid(infos[k].body)) continue;
                const glm::vec3 J = impulses[k * 2], L = impulses[k * 2 + 1];
                const float magnitude = glm::length(J);
                if (magnitude <= 0.0f && glm::length(L) <= 0.0f) continue;
                Physics::AddImpulse(infos[k].body, J);
                if (dt > 0.0f) Physics::AddTorque(infos[k].body, L / dt);
                ++g->coupledBodies;
                g->coupledImpulse += magnitude;
                ++g->coupledApplied;
            }
        }

        // ------------------------------------------------------------------ particles
        std::unique_ptr<Particles> CreateParticles(const ParticleSystem& ps, uint32_t entity, const glm::vec3& origin) {
            auto p = std::make_unique<Particles>();
            p->params = ps;
            p->entity = entity;
            p->count = std::max(ps.count, 1u);
            p->cellSize = 2.0f * ps.radius;
            p->emitter = ps.emissionRate > 0.0f;
            p->rng = ps.seed * 747796405u + 2891336453u;
            std::vector<glm::vec4> data(p->count * 2);
            if (p->emitter) {
                for (uint32_t i = 0; i < p->count; ++i) { data[i * 2] = DEAD_POSITION; data[i * 2 + 1] = glm::vec4(0.0f, 0.0f, 0.0f, ps.mass); }
                p->birth.assign(p->count, -1.0f);
            }
            else {
                const int n = int(std::ceil(std::cbrt(double(p->count))));
                uint32_t seed = p->rng;
                for (uint32_t i = 0; i < p->count; ++i) {
                    const glm::vec3 cell(float(i % n), float((i / n) % n), float(i / (n * n)));
                    const glm::vec3 jitter = RandomUnit(seed) - 0.5f;
                    const glm::vec3 t = (cell + 0.5f + jitter * 0.3f) / float(n);
                    data[i * 2] = glm::vec4(origin + (t - 0.5f) * ps.spawnSize, ps.radius);
                    data[i * 2 + 1] = glm::vec4(ps.initialVelocity, ps.mass);
                }
                p->alive = p->count;
            }
            const size_t bytes = data.size() * sizeof(glm::vec4);
            if (g->cpu) {
                p->cpu[0] = data;
                p->cpu[1] = data;
                if (g->render) p->renderBuffer = MakeBuffer(bytes, data.data());
            }
            else {
                p->state[0] = MakeBuffer(bytes, data.data());
                p->state[1] = MakeBuffer(bytes, data.data());
                p->accel = MakeBuffer(p->count * sizeof(glm::vec4));
                p->hash.Ensure(TableSizeFor(p->count), p->count);
            }
            return p;
        }

        // Spawns / retires emitter slots for this step; returns the slot writes.
        void Emit(Particles& p, Scene& scene, float dt, std::vector<std::pair<uint32_t, std::array<glm::vec4, 2>>>& writes) {
            if (!p.emitter) return;
            p.time += dt;
            const ParticleSystem& ps = p.params;
            if (ps.lifetime > 0.0f) {
                for (uint32_t i = 0; i < p.count; ++i) {
                    if (p.birth[i] >= 0.0f && p.time - p.birth[i] >= ps.lifetime) {
                        p.birth[i] = -1.0f;
                        --p.alive;
                        writes.push_back({ i, { DEAD_POSITION, glm::vec4(0.0f, 0.0f, 0.0f, ps.mass) } });
                    }
                }
            }
            p.accumulator += ps.emissionRate * dt;
            const uint32_t spawn = uint32_t(std::min(p.accumulator, float(p.count)));
            p.accumulator -= float(spawn);
            if (spawn == 0) return;
            const Entity e = scene.FromId(p.entity);
            const glm::vec3 origin = e ? e.WorldPosition() : glm::vec3(0.0f);
            const glm::quat rotation = e ? e.WorldRotation() : glm::quat(1, 0, 0, 0);
            glm::vec3 dir = rotation * (glm::length(ps.emitDirection) > 1e-6f ? glm::normalize(ps.emitDirection) : glm::vec3(0, 1, 0));
            const glm::vec3 tangent = glm::normalize(std::fabs(dir.y) < 0.9f ? glm::cross(dir, glm::vec3(0, 1, 0)) : glm::cross(dir, glm::vec3(1, 0, 0)));
            const glm::vec3 bitangent = glm::cross(dir, tangent);
            for (uint32_t k = 0; k < spawn; ++k) {
                const uint32_t slot = p.cursor;
                p.cursor = (p.cursor + 1) % p.count;
                if (p.birth[slot] < 0.0f) ++p.alive;
                p.birth[slot] = p.time;
                const glm::vec3 r = RandomUnit(p.rng);
                const glm::vec3 position = origin + rotation * ((r - 0.5f) * ps.spawnSize);
                const glm::vec3 r2 = RandomUnit(p.rng);
                const float cosTheta = 1.0f - r2.x * (1.0f - std::cos(ps.emitSpread));
                const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const float phi = 6.2831853f * r2.y;
                const glm::vec3 d = dir * cosTheta + (tangent * std::cos(phi) + bitangent * std::sin(phi)) * sinTheta;
                writes.push_back({ slot, { glm::vec4(position, ps.radius), glm::vec4(ps.initialVelocity + d * ps.emitSpeed, ps.mass) } });
                ++p.emitted;
            }
        }

        void StepParticlesGpu(Particles& p, float dt, const std::vector<std::pair<uint32_t, std::array<glm::vec4, 2>>>& writes) {
            const GLuint in = p.state[p.current], out = p.state[1 - p.current];
            for (const auto& [slot, value] : writes) glNamedBufferSubData(in, GLintptr(slot) * 32, 32, value.data());
            p.hash.Build(in, p.count, 2, p.cellSize, p.emitter);
            if (g->particleNBody) {
                const GLuint prog = Use(Shaders::NBODY);
                U1u(prog, "u_count", p.count); U1f(prog, "u_G", g->nbodyG); U1f(prog, "u_softening2", g->nbodySoftening * g->nbodySoftening);
                Bind(4, in); Bind(8, p.accel);
                CountedDispatch(Groups(p.count, 256), 1, 1);
                Barrier();
            }
            const GLuint prog = Use(Shaders::PARTICLES_STEP);
            U1u(prog, "u_count", p.count); U1f(prog, "u_dt", dt);
            U3f(prog, "u_gravity", p.params.useGravity ? glm::vec3(0.0f, -9.81f, 0.0f) : glm::vec3(0.0f));
            U1i(prog, "u_useNBody", g->particleNBody ? 1 : 0);
            U1f(prog, "u_friction", p.params.friction); U1f(prog, "u_damping", p.params.damping);
            U1i(prog, "u_fieldCount", int(g->fieldCount)); U1i(prog, "u_colliderCount", int(g->colliderCount));
            U1f(prog, "u_invCellSize", 1.0f / p.cellSize); U1u(prog, "u_tableSize", p.hash.tableSize);
            U1i(prog, "u_coupling", p.params.pushRigidBodies ? 1 : 0); U1f(prog, "u_impulseScale", IMPULSE_SCALE);
            Bind(4, in); Bind(6, p.hash.cellStart); Bind(7, p.hash.sorted); Bind(8, p.accel); Bind(13, out);
            BindShared();
            CountedDispatch(Groups(p.count, 256), 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
            p.current = 1 - p.current;
        }

        // Mirrors PARTICLES_STEP (particles.comp.glsl) on the worker pool.
        void StepParticlesCpu(Particles& p, float dt, const std::vector<std::pair<uint32_t, std::array<glm::vec4, 2>>>& writes, std::vector<glm::vec3>& impulses) {
            std::vector<glm::vec4>& in = p.cpu[p.current];
            std::vector<glm::vec4>& out = p.cpu[1 - p.current];
            for (const auto& [slot, value] : writes) { in[slot * 2] = value[0]; in[slot * 2 + 1] = value[1]; }
            const uint32_t n = p.count;
            // spatial hash of the live particles (counting sort, same cell hash as the GPU)
            const uint32_t table = TableSizeFor(n);
            std::vector<uint32_t> cellStart(table + 1, 0), sorted;
            sorted.reserve(n);
            std::vector<uint32_t> hashOf(n, UINT32_MAX);
            for (uint32_t i = 0; i < n; ++i) {
                if (in[i * 2].w <= 0.0f) continue;
                hashOf[i] = Compute::SpatialHash::Hash(Compute::SpatialHash::Cell(glm::vec3(in[i * 2]), p.cellSize), table);
                ++cellStart[hashOf[i] + 1];
            }
            for (uint32_t h = 0; h < table; ++h) cellStart[h + 1] += cellStart[h];
            sorted.resize(cellStart[table]);
            std::vector<uint32_t> cursor(cellStart.begin(), cellStart.end() - 1);
            for (uint32_t i = 0; i < n; ++i) if (hashOf[i] != UINT32_MAX) sorted[cursor[hashOf[i]]++] = i;
            // n-body
            std::vector<glm::vec3> accel;
            if (g->particleNBody) {
                accel.assign(n, glm::vec3(0.0f));
                const float eps2 = g->nbodySoftening * g->nbodySoftening;
                Compute::ThreadPool::Get().ParallelFor(n, 64, [&](size_t, size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i) {
                        if (in[i * 2].w <= 0.0f) continue;
                        const glm::vec3 pi(in[i * 2]);
                        glm::vec3 a(0.0f);
                        for (uint32_t j = 0; j < n; ++j) {
                            if (j == i || in[j * 2].w <= 0.0f) continue;
                            const glm::vec3 d = glm::vec3(in[j * 2]) - pi;
                            const float r2 = glm::dot(d, d) + eps2;
                            a += d * (in[j * 2 + 1].w / (r2 * std::sqrt(r2)));
                        }
                        accel[i] = a * g->nbodyG;
                    }
                });
            }
            const glm::vec3 gravity = p.params.useGravity ? glm::vec3(0.0f, -9.81f, 0.0f) : glm::vec3(0.0f);
            const size_t chunks = Compute::ThreadPool::ChunkCount(n, 256);
            std::vector<std::vector<glm::vec3>> chunkImpulses(chunks, std::vector<glm::vec3>(g->cpuColliders.size() * 2, glm::vec3(0.0f)));
            const bool coupling = p.params.pushRigidBodies;
            const float inv = 1.0f / p.cellSize;
            (void)inv;
            Compute::ThreadPool::Get().ParallelFor(n, 256, [&](size_t chunk, size_t begin, size_t end) {
                glm::vec3* myImpulses = coupling && !chunkImpulses[chunk].empty() ? chunkImpulses[chunk].data() : nullptr;
                for (size_t i = begin; i < end; ++i) {
                    const glm::vec3 x(in[i * 2]);
                    const float r = in[i * 2].w;
                    glm::vec3 v(in[i * 2 + 1]);
                    const float mass = in[i * 2 + 1].w;
                    if (r <= 0.0f) { out[i * 2] = in[i * 2]; out[i * 2 + 1] = in[i * 2 + 1]; continue; }
                    glm::vec3 a = gravity + FieldAccelerationCpu(x, 2u);
                    if (!accel.empty()) a += accel[i];
                    glm::vec3 correction(0.0f);
                    const glm::ivec3 c = Compute::SpatialHash::Cell(x, p.cellSize);
                    uint32_t visited[27];
                    int visitedCount = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const uint32_t h = Compute::SpatialHash::Hash(c + glm::ivec3(dx, dy, dz), table);
                                bool seen = false;
                                for (int k = 0; k < visitedCount; ++k) seen = seen || visited[k] == h;
                                if (seen) continue;
                                visited[visitedCount++] = h;
                                for (uint32_t s = cellStart[h]; s < cellStart[h + 1]; ++s) {
                                    const uint32_t j = sorted[s];
                                    if (j == i) continue;
                                    const glm::vec3 d = x - glm::vec3(in[j * 2]);
                                    const float rsum = r + in[j * 2].w;
                                    const float d2 = glm::dot(d, d);
                                    if (d2 >= rsum * rsum) continue;
                                    const float dist = std::sqrt(d2);
                                    const glm::vec3 nrm = dist > 1e-6f ? d / dist : glm::normalize(glm::vec3(float(i & 7u) - 3.5f, 1.0f, float(j & 7u) - 3.5f));
                                    correction += nrm * (rsum - dist) * 0.5f;
                                    const float approach = glm::dot(v - glm::vec3(in[j * 2 + 1]), nrm);
                                    if (approach < 0.0f) v -= nrm * approach * 0.5f;
                                }
                            }
                    v += a * dt;
                    v *= std::max(0.0f, 1.0f - p.params.damping * dt);
                    glm::vec3 next = x + v * dt + correction;
                    if (CollideAllCpu(next, x, r, p.params.friction, mass / dt, myImpulses)) v = (next - x) / dt;
                    out[i * 2] = glm::vec4(next, r);
                    out[i * 2 + 1] = glm::vec4(v, mass);
                }
            });
            if (coupling) for (const auto& ci : chunkImpulses) for (size_t k = 0; k < ci.size(); ++k) impulses[k] += ci[k];
            p.current = 1 - p.current;
            if (p.renderBuffer) Render::GL::BufferSubData(p.renderBuffer, 0, out.size() * sizeof(glm::vec4), out.data());
        }

        // ------------------------------------------------------------------ soft bodies
        void UploadSoft(Soft& s) {
            const Xpbd::Model& m = s.model;
            s.count = uint32_t(m.x.size());
            if (g->cpu) {
                if (!g->render) return;
                s.x = MakeBuffer(m.x.size() * sizeof(glm::vec4), m.x.data());
                s.normals = MakeBuffer(m.x.size() * sizeof(glm::vec4));
                if (!m.triangles.empty()) {
                    s.tris = MakeBuffer(m.triangles.size() * 4, m.triangles.data());
                    s.vao = Render::GL::CreateVertexArray();
                    Render::GL::SetElementBuffer(s.vao, s.tris);
                }
                return;
            }
            std::vector<glm::vec4> v(m.x.size());
            for (size_t i = 0; i < v.size(); ++i) v[i] = glm::vec4(i < m.v.size() ? m.v[i] : glm::vec3(0.0f), 0.0f);
            std::vector<ConstraintGpu> c(m.constraints.size());
            for (size_t i = 0; i < c.size(); ++i) {
                const Xpbd::Constraint& k = m.constraints[i];
                c[i] = { glm::uvec4(k.i[0], k.i[1], k.i[2], k.i[3]), glm::vec4(float(k.type), k.rest, k.compliance, 0.0f) };
            }
            s.x = MakeBuffer(m.x.size() * sizeof(glm::vec4), m.x.data());
            s.v = MakeBuffer(v.size() * sizeof(glm::vec4), v.data());
            s.prev = MakeBuffer(m.x.size() * sizeof(glm::vec4), m.x.data());
            s.constraints = MakeBuffer(c.size() * sizeof(ConstraintGpu), c.data());
            s.colorStarts = MakeBuffer(m.colorStart.size() * sizeof(uint32_t), m.colorStart.data());
            s.normals = MakeBuffer(m.x.size() * sizeof(glm::vec4));
            if (!m.triangles.empty()) {
                // vertex -> adjacent triangles (CSR) for the normals pass
                std::vector<uint32_t> start(m.x.size() + 1, 0), list(m.triangles.size());
                for (uint32_t idx : m.triangles) ++start[idx + 1];
                for (size_t i = 0; i < m.x.size(); ++i) start[i + 1] += start[i];
                std::vector<uint32_t> cursor(start.begin(), start.end() - 1);
                for (size_t t = 0; t < m.triangles.size() / 3; ++t)
                    for (int k = 0; k < 3; ++k) list[cursor[m.triangles[t * 3 + k]]++] = uint32_t(t);
                s.adjStart = MakeBuffer(start.size() * 4, start.data());
                s.adjTris = MakeBuffer(list.size() * 4, list.data());
                s.tris = MakeBuffer(m.triangles.size() * 4, m.triangles.data());
                s.vao = Render::GL::CreateVertexArray();
                Render::GL::SetElementBuffer(s.vao, s.tris);
            }
        }

        Xpbd::Model BuildModel(const SoftBody& sb, const glm::vec3& origin, const glm::quat& rotation) {
            Xpbd::Model m;
            const glm::vec3 right = rotation * glm::vec3(1, 0, 0), down = rotation * glm::vec3(0, -1, 0);
            if (sb.kind == SoftBodyKind::Rope) {
                m = Xpbd::MakeRope(origin, origin + down * sb.size.y, std::max(sb.resolution.x, 1), sb.mass, sb.compliance, sb.pinned);
            }
            else if (sb.kind == SoftBodyKind::Cloth) {
                m = Xpbd::MakeCloth(origin - right * (0.5f * sb.size.x), right * sb.size.x, down * sb.size.y,
                                    std::max(sb.resolution.x, 2), std::max(sb.resolution.y, 2), sb.mass, sb.compliance, sb.bendCompliance, sb.pinned);
            }
            else {
                m = Xpbd::MakeSoftBox(origin, sb.size, std::max(sb.resolution.x, 1), sb.mass, sb.compliance, sb.volumeCompliance);
            }
            m.damping = sb.damping;
            m.friction = sb.friction;
            return m;
        }

        // Bodies up to this size run the fused single-workgroup solver (one dispatch per fixed step);
        // larger ones use one dispatch per colour per sub-step across the whole GPU.
        constexpr uint32_t FUSED_MAX_NODES = 8192, FUSED_MAX_CONSTRAINTS = 32768;
        bool UseFused(const Soft& s) { return s.count <= FUSED_MAX_NODES && s.model.constraints.size() <= FUSED_MAX_CONSTRAINTS; }

        void StepSoftPasses(Soft& s, float h, int n, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount, bool coupling);

        void StepSoft(Soft& s, float dt, int substeps, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount, bool allowFused = true, bool coupling = false) {
            const int n = std::max(substeps, 1);
            const float h = dt / n;
            if (allowFused && UseFused(s)) {
                const GLuint fused = Use(Shaders::XPBD_FUSED);
                U1u(fused, "u_count", s.count); U1f(fused, "u_h", h); U3f(fused, "u_gravity", gravity); U1i(fused, "u_fieldCount", int(fieldCount));
                U1f(fused, "u_radius", s.model.particleRadius); U1f(fused, "u_friction", s.model.friction); U1f(fused, "u_damping", s.model.damping);
                U1i(fused, "u_colliderCount", int(colliderCount)); U1i(fused, "u_substeps", n);
                U1i(fused, "u_coupling", coupling ? 1 : 0); U1f(fused, "u_impulseScale", IMPULSE_SCALE);
                U1u(fused, "u_colorCount", uint32_t(s.model.colorStart.empty() ? 0 : s.model.colorStart.size() - 1));
                Bind(4, s.x); Bind(5, s.v); Bind(6, s.prev); Bind(7, s.constraints); Bind(8, s.colorStarts);
                CountedDispatch(1, 1, 1);
                Barrier();
            }
            else {
                StepSoftPasses(s, h, n, gravity, fieldCount, colliderCount, coupling);
            }
            if (s.vao) {
                const GLuint p = Use(Shaders::XPBD_NORMALS);
                U1u(p, "u_count", s.count);
                Bind(4, s.x); Bind(8, s.normals); Bind(13, s.adjStart); Bind(14, s.adjTris); Bind(15, s.tris);
                CountedDispatch(Groups(s.count, 128), 1, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
        }

        void StepSoftPasses(Soft& s, float h, int n, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount, bool coupling) {
            const GLuint predict = Render::Internal::ProgramHandle(Shaders::XPBD_PREDICT);
            const GLuint solve = Render::Internal::ProgramHandle(Shaders::XPBD_SOLVE);
            const GLuint finish = Render::Internal::ProgramHandle(Shaders::XPBD_FINISH);
            U1u(predict, "u_count", s.count); U1f(predict, "u_h", h); U3f(predict, "u_gravity", gravity); U1i(predict, "u_fieldCount", int(fieldCount));
            U1f(solve, "u_h", h);
            U1u(finish, "u_count", s.count); U1f(finish, "u_h", h); U1f(finish, "u_radius", s.model.particleRadius);
            U1f(finish, "u_friction", s.model.friction); U1f(finish, "u_damping", s.model.damping); U1i(finish, "u_colliderCount", int(colliderCount));
            U1i(finish, "u_coupling", coupling ? 1 : 0); U1f(finish, "u_impulseScale", IMPULSE_SCALE);
            Bind(4, s.x); Bind(5, s.v); Bind(6, s.prev); Bind(7, s.constraints);
            const uint32_t groups = Groups(s.count, 128);
            for (int sub = 0; sub < n; ++sub) {
                glUseProgram(predict);
                CountedDispatch(groups, 1, 1);
                Barrier();
                glUseProgram(solve);
                for (size_t k = 0; k + 1 < s.model.colorStart.size(); ++k) {
                    const uint32_t first = s.model.colorStart[k], count = s.model.colorStart[k + 1] - first;
                    if (!count) continue;
                    U1u(solve, "u_first", first);
                    U1u(solve, "u_constraintCount", count);
                    CountedDispatch(Groups(count, 128), 1, 1);
                    Barrier();
                }
                glUseProgram(finish);
                CountedDispatch(groups, 1, 1);
                Barrier();
            }
        }

        void SurfaceNormals(Soft& s) {
            const Xpbd::Model& m = s.model;
            s.cpuNormals.assign(m.x.size(), glm::vec4(0.0f));
            for (size_t t = 0; t + 2 < m.triangles.size(); t += 3) {
                const uint32_t a = m.triangles[t], b = m.triangles[t + 1], c = m.triangles[t + 2];
                const glm::vec3 n = glm::cross(glm::vec3(m.x[b]) - glm::vec3(m.x[a]), glm::vec3(m.x[c]) - glm::vec3(m.x[a]));
                for (uint32_t i : { a, b, c }) s.cpuNormals[i] += glm::vec4(n, 0.0f);
            }
            for (glm::vec4& n : s.cpuNormals) {
                const float len = glm::length(glm::vec3(n));
                n = len > 1e-12f ? glm::vec4(glm::vec3(n) / len, 0.0f) : glm::vec4(0, 1, 0, 0);
            }
        }

        void StepSoftCpu(Soft& s, float dt, std::vector<glm::vec3>& impulses) {
            Xpbd::StepParams params;
            params.dt = dt;
            params.substeps = s.params.substeps;
            params.fields = g->placedFields.data();
            params.fieldCount = g->placedFields.size();
            params.colliders = g->cpuColliders.data();
            params.colliderCount = g->cpuColliders.size();
            params.colliderImpulses = s.params.pushRigidBodies && !impulses.empty() ? impulses.data() : nullptr;
            Xpbd::Step(s.model, params);
        }

        void UploadSoftCpu(Soft& s) {
            if (!s.x) return;
            Render::GL::BufferSubData(s.x, 0, s.model.x.size() * sizeof(glm::vec4), s.model.x.data());
            if (s.vao) {
                SurfaceNormals(s);
                Render::GL::BufferSubData(s.normals, 0, s.cpuNormals.size() * sizeof(glm::vec4), s.cpuNormals.data());
            }
        }

        // ------------------------------------------------------------------ scene gathering
        void GatherFields(Scene& scene) {
            std::vector<FieldGpu> fields;
            g->particleNBody = false;
            g->placedFields.clear();
            g->fieldMasks.clear();
            scene.Each<ForceField>([&](Entity e, ForceField& f) {
                const uint32_t mask = (f.affectsRigidBodies ? 1u : 0u) | (f.affectsParticles ? 2u : 0u) | (f.affectsSoftBodies ? 4u : 0u);
                if (f.type == FieldType::NBody) {
                    if (f.affectsParticles && !g->particleNBody) { g->particleNBody = true; g->nbodyG = f.strength; g->nbodySoftening = f.softening; }
                    return;
                }
                const glm::vec3 axis = glm::normalize(e.WorldRotation() * f.axis);
                fields.push_back({ glm::vec4(e.WorldPosition(), float(uint32_t(f.type))), glm::vec4(f.vector, f.strength),
                                   glm::vec4(axis, f.radius), glm::vec4(f.falloff, f.influenceRadius, f.softening, float(mask)) });
                g->placedFields.push_back({ f, e.WorldPosition(), axis });
                g->fieldMasks.push_back(mask);
            });
            g->fieldCount = uint32_t(fields.size());
            if (g->cpu) return;
            if (fields.empty()) fields.push_back({});
            Upload(g->fields, g->fieldsCap, fields.data(), fields.size() * sizeof(FieldGpu));
        }

        void GatherColliders(Scene& scene) {
            std::vector<ColliderGpu> list;
            g->cpuColliders.clear();
            g->colliderInfo.clear();
            scene.Each<Collider>([&](Entity e, Collider& c) {
                if (list.size() >= MAX_COLLIDERS || c.shape.isTrigger) return;
                const glm::vec3 scale = glm::abs(e.WorldScale());
                const glm::quat rot = e.WorldRotation() * c.shape.localRotation;
                const glm::vec3 center = glm::vec3(e.WorldMatrix() * glm::vec4(c.shape.localPosition, 1.0f));
                ColliderInfo info;
                const Rigidbody* rb = e.TryGet<Rigidbody>();
                info.body = e.GetBody();
                info.dynamic = rb && rb->type == BodyType::Dynamic && info.body.Valid();
                const float dynamicFlag = info.dynamic ? 1.0f : 0.0f;
                Xpbd::Collider cpu;
                cpu.center = center;
                if (c.shape.shape == ShapeType::Box) {
                    list.push_back({ glm::vec4(center, 2.0f), glm::vec4(0.0f), glm::vec4(c.shape.halfExtents * scale, dynamicFlag), glm::vec4(rot.x, rot.y, rot.z, rot.w) });
                    cpu.type = Xpbd::Box;
                    cpu.halfExtents = c.shape.halfExtents * scale;
                    cpu.rotation = rot;
                }
                else if (c.shape.shape == ShapeType::Sphere) {
                    const float radius = c.shape.radius * std::max({ scale.x, scale.y, scale.z });
                    list.push_back({ glm::vec4(center, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, radius), glm::vec4(0.0f, 0.0f, 0.0f, dynamicFlag), glm::vec4(0, 0, 0, 1) });
                    cpu.type = Xpbd::Sphere;
                    cpu.radius = radius;
                }
                else return;
                g->cpuColliders.push_back(cpu);
                g->colliderInfo.push_back(info);
            });
            g->colliderCount = uint32_t(list.size());
            if (g->cpu) return;
            if (list.empty()) list.push_back({});
            Upload(g->colliders, g->collidersCap, list.data(), list.size() * sizeof(ColliderGpu));
        }

        template<class T, class Map, class Make>
        void Sync(Scene& scene, Map& map, Make make) {
            std::vector<uint32_t> seen;
            scene.Each<T>([&](Entity e, T& component) {
                seen.push_back(e.Id());
                if (!map.count(e.Id())) map[e.Id()] = make(e, component);
            });
            for (auto it = map.begin(); it != map.end();) {
                if (std::find(seen.begin(), seen.end(), it->first) == seen.end()) { it->second->Release(); it = map.erase(it); }
                else ++it;
            }
        }

        // GPU coupling: read the finished slots (never stalls except for the slot about to be reused).
        void CollectGpuImpulses(int reuseSlot) {
            for (int age = IMPULSE_SLOTS - 1; age >= 0; --age) {
                const int slot = int((g->steps + IMPULSE_SLOTS - age) % IMPULSE_SLOTS);
                GLsync& fence = g->impulseFences[slot];
                if (!fence) continue;
                GLenum status = glClientWaitSync(fence, 0, 0);
                if (slot == reuseSlot && status == GL_TIMEOUT_EXPIRED) status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
                if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) continue;
                glDeleteSync(fence);
                fence = nullptr;
                const std::vector<ColliderInfo>& infos = g->slotInfo[slot];
                std::vector<int32_t> raw(infos.size() * 8, 0);
                if (!raw.empty()) glGetNamedBufferSubData(g->impulseBuffers[slot], 0, GLsizeiptr(raw.size() * 4), raw.data());
                std::vector<glm::vec3> impulses(infos.size() * 2);
                for (size_t k = 0; k < infos.size(); ++k) {
                    impulses[k * 2] = glm::vec3(float(raw[k * 8]), float(raw[k * 8 + 1]), float(raw[k * 8 + 2])) / IMPULSE_SCALE;
                    impulses[k * 2 + 1] = glm::vec3(float(raw[k * 8 + 4]), float(raw[k * 8 + 5]), float(raw[k * 8 + 6])) / IMPULSE_SCALE;
                }
                ApplyImpulses(impulses.data(), infos, g->slotDt[slot]);
            }
        }
    }

    // ====================================================================== engine-private API
    bool Init() {
        if (g) return true;
        g = std::make_unique<State>();
        g->render = Render::Internal::IsInitialized();
        // PHISIKS_CPU_COMPUTE=1 forces the CPU backend on the 4.6 tier too (comparisons, debugging).
        const char* force = std::getenv("PHISIKS_CPU_COMPUTE");
        g->cpu = !g->render || Render::GL::Compat() || (force && *force == '1');
        if (g->cpu) {
            Log::Info() << "Advanced physics: CPU backend (" << Compute::ThreadPool::Get().Workers() + 1 << " threads)"
                        << (g->render ? ", drawing through the 4.1 renderer" : ", headless");
            return true;
        }
        const FieldGpu noField{};
        const ColliderGpu noCollider{};
        Upload(g->fields, g->fieldsCap, &noField, sizeof(noField));
        Upload(g->colliders, g->collidersCap, &noCollider, sizeof(noCollider));
        for (GLuint& b : g->impulseBuffers) b = MakeBuffer(MAX_COLLIDERS * 8 * 4);
        GLint blocks = 0, invocations = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
        Log::Info() << "Advanced physics: GPU compute ready (" << blocks << " storage blocks, " << invocations << " invocations per work group)";
        return true;
    }

    void Clear() {
        if (!g) return;
        for (auto& [id, p] : g->particles) p->Release();
        for (auto& [id, s] : g->soft) s->Release();
        g->particles.clear();
        g->soft.clear();
        g->scene = nullptr;
        if (!g->cpu) for (GLsync& f : g->impulseFences) { if (f) glDeleteSync(f); f = nullptr; }
    }

    void Shutdown() {
        if (!g) return;
        Clear();
        if (g->render) {
            FreeBuffer(g->fields);
            FreeBuffer(g->colliders);
            FreeBuffer(g->samples);
            for (GLuint& b : g->impulseBuffers) FreeBuffer(b);
        }
        g.reset();
    }

    bool Available() { return g != nullptr; }

    void Step(Scene& scene, float dt) {
        if (!g) return;
        if (g->scene != &scene) Clear();
        g->scene = &scene;
        GatherFields(scene);
        GatherColliders(scene);
        Sync<ParticleSystem>(scene, g->particles, [](Entity e, ParticleSystem& ps) { return CreateParticles(ps, e.Id(), e.WorldPosition()); });
        Sync<SoftBody>(scene, g->soft, [](Entity e, SoftBody& sb) {
            auto s = std::make_unique<Soft>();
            s->params = sb;
            s->entity = e.Id();
            s->model = BuildModel(sb, e.WorldPosition(), e.WorldRotation());
            UploadSoft(*s);
            return s;
        });
        if (g->particles.empty() && g->soft.empty()) return;
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t dispatchesBefore = g_dispatches;
        std::vector<std::pair<uint32_t, std::array<glm::vec4, 2>>> writes;
        if (g->cpu) {
            std::vector<glm::vec3> impulses(g->cpuColliders.size() * 2, glm::vec3(0.0f));
            for (auto& [id, p] : g->particles) {
                writes.clear();
                Emit(*p, scene, dt, writes);
                StepParticlesCpu(*p, dt, writes, impulses);
            }
            // soft bodies in parallel (independent); impulses merged in entity order
            std::vector<Soft*> bodies;
            for (auto& [id, s] : g->soft) bodies.push_back(s.get());
            std::vector<std::vector<glm::vec3>> bodyImpulses(bodies.size(), std::vector<glm::vec3>(impulses.size(), glm::vec3(0.0f)));
            Compute::ThreadPool::Get().ParallelFor(bodies.size(), 1, [&](size_t, size_t begin, size_t end) {
                for (size_t b = begin; b < end; ++b) StepSoftCpu(*bodies[b], dt, bodyImpulses[b]);
            });
            for (size_t b = 0; b < bodies.size(); ++b) {
                for (size_t k = 0; k < impulses.size(); ++k) impulses[k] += bodyImpulses[b][k];
                UploadSoftCpu(*bodies[b]);
            }
            ApplyImpulses(impulses.data(), g->colliderInfo, dt);
        }
        else {
            const int slot = int(g->steps % IMPULSE_SLOTS);
            CollectGpuImpulses(slot);
            const uint32_t zero = 0;
            glClearNamedBufferData(g->impulseBuffers[slot], GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            Bind(12, g->impulseBuffers[slot]);
            BindShared();
            for (auto& [id, p] : g->particles) {
                writes.clear();
                Emit(*p, scene, dt, writes);
                StepParticlesGpu(*p, dt, writes);
            }
            BindShared();
            Bind(12, g->impulseBuffers[slot]);
            for (auto& [id, s] : g->soft) StepSoft(*s, dt, s->params.substeps, glm::vec3(0.0f, -9.81f, 0.0f), g->fieldCount, g->colliderCount, true, s->params.pushRigidBodies);
            g->slotInfo[slot] = g->colliderInfo;
            g->slotDt[slot] = dt;
            g->impulseFences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        ++g->steps;
        g->lastDispatches = uint32_t(g_dispatches - dispatchesBefore);
        g->lastCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void CollectDraws(std::vector<SoftBodyDraw>& softBodies, std::vector<SpriteDraw>& sprites) {
        if (!g || !g->render) return;
        for (auto& [id, s] : g->soft) {
            if (!s->x) continue;
            if (s->vao) softBodies.push_back({ s->x, s->normals, s->vao, uint32_t(s->model.triangles.size()), id, s->params.material });
            else sprites.push_back({ s->x, s->count, 1, s->model.particleRadius, glm::vec3(0.85f, 0.75f, 0.55f), glm::vec3(0.85f, 0.75f, 0.55f), 1.0f, id });
        }
        for (auto& [id, p] : g->particles) {
            const GLuint buffer = g->cpu ? p->renderBuffer : p->state[p->current];
            if (buffer) sprites.push_back({ buffer, p->count, 2, 0.0f, p->params.color, p->params.fastColor, p->params.fastSpeed, id });
        }
    }

    namespace {
        void SampleCpu(const glm::vec3& lo, const glm::vec3& hi, const glm::ivec3& res, std::vector<glm::vec4>& samples) {
            const glm::ivec3 r = glm::max(res, glm::ivec3(1));
            samples.resize(size_t(r.x) * r.y * r.z);
            for (size_t i = 0; i < samples.size(); ++i) {
                const glm::ivec3 c(int(i) % r.x, (int(i) / r.x) % r.y, int(i) / (r.x * r.y));
                const glm::vec3 p = glm::mix(lo, hi, (glm::vec3(c) + 0.5f) / glm::vec3(r));
                const glm::vec3 a = FieldAccelerationCpu(p, 7u);
                samples[i] = glm::vec4(a, glm::length(a));
            }
        }
    }

    uint32_t SampleFieldGrid(const glm::vec3& lo, const glm::vec3& hi, const glm::ivec3& res) {
        if (!g || !g->render) return 0;
        const uint32_t n = uint32_t(std::max(res.x, 1) * std::max(res.y, 1) * std::max(res.z, 1));
        if (!g->samples || n * 16ull > g->samplesCap) {
            FreeBuffer(g->samples);
            g->samplesCap = n * 16ull;
            g->samples = MakeBuffer(g->samplesCap);
        }
        if (g->cpu) {
            std::vector<glm::vec4> samples;
            SampleCpu(lo, hi, res, samples);
            Render::GL::BufferSubData(g->samples, 0, samples.size() * sizeof(glm::vec4), samples.data());
            return g->samples;
        }
        const GLuint p = Use(Shaders::FIELD_SAMPLE);
        glProgramUniform3i(p, glGetUniformLocation(p, "u_resolution"), res.x, res.y, res.z);
        U3f(p, "u_boundsMin", lo); U3f(p, "u_boundsMax", hi); U1i(p, "u_fieldCount", int(g->fieldCount));
        Bind(10, g->fields); Bind(14, g->samples);
        CountedDispatch(Groups(n, 64), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        return g->samples;
    }

    uint32_t FieldCount() { return g ? g->fieldCount : 0; }
}

// ====================================================================== public API
namespace Phisiks::Physics::Advanced {
    namespace W = Phisiks::Advanced;

    Stats GetStats() {
        Stats s;
        if (!W::g) return s;
        s.gpuAvailable = !W::g->cpu;
        s.cpuBackend = W::g->cpu;
        s.particleSystems = uint32_t(W::g->particles.size());
        for (auto& [id, p] : W::g->particles) {
            s.particles += p->count;
            s.particlesAlive += p->alive;
            s.particlesEmitted += p->emitted;
        }
        s.softBodies = uint32_t(W::g->soft.size());
        for (auto& [id, b] : W::g->soft) {
            s.softBodyNodes += b->count;
            s.constraints += uint32_t(b->model.constraints.size());
            s.colorsMax = std::max(s.colorsMax, uint32_t(b->model.colorStart.empty() ? 0 : b->model.colorStart.size() - 1));
        }
        s.fields = W::g->fieldCount;
        s.colliders = W::g->colliderCount;
        s.steps = W::g->steps;
        s.dispatchesLastStep = W::g->lastDispatches;
        s.cpuMsLastStep = W::g->lastCpuMs;
        s.coupledBodies = W::g->coupledBodies;
        s.coupledImpulse = W::g->coupledImpulse;
        s.coupledImpulsesApplied = W::g->coupledApplied;
        return s;
    }

    bool ReadSoftBody(Entity entity, std::vector<glm::vec4>& positions) {
        if (!W::g) return false;
        auto it = W::g->soft.find(entity.Id());
        if (it == W::g->soft.end()) return false;
        if (W::g->cpu) { positions = it->second->model.x; return true; }
        positions.resize(it->second->count);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(it->second->x, 0, GLsizeiptr(positions.size() * sizeof(glm::vec4)), positions.data());
        return true;
    }

    bool ReadParticles(Entity entity, std::vector<glm::vec4>& positionRadius) {
        if (!W::g) return false;
        auto it = W::g->particles.find(entity.Id());
        if (it == W::g->particles.end()) return false;
        const W::Particles& p = *it->second;
        std::vector<glm::vec4> raw;
        if (W::g->cpu) raw = p.cpu[p.current];
        else {
            raw.resize(p.count * 2);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glGetNamedBufferSubData(p.state[p.current], 0, GLsizeiptr(raw.size() * sizeof(glm::vec4)), raw.data());
        }
        positionRadius.resize(p.count);
        for (uint32_t i = 0; i < p.count; ++i) positionRadius[i] = raw[i * 2];
        return true;
    }

    bool GpuNeighbourCounts(const std::vector<glm::vec3>& positions, float cellSize, uint32_t tableSize, float radius, std::vector<uint32_t>& counts) {
        if (!W::g || W::g->cpu || positions.empty()) return false;
        const uint32_t n = uint32_t(positions.size());
        std::vector<glm::vec4> data(n);
        for (uint32_t i = 0; i < n; ++i) data[i] = glm::vec4(positions[i], 0.0f);
        GLuint elements = W::MakeBuffer(n * sizeof(glm::vec4), data.data());
        GLuint out = W::MakeBuffer(n * 4);
        W::HashGrid grid;
        grid.Ensure(tableSize, n);
        grid.Build(elements, n, 1, cellSize, false);
        const GLuint p = W::Use(Shaders::HASH_QUERY);
        W::U1u(p, "u_count", n); W::U1u(p, "u_stride", 1); W::U1f(p, "u_invCellSize", 1.0f / cellSize);
        W::U1u(p, "u_tableSize", tableSize); W::U1f(p, "u_queryRadius", radius);
        W::Bind(4, elements); W::Bind(6, grid.cellStart); W::Bind(7, grid.sorted); W::Bind(8, out);
        CountedDispatch(W::Groups(n, 256), 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        counts.resize(n);
        glGetNamedBufferSubData(out, 0, GLsizeiptr(n * 4), counts.data());
        grid.Release();
        W::FreeBuffer(elements);
        W::FreeBuffer(out);
        return true;
    }

    bool GpuStepModel(Xpbd::Model& model, float dt, int substeps, int steps, bool ground, bool singleWorkgroup) {
        if (!W::g || W::g->cpu) return false;
        W::Soft s;
        s.model = model;
        W::UploadSoft(s);
        W::ColliderGpu plane{ glm::vec4(0, 0, 0, 0), glm::vec4(0, 1, 0, 0), glm::vec4(0), glm::vec4(0, 0, 0, 1) };
        GLuint colliders = W::MakeBuffer(sizeof(plane), &plane);
        W::FieldGpu noField{};
        GLuint fields = W::MakeBuffer(sizeof(noField), &noField);
        W::Bind(10, fields);
        W::Bind(11, colliders);
        for (int i = 0; i < steps; ++i) W::StepSoft(s, dt, substeps, glm::vec3(0.0f, -9.81f, 0.0f), 0, ground ? 1u : 0u, singleWorkgroup, false);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(s.x, 0, GLsizeiptr(model.x.size() * sizeof(glm::vec4)), model.x.data());
        std::vector<glm::vec4> v(model.x.size());
        glGetNamedBufferSubData(s.v, 0, GLsizeiptr(v.size() * sizeof(glm::vec4)), v.data());
        for (size_t i = 0; i < v.size(); ++i) model.v[i] = glm::vec3(v[i]);
        s.Release();
        W::FreeBuffer(colliders);
        W::FreeBuffer(fields);
        return true;
    }

    bool GpuNBody(const std::vector<glm::vec3>& positions, const std::vector<float>& masses, float G, float softening, std::vector<glm::vec3>& accelerations) {
        if (!W::g || W::g->cpu || positions.empty() || masses.size() != positions.size()) return false;
        const uint32_t n = uint32_t(positions.size());
        std::vector<glm::vec4> data(n * 2);
        for (uint32_t i = 0; i < n; ++i) { data[i * 2] = glm::vec4(positions[i], 0.0f); data[i * 2 + 1] = glm::vec4(0.0f, 0.0f, 0.0f, masses[i]); }
        GLuint elements = W::MakeBuffer(data.size() * sizeof(glm::vec4), data.data());
        GLuint accel = W::MakeBuffer(n * sizeof(glm::vec4));
        const GLuint p = W::Use(Shaders::NBODY);
        W::U1u(p, "u_count", n); W::U1f(p, "u_G", G); W::U1f(p, "u_softening2", softening * softening);
        W::Bind(4, elements); W::Bind(8, accel);
        CountedDispatch(W::Groups(n, 256), 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        std::vector<glm::vec4> out(n);
        glGetNamedBufferSubData(accel, 0, GLsizeiptr(n * sizeof(glm::vec4)), out.data());
        accelerations.resize(n);
        for (uint32_t i = 0; i < n; ++i) accelerations[i] = glm::vec3(out[i]);
        W::FreeBuffer(elements);
        W::FreeBuffer(accel);
        return true;
    }

    bool SampleFields(const glm::vec3& lo, const glm::vec3& hi, const glm::ivec3& res, std::vector<glm::vec4>& samples) {
        if (!W::g) return false;
        if (W::g->cpu) { W::SampleCpu(lo, hi, res, samples); return true; }
        const uint32_t buffer = W::SampleFieldGrid(lo, hi, res);
        if (!buffer) return false;
        samples.resize(size_t(res.x) * res.y * res.z);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(buffer, 0, GLsizeiptr(samples.size() * sizeof(glm::vec4)), samples.data());
        return true;
    }
}
