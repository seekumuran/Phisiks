#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Physics/Fields.h"
#include "scene/SceneInternal.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks {
    using namespace SceneInternal;

    // ====================================================================== backends
    namespace SceneInternal {
        namespace {
            IRenderBackend* g_render = nullptr;
            IAudioBackend* g_audio = nullptr;
        }
        void SetBackends(IRenderBackend* render, IAudioBackend* audio) { g_render = render; g_audio = audio; }
        IRenderBackend* Render() { return g_render; }
        IAudioBackend* Audio() { return g_audio; }
    }

    RenderStats GetRenderStats() {
        return SceneInternal::Render() ? SceneInternal::Render()->Stats() : RenderStats{};
    }

    namespace {
        // Runtime resources are released by EnTT destroy hooks: removing a runtime component or
        // destroying its entity both end up here.
        void ReleasePhysics(entt::registry& registry, entt::entity entity) {
            Physics::DestroyBody(registry.get<PhysicsRuntime>(entity).body);
        }
        void ReleaseMesh(entt::registry& registry, entt::entity entity) {
            if (IRenderBackend* render = SceneInternal::Render()) render->DestroyMesh(registry.get<MeshRuntime>(entity).object);
        }
        void ReleaseLight(entt::registry& registry, entt::entity entity) {
            if (IRenderBackend* render = SceneInternal::Render()) render->DestroyLight(registry.get<LightRuntime>(entity).light);
        }
        void ReleaseVoice(entt::registry& registry, entt::entity entity) {
            const AudioSource& source = registry.get<AudioSource>(entity);
            if (source.voice != 0) if (IAudioBackend* audio = SceneInternal::Audio()) audio->Stop(source.voice);
        }

        glm::mat4 Compose(const Transform& t) {
            return glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(glm::normalize(t.rotation)) * glm::scale(glm::mat4(1.0f), t.scale);
        }

        // Decomposes a TRS matrix (no shear).
        Transform Decompose(const glm::mat4& m) {
            Transform t;
            t.position = glm::vec3(m[3]);
            t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            glm::mat3 r(glm::vec3(m[0]) / std::max(t.scale.x, 1e-12f), glm::vec3(m[1]) / std::max(t.scale.y, 1e-12f), glm::vec3(m[2]) / std::max(t.scale.z, 1e-12f));
            if (glm::determinant(r) < 0.0f) { t.scale.x = -t.scale.x; r[0] = -r[0]; }
            t.rotation = glm::normalize(glm::quat_cast(r));
            return t;
        }

        bool SameMatrix(const glm::mat4& a, const glm::mat4& b) {
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) if (std::fabs(a[c][r] - b[c][r]) > 1e-6f) return false;
            return true;
        }
        bool SameRenderer(const MeshRenderer& a, const MeshRenderer& b) {
            return a.mesh == b.mesh && a.material == b.material && a.visible == b.visible && a.castShadows == b.castShadows && a.uvScale == b.uvScale;
        }
        bool SameLight(const Light& a, const Light& b) {
            return a.type == b.type && a.color == b.color && a.intensity == b.intensity && a.range == b.range &&
                   a.innerConeAngle == b.innerConeAngle && a.outerConeAngle == b.outerConeAngle && a.castShadows == b.castShadows;
        }

        Physics::ColliderDesc ScaledCollider(const Physics::ColliderDesc& c, const glm::vec3& worldScale) {
            const glm::vec3 s = glm::abs(worldScale);
            Physics::ColliderDesc out = c;
            out.localPosition = c.localPosition * s;
            switch (c.shape) {
                case Physics::ShapeType::Box:     out.halfExtents = c.halfExtents * s; break;
                case Physics::ShapeType::Sphere:  out.radius = c.radius * std::max(s.x, std::max(s.y, s.z)); break;
                case Physics::ShapeType::Capsule: out.radius = c.radius * std::max(s.x, s.z); out.halfHeight = c.halfHeight * s.y; break;
                case Physics::ShapeType::ConvexMesh:
                case Physics::ShapeType::TriangleMesh:
                    for (glm::vec3& v : out.vertices) v *= s;
                    break;
            }
            return out;
        }
    }

    // ====================================================================== Entity
    bool Entity::Valid() const { return m_scene && m_scene->m_registry.valid(m_handle); }

    const std::string& Entity::GetName() const {
        static const std::string empty;
        return Valid() ? Get<Phisiks::Name>().value : empty;
    }
    void Entity::SetName(const std::string& name) { if (Valid()) Get<Phisiks::Name>().value = name; }

    Entity Entity::GetParent() const {
        if (!Valid()) return {};
        const entt::entity parent = m_scene->m_registry.get<Hierarchy>(m_handle).parent;
        return parent == entt::null ? Entity() : Entity(m_scene, parent);
    }

    std::vector<Entity> Entity::GetChildren() const {
        std::vector<Entity> children;
        if (!Valid()) return children;
        for (entt::entity child : m_scene->m_registry.get<Hierarchy>(m_handle).children) children.emplace_back(m_scene, child);
        return children;
    }

    bool Entity::IsDescendantOf(Entity ancestor) const {
        if (!Valid() || !ancestor.Valid()) return false;
        for (Entity p = GetParent(); p.Valid(); p = p.GetParent()) if (p == ancestor) return true;
        return false;
    }

    bool Entity::SetParent(Entity parent, bool keepWorldTransform) {
        if (!Valid()) return false;
        if (parent.Valid() && (parent == *this || parent.IsDescendantOf(*this) || parent.m_scene != m_scene)) return false;
        const glm::mat4 world = WorldMatrix();
        m_scene->Unlink(m_handle);
        if (parent.Valid()) {
            m_scene->m_registry.get<Hierarchy>(m_handle).parent = parent.m_handle;
            m_scene->m_registry.get<Hierarchy>(parent.m_handle).children.push_back(m_handle);
        }
        if (keepWorldTransform) {
            const glm::mat4 parentWorld = parent.Valid() ? parent.WorldMatrix() : glm::mat4(1.0f);
            Get<Transform>() = Decompose(glm::inverse(parentWorld) * world);
        }
        return true;
    }

    glm::mat4 Entity::LocalMatrix() const { return Compose(Get<Transform>()); }

    glm::mat4 Entity::WorldMatrix() const {
        if (!Valid()) return glm::mat4(1.0f);
        const Entity parent = GetParent();
        return parent.Valid() ? parent.WorldMatrix() * LocalMatrix() : LocalMatrix();
    }

    glm::vec3 Entity::WorldPosition() const { return glm::vec3(WorldMatrix()[3]); }

    glm::quat Entity::WorldRotation() const {
        if (!Valid()) return glm::quat(1, 0, 0, 0);
        const Entity parent = GetParent();
        const glm::quat local = glm::normalize(Get<Transform>().rotation);
        return parent.Valid() ? glm::normalize(parent.WorldRotation() * local) : local;
    }

    glm::vec3 Entity::WorldScale() const {
        if (!Valid()) return glm::vec3(1.0f);
        const Entity parent = GetParent();
        return parent.Valid() ? parent.WorldScale() * Get<Transform>().scale : Get<Transform>().scale;
    }

    namespace {
        void SetWorldPoseLocalOnly(Entity entity, const glm::vec3& position, const glm::quat& rotation) {
            Transform& local = entity.Get<Transform>();
            const Entity parent = entity.GetParent();
            if (parent.Valid()) {
                local.position = glm::vec3(glm::inverse(parent.WorldMatrix()) * glm::vec4(position, 1.0f));
                local.rotation = glm::normalize(glm::inverse(parent.WorldRotation()) * rotation);
            }
            else {
                local.position = position;
                local.rotation = rotation;
            }
        }
    }

    void Entity::SetWorldPose(const glm::vec3& position, const glm::quat& rotation) {
        if (!Valid()) return;
        SetWorldPoseLocalOnly(*this, position, rotation);
        if (const PhysicsRuntime* runtime = m_scene->m_registry.try_get<PhysicsRuntime>(m_handle)) {
            Physics::SetPose(runtime->body, position, rotation);
        }
    }

    Physics::BodyId Entity::GetBody() const {
        if (!Valid()) return {};
        const PhysicsRuntime* runtime = m_scene->m_registry.try_get<PhysicsRuntime>(m_handle);
        return runtime ? runtime->body : Physics::BodyId{};
    }
    void Entity::AddImpulse(const glm::vec3& impulse) { Physics::AddImpulse(GetBody(), impulse); }
    void Entity::AddImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& point) { Physics::AddImpulseAtPoint(GetBody(), impulse, point); }
    glm::vec3 Entity::GetLinearVelocity() const { return Physics::GetLinearVelocity(GetBody()); }
    void Entity::MarkPhysicsDirty() { if (Valid()) m_scene->m_physicsDirty.insert(m_handle); }
    void Entity::Destroy() { if (Valid()) m_scene->QueueDestroy(*this); }

    // ====================================================================== Scene
    Scene::Scene(std::string name) : m_name(std::move(name)) {
        m_registry.on_construct<Collider>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_update<Collider>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_destroy<Collider>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_construct<Rigidbody>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_update<Rigidbody>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_destroy<Rigidbody>().connect<&Scene::OnPhysicsShapeChanged>(this);
        m_registry.on_destroy<PhysicsRuntime>().connect<&ReleasePhysics>();
        m_registry.on_destroy<MeshRuntime>().connect<&ReleaseMesh>();
        m_registry.on_destroy<LightRuntime>().connect<&ReleaseLight>();
        m_registry.on_destroy<AudioSource>().connect<&ReleaseVoice>();
    }

    Scene::~Scene() {
        Clear();
    }

    Entity Scene::CreateEntity(const std::string& name) {
        const entt::entity handle = m_registry.create();
        m_registry.emplace<Phisiks::Name>(handle, Phisiks::Name{ name });
        m_registry.emplace<Transform>(handle);
        m_registry.emplace<Hierarchy>(handle);
        return Entity(this, handle);
    }

    void Scene::Unlink(entt::entity entity) {
        Hierarchy& node = m_registry.get<Hierarchy>(entity);
        if (node.parent != entt::null && m_registry.valid(node.parent)) {
            auto& siblings = m_registry.get<Hierarchy>(node.parent).children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }
        node.parent = entt::null;
    }

    void Scene::DestroyRecursive(entt::entity entity) {
        if (!m_registry.valid(entity)) return;
        const std::vector<entt::entity> children = m_registry.get<Hierarchy>(entity).children;
        for (entt::entity child : children) DestroyRecursive(child);
        m_registry.get<Hierarchy>(entity).children.clear();
        Unlink(entity);
        m_registry.destroy(entity);   // fires release hooks for physics/render/audio
        m_physicsDirty.erase(entity); // the Collider/Rigidbody destroy signals re-inserted it
    }

    void Scene::DestroyImmediate(Entity entity) {
        if (entity.GetScene() == this && entity.Valid()) DestroyRecursive(entity.Handle());
    }

    void Scene::QueueDestroy(Entity entity) {
        if (entity.GetScene() != this || !entity.Valid() || IsPendingDestroy(entity)) return;
        m_destroyQueue.push_back(entity.Handle());
    }

    bool Scene::IsPendingDestroy(Entity entity) const {
        if (std::find(m_destroyQueue.begin(), m_destroyQueue.end(), entity.Handle()) != m_destroyQueue.end()) return true;
        for (Entity p = entity.GetParent(); p.Valid(); p = p.GetParent()) {   // children of a queued parent
            if (std::find(m_destroyQueue.begin(), m_destroyQueue.end(), p.Handle()) != m_destroyQueue.end()) return true;
        }
        return false;
    }

    void Scene::FlushDestroyed() {
        std::vector<entt::entity> queue;
        queue.swap(m_destroyQueue);
        for (entt::entity entity : queue) DestroyRecursive(entity);   // stale/duplicate handles are ignored
    }

    void Scene::Clear() {
        std::vector<entt::entity> all;
        for (entt::entity entity : m_registry.view<Phisiks::Name>()) all.push_back(entity);
        for (entt::entity entity : all) if (m_registry.valid(entity)) m_registry.destroy(entity);
        m_destroyQueue.clear();
        m_physicsDirty.clear();
        if (m_cameraBound) {
            if (IRenderBackend* render = SceneInternal::Render()) render->SetCameras({});
            m_cameraBound = false;
        }
    }

    Entity Scene::FindByName(const std::string& name) const {
        for (auto [entity, n] : m_registry.view<const Phisiks::Name>().each()) {
            const Entity candidate(const_cast<Scene*>(this), entity);
            if (n.value == name && !IsPendingDestroy(candidate)) return candidate;
        }
        return {};
    }

    Entity Scene::FromId(uint32_t id) const {
        const entt::entity handle = static_cast<entt::entity>(id);
        return m_registry.valid(handle) ? Entity(const_cast<Scene*>(this), handle) : Entity();
    }

    Entity Scene::FromBody(Physics::BodyId body) const {
        const uint64_t userData = Physics::GetUserData(body);
        return userData == 0 ? Entity() : FromId(static_cast<uint32_t>(userData - 1));
    }

    size_t Scene::EntityCount() const { return m_registry.view<const Phisiks::Name>().size(); }

    // ---------------------------------------------------------------------- physics systems
    void Scene::SyncPhysicsBodies() {
        if (!Physics::IsInitialized()) return;

        const std::vector<entt::entity> dirty(m_physicsDirty.begin(), m_physicsDirty.end());
        m_physicsDirty.clear();
        for (entt::entity handle : dirty) {
            if (!m_registry.valid(handle)) continue;
            Entity entity(this, handle);
            if (m_registry.all_of<PhysicsRuntime>(handle)) m_registry.remove<PhysicsRuntime>(handle);   // releases the old body
            const Collider* collider = m_registry.try_get<Collider>(handle);
            if (!collider) continue;                                                                       // collider removed: no body

            const Rigidbody* rigidbody = m_registry.try_get<Rigidbody>(handle);
            Physics::BodyDesc desc;
            desc.type = rigidbody ? rigidbody->type : Physics::BodyType::Static;
            desc.position = entity.WorldPosition();
            desc.rotation = entity.WorldRotation();
            desc.layer = collider->layer;
            desc.userData = uint64_t(entity.Id()) + 1;   // 0 means "no entity"
            desc.colliders.push_back(ScaledCollider(collider->shape, entity.WorldScale()));
            if (rigidbody) {
                desc.mass = rigidbody->mass;
                desc.linearDamping = rigidbody->linearDamping;
                desc.angularDamping = rigidbody->angularDamping;
                desc.continuousCollision = rigidbody->continuousCollision;
                desc.enableGravity = rigidbody->useGravity;
            }
            const Physics::BodyId body = Physics::CreateBody(desc);
            if (!body.Valid()) {
                Log::Error() << "Scene '" << m_name << "': physics body for '" << entity.GetName() << "' could not be created";
                continue;
            }
            m_registry.emplace<PhysicsRuntime>(handle, PhysicsRuntime{ body });
        }

        // Kinematic bodies follow their transforms.
        for (auto [handle, rigidbody, runtime] : m_registry.view<Rigidbody, PhysicsRuntime>().each()) {
            if (rigidbody.type != Physics::BodyType::Kinematic) continue;
            const Entity entity(this, handle);
            Physics::SetKinematicTarget(runtime.body, entity.WorldPosition(), entity.WorldRotation());
        }
    }

    void Scene::ApplyForceFields(float fixedDt) {
        struct Placed { Physics::ForceField field; glm::vec3 origin, axis; };
        std::vector<Placed> fields;
        const Physics::ForceField* nbody = nullptr;
        glm::vec3 nbodyOrigin(0.0f);
        for (auto [handle, field] : m_registry.view<Physics::ForceField>().each()) {
            if (!field.affectsRigidBodies) continue;
            const Entity e(this, handle);
            if (field.type == Physics::FieldType::NBody) { if (!nbody) { nbody = &field; nbodyOrigin = e.WorldPosition(); } continue; }
            fields.push_back({ field, e.WorldPosition(), glm::normalize(e.WorldRotation() * field.axis) });
        }
        if (fields.empty() && !nbody) return;

        std::vector<Physics::BodyId> bodies;
        std::vector<glm::vec3> positions;
        std::vector<float> masses;
        for (auto [handle, rigidbody, runtime] : m_registry.view<Rigidbody, SceneInternal::PhysicsRuntime>().each()) {
            if (rigidbody.type != Physics::BodyType::Dynamic) continue;
            bodies.push_back(runtime.body);
            positions.push_back(Physics::FieldMath::SamplePosition(Physics::GetPosition(runtime.body), Physics::GetLinearVelocity(runtime.body),
                                                                   fixedDt, Physics::GetSettings().substeps));
            masses.push_back(Physics::GetMass(runtime.body));
        }
        // N-body: the bodies inside the field's influence radius (all of them if it is 0) attract each other.
        std::vector<glm::vec3> mutual(bodies.size(), glm::vec3(0.0f));
        if (nbody) {
            std::vector<size_t> members;
            std::vector<glm::vec3> p;
            std::vector<float> m;
            for (size_t i = 0; i < bodies.size(); ++i) {
                if (nbody->influenceRadius > 0.0f && glm::length(positions[i] - nbodyOrigin) > nbody->influenceRadius) continue;
                members.push_back(i);
                p.push_back(positions[i]);
                m.push_back(masses[i]);
            }
            std::vector<glm::vec3> a(members.size());
            if (members.size() > 1) Physics::FieldMath::NBodyAccelerations(p.data(), m.data(), members.size(), nbody->strength, nbody->softening, a.data());
            for (size_t k = 0; k < members.size(); ++k) mutual[members[k]] = a[k];
        }
        for (size_t i = 0; i < bodies.size(); ++i) {
            glm::vec3 a = mutual[i];
            for (const Placed& f : fields) a += Physics::FieldMath::Acceleration(f.field, f.origin, f.axis, positions[i]);
            if (glm::dot(a, a) > 0.0f) Physics::AddForce(bodies[i], a * masses[i]);
        }
    }

    void Scene::PullPhysicsPoses() {
        for (auto [handle, rigidbody, runtime] : m_registry.view<Rigidbody, PhysicsRuntime>().each()) {
            if (rigidbody.type != Physics::BodyType::Dynamic) continue;
            SetWorldPoseLocalOnly(Entity(this, handle), Physics::GetPosition(runtime.body), Physics::GetRotation(runtime.body));
        }
    }

    // ---------------------------------------------------------------------- presentation systems
    Entity Scene::GetPrimaryCamera() {
        for (auto [handle, camera] : m_registry.view<Camera>().each()) {
            if (camera.primary && camera.enabled) return Entity(this, handle);
        }
        return {};
    }

    void Scene::SyncPresentation() {
        if (IRenderBackend* render = SceneInternal::Render()) {
            // Drop runtime objects whose component was removed.
            std::vector<entt::entity> orphans;
            for (entt::entity e : m_registry.view<MeshRuntime>(entt::exclude<MeshRenderer>)) orphans.push_back(e);
            for (entt::entity e : orphans) m_registry.remove<MeshRuntime>(e);
            orphans.clear();
            for (entt::entity e : m_registry.view<LightRuntime>(entt::exclude<Light>)) orphans.push_back(e);
            for (entt::entity e : orphans) m_registry.remove<LightRuntime>(e);

            for (auto [handle, renderer] : m_registry.view<MeshRenderer>().each()) {
                const glm::mat4 world = Entity(this, handle).WorldMatrix();
                if (MeshRuntime* runtime = m_registry.try_get<MeshRuntime>(handle)) {
                    if (SameMatrix(runtime->world, world) && SameRenderer(runtime->synced, renderer)) continue;
                    render->UpdateMesh(runtime->object, renderer, world);
                    runtime->world = world;
                    runtime->synced = renderer;
                }
                else {
                    const uint64_t object = render->CreateMesh(renderer, world, entt::to_integral(handle));
                    if (object != 0) m_registry.emplace<MeshRuntime>(handle, MeshRuntime{ object, world, renderer });
                }
            }

            for (auto [handle, light] : m_registry.view<Light>().each()) {
                const Entity entity(this, handle);
                const glm::vec3 position = entity.WorldPosition();
                const glm::vec3 direction = glm::normalize(entity.WorldRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
                if (LightRuntime* runtime = m_registry.try_get<LightRuntime>(handle)) {
                    if (glm::length(runtime->position - position) < 1e-6f && glm::length(runtime->direction - direction) < 1e-6f &&
                        SameLight(runtime->synced, light)) continue;
                    render->UpdateLight(runtime->light, light, position, direction);
                    runtime->position = position;
                    runtime->direction = direction;
                    runtime->synced = light;
                }
                else {
                    const uint64_t id = render->CreateLight(light, position, direction);
                    if (id != 0) m_registry.emplace<LightRuntime>(handle, LightRuntime{ id, position, direction, light });
                }
            }

            std::vector<CameraView> views;
            for (auto [handle, camera] : m_registry.view<Camera>().each()) {
                if (!camera.enabled) continue;
                const Entity entity(this, handle);
                const glm::mat4 cameraWorld = glm::translate(glm::mat4(1.0f), entity.WorldPosition()) * glm::mat4_cast(entity.WorldRotation());
                views.push_back({ glm::inverse(cameraWorld), camera, uint32_t(entt::to_integral(handle)) });
            }
            std::stable_sort(views.begin(), views.end(), [](const CameraView& a, const CameraView& b) { return a.camera.order < b.camera.order; });
            if (!views.empty() || m_cameraBound) {
                m_cameraBound = !views.empty();
                render->SetCameras(views);
            }
        }

        IAudioBackend* audio = SceneInternal::Audio();
        for (auto [handle, source] : m_registry.view<AudioSource>().each()) {
            if (!source.started) {
                source.started = true;
                if (source.playOnStart) source.playRequested = true;
            }
            if (!audio) { source.playRequested = source.stopRequested = false; source.isPlaying = false; continue; }
            if (source.stopRequested && source.voice) { audio->Stop(source.voice); source.voice = 0; }
            if (source.playRequested) {
                if (source.voice) audio->Stop(source.voice);
                source.voice = audio->Play(source.clip, source.volume, source.loop);
                if (!source.voice) Log::Warning() << "AudioSource on '" << m_registry.get<Phisiks::Name>(handle).value << "': could not play '" << source.clip << "'";
            }
            source.playRequested = source.stopRequested = false;
            source.isPlaying = source.voice != 0 && audio->IsPlaying(source.voice);
        }
    }
}
