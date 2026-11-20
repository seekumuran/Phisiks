#pragma once
// Generic gameplay services: interaction traces, interaction, damage/health.

#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Framework/PlayerController.h"

#include <glm/vec3.hpp>

#include <string>

namespace Phisiks {
    struct TraceHit {
        bool hit = false;
        Entity entity;                      // invalid if the hit body does not belong to an entity
        glm::vec3 point{ 0.0f };
        glm::vec3 normal{ 0.0f };
        float distance = 0.0f;
    };

    struct DamageEvent {
        Entity target;
        Entity instigator;
        float amount = 0.0f;
        float healthAfter = 0.0f;
        bool killed = false;
    };

    namespace Gameplay {
        // Physics ray from the active camera (ignores the player capsule).
        TraceHit TraceFromCamera(GameContext& ctx, float range);
        TraceHit Trace(GameContext& ctx, const glm::vec3& origin, const glm::vec3& direction, float range);

        // Calls the target's onInteract. Returns false if the target is not interactable.
        bool Interact(GameContext& ctx, Entity target, Entity instigator);

        // Reduces health; on reaching zero marks the entity dead and calls onDeath
        // (default behaviour: destroy the entity). Returns the resulting event.
        DamageEvent ApplyDamage(GameContext& ctx, Entity target, float amount, Entity instigator);
        void Heal(GameContext& ctx, Entity target, float amount);

        // Spawns the local player in the current scene: a character controller plus a render-only body
        // entity named "PlayerBody" (capsule-sized box). Returns the body entity.
        Entity SpawnPlayer(GameContext& ctx, const glm::vec3& feetPosition, const PlayerSettings* settings = nullptr);
        // Once per frame after physics: moves the body to the controller, faces it along the camera
        // yaw, and hides it in first person (so it never occludes the view).
        void UpdatePlayerBody(GameContext& ctx);

        // Default player verbs, driven by input actions once per frame:
        //   Interact -> Interact() on the traced entity
        //   Primary  -> impulse on a traced dynamic body + damage if it has health
        struct PlayerActionSettings {
            float range = 6.0f;
            float pushImpulse = 40.0f;    // N*s, applied horizontally (camera forward flattened)
            float pushLift = 0.2f;        // upward fraction added to the push direction
            float damage = 25.0f;
        };
        void UpdatePlayerActions(GameContext& ctx, const PlayerActionSettings& settings = PlayerActionSettings());
    }
}
