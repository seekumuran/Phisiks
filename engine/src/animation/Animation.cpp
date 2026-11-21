// Skeletal animation (clip sampling, dual-quaternion / linear blend skinning), spline camera paths
// and the scene system that drives SkinnedModel / CameraPath components.
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace Phisiks::Animation {
    // ====================================================================== sampling
    namespace {
        size_t FindKey(const std::vector<float>& times, float t) {
            // last key with time <= t
            const auto it = std::upper_bound(times.begin(), times.end(), t);
            return it == times.begin() ? 0 : size_t(it - times.begin()) - 1;
        }

        glm::vec4 Hermite(const glm::vec4& v0, const glm::vec4& out0, const glm::vec4& in1, const glm::vec4& v1, float s, float dt) {
            const float s2 = s * s, s3 = s2 * s;
            return (2.0f * s3 - 3.0f * s2 + 1.0f) * v0 + (s3 - 2.0f * s2 + s) * dt * out0 + (-2.0f * s3 + 3.0f * s2) * v1 + (s3 - s2) * dt * in1;
        }

        glm::vec4 SampleChannel(const Channel& c, float t) {
            const size_t n = c.times.size();
            const bool cubic = c.interpolation == Interpolation::CubicSpline;
            auto value = [&](size_t k) { return cubic ? c.values[k * 3 + 1] : c.values[k]; };
            if (n == 0) return glm::vec4(0.0f);
            if (n == 1 || t <= c.times.front()) return value(0);
            if (t >= c.times.back()) return value(n - 1);
            const size_t k = FindKey(c.times, t);
            const float t0 = c.times[k], t1 = c.times[k + 1];
            const float dt = std::max(t1 - t0, 1e-9f);
            const float s = std::clamp((t - t0) / dt, 0.0f, 1.0f);
            switch (c.interpolation) {
                case Interpolation::Step: return value(k);
                case Interpolation::CubicSpline: {
                    glm::vec4 r = Hermite(c.values[k * 3 + 1], c.values[k * 3 + 2], c.values[(k + 1) * 3], c.values[(k + 1) * 3 + 1], s, dt);
                    if (c.path == TargetPath::Rotation) r = glm::normalize(r);
                    return r;
                }
                case Interpolation::Linear:
                    if (c.path == TargetPath::Rotation) {
                        const glm::vec4 a = c.values[k], b = c.values[k + 1];
                        const glm::quat q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), s);
                        return glm::vec4(q.x, q.y, q.z, q.w);
                    }
                    return glm::mix(c.values[k], c.values[k + 1], s);
            }
            return value(k);
        }
    }

    void SamplePose(const Model& model, const Clip* clip, float time, bool loop, std::vector<Transform>& nodeLocals) {
        nodeLocals.resize(model.nodes.size());
        for (size_t i = 0; i < model.nodes.size(); ++i) nodeLocals[i] = model.nodes[i].local;
        if (!clip) return;
        float t = time;
        if (clip->duration > 0.0f) t = loop ? std::fmod(std::fmod(time, clip->duration) + clip->duration, clip->duration) : std::clamp(time, 0.0f, clip->duration);
        for (const Channel& c : clip->channels) {
            if (c.node < 0 || c.node >= int(nodeLocals.size())) continue;
            const glm::vec4 v = SampleChannel(c, t);
            Transform& local = nodeLocals[c.node];
            switch (c.path) {
                case TargetPath::Translation: local.position = glm::vec3(v); break;
                case TargetPath::Rotation: local.rotation = glm::normalize(glm::quat(v.w, v.x, v.y, v.z)); break;
                case TargetPath::Scale: local.scale = glm::vec3(v); break;
            }
        }
    }

    void GlobalMatrices(const Model& model, const std::vector<Transform>& locals, std::vector<glm::mat4>& out) {
        const size_t n = model.nodes.size();
        out.assign(n, glm::mat4(1.0f));
        std::vector<uint8_t> done(n, 0);
        std::vector<int> chain;
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            chain.clear();
            for (int k = int(i); k >= 0 && !done[k]; k = model.nodes[k].parent) chain.push_back(k);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const int k = *it;
                const Transform& t = locals[k];
                const glm::mat4 local = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1.0f), t.scale);
                const int parent = model.nodes[k].parent;
                out[k] = parent >= 0 ? out[parent] * local : local;
                done[k] = 1;
            }
        }
    }

    void SkinTransforms(const Model& model, int skin, const std::vector<glm::mat4>& globals, std::vector<Math::DualQuat>& dualQuats, std::vector<glm::mat4>& matrices) {
        dualQuats.clear();
        matrices.clear();
        if (skin < 0 || skin >= int(model.skins.size())) return;
        const Skin& s = model.skins[skin];
        dualQuats.resize(s.joints.size());
        matrices.resize(s.joints.size());
        for (size_t j = 0; j < s.joints.size(); ++j) {
            const glm::mat4 m = globals[s.joints[j]] * s.inverseBind[j];
            matrices[j] = m;
            // rigid part (dual quaternions carry rotation + translation; scale is normalised out)
            glm::mat3 r(m);
            for (int c = 0; c < 3; ++c) { const float len = glm::length(r[c]); if (len > 1e-12f) r[c] /= len; }
            dualQuats[j] = Math::DualQuat::FromRotationTranslation(glm::normalize(glm::quat_cast(r)), glm::vec3(m[3]));
        }
    }

    void SkinVertices(const Primitive& p, const std::vector<Math::DualQuat>& dq, const std::vector<glm::mat4>& matrices, SkinningMethod method, std::vector<Render::Vertex>& out) {
        out = p.vertices;
        if (p.joints.size() != p.vertices.size() || dq.empty()) return;
        const uint32_t count = uint32_t(dq.size());
        for (size_t i = 0; i < p.vertices.size(); ++i) {
            const glm::uvec4 j = glm::min(p.joints[i], glm::uvec4(count - 1));
            const glm::vec4 w = p.weights[i];
            const Render::Vertex& v = p.vertices[i];
            if (method == SkinningMethod::DualQuaternion) {
                const Math::DualQuat transforms[4] = { dq[j.x], dq[j.y], dq[j.z], dq[j.w] };
                const float weights[4] = { w.x, w.y, w.z, w.w };
                const Math::DualQuat b = Math::DualQuat::Blend(transforms, weights);
                out[i].position = b.TransformPoint(v.position);
                out[i].normal = glm::normalize(b.TransformVector(v.normal));
            }
            else {
                const glm::mat4 m = matrices[j.x] * w.x + matrices[j.y] * w.y + matrices[j.z] * w.z + matrices[j.w] * w.w;
                out[i].position = glm::vec3(m * glm::vec4(v.position, 1.0f));
                const glm::vec3 n = glm::mat3(m) * v.normal;
                out[i].normal = glm::length(n) > 1e-12f ? glm::normalize(n) : v.normal;
            }
        }
    }

    // ====================================================================== splines
    glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t, float alpha) {
        // Barry-Goldman pyramidal evaluation with knot spacing |p_i+1 - p_i|^alpha.
        auto knot = [alpha](float ti, const glm::vec3& a, const glm::vec3& b) { return ti + std::max(std::pow(glm::length(b - a), alpha), 1e-4f); };
        const float t0 = 0.0f, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
        const float u = t1 + (t2 - t1) * t;
        const glm::vec3 a1 = (t1 - u) / (t1 - t0) * p0 + (u - t0) / (t1 - t0) * p1;
        const glm::vec3 a2 = (t2 - u) / (t2 - t1) * p1 + (u - t1) / (t2 - t1) * p2;
        const glm::vec3 a3 = (t3 - u) / (t3 - t2) * p2 + (u - t2) / (t3 - t2) * p3;
        const glm::vec3 b1 = (t2 - u) / (t2 - t0) * a1 + (u - t0) / (t2 - t0) * a2;
        const glm::vec3 b2 = (t3 - u) / (t3 - t1) * a2 + (u - t1) / (t3 - t1) * a3;
        return (t2 - u) / (t2 - t1) * b1 + (u - t1) / (t2 - t1) * b2;
    }

    glm::vec3 CubicBezier(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
        const float s = 1.0f - t;
        return s * s * s * p0 + 3.0f * s * s * t * p1 + 3.0f * s * t * t * p2 + t * t * t * p3;
    }

    bool Spline::Build(SplineType type, const std::vector<glm::vec3>& points, bool closed, int samplesPerSegment) {
        m_type = type;
        m_points = points;
        m_closed = closed && type == SplineType::CatmullRom;
        m_segments = 0;
        m_arc.clear();
        m_length = 0.0f;
        if (type == SplineType::CatmullRom) {
            if (points.size() < 2) return false;
            m_segments = int(m_closed ? points.size() : points.size() - 1);
        }
        else {
            if (points.size() < 4 || (points.size() - 1) % 3 != 0) return false;
            m_segments = int((points.size() - 1) / 3);
        }
        const int samples = std::max(samplesPerSegment, 4) * m_segments;
        m_arc.resize(size_t(samples) + 1, 0.0f);
        glm::vec3 previous = Raw(0.0f);
        for (int i = 1; i <= samples; ++i) {
            const glm::vec3 p = Raw(float(i) / float(samples) * float(m_segments));
            m_arc[i] = m_arc[i - 1] + glm::length(p - previous);
            previous = p;
        }
        m_length = m_arc.back();
        return true;
    }

    glm::vec3 Spline::Raw(float s) const {
        s = std::clamp(s, 0.0f, float(m_segments));
        int seg = std::min(int(s), m_segments - 1);
        const float t = s - float(seg);
        if (m_type == SplineType::Bezier) return CubicBezier(m_points[seg * 3], m_points[seg * 3 + 1], m_points[seg * 3 + 2], m_points[seg * 3 + 3], t);
        const int n = int(m_points.size());
        auto at = [&](int i) -> glm::vec3 {
            if (m_closed) return m_points[((i % n) + n) % n];
            if (i < 0) return 2.0f * m_points[0] - m_points[1];
            if (i >= n) return 2.0f * m_points[n - 1] - m_points[n - 2];
            return m_points[i];
        };
        return CatmullRom(at(seg - 1), at(seg), at(seg + 1), at(seg + 2), t);
    }

    glm::vec3 Spline::Evaluate(float u) const {
        if (m_segments == 0) return m_points.empty() ? glm::vec3(0.0f) : m_points.front();
        if (m_length <= 1e-9f) return Raw(0.0f);
        const float target = std::clamp(u, 0.0f, 1.0f) * m_length;
        const auto it = std::lower_bound(m_arc.begin(), m_arc.end(), target);
        const size_t i = std::clamp<size_t>(size_t(it - m_arc.begin()), 1, m_arc.size() - 1);
        const float a = m_arc[i - 1], b = m_arc[i];
        const float f = b > a ? (target - a) / (b - a) : 0.0f;
        const float samples = float(m_arc.size() - 1);
        return Raw((float(i - 1) + f) / samples * float(m_segments));
    }

    glm::vec3 Spline::Tangent(float u) const {
        const float e = 1e-3f;
        const glm::vec3 d = Evaluate(std::min(u + e, 1.0f)) - Evaluate(std::max(u - e, 0.0f));
        return glm::length(d) > 1e-9f ? glm::normalize(d) : glm::vec3(0, 0, -1);
    }

    // ====================================================================== scene system
    namespace {
        struct Part {
            entt::entity child = entt::null;
            int mesh = 0, primitive = 0, node = -1;
            bool skinned = false;
            std::string meshName;
            size_t poolSlot = 0;
        };
        struct ModelRuntime {
            std::shared_ptr<const Model> model;
            std::string path;
            std::vector<Part> parts;
            std::vector<Transform> locals;
            std::vector<glm::mat4> globals;
            std::vector<Math::DualQuat> dq;
            std::vector<glm::mat4> matrices;
            std::vector<Render::Vertex> scratch;
            int clip = -1;
            std::string clipName;
        };

        std::map<std::string, std::shared_ptr<const Model>> g_cache;
        std::map<std::string, std::vector<bool>> g_meshPool;   // skinned mesh instances (base name -> slot in use)
        Stats g_stats;

        std::shared_ptr<const Model> GetModel(const std::string& path, std::string& error) {
            auto it = g_cache.find(path);
            if (it != g_cache.end()) return it->second;
            auto model = std::make_shared<Model>();
            if (!LoadGltf(path, *model, &error)) {
                Log::Error() << "Animation: cannot load '" << path << "': " << error;
                g_cache[path] = nullptr;
                return nullptr;
            }
            size_t vertices = 0;
            for (const Mesh& m : model->meshes) for (const Primitive& p : m.primitives) vertices += p.vertices.size();
            Log::Info() << "Animation: loaded '" << path << "' (" << model->nodes.size() << " nodes, " << model->meshes.size() << " meshes, "
                        << vertices << " vertices, " << model->skins.size() << " skins, " << model->clips.size() << " clips)";
            g_cache[path] = model;
            return model;
        }

        void ReleaseModel(entt::registry& registry, entt::entity entity) {
            for (const Part& part : registry.get<ModelRuntime>(entity).parts) {
                if (!part.skinned) continue;
                const size_t hash = part.meshName.rfind('@');
                auto it = g_meshPool.find(part.meshName.substr(0, hash));
                if (it != g_meshPool.end() && part.poolSlot < it->second.size()) it->second[part.poolSlot] = false;
            }
        }

        std::string MaterialName(const std::string& path, int index) { return "gltf:" + path + "#material" + std::to_string(index); }

        void RegisterMaterials(const std::string& path, const Model& model) {
            for (size_t i = 0; i < model.materials.size(); ++i) {
                const std::string name = MaterialName(path, int(i));
                if (Render::HasMaterial(name)) continue;
                const Material& m = model.materials[i];
                Render::MaterialDesc desc;
                desc.baseColor = glm::vec3(m.baseColor);
                desc.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
                desc.roughness = std::clamp(m.roughness, 0.04f, 1.0f);
                desc.emissive = m.emissive;
                if (m.baseColorImage >= 0 && m.baseColorImage < int(model.images.size()) && model.images[m.baseColorImage].width > 0) {
                    const Image& image = model.images[m.baseColorImage];
                    const std::string texture = "gltf:" + path + "#image" + std::to_string(m.baseColorImage);
                    Render::RegisterTexture(texture, image.width, image.height, image.rgba);
                    desc.albedoTexture = texture;
                    desc.textureUsesMeshUV = true;
                }
                Render::RegisterMaterial(name, desc);
            }
        }

        bool Setup(Scene& scene, Entity entity, SkinnedModel& component, ModelRuntime& runtime) {
            std::string error;
            runtime.model = GetModel(component.path, error);
            runtime.path = component.path;
            if (!runtime.model) { component.error = error.empty() ? "load failed" : error; return false; }
            const Model& model = *runtime.model;
            RegisterMaterials(component.path, model);
            std::vector<glm::mat4> rest;
            SamplePose(model, nullptr, 0.0f, false, runtime.locals);
            GlobalMatrices(model, runtime.locals, rest);
            for (size_t n = 0; n < model.nodes.size(); ++n) {
                const Node& node = model.nodes[n];
                if (node.mesh < 0) continue;
                const Mesh& mesh = model.meshes[node.mesh];
                for (size_t p = 0; p < mesh.primitives.size(); ++p) {
                    const Primitive& prim = mesh.primitives[p];
                    Part part;
                    part.mesh = node.mesh;
                    part.primitive = int(p);
                    part.node = int(n);
                    part.skinned = node.skin >= 0 && !prim.joints.empty();
                    const std::string base = "gltf:" + component.path + "#mesh" + std::to_string(node.mesh) + "." + std::to_string(p);
                    if (part.skinned) {   // one deformable copy per instance (pooled across scene reloads)
                        std::vector<bool>& pool = g_meshPool[base];
                        size_t slot = 0;
                        while (slot < pool.size() && pool[slot]) ++slot;
                        if (slot == pool.size()) pool.push_back(false);
                        pool[slot] = true;
                        part.poolSlot = slot;
                        part.meshName = base + "@" + std::to_string(slot);
                    }
                    else part.meshName = base;
                    if (!Render::HasMesh(part.meshName)) {
                        Render::MeshData data{ prim.vertices, prim.indices };
                        Render::RegisterMesh(part.meshName, data);
                    }
                    Entity child = scene.CreateEntity(entity.GetName() + "/" + (mesh.name.empty() ? "mesh" + std::to_string(node.mesh) : mesh.name));
                    child.SetParent(entity, false);
                    // Skinned vertices are in model space (the joint matrices place them); rigid parts follow their node.
                    if (!part.skinned) {
                        const glm::mat4& m = rest[n];
                        Transform& t = child.LocalTransform();
                        t.position = glm::vec3(m[3]);
                        t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
                        t.rotation = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / t.scale.x, glm::vec3(m[1]) / t.scale.y, glm::vec3(m[2]) / t.scale.z)));
                    }
                    MeshRenderer& renderer = child.Add<MeshRenderer>();
                    renderer.mesh = part.meshName;
                    renderer.material = prim.material >= 0 ? MaterialName(component.path, prim.material) : std::string();
                    part.child = child.Handle();
                    runtime.parts.push_back(std::move(part));
                }
            }
            component.loaded = true;
            component.error.clear();
            return true;
        }
    }

    void UpdateScene(Scene& scene, float dt) {
        entt::registry& registry = scene.Registry();
        registry.on_destroy<ModelRuntime>().connect<&ReleaseModel>();
        const double start = Time::NowSeconds();
        Stats stats;
        std::vector<entt::entity> pending;
        for (auto [handle, component] : registry.view<SkinnedModel>(entt::exclude<ModelRuntime>).each()) {
            if (!component.loaded && component.error.empty()) pending.push_back(handle);
        }
        for (entt::entity handle : pending) {
            ModelRuntime runtime;
            SkinnedModel& component = registry.get<SkinnedModel>(handle);
            if (Setup(scene, Entity(&scene, handle), component, runtime)) registry.emplace<ModelRuntime>(handle, std::move(runtime));
        }
        for (auto [handle, component, runtime] : registry.view<SkinnedModel, ModelRuntime>().each()) {
            ++stats.models;
            const Model& model = *runtime.model;
            if (runtime.clipName != component.clip || runtime.clip < 0) {
                runtime.clip = component.clip.empty() ? (model.clips.empty() ? -1 : 0) : model.FindClip(component.clip);
                runtime.clipName = component.clip;
            }
            const Clip* clip = runtime.clip >= 0 ? &model.clips[runtime.clip] : nullptr;
            if (component.playing && clip) {
                component.time += dt * component.speed;
                ++stats.clipsPlaying;
            }
            SamplePose(model, clip, component.time, component.loop, runtime.locals);
            GlobalMatrices(model, runtime.locals, runtime.globals);
            component.vertexCount = 0;
            for (Part& part : runtime.parts) {
                if (!registry.valid(part.child)) continue;
                const Node& node = model.nodes[part.node];
                const Primitive& prim = model.meshes[part.mesh].primitives[part.primitive];
                if (part.skinned) {
                    SkinTransforms(model, node.skin, runtime.globals, runtime.dq, runtime.matrices);
                    SkinVertices(prim, runtime.dq, runtime.matrices, component.method, runtime.scratch);
                    Render::UpdateMeshVertices(part.meshName, runtime.scratch);
                    component.vertexCount += uint32_t(prim.vertices.size());
                }
                else if (clip) {
                    const glm::mat4& m = runtime.globals[part.node];
                    Transform& t = registry.get<Transform>(part.child);
                    t.position = glm::vec3(m[3]);
                    t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
                    t.rotation = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / std::max(t.scale.x, 1e-9f), glm::vec3(m[1]) / std::max(t.scale.y, 1e-9f), glm::vec3(m[2]) / std::max(t.scale.z, 1e-9f))));
                }
            }
            stats.skinnedVertices += component.vertexCount;
        }
        stats.cameraPaths = g_stats.cameraPaths;
        stats.skinningMs = float((Time::NowSeconds() - start) * 1000.0);
        g_stats = stats;
    }

    void UpdateCameraPaths(Scene& scene, float dt) {
        uint32_t count = 0;
        for (auto [handle, path] : scene.Registry().view<CameraPath>().each()) {
            ++count;
            if (!path.built) {
                path.positionSpline.Build(path.type, path.points, path.closed);
                if (path.lookAt.size() == path.points.size()) path.targetSpline.Build(path.type, path.lookAt, path.closed);
                else path.targetSpline = Spline{};
                path.built = true;
            }
            if (!path.positionSpline.Valid()) continue;
            if (path.playing) path.time += dt;
            const float duration = std::max(path.duration, 1e-3f);
            float u = path.time / duration;
            u = path.loop ? u - std::floor(u) : std::clamp(u, 0.0f, 1.0f);
            const glm::vec3 position = path.positionSpline.Evaluate(u);
            glm::vec3 forward = path.targetSpline.Valid() ? path.targetSpline.Evaluate(u) - position : path.positionSpline.Tangent(u);
            if (glm::length(forward) < 1e-6f) forward = glm::vec3(0, 0, -1);
            forward = glm::normalize(forward);
            const glm::vec3 up = std::fabs(forward.y) > 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            Entity(&scene, handle).SetWorldPose(position, glm::quatLookAtRH(forward, up));
        }
        g_stats.cameraPaths = count;
    }

    Stats GetStats() { return g_stats; }
    void ClearCache() { g_cache.clear(); }
}
