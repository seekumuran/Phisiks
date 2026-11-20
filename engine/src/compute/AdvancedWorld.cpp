// GPU particle systems, XPBD soft bodies, force-field and collider buffers (OpenGL 4.6 compute).
#include "compute/AdvancedWorld.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Scene/Scene.h"
#include "render/RendererInternal.h"
#include "render/ShaderPrograms.h"

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <chrono>
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

        GLuint MakeBuffer(size_t bytes, const void* data = nullptr) {
            GLuint b = 0;
            glCreateBuffers(1, &b);
            glNamedBufferStorage(b, GLsizeiptr(std::max<size_t>(bytes, 16)), data, GL_DYNAMIC_STORAGE_BIT);
            return b;
        }
        void FreeBuffer(GLuint& b) { if (b) glDeleteBuffers(1, &b); b = 0; }
        void Upload(GLuint& b, size_t& capacity, const void* data, size_t bytes) {
            if (!b || bytes > capacity) {
                FreeBuffer(b);
                capacity = std::max<size_t>(bytes + bytes / 2, 256);
                b = MakeBuffer(capacity);
            }
            if (bytes) glNamedBufferSubData(b, 0, GLsizeiptr(bytes), data);
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

        // ------------------------------------------------------------------ spatial hash
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
            void Build(GLuint elements, uint32_t count, uint32_t stride, float cellSize) {
                const uint32_t zero = 0;
                glClearNamedBufferData(counters, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                GLuint p = Use(Shaders::HASH_COUNT);
                U1u(p, "u_count", count); U1u(p, "u_stride", stride); U1f(p, "u_invCellSize", 1.0f / cellSize); U1u(p, "u_tableSize", tableSize);
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
                Bind(4, elements); Bind(5, counters); Bind(6, cellStart); Bind(7, sorted);
                CountedDispatch(Groups(count, 256), 1, 1);
                Barrier();
            }
        };

        uint32_t TableSizeFor(uint32_t count) { return std::clamp(NextPow2(count * 2), 1024u, 512u * 512u - 1u); }

        // ------------------------------------------------------------------ systems
        struct Particles {
            ParticleSystem params;
            GLuint state[2] = {};
            int current = 0;
            uint32_t count = 0;
            float cellSize = 0.1f;
            HashGrid hash;
            GLuint accel = 0;
            void Release() { FreeBuffer(state[0]); FreeBuffer(state[1]); FreeBuffer(accel); hash.Release(); }
        };

        struct Soft {
            SoftBody params;
            Xpbd::Model model;   // CPU copy: colour ranges, triangles, radius (positions live on the GPU)
            GLuint x = 0, v = 0, prev = 0, constraints = 0, colorStarts = 0, normals = 0, adjStart = 0, adjTris = 0, tris = 0, vao = 0;
            uint32_t count = 0;
            void Release() {
                for (GLuint* b : { &x, &v, &prev, &constraints, &colorStarts, &normals, &adjStart, &adjTris, &tris }) FreeBuffer(*b);
                if (vao) glDeleteVertexArrays(1, &vao);
                vao = 0;
            }
        };

        struct State {
            const Scene* scene = nullptr;
            std::unordered_map<uint32_t, std::unique_ptr<Particles>> particles;
            std::unordered_map<uint32_t, std::unique_ptr<Soft>> soft;
            GLuint fields = 0, colliders = 0, samples = 0;
            uint32_t lastDispatches = 0;
            float lastCpuMs = 0.0f;
            size_t fieldsCap = 0, collidersCap = 0, samplesCap = 0;
            uint32_t fieldCount = 0, colliderCount = 0;
            bool particleNBody = false;
            float nbodyG = 0.0f, nbodySoftening = 0.05f;
            uint64_t steps = 0;
        };
        std::unique_ptr<State> g;

        void BindShared() { Bind(10, g->fields); Bind(11, g->colliders); }

        // ------------------------------------------------------------------ particles
        std::unique_ptr<Particles> CreateParticles(const ParticleSystem& ps, const glm::vec3& origin) {
            auto p = std::make_unique<Particles>();
            p->params = ps;
            p->count = std::max(ps.count, 1u);
            p->cellSize = 2.0f * ps.radius;
            const int n = int(std::ceil(std::cbrt(double(p->count))));
            std::vector<glm::vec4> data(p->count * 2);
            uint32_t seed = ps.seed * 747796405u + 2891336453u;
            auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
            for (uint32_t i = 0; i < p->count; ++i) {
                const glm::vec3 cell(float(i % n), float((i / n) % n), float(i / (n * n)));
                const glm::vec3 jitter(rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f);
                const glm::vec3 t = (cell + 0.5f + jitter * 0.3f) / float(n);
                data[i * 2] = glm::vec4(origin + (t - 0.5f) * ps.spawnSize, ps.radius);
                data[i * 2 + 1] = glm::vec4(ps.initialVelocity, ps.mass);
            }
            const size_t bytes = data.size() * sizeof(glm::vec4);
            p->state[0] = MakeBuffer(bytes, data.data());
            p->state[1] = MakeBuffer(bytes, data.data());
            p->accel = MakeBuffer(p->count * sizeof(glm::vec4));
            p->hash.Ensure(TableSizeFor(p->count), p->count);
            return p;
        }

        void StepParticles(Particles& p, float dt) {
            const GLuint in = p.state[p.current], out = p.state[1 - p.current];
            p.hash.Build(in, p.count, 2, p.cellSize);
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
            Bind(4, in); Bind(6, p.hash.cellStart); Bind(7, p.hash.sorted); Bind(8, p.accel); Bind(13, out);
            BindShared();
            CountedDispatch(Groups(p.count, 256), 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
            p.current = 1 - p.current;
        }

        // ------------------------------------------------------------------ soft bodies
        void UploadSoft(Soft& s) {
            const Xpbd::Model& m = s.model;
            s.count = uint32_t(m.x.size());
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
                glCreateVertexArrays(1, &s.vao);
                glVertexArrayElementBuffer(s.vao, s.tris);
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

        void StepSoftPasses(Soft& s, float h, int n, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount);

        void StepSoft(Soft& s, float dt, int substeps, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount, bool allowFused = true) {
            const int n = std::max(substeps, 1);
            const float h = dt / n;
            if (allowFused && UseFused(s)) {
                const GLuint fused = Use(Shaders::XPBD_FUSED);
                U1u(fused, "u_count", s.count); U1f(fused, "u_h", h); U3f(fused, "u_gravity", gravity); U1i(fused, "u_fieldCount", int(fieldCount));
                U1f(fused, "u_radius", s.model.particleRadius); U1f(fused, "u_friction", s.model.friction); U1f(fused, "u_damping", s.model.damping);
                U1i(fused, "u_colliderCount", int(colliderCount)); U1i(fused, "u_substeps", n);
                U1u(fused, "u_colorCount", uint32_t(s.model.colorStart.empty() ? 0 : s.model.colorStart.size() - 1));
                Bind(4, s.x); Bind(5, s.v); Bind(6, s.prev); Bind(7, s.constraints); Bind(8, s.colorStarts);
                CountedDispatch(1, 1, 1);
                Barrier();
            }
            else {
                StepSoftPasses(s, h, n, gravity, fieldCount, colliderCount);
            }
            if (s.vao) {
                const GLuint p = Use(Shaders::XPBD_NORMALS);
                U1u(p, "u_count", s.count);
                Bind(4, s.x); Bind(8, s.normals); Bind(13, s.adjStart); Bind(14, s.adjTris); Bind(15, s.tris);
                CountedDispatch(Groups(s.count, 128), 1, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
        }

        void StepSoftPasses(Soft& s, float h, int n, const glm::vec3& gravity, uint32_t fieldCount, uint32_t colliderCount) {
            const GLuint predict = Render::Internal::ProgramHandle(Shaders::XPBD_PREDICT);
            const GLuint solve = Render::Internal::ProgramHandle(Shaders::XPBD_SOLVE);
            const GLuint finish = Render::Internal::ProgramHandle(Shaders::XPBD_FINISH);
            U1u(predict, "u_count", s.count); U1f(predict, "u_h", h); U3f(predict, "u_gravity", gravity); U1i(predict, "u_fieldCount", int(fieldCount));
            U1f(solve, "u_h", h);
            U1u(finish, "u_count", s.count); U1f(finish, "u_h", h); U1f(finish, "u_radius", s.model.particleRadius);
            U1f(finish, "u_friction", s.model.friction); U1f(finish, "u_damping", s.model.damping); U1i(finish, "u_colliderCount", int(colliderCount));
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

        // ------------------------------------------------------------------ scene gathering
        void GatherFields(Scene& scene) {
            std::vector<FieldGpu> fields;
            g->particleNBody = false;
            scene.Each<ForceField>([&](Entity e, ForceField& f) {
                const uint32_t mask = (f.affectsRigidBodies ? 1u : 0u) | (f.affectsParticles ? 2u : 0u) | (f.affectsSoftBodies ? 4u : 0u);
                if (f.type == FieldType::NBody) {
                    if (f.affectsParticles && !g->particleNBody) { g->particleNBody = true; g->nbodyG = f.strength; g->nbodySoftening = f.softening; }
                    return;
                }
                const glm::vec3 axis = glm::normalize(e.WorldRotation() * f.axis);
                fields.push_back({ glm::vec4(e.WorldPosition(), float(uint32_t(f.type))), glm::vec4(f.vector, f.strength),
                                   glm::vec4(axis, f.radius), glm::vec4(f.falloff, f.influenceRadius, f.softening, float(mask)) });
            });
            g->fieldCount = uint32_t(fields.size());
            if (fields.empty()) fields.push_back({});
            Upload(g->fields, g->fieldsCap, fields.data(), fields.size() * sizeof(FieldGpu));
        }

        void GatherColliders(Scene& scene) {
            std::vector<ColliderGpu> list;
            scene.Each<Collider>([&](Entity e, Collider& c) {
                if (list.size() >= 128 || c.shape.isTrigger) return;
                const glm::vec3 scale = glm::abs(e.WorldScale());
                const glm::quat rot = e.WorldRotation() * c.shape.localRotation;
                const glm::vec3 center = glm::vec3(e.WorldMatrix() * glm::vec4(c.shape.localPosition, 1.0f));
                if (c.shape.shape == ShapeType::Box) {
                    list.push_back({ glm::vec4(center, 2.0f), glm::vec4(0.0f), glm::vec4(c.shape.halfExtents * scale, 0.0f), glm::vec4(rot.x, rot.y, rot.z, rot.w) });
                }
                else if (c.shape.shape == ShapeType::Sphere) {
                    list.push_back({ glm::vec4(center, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, c.shape.radius * std::max({ scale.x, scale.y, scale.z })),
                                     glm::vec4(0.0f), glm::vec4(0, 0, 0, 1) });
                }
            });
            g->colliderCount = uint32_t(list.size());
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
    }

    // ====================================================================== engine-private API
    bool Init() {
        if (g) return true;
        if (!Render::Internal::IsInitialized()) return false;
        g = std::make_unique<State>();
        const FieldGpu noField{};
        const ColliderGpu noCollider{};
        Upload(g->fields, g->fieldsCap, &noField, sizeof(noField));
        Upload(g->colliders, g->collidersCap, &noCollider, sizeof(noCollider));
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
    }

    void Shutdown() {
        if (!g) return;
        Clear();
        FreeBuffer(g->fields);
        FreeBuffer(g->colliders);
        FreeBuffer(g->samples);
        g.reset();
    }

    bool Available() { return g != nullptr; }

    void Step(Scene& scene, float dt) {
        if (!g) return;
        if (g->scene != &scene) Clear();
        g->scene = &scene;
        GatherFields(scene);
        GatherColliders(scene);
        Sync<ParticleSystem>(scene, g->particles, [](Entity e, ParticleSystem& ps) { return CreateParticles(ps, e.WorldPosition()); });
        Sync<SoftBody>(scene, g->soft, [](Entity e, SoftBody& sb) {
            auto s = std::make_unique<Soft>();
            s->params = sb;
            s->model = BuildModel(sb, e.WorldPosition(), e.WorldRotation());
            UploadSoft(*s);
            return s;
        });
        if (g->particles.empty() && g->soft.empty()) return;
        const uint64_t dispatchesBefore = g_dispatches;
        const auto t0 = std::chrono::steady_clock::now();
        BindShared();
        for (auto& [id, p] : g->particles) StepParticles(*p, dt);
        BindShared();
        for (auto& [id, s] : g->soft) StepSoft(*s, dt, s->params.substeps, glm::vec3(0.0f, -9.81f, 0.0f), g->fieldCount, g->colliderCount);
        ++g->steps;
        g->lastDispatches = uint32_t(g_dispatches - dispatchesBefore);
        g->lastCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void CollectDraws(std::vector<SoftBodyDraw>& softBodies, std::vector<SpriteDraw>& sprites) {
        if (!g) return;
        for (auto& [id, s] : g->soft) {
            if (s->vao) softBodies.push_back({ s->x, s->normals, s->vao, uint32_t(s->model.triangles.size()), id, s->params.material });
            else sprites.push_back({ s->x, s->count, 1, s->model.particleRadius, glm::vec3(0.85f, 0.75f, 0.55f), glm::vec3(0.85f, 0.75f, 0.55f), 1.0f, id });
        }
        for (auto& [id, p] : g->particles) {
            sprites.push_back({ p->state[p->current], p->count, 2, 0.0f, p->params.color, p->params.fastColor, p->params.fastSpeed, id });
        }
    }

    uint32_t SampleFieldGrid(const glm::vec3& lo, const glm::vec3& hi, const glm::ivec3& res) {
        if (!g) return 0;
        const uint32_t n = uint32_t(std::max(res.x, 1) * std::max(res.y, 1) * std::max(res.z, 1));
        if (!g->samples || n * 16ull > g->samplesCap) {
            FreeBuffer(g->samples);
            g->samplesCap = n * 16ull;
            g->samples = MakeBuffer(g->samplesCap);
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
        s.gpuAvailable = true;
        s.particleSystems = uint32_t(W::g->particles.size());
        for (auto& [id, p] : W::g->particles) s.particles += p->count;
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
        return s;
    }

    bool ReadSoftBody(Entity entity, std::vector<glm::vec4>& positions) {
        if (!W::g) return false;
        auto it = W::g->soft.find(entity.Id());
        if (it == W::g->soft.end()) return false;
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
        std::vector<glm::vec4> raw(p.count * 2);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(p.state[p.current], 0, GLsizeiptr(raw.size() * sizeof(glm::vec4)), raw.data());
        positionRadius.resize(p.count);
        for (uint32_t i = 0; i < p.count; ++i) positionRadius[i] = raw[i * 2];
        return true;
    }

    bool GpuNeighbourCounts(const std::vector<glm::vec3>& positions, float cellSize, uint32_t tableSize, float radius, std::vector<uint32_t>& counts) {
        if (!W::g || positions.empty()) return false;
        const uint32_t n = uint32_t(positions.size());
        std::vector<glm::vec4> data(n);
        for (uint32_t i = 0; i < n; ++i) data[i] = glm::vec4(positions[i], 0.0f);
        GLuint elements = W::MakeBuffer(n * sizeof(glm::vec4), data.data());
        GLuint out = W::MakeBuffer(n * 4);
        W::HashGrid grid;
        grid.Ensure(tableSize, n);
        grid.Build(elements, n, 1, cellSize);
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
        if (!W::g) return false;
        W::Soft s;
        s.model = model;
        W::UploadSoft(s);
        W::ColliderGpu plane{ glm::vec4(0, 0, 0, 0), glm::vec4(0, 1, 0, 0), glm::vec4(0), glm::vec4(0, 0, 0, 1) };
        GLuint colliders = W::MakeBuffer(sizeof(plane), &plane);
        W::FieldGpu noField{};
        GLuint fields = W::MakeBuffer(sizeof(noField), &noField);
        W::Bind(10, fields);
        W::Bind(11, colliders);
        for (int i = 0; i < steps; ++i) W::StepSoft(s, dt, substeps, glm::vec3(0.0f, -9.81f, 0.0f), 0, ground ? 1u : 0u, singleWorkgroup);
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
        if (!W::g || positions.empty() || masses.size() != positions.size()) return false;
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
        const uint32_t buffer = W::SampleFieldGrid(lo, hi, res);
        if (!buffer) return false;
        samples.resize(size_t(res.x) * res.y * res.z);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(buffer, 0, GLsizeiptr(samples.size() * sizeof(glm::vec4)), samples.data());
        return true;
    }
}
