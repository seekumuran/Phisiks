#include "Vehicle.h"

#include "Phisiks/Physics/Physics.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using namespace Phisiks;

namespace Showcase {
    void Vehicle::Setup(Entity chassis, const VehicleDesc& desc, const std::vector<WheelDesc>& wheels) {
        m_chassis = chassis;
        m_desc = desc;
        m_wheels = wheels;
        m_state.assign(wheels.size(), WheelState());
    }

    int Vehicle::GroundedWheels() const {
        int n = 0;
        for (const WheelState& w : m_state) n += w.grounded ? 1 : 0;
        return n;
    }

    void Vehicle::Reset(const glm::vec3& position, float yaw) {
        if (!m_chassis.Valid()) return;
        // SetWorldPose teleports and wakes the body with zero velocity (SetBodyState with zero
        // velocities lets the native solver put a body to sleep at once, even in mid-air).
        m_chassis.SetWorldPose(position, glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
        for (WheelState& w : m_state) w = WheelState();
    }

    void Vehicle::FixedUpdate(float dt, float throttle, float steer, float brake) {
        const Physics::BodyId body = m_chassis.Valid() ? m_chassis.GetBody() : Physics::BodyId();
        if (!body.Valid() || dt <= 0.0f) return;
        const glm::vec3 com = Physics::GetPosition(body);
        const glm::quat rot = Physics::GetRotation(body);
        const glm::vec3 v = Physics::GetLinearVelocity(body);
        const glm::vec3 w = Physics::GetAngularVelocity(body);
        const glm::vec3 up = rot * glm::vec3(0, 1, 0);
        const glm::vec3 chassisForward = rot * glm::vec3(0, 0, -1);
        m_forwardSpeed = glm::dot(v, chassisForward);
        int driven = 0;
        for (const WheelDesc& d : m_wheels) driven += d.drive ? 1 : 0;
        const float speedFactor = std::clamp(1.0f - std::abs(m_forwardSpeed) / m_desc.maxSpeed, 0.0f, 1.0f);
        Physics::QueryOptions options;
        options.ignoreBody = body;
        options.layers = m_desc.groundLayers;

        for (size_t i = 0; i < m_wheels.size(); ++i) {
            const WheelDesc& d = m_wheels[i];
            WheelState& s = m_state[i];
            s.steer = d.steer ? steer * m_desc.maxSteer : 0.0f;
            const glm::vec3 origin = com + rot * d.mount;
            const float reach = m_desc.restLength + d.radius;
            const auto hit = Physics::Raycast(origin, -up, reach, options);
            s.lastCompression = s.compression;
            s.grounded = hit.has_value() && glm::dot(hit->normal, up) > 0.3f;
            if (!s.grounded) {
                s.compression = 0.0f;
                s.suspensionForce = 0.0f;
                s.groundEntity = 0;
                continue;
            }
            s.contact = hit->point;
            s.normal = hit->normal;
            s.groundEntity = uint32_t(hit->userData);
            s.compression = reach - hit->distance;
            const float rate = (s.compression - s.lastCompression) / dt;
            const float load = std::max(0.0f, m_desc.stiffness * s.compression + m_desc.damping * rate);
            s.suspensionForce = load;

            const glm::quat steerRot = glm::angleAxis(-s.steer, up);
            glm::vec3 forward = steerRot * chassisForward;
            forward = glm::normalize(forward - s.normal * glm::dot(forward, s.normal));
            const glm::vec3 right = glm::normalize(glm::cross(forward, s.normal));
            const glm::vec3 pointVelocity = v + glm::cross(w, s.contact - com);
            const float vf = glm::dot(pointVelocity, forward);
            const float vl = glm::dot(pointVelocity, right);
            s.lateralSlip = vl;

            float fx = 0.0f;
            if (d.drive && driven > 0) fx += throttle * m_desc.engineForce / float(driven) * (throttle * m_forwardSpeed >= 0.0f ? speedFactor : 1.0f);
            if (brake > 0.0f) fx -= std::clamp(vf * 4.0f, -1.0f, 1.0f) * brake * m_desc.brakeForce / float(m_wheels.size());
            fx -= vf * m_desc.rollingResistance;
            float fy = -vl * m_desc.lateralStiffness;
            const float limit = m_desc.friction * load;
            const float magnitude = std::sqrt(fx * fx + fy * fy);
            if (magnitude > limit && magnitude > 1e-4f) {
                fx *= limit / magnitude;
                fy *= limit / magnitude;
            }
            Physics::AddImpulseAtPoint(body, up * (load * dt), s.contact);
            Physics::AddImpulseAtPoint(body, (forward * fx + right * fy) * dt, s.contact + up * m_desc.rollLift);
        }
    }

    void Vehicle::UpdateVisuals(float dt) {
        if (!m_chassis.Valid()) return;
        Scene* scene = m_chassis.GetScene();
        const glm::vec3 position = m_chassis.WorldPosition();
        const glm::quat rot = m_chassis.WorldRotation();
        const glm::vec3 up = rot * glm::vec3(0, 1, 0);
        for (size_t i = 0; i < m_wheels.size(); ++i) {
            const WheelDesc& d = m_wheels[i];
            WheelState& s = m_state[i];
            if (d.visual.empty()) continue;
            Entity wheel = scene->FindByName(d.visual);
            if (!wheel.Valid()) continue;
            const float travel = s.grounded ? m_desc.restLength - s.compression : m_desc.restLength;
            const glm::vec3 centre = position + rot * d.mount - up * travel;
            s.spin += m_forwardSpeed / std::max(d.radius, 0.05f) * dt;
            const glm::quat q = rot * glm::angleAxis(-s.steer, glm::vec3(0, 1, 0)) * glm::angleAxis(-s.spin, glm::vec3(1, 0, 0))
                                * glm::angleAxis(1.5707963f, glm::vec3(0, 0, 1));   // cylinder-like sphere/cube along X
            wheel.SetWorldPose(centre, q);
        }
    }
}
