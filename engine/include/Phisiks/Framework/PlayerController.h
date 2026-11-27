#pragma once
// First/third-person character movement on top of the Phisiks::Physics character controller.
// Movement runs in FixedUpdate (60 Hz) so it is frame-rate independent.

#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Physics/PhysicsTypes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace Phisiks {
    class InputSystem;

    struct PlayerSettings {
        float capsuleHeight = 1.1f;   // cylinder part; total height = height + 2 * radius
        float capsuleRadius = 0.3f;
        float eyeHeight = 1.6f;       // above the feet
        float walkSpeed = 3.5f;
        float sprintSpeed = 6.0f;
        float acceleration = 30.0f;   // m/s^2 towards target horizontal velocity (ground)
        float airControl = 0.3f;      // fraction of acceleration available in the air
        float jumpSpeed = 4.8f;
        float gravity = 9.81f;
        float maxFallSpeed = 30.0f;
    };

    class PlayerController {
    public:
        bool Spawn(const glm::vec3& feetPosition, const PlayerSettings& settings = PlayerSettings());
        void Despawn();
        bool IsSpawned() const { return m_character.Valid(); }

        // Called once per rendered frame: latches edge-triggered input (jump) for the next fixed step.
        void CaptureInput(const InputSystem& input, bool movementEnabled);
        // Called once per fixed step. yaw = camera yaw in radians (movement is camera-relative).
        void FixedUpdate(float yaw, float dt);

        void Teleport(const glm::vec3& feetPosition);

        glm::vec3 GetFeetPosition() const;
        glm::vec3 GetEyePosition() const;
        glm::vec3 GetVelocity() const { return m_velocity; }
        bool IsGrounded() const { return m_grounded; }
        Physics::CharacterId GetCharacter() const { return m_character; }
        const PlayerSettings& GetSettings() const { return m_settings; }

        Entity GetBodyEntity() const { return m_body; }
        void SetBodyEntity(Entity body) { m_body = body; }

    private:
        PlayerSettings m_settings;
        Physics::CharacterId m_character;
        Entity m_body;
        glm::vec3 m_velocity{ 0.0f };
        glm::vec2 m_moveAxis{ 0.0f };
        bool m_sprint = false;
        bool m_jumpRequested = false;
        bool m_grounded = false;
    };
}
