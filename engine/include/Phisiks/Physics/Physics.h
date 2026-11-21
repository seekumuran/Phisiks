#pragma once
// Phisiks::Physics — rigid bodies, colliders, queries, collision/trigger events and a character
// controller. Two interchangeable backends (hidden): NVIDIA PhysX 5 (Windows default) and the
// native Phisiks solver (macOS, Linux without PhysX; -DPHISIKS_PHYSICS_BACKEND=Native anywhere).
// Runs headless (no window or GPU needed).
//
//   Physics::Init();
//   BodyDesc ground{ .type = BodyType::Static };  ground.colliders.push_back(box);
//   BodyId id = Physics::CreateBody(ground);
//   Physics::Step(1.0f / 60.0f);                   // fixed step; events available afterwards
//   if (auto hit = Physics::Raycast(origin, dir, 10.0f)) { ... }

#include "Phisiks/Physics/PhysicsTypes.h"

#include <glm/vec3.hpp>

#include <optional>
#include <vector>

namespace Phisiks::Physics {

    bool Init(const Settings& settings = Settings());
    const char* BackendName();                                   // "PhysX 5.x" or "Phisiks native"
    void Shutdown();
    bool IsInitialized();

    const Settings& GetSettings();
    void SetGravity(const glm::vec3& gravity);
    void SetSubsteps(uint32_t substeps);
    void SetDefaultMaterial(const PhysicsMaterial& material);   // affects colliders created afterwards

    // Collision matrix (symmetric). Changes apply to bodies created afterwards and, via
    // RefreshFiltering(), to existing ones.
    void SetLayersCollide(Layer a, Layer b, bool collide);
    bool LayersCollide(Layer a, Layer b);
    void RefreshFiltering();

    // Advances the simulation by `dt` seconds (split into Settings::substeps sub-steps).
    // Collision/trigger events produced during the step are appended to the event lists.
    void Step(float dt);
    void ClearEvents();                                         // usually once per frame
    const std::vector<CollisionEvent>& GetCollisionEvents();
    const std::vector<TriggerEvent>& GetTriggerEvents();

    // ---- Bodies
    BodyId CreateBody(const BodyDesc& desc);
    void DestroyBody(BodyId body);                              // safe on stale ids
    void DestroyAll();                                          // bodies and characters
    bool IsValid(BodyId body);
    BodyType GetBodyType(BodyId body);
    uint64_t GetUserData(BodyId body);

    glm::vec3 GetPosition(BodyId body);
    glm::quat GetRotation(BodyId body);
    void SetPose(BodyId body, const glm::vec3& position, const glm::quat& rotation);   // teleport; resets velocity of dynamics
    // Exact state restore (replay / timeline scrubbing): pose and velocities, body woken.
    void SetBodyState(BodyId body, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& linearVelocity, const glm::vec3& angularVelocity);
    void SetKinematicTarget(BodyId body, const glm::vec3& position, const glm::quat& rotation);   // kinematic: moves during the next step, pushing dynamics

    glm::vec3 GetLinearVelocity(BodyId body);
    glm::vec3 GetAngularVelocity(BodyId body);
    void SetLinearVelocity(BodyId body, const glm::vec3& velocity);
    void SetAngularVelocity(BodyId body, const glm::vec3& velocity);
    void AddForce(BodyId body, const glm::vec3& force);                                   // N, applied over the next step
    void AddImpulse(BodyId body, const glm::vec3& impulse);                               // N*s, instant
    void AddImpulseAtPoint(BodyId body, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void AddTorque(BodyId body, const glm::vec3& torque);
    float GetMass(BodyId body);
    bool IsSleeping(BodyId body);
    void WakeUp(BodyId body);

    // World-space bounds of all colliders of the body.
    void GetBounds(BodyId body, glm::vec3& outMin, glm::vec3& outMax);

    // ---- Queries (triggers excluded unless QueryOptions::includeTriggers)
    std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options = {});
    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options = {});   // sorted by distance
    std::optional<RaycastHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const QueryOptions& options = {});
    std::optional<RaycastHit> BoxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const QueryOptions& options = {});
    std::optional<RaycastHit> CapsuleCast(const glm::vec3& center, float radius, float halfHeight, const glm::vec3& direction, float maxDistance, const QueryOptions& options = {});
    std::vector<BodyId> OverlapSphere(const glm::vec3& center, float radius, const QueryOptions& options = {});
    std::vector<BodyId> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const QueryOptions& options = {});

    // ---- Character controller (kinematic capsule with step-up, slope limit, and pushing)
    CharacterId CreateCharacter(const CharacterDesc& desc);
    void DestroyCharacter(CharacterId character);
    bool IsValid(CharacterId character);
    CharacterMoveResult MoveCharacter(CharacterId character, const glm::vec3& displacement, float dt);
    glm::vec3 GetCharacterFootPosition(CharacterId character);
    void SetCharacterFootPosition(CharacterId character, const glm::vec3& footPosition);
    void GetCharacterBounds(CharacterId character, glm::vec3& outMin, glm::vec3& outMax);

    Stats GetStats();
}
