#pragma once
// Camera controllers. The controller drives a camera entity ("MainCamera", with a Camera
// component) in the current scene; the scene's presentation system hands it to the renderer.

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Phisiks {
    class InputSystem;
    class PlayerController;

    // Orbit = CAD-style inspection camera (RMB drag orbits, MMB drag pans, wheel zooms). It is not
    // part of the V cycle; it is entered with SetOrbit / the ToggleOrbitCamera action.
    enum class CameraMode { FirstPerson, ThirdPerson, FreeFly, Orbit };
    const char* ToString(CameraMode mode);

    struct CameraSettings {
        float verticalFovRadians = 1.0f;     // matches the engine's default viewport FOV
        float mouseSensitivity = 0.0025f;    // radians per pixel
        float thirdPersonDistance = 3.5f;
        float thirdPersonPivotHeight = 1.5f; // above the feet
        float thirdPersonCollisionPadding = 0.2f;
        float freeFlySpeed = 6.0f;
        float orbitPanSpeed = 0.0015f;       // fraction of the orbit distance per pixel
        float orbitZoomPerNotch = 0.88f;     // distance multiplier per wheel notch
        float orbitMinDistance = 0.5f;
        float orbitMaxDistance = 250.0f;
        float minPitch = -1.5f;
        float maxPitch = 1.5f;
    };

    class CameraController {
    public:
        void SetMode(CameraMode mode) { m_mode = mode; }
        CameraMode GetMode() const { return m_mode; }
        void CycleMode();

        void SetYawPitch(float yaw, float pitch);
        void LookAt(const glm::vec3& eye, const glm::vec3& target);   // sets yaw/pitch from eye -> target
        // Switches to Orbit mode around `target` (camera = target - forward * distance).
        void SetOrbit(const glm::vec3& target, float distance, float yaw, float pitch);
        glm::vec3 GetOrbitTarget() const { return m_orbitTarget; }
        float GetOrbitDistance() const { return m_orbitDistance; }

        // Applies mouse look; in FreeFly also moves the camera. Then computes the camera pose.
        void Update(const InputSystem& input, const PlayerController* player, float dt, bool lookEnabled);
        // Writes the pose into the scene's "MainCamera" entity (created on first use).
        void ApplyToScene(class Scene& scene) const;

        float GetYaw() const { return m_yaw; }
        float GetPitch() const { return m_pitch; }
        glm::vec3 GetPosition() const { return m_position; }
        glm::vec3 GetForward() const;
        glm::mat4 GetViewMatrix() const;
        CameraSettings& Settings() { return m_settings; }

    private:
        void ComputePose(const PlayerController* player);

        CameraSettings m_settings;
        CameraMode m_mode = CameraMode::FirstPerson;
        float m_yaw = 0.0f;
        float m_pitch = 0.0f;
        glm::vec3 m_position{ 0.0f, 2.0f, 5.0f };
        glm::vec3 m_orbitTarget{ 0.0f };
        float m_orbitDistance = 10.0f;
    };
}
