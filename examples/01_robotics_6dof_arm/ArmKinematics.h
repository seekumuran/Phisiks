#pragma once
// Kinematics of the showcase 6-DOF industrial arm (no rendering; unit-tested).
//
// Zero pose (metres; X forward, Y up, Z left): J1 (vertical turret) at (0, 0.30, 0), J2 shoulder
// and J3 elbow (horizontal, pitching forward), J4 forearm roll, J5 wrist pitch at the wrist centre
// (0.95, 1.47, 0), J6 flange roll at (1.05, 1.47, 0); the tool centre point (TCP, between the
// gripper pads) is 0.19 m beyond the flange. This is the classic anthropomorphic arm with a
// spherical wrist (J4-J6 axes meet at the wrist centre).
//
// Forward kinematics composes unit dual quaternions (Phisiks::Math::DualQuat):
//   T_i = T_(i-1) * Trans(p_i - p_(i-1)) * Rot(axis_i, q_i)
// with p_i / axis_i taken in the zero pose, so frame i maps zero-pose coordinates relative to
// joint i to the world (the link meshes are modelled exactly that way). Inverse kinematics is
// damped least squares on the 6 x 6 geometric Jacobian (position + orientation error, angular rows
// weighted by a 0.4 m lever), clamped to the joint limits.
#include "Phisiks/Math/DualQuat.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Showcase::Arm {
    constexpr int DOF = 6;
    using Angles = std::array<float, DOF>;   // radians

    struct JointDef {
        glm::vec3 position;      // zero pose (world)
        glm::vec3 axis;          // zero pose (world), right-handed positive rotation
        float minDeg, maxDeg;
        const char* name;
    };
    inline const JointDef JOINTS[DOF] = {
        { { 0.00f, 0.30f, 0.0f }, { 0, 1, 0 },  -170.0f, 170.0f, "J1 base" },
        { { 0.15f, 0.65f, 0.0f }, { 0, 0, -1 }, -90.0f, 110.0f, "J2 shoulder" },
        { { 0.15f, 1.35f, 0.0f }, { 0, 0, -1 }, -110.0f, 80.0f, "J3 elbow" },
        { { 0.47f, 1.47f, 0.0f }, { 1, 0, 0 },  -180.0f, 180.0f, "J4 wrist roll" },
        { { 0.95f, 1.47f, 0.0f }, { 0, 0, -1 }, -125.0f, 125.0f, "J5 wrist pitch" },
        { { 1.05f, 1.47f, 0.0f }, { 1, 0, 0 },  -350.0f, 350.0f, "J6 flange" },
    };
    constexpr float TCP_OFFSET = 0.19f;                  // flange -> TCP along the flange axis
    inline const glm::vec3 TCP_ZERO(1.05f + TCP_OFFSET, 1.47f, 0.0f);
    constexpr float DEG = 3.14159265f / 180.0f;

    struct Pose {
        Phisiks::Math::DualQuat frames[DOF];   // frame of each joint (after its rotation)
        Phisiks::Math::DualQuat tool;          // TCP frame (zero pose: axes = world axes; tool axis = +X)
        glm::vec3 Position(int i) const { return frames[i].Translation(); }
        glm::vec3 TcpPosition() const { return tool.Translation(); }
        glm::quat TcpRotation() const { return tool.Rotation(); }
    };

    inline Pose Forward(const Angles& q) {
        using Phisiks::Math::DualQuat;
        Pose pose;
        DualQuat t = DualQuat::Identity();
        glm::vec3 previous(0.0f);
        for (int i = 0; i < DOF; ++i) {
            t = (t * DualQuat::FromTranslation(JOINTS[i].position - previous) * DualQuat::FromRotation(glm::angleAxis(q[size_t(i)], JOINTS[i].axis))).Normalized();
            pose.frames[i] = t;
            previous = JOINTS[i].position;
        }
        pose.tool = (t * DualQuat::FromTranslation(TCP_ZERO - JOINTS[DOF - 1].position)).Normalized();
        return pose;
    }

    inline Angles ClampToLimits(Angles q) {
        for (int i = 0; i < DOF; ++i) q[size_t(i)] = std::clamp(q[size_t(i)], JOINTS[i].minDeg * DEG, JOINTS[i].maxDeg * DEG);
        return q;
    }

    // Rotation error as a vector (axis * angle) taking `from` to `to`.
    inline glm::vec3 RotationError(const glm::quat& from, const glm::quat& to) {
        glm::quat d = to * glm::conjugate(from);
        if (d.w < 0.0f) d = -d;
        const float s = std::sqrt(std::max(0.0f, 1.0f - d.w * d.w));
        if (s < 1e-7f) return glm::vec3(d.x, d.y, d.z) * 2.0f;
        return glm::vec3(d.x, d.y, d.z) / s * (2.0f * std::acos(std::clamp(d.w, -1.0f, 1.0f)));
    }

    struct IkResult {
        Angles q{};
        float positionError = 0.0f;   // m
        float angleError = 0.0f;      // rad
        int iterations = 0;
        bool converged = false;
    };

    // Damped least squares from `seed`. `useOrientation` = false solves position only (the angular
    // rows are dropped by zero weight).
    inline IkResult SolveIk(const Angles& seed, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool useOrientation = true,
                            int maxIterations = 60, float lambda = 0.02f) {
        IkResult r;
        r.q = ClampToLimits(seed);
        const double w = useOrientation ? 0.4 : 0.0;   // metres per radian in the error norm
        for (int it = 0; it < maxIterations; ++it) {
            const Pose pose = Forward(r.q);
            const glm::vec3 p = pose.TcpPosition();
            const glm::vec3 ep = targetPosition - p;
            const glm::vec3 ew = useOrientation ? RotationError(pose.TcpRotation(), targetRotation) : glm::vec3(0.0f);
            r.positionError = glm::length(ep);
            r.angleError = glm::length(ew);
            r.iterations = it;
            if (r.positionError < 2e-4f && (!useOrientation || r.angleError < 1e-3f)) { r.converged = true; break; }
            // Jacobian columns: revolute joint i with world axis a_i through o_i
            double J[6][DOF];
            for (int i = 0; i < DOF; ++i) {
                const glm::vec3 a = pose.frames[i].Rotation() * JOINTS[i].axis;
                const glm::vec3 o = pose.Position(i);
                const glm::vec3 v = glm::cross(a, p - o);
                J[0][i] = v.x; J[1][i] = v.y; J[2][i] = v.z;
                J[3][i] = a.x * w; J[4][i] = a.y * w; J[5][i] = a.z * w;
            }
            const double e[6] = { ep.x, ep.y, ep.z, ew.x * w, ew.y * w, ew.z * w };
            // (J^T J + lambda^2 I) dq = J^T e, Gaussian elimination with partial pivoting
            double A[DOF][DOF + 1];
            for (int i = 0; i < DOF; ++i) {
                for (int j = 0; j < DOF; ++j) {
                    double s = 0.0;
                    for (int k = 0; k < 6; ++k) s += J[k][i] * J[k][j];
                    A[i][j] = s + (i == j ? double(lambda) * lambda : 0.0);
                }
                double b = 0.0;
                for (int k = 0; k < 6; ++k) b += J[k][i] * e[k];
                A[i][DOF] = b;
            }
            for (int c = 0; c < DOF; ++c) {
                int piv = c;
                for (int rr = c + 1; rr < DOF; ++rr) if (std::abs(A[rr][c]) > std::abs(A[piv][c])) piv = rr;
                if (std::abs(A[piv][c]) < 1e-14) continue;
                for (int k = 0; k <= DOF; ++k) std::swap(A[c][k], A[piv][k]);
                for (int rr = 0; rr < DOF; ++rr) {
                    if (rr == c) continue;
                    const double f = A[rr][c] / A[c][c];
                    for (int k = c; k <= DOF; ++k) A[rr][k] -= f * A[c][k];
                }
            }
            double maxStep = 0.0;
            Angles dq{};
            for (int i = 0; i < DOF; ++i) {
                dq[size_t(i)] = std::abs(A[i][i]) > 1e-14 ? float(A[i][DOF] / A[i][i]) : 0.0f;
                maxStep = std::max(maxStep, double(std::abs(dq[size_t(i)])));
            }
            const float scale = maxStep > 0.25 ? float(0.25 / maxStep) : 1.0f;   // at most ~14 degrees per iteration
            for (int i = 0; i < DOF; ++i) r.q[size_t(i)] += dq[size_t(i)] * scale;
            r.q = ClampToLimits(r.q);
        }
        return r;
    }

    // Tool orientation for a top-down grasp: tool axis (+X) along world -Y, jaws opening along `jaw`.
    inline glm::quat DownwardTool(float yaw) {
        const glm::quat down = glm::angleAxis(-1.5707963f, glm::vec3(0, 0, 1));   // +X -> -Y
        return glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * down;
    }

    // Z-Y-X (yaw, pitch, roll) Euler angles of a rotation, degrees.
    inline glm::vec3 EulerZyxDegrees(const glm::quat& q) {
        const glm::mat3 m = glm::mat3_cast(q);
        const float pitch = std::asin(std::clamp(-m[0][2], -1.0f, 1.0f));
        const float yaw = std::atan2(m[0][1], m[0][0]);
        const float roll = std::atan2(m[1][2], m[2][2]);
        return glm::vec3(yaw, pitch, roll) / DEG;
    }
}
