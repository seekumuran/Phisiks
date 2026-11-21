#pragma once
// Scenes and entities (entity-component system backed by EnTT).
//
//   class Level : public Phisiks::Scene {
//   public:
//       Level() : Scene("Level") {}
//       void OnEnter(GameContext& ctx) override {
//           Entity crate = CreateEntity("Crate");
//           crate.Get<Transform>().position = { 0, 2, 0 };
//           crate.Add<MeshRenderer>();
//           crate.Add<Collider>().shape.halfExtents = glm::vec3(0.5f);
//           crate.Add<Rigidbody>().mass = 10.0f;
//       }
//   };
//
// Every entity has Name and Transform. Transforms are local to the parent (SetParent); world
// values are derived on demand. Destroy() is deferred to the end of the frame and takes the
// children with it; physics bodies, render objects, lights and voices are released automatically.

#include "Phisiks/Scene/Components.h"

#include <entt.hpp>
#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Phisiks {

    struct GameContext;
    class Scene;

    class Entity {
    public:
        Entity() = default;
        Entity(Scene* scene, entt::entity handle) : m_scene(scene), m_handle(handle) {}

        bool Valid() const;                         // false after destruction (ids are versioned)
        explicit operator bool() const { return Valid(); }
        uint32_t Id() const { return static_cast<uint32_t>(entt::to_integral(m_handle)); }
        Scene* GetScene() const { return m_scene; }
        entt::entity Handle() const { return m_handle; }
        friend bool operator==(const Entity& a, const Entity& b) { return a.m_scene == b.m_scene && a.m_handle == b.m_handle; }
        friend bool operator!=(const Entity& a, const Entity& b) { return !(a == b); }

        const std::string& GetName() const;
        void SetName(const std::string& name);

        template<class T, class... Args> T& Add(Args&&... args);
        template<class T> T& Get();
        template<class T> const T& Get() const;
        template<class T> T* TryGet();
        template<class T> bool Has() const;
        template<class T> void Remove();

        // Hierarchy. keepWorldTransform = true preserves the current world pose.
        bool SetParent(Entity parent, bool keepWorldTransform = true);   // false if it would create a cycle
        void ClearParent(bool keepWorldTransform = true) { SetParent(Entity(), keepWorldTransform); }
        Entity GetParent() const;
        std::vector<Entity> GetChildren() const;
        bool IsDescendantOf(Entity ancestor) const;

        // Transforms
        Transform& LocalTransform() { return Get<Transform>(); }
        glm::mat4 LocalMatrix() const;
        glm::mat4 WorldMatrix() const;
        glm::vec3 WorldPosition() const;
        glm::quat WorldRotation() const;
        glm::vec3 WorldScale() const;
        // Sets the world pose (converted to local) and teleports an existing physics body.
        void SetWorldPose(const glm::vec3& position, const glm::quat& rotation);
        void SetWorldPosition(const glm::vec3& position) { SetWorldPose(position, WorldRotation()); }

        // Physics convenience (no-ops without a dynamic body).
        Physics::BodyId GetBody() const;
        void AddImpulse(const glm::vec3& impulse);
        void AddImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& worldPoint);
        glm::vec3 GetLinearVelocity() const;
        void MarkPhysicsDirty();                    // rebuild the body after editing Collider/Rigidbody in place

        void Destroy();                             // deferred (end of frame), includes children

    private:
        Scene* m_scene = nullptr;
        entt::entity m_handle = entt::null;
    };

    class Scene {
    public:
        explicit Scene(std::string name = "Scene");
        virtual ~Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        // ---- Game hooks (override in game scenes)
        virtual void OnEnter(GameContext& /*ctx*/) {}
        virtual void OnExit(GameContext& /*ctx*/) {}
        virtual void Update(GameContext& /*ctx*/, float /*deltaTime*/) {}
        virtual void FixedUpdate(GameContext& /*ctx*/, float /*fixedDeltaTime*/) {}
        virtual void DrawUI(GameContext& /*ctx*/) {}

        const std::string& Name() const { return m_name; }
        uint64_t Serial() const { return m_serial; }      // unique per Scene instance (never reused)

        // ---- Entities
        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyImmediate(Entity entity);          // entity and children, now
        void QueueDestroy(Entity entity);              // what Entity::Destroy calls
        void FlushDestroyed();                         // destroys queued entities
        void Clear();                                  // destroy everything now
        Entity FindByName(const std::string& name) const;
        Entity FromId(uint32_t id) const;              // e.g. physics user data; invalid if stale
        Entity FromBody(Physics::BodyId body) const;   // entity owning a physics body (from hits/events)
        size_t EntityCount() const;
        bool IsPendingDestroy(Entity entity) const;

        // f(Entity, Components&...) for every entity that has all listed components.
        template<class... C, class F>
        void Each(F&& f) {
            for (auto handle : m_registry.view<C...>()) {
                f(Entity(this, handle), m_registry.get<C>(handle)...);
            }
        }

        // ---- Systems (the Application calls these; tests may call them directly)
        void SyncPhysicsBodies();                      // create/rebuild/destroy bodies; push kinematic targets
        void PullPhysicsPoses();                       // dynamic bodies -> transforms
        // ForceField components -> forces on dynamic rigid bodies for the next physics step
        // (uniform / planetary / vortex fields, and mutual gravity for NBody fields).
        void ApplyForceFields(float fixedDt);
        void SyncPresentation();                       // renderers, lights, camera, audio (backends)
        Entity GetPrimaryCamera();

        entt::registry& Registry() { return m_registry; }

    private:
        friend class Entity;
        void OnPhysicsShapeChanged(entt::registry&, entt::entity entity) { m_physicsDirty.insert(entity); }
        void DestroyRecursive(entt::entity entity);
        void Unlink(entt::entity entity);

        std::string m_name;
        uint64_t m_serial = 0;
        entt::registry m_registry;
        std::unordered_set<entt::entity> m_physicsDirty;
        std::vector<entt::entity> m_destroyQueue;
        bool m_cameraBound = false;
        glm::vec3 m_listenerPosition{ 0.0f };
        bool m_listenerValid = false;
    };

    // ------------------------------------------------------------------ Entity templates
    template<class T, class... Args>
    T& Entity::Add(Args&&... args) { return m_scene->m_registry.emplace_or_replace<T>(m_handle, std::forward<Args>(args)...); }
    template<class T> T& Entity::Get() { return m_scene->m_registry.get<T>(m_handle); }
    template<class T> const T& Entity::Get() const { return m_scene->m_registry.get<T>(m_handle); }
    template<class T> T* Entity::TryGet() { return Valid() ? m_scene->m_registry.try_get<T>(m_handle) : nullptr; }
    template<class T> bool Entity::Has() const { return Valid() && m_scene->m_registry.all_of<T>(m_handle); }
    template<class T> void Entity::Remove() { if (Valid()) m_scene->m_registry.remove<T>(m_handle); }
}
