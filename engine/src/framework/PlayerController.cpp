#include "Phisiks/Framework/PlayerController.h"
#include "Phisiks/Framework/Input.h"
#include "framework/PhysicsLayers.h"
#include "Phisiks/Core/Log.h"

#include "Phisiks/Physics/Physics.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks {
    namespace {
        glm::vec3 MoveTowards(const glm::vec3& current, const glm::vec3& target, float maxDelta) {
            const glm::vec3 delta = target - current;
            const float length = glm::length(delta);
            if (length <= maxDelta || length < 1e-6f) return target;
            return current + delta / length * maxDelta;
        }
    }

    bool PlayerController::Spawn(const glm::vec3& feetPosition, const PlayerSettings& settings) {
        Despawn();
        m_settings = settings;
        Physics::CharacterDesc desc;
        desc.footPosition = feetPosition;
        desc.height = settings.capsuleHeight;
        desc.radius = settings.capsuleRadius;
        desc.layer = PhysicsLayers::PLAYER;
        m_character = Physics::CreateCharacter(desc);
        if (!m_character.Valid()) {
            Log::Error() << "PlayerController::Spawn: character creation failed";
            return false;
        }
        Teleport(feetPosition);
        Log::Info() << "Player spawned at (" << feetPosition.x << ", " << feetPosition.y << ", " << feetPosition.z << ")";
        return true;
    }

    void PlayerController::Despawn() {
        if (!m_character.Valid()) return;
        Physics::DestroyCharacter(m_character);
        m_character = {};
        m_velocity = glm::vec3(0.0f);
        m_grounded = false;
    }

    void PlayerController::CaptureInput(const InputSystem& input, bool movementEnabled) {
        if (!movementEnabled) {
            m_moveAxis = glm::vec2(0.0f);
            m_sprint = false;
            return;
        }
        m_moveAxis = input.MoveAxis();
        m_sprint = input.Down(Action::Sprint);
        if (input.Pressed(Action::Jump)) m_jumpRequested = true;   // consumed by the next fixed step
    }

    void PlayerController::FixedUpdate(float yaw, float dt) {
        if (!Physics::IsValid(m_character)) return;

        // Camera-relative horizontal intent. yaw = 0 looks down -Z.
        const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
        const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
        const float speed = m_sprint ? m_settings.sprintSpeed : m_settings.walkSpeed;
        const glm::vec3 targetHorizontal = (forward * m_moveAxis.y + right * m_moveAxis.x) * speed;

        glm::vec3 horizontal(m_velocity.x, 0.0f, m_velocity.z);
        const float accel = m_settings.acceleration * (m_grounded ? 1.0f : m_settings.airControl);
        horizontal = MoveTowards(horizontal, targetHorizontal, accel * dt);

        float vertical = m_velocity.y;
        if (m_grounded && m_jumpRequested) {
            vertical = m_settings.jumpSpeed;
            m_grounded = false;
        }
        else if (m_grounded) {
            vertical = -1.0f;   // small downward bias keeps the capsule snapped to the ground
        }
        else {
            vertical = std::max(vertical - m_settings.gravity * dt, -m_settings.maxFallSpeed);
        }
        m_jumpRequested = false;

        const Physics::CharacterMoveResult move = Physics::MoveCharacter(m_character, glm::vec3(horizontal.x, vertical, horizontal.z) * dt, dt);
        m_grounded = move.grounded && vertical <= 0.0f;

        // Use the displacement the controller actually allowed, so collisions kill velocity correctly.
        const glm::vec3 actual = move.displacement / dt;
        m_velocity = glm::vec3(actual.x, m_grounded ? 0.0f : vertical, actual.z);
        if (move.hitCeiling && m_velocity.y > 0.0f) m_velocity.y = 0.0f;
    }

    void PlayerController::Teleport(const glm::vec3& feetPosition) {
        Physics::SetCharacterFootPosition(m_character, feetPosition);
        m_velocity = glm::vec3(0.0f);
        m_grounded = false;
    }

    glm::vec3 PlayerController::GetFeetPosition() const {
        return Physics::GetCharacterFootPosition(m_character);
    }

    glm::vec3 PlayerController::GetEyePosition() const {
        return GetFeetPosition() + glm::vec3(0.0f, m_settings.eyeHeight, 0.0f);
    }
}
