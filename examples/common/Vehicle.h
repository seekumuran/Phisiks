#pragma once
// Raycast vehicle with independent spring-damper suspension (shared by the stunt car and the
// rover showcases). The chassis is an ordinary dynamic rigid body; each wheel casts a ray down
// the chassis -Y axis from its mount point and, when it touches the ground, applies
//   suspension  F = k x + c dx/dt (x = compression, never pulls),
//   traction    drive / brake / rolling resistance along the wheel's forward axis,
//   grip        lateral force opposing the sideways slip velocity,
// with traction and grip clamped to the friction circle mu * F. Forces become impulses at the
// contact point (lateral ones slightly raised towards the centre of mass: less roll-over).
#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Showcase {
    struct WheelDesc {
        glm::vec3 mount{ 0.0f };      // chassis-local suspension top
        float radius = 0.35f;
        bool steer = false;
        bool drive = true;
        std::string visual;           // entity moved to the wheel pose (optional)
    };

    struct VehicleDesc {
        float restLength = 0.45f;     // suspension travel from the mount to the wheel centre at rest (no load)
        float stiffness = 30000.0f;   // N/m per wheel
        float damping = 2400.0f;      // N s/m per wheel
        float maxSteer = 0.55f;       // rad
        float engineForce = 9000.0f;  // N total at full throttle
        float brakeForce = 12000.0f;  // N total
        float friction = 1.4f;        // tyre mu (friction circle)
        float lateralStiffness = 9000.0f;   // N per (m/s) of sideways slip, per wheel
        float rollingResistance = 30.0f;    // N per (m/s), per wheel
        float maxSpeed = 40.0f;       // m/s (engine cut-off)
        float rollLift = 0.25f;       // m: lateral forces act this much above the contact point
        Phisiks::Physics::LayerMask groundLayers = Phisiks::Physics::ALL_LAYERS;   // what the suspension rays hit
    };

    struct WheelState {
        bool grounded = false;
        float compression = 0.0f, lastCompression = 0.0f;
        float suspensionForce = 0.0f;
        float lateralSlip = 0.0f;     // m/s
        float spin = 0.0f;            // rad (visual)
        float steer = 0.0f;
        glm::vec3 contact{ 0.0f }, normal{ 0.0f, 1.0f, 0.0f };
        uint32_t groundEntity = 0;    // Entity::Id() + 1 of the surface (0 = none / static world)
    };

    class Vehicle {
    public:
        void Setup(Phisiks::Entity chassis, const VehicleDesc& desc, const std::vector<WheelDesc>& wheels);
        bool Valid() const { return m_chassis.Valid(); }
        Phisiks::Entity Chassis() const { return m_chassis; }
        // throttle -1..1 (reverse), steer -1..1 (right positive), brake 0..1
        void FixedUpdate(float dt, float throttle, float steer, float brake);
        void UpdateVisuals(float dt);   // wheel entities (after physics)
        float ForwardSpeed() const { return m_forwardSpeed; }
        int GroundedWheels() const;
        const std::vector<WheelState>& Wheels() const { return m_state; }
        const std::vector<WheelDesc>& WheelDescs() const { return m_wheels; }
        const VehicleDesc& Desc() const { return m_desc; }
        VehicleDesc& Desc() { return m_desc; }
        // Puts the chassis upright at `position` facing `yaw` (radians), at rest.
        void Reset(const glm::vec3& position, float yaw);

    private:
        Phisiks::Entity m_chassis;
        VehicleDesc m_desc;
        std::vector<WheelDesc> m_wheels;
        std::vector<WheelState> m_state;
        float m_forwardSpeed = 0.0f;
    };
}
