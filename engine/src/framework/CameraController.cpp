#include "Phisiks/Framework/CameraController.h"
#include "Phisiks/Framework/Input.h"
#include "Phisiks/Framework/PlayerController.h"

#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks {
    const char* ToString(CameraMode mode) {
        switch (mode) {
            case CameraMode::FirstPerson: return "First person";
            case CameraMode::ThirdPerson: return "Third person";
            case CameraMode::FreeFly:     return "Free fly";
            case CameraMode::Orbit:       return "Orbit";
        }
        return "?";
    }

    void CameraController::CycleMode() {
        switch (m_mode) {
            case CameraMode::FirstPerson: m_mode = CameraMode::ThirdPerson; break;
            case CameraMode::ThirdPerson: m_mode = CameraMode::FreeFly; break;
            case CameraMode::FreeFly:     m_mode = CameraMode::FirstPerson; break;
            case CameraMode::Orbit:       m_mode = CameraMode::FirstPerson; break;
        }
    }

    void CameraController::SetYawPitch(float yaw, float pitch) {
        m_yaw = yaw;
        m_pitch = std::clamp(pitch, m_settings.minPitch, m_settings.maxPitch);
    }

    void CameraController::LookAt(const glm::vec3& eye, const glm::vec3& target) {
        const glm::vec3 d = glm::normalize(target - eye);
        // forward = (-sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch))
        SetYawPitch(std::atan2(-d.x, -d.z), std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    }

    void CameraController::SetOrbit(const glm::vec3& target, float distance, float yaw, float pitch) {
        m_mode = CameraMode::Orbit;
        m_orbitTarget = target;
        m_orbitDistance = std::clamp(distance, m_settings.orbitMinDistance, m_settings.orbitMaxDistance);
        SetYawPitch(yaw, pitch);
        m_position = m_orbitTarget - GetForward() * m_orbitDistance;
    }

    glm::vec3 CameraController::GetForward() const {
        return glm::vec3(-std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch), -std::cos(m_yaw) * std::cos(m_pitch));
    }

    glm::mat4 CameraController::GetViewMatrix() const {
        return glm::lookAt(m_position, m_position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void CameraController::Update(const InputSystem& input, const PlayerController* player, float dt, bool lookEnabled) {
        if (m_mode == CameraMode::Orbit) {
            const glm::vec2 mouse = input.MouseDelta();
            if (lookEnabled && input.Down(Action::Secondary)) {
                SetYawPitch(m_yaw - mouse.x * m_settings.mouseSensitivity, m_pitch - mouse.y * m_settings.mouseSensitivity);
            }
            if (lookEnabled && input.Down(Action::Pan)) {
                const glm::vec3 forward = GetForward();
                const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                const glm::vec3 up = glm::cross(right, forward);
                const float scale = m_orbitDistance * m_settings.orbitPanSpeed;
                m_orbitTarget += (-right * mouse.x + up * mouse.y) * scale;
            }
            if (lookEnabled && input.Scroll() != 0.0f) {
                m_orbitDistance = std::clamp(m_orbitDistance * std::pow(m_settings.orbitZoomPerNotch, input.Scroll()),
                                             m_settings.orbitMinDistance, m_settings.orbitMaxDistance);
            }
            m_position = m_orbitTarget - GetForward() * m_orbitDistance;
            return;
        }
        if (lookEnabled) {
            const glm::vec2 mouse = input.MouseDelta();
            SetYawPitch(m_yaw - mouse.x * m_settings.mouseSensitivity, m_pitch - mouse.y * m_settings.mouseSensitivity);
        }

        if (m_mode == CameraMode::FreeFly && lookEnabled) {
            const glm::vec2 axis = input.MoveAxis();
            const glm::vec3 forward = GetForward();
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            float vertical = 0.0f;
            if (input.Down(Action::Jump))   vertical += 1.0f;
            if (input.Down(Action::Crouch)) vertical -= 1.0f;
            const float speed = m_settings.freeFlySpeed * (input.Down(Action::Sprint) ? 3.0f : 1.0f);
            m_position += (forward * axis.y + right * axis.x + glm::vec3(0.0f, vertical, 0.0f)) * speed * dt;
        }

        ComputePose(player);
    }

    void CameraController::ComputePose(const PlayerController* player) {
        if (m_mode == CameraMode::FreeFly || !player || !player->IsSpawned()) return;

        if (m_mode == CameraMode::FirstPerson) {
            m_position = player->GetEyePosition();
            return;
        }

        // Third person: orbit behind a pivot above the feet; pull in if world geometry is in the way.
        const glm::vec3 pivot = player->GetFeetPosition() + glm::vec3(0.0f, m_settings.thirdPersonPivotHeight, 0.0f);
        const glm::vec3 back = -GetForward();
        float distance = m_settings.thirdPersonDistance;
        Physics::QueryOptions options;
        options.ignoreCharacter = player->GetCharacter();
        if (const auto hit = Physics::Raycast(pivot, back, distance, options)) {
            distance = std::max(0.1f, hit->distance - m_settings.thirdPersonCollisionPadding);
        }
        m_position = pivot + back * distance;
    }

    void CameraController::ApplyToScene(Scene& scene) const {
        Entity camera = scene.FindByName("MainCamera");
        if (!camera.Valid()) {
            camera = scene.CreateEntity("MainCamera");
            camera.Add<Camera>();
        }
        camera.Get<Camera>().verticalFov = m_settings.verticalFovRadians;
        // Camera looks down local -Z: yaw about +Y, then pitch about local +X.
        const glm::quat rotation = glm::angleAxis(m_yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(m_pitch, glm::vec3(1, 0, 0));
        camera.SetWorldPose(m_position, rotation);
    }
}
