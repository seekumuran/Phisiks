// Showcase 03 (robotics & mechanics): 6-DOF industrial arm and a rough-terrain rover.
//
//   * The arm is six physics bodies chained by motorised joints (turret yaw, shoulder / elbow
//     pitch, wrist roll / pitch hinges and a prismatic gripper slide). Links are gravity-compensated
//     (as real servo controllers do); every joint is a P-controller on the joint motor speed.
//   * Inverse kinematics: forward kinematics composed from unit dual quaternions
//     (Phisiks::Math::DualQuat), solved by damped least squares on position + tool direction.
//   * Obstacle avoidance: the transit path is pushed out of a signed distance field (SDF union of a
//     sensor dome and a gantry beam) to a safety margin; link clearance is measured every frame.
//   * The arm loads a crate onto a 6-wheel spring-damper rover, which drives over fBm heightmap
//     terrain (height-field collision) and returns. Joint torque / force readouts and replay
//     timeline scrubbing (|<  >  ||  >|) in the panel.
#include "Showcase.h"
#include "ShowcaseTest.h"
#include "Vehicle.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Math/DualQuat.h"
#include "Phisiks/Math/SDF.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/World/Terrain.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace Phisiks;
using Phisiks::Math::DualQuat;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr Physics::Layer ARM_LAYER = 5, CARGO_LAYER = 6, ROVER_LAYER = 7;   // the arm ignores itself, the crate and the rover
        constexpr int JOINTS = 6;   // 5 revolute + 1 prismatic
        const char* LINK_NAMES[JOINTS] = { "ArmTurret", "ArmUpper", "ArmFore", "ArmWristRoll", "ArmWristPitch", "ArmGripper" };
        // zero pose: everything stacked along +Y above the pedestal top (y = 0.4)
        const glm::vec3 JOINT_POS[JOINTS] = { { 0, 0.4f, 0 }, { 0, 0.8f, 0 }, { 0, 2.3f, 0 }, { 0, 3.6f, 0 }, { 0, 3.8f, 0 }, { 0, 4.05f, 0 } };
        const glm::vec3 JOINT_AXIS[JOINTS] = { { 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        const glm::vec3 LINK_CENTER[JOINTS] = { { 0, 0.6f, 0 }, { 0, 1.55f, 0 }, { 0, 2.95f, 0 }, { 0, 3.7f, 0 }, { 0, 3.925f, 0 }, { 0, 4.2f, 0 } };
        const glm::vec3 LINK_SIZE[JOINTS] = { { 0.7f, 0.4f, 0.7f }, { 0.26f, 1.5f, 0.3f }, { 0.22f, 1.3f, 0.24f }, { 0.2f, 0.2f, 0.2f }, { 0.18f, 0.25f, 0.18f }, { 0.12f, 0.3f, 0.12f } };
        const float LINK_MASS[JOINTS] = { 30.0f, 18.0f, 12.0f, 4.0f, 3.0f, 2.0f };
        const float TIP = 4.35f;    // gripper tip height in the zero pose
        const glm::vec3 PICKUP(-1.6f, 0.0f, 0.7f);
        const glm::vec3 DOCK(2.3f, 0.0f, 0.0f);
        const glm::vec3 DOME(0.3f, 1.7f, 1.4f);
        const float DOME_R = 0.45f;
        const glm::vec3 BEAM_A(-1.6f, 3.5f, 2.6f), BEAM_B(1.8f, 3.5f, 1.0f);

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }

        // Forward kinematics with dual quaternions: T = prod( Trans(offset_i) * Rot(axis_i, q_i) ).
        struct Fk { DualQuat frames[JOINTS + 1]; };   // frame i = after joint i; last = tool tip
        Fk Forward(const std::array<float, JOINTS>& q) {
            Fk fk;
            DualQuat t = DualQuat::Identity();
            glm::vec3 previous(0.0f);
            for (int i = 0; i < JOINTS; ++i) {
                const DualQuat offset = DualQuat::FromTranslation(JOINT_POS[i] - previous);
                const DualQuat motion = i == JOINTS - 1 ? DualQuat::FromTranslation(JOINT_AXIS[i] * q[size_t(i)])
                                                        : DualQuat::FromRotation(glm::angleAxis(q[size_t(i)], JOINT_AXIS[i]));
                t = (t * offset * motion).Normalized();
                fk.frames[i] = t;
                previous = JOINT_POS[i];
            }
            fk.frames[JOINTS] = (t * DualQuat::FromTranslation(glm::vec3(0.0f, TIP - JOINT_POS[JOINTS - 1].y, 0.0f))).Normalized();
            return fk;
        }

        // Solves A x = b (n x n, Gaussian elimination with partial pivoting).
        template<int N> bool Solve(std::array<std::array<double, N>, N> a, std::array<double, N> b, std::array<double, N>& x) {
            for (int c = 0; c < N; ++c) {
                int p = c;
                for (int r = c + 1; r < N; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
                if (std::abs(a[p][c]) < 1e-12) return false;
                std::swap(a[c], a[p]);
                std::swap(b[c], b[p]);
                for (int r = c + 1; r < N; ++r) {
                    const double f = a[r][c] / a[c][c];
                    for (int k = c; k < N; ++k) a[r][k] -= f * a[c][k];
                    b[r] -= f * b[c];
                }
            }
            for (int r = N - 1; r >= 0; --r) {
                double s = b[r];
                for (int k = r + 1; k < N; ++k) s -= a[r][k] * x[k];
                x[r] = s / a[r][r];
            }
            return true;
        }
    }

    class RoboticsScene : public ShowcaseScene {
    public:
        RoboticsScene() : ShowcaseScene("Robotics") {}

        std::array<float, JOINTS> target{}, ikSolution{};
        float ikError = 0.0f, trackingError = 0.0f, minClearance = 1e9f, runMinClearance = 1e9f;
        int stage = 0, deliveries = 0, worstLink = -1, runWorstLink = -1, runWorstStage = -1;
        float stageTime = 0.0f;
        bool gripped = false, autoRun = true, roverDriving = false;
        glm::vec3 eeTarget{ 0.0f, 2.5f, 1.5f }, stageFrom{ 0.0f }, stageTo{ 0.0f }, liftBase{ 0.0f };
        std::vector<glm::vec3> transit;
        Vehicle rover;
        int roverWaypoint = 0;
        float roverStartY = 0.0f, roverMaxClimb = 0.0f, roverDistance = 0.0f;
        glm::vec3 roverLast{ 0.0f };
        float holdTorque[JOINTS] = {};

        // ---------------------------------------------------------------- SDF obstacles
        static float Obstacles(const glm::vec3& p) {
            const float dome = Math::SDF::Sphere(p - DOME, DOME_R);
            const float beam = Math::SDF::Capsule(p, BEAM_A, BEAM_B, 0.12f);
            return Math::SDF::SmoothUnion(dome, beam, 0.2f);
        }

        void Build(GameContext& ctx) override {
            Material("Robot/Orange", { 0.95f, 0.42f, 0.05f }, 0.35f, 0.1f);
            Material("Robot/Dark", { 0.08f, 0.08f, 0.09f }, 0.45f, 0.8f);
            Material("Robot/Steel", { 0.6f, 0.62f, 0.65f }, 0.3f, 1.0f);
            Material("Robot/Pad", { 0.35f, 0.36f, 0.38f }, 0.8f, 0.0f, {}, Render::Pattern::Tiles, { 0.9f, 0.75f, 0.1f }, 1.0f, 0.04f);
            Material("Robot/Crate", { 0.15f, 0.5f, 0.25f }, 0.6f, 0.0f, {}, Render::Pattern::Grid, { 0.9f, 0.9f, 0.8f }, 2.0f, 0.06f);
            Material("Robot/Dome", { 0.2f, 0.6f, 0.9f }, 0.15f, 0.3f, { 0.05f, 0.25f, 0.5f });
            Material("Robot/Rover", { 0.85f, 0.85f, 0.8f }, 0.4f, 0.2f, {}, Render::Pattern::Grid, { 0.2f, 0.2f, 0.25f }, 2.0f, 0.03f);
            Material("Robot/Tyre", { 0.04f, 0.04f, 0.04f }, 0.9f);
            Render::RenderSettings& r = Render::Settings();
            r.skyColorZenith = { 0.12f, 0.25f, 0.55f };
            r.skyColorHorizon = { 0.6f, 0.65f, 0.7f };
            r.ambientUp = { 0.35f, 0.38f, 0.45f };
            r.fog.color = { 0.5f, 0.55f, 0.62f };
            r.fog.distanceDensity = 0.008f;
            Physics::SetLayersCollide(ARM_LAYER, ARM_LAYER, false);
            Physics::SetLayersCollide(ARM_LAYER, CARGO_LAYER, false);
            Physics::SetLayersCollide(ARM_LAYER, ROVER_LAYER, false);

            Sun(glm::vec3(-0.4f, -0.8f, -0.45f), glm::vec3(1.0f, 0.96f, 0.9f), 3.0f);
            Entity ground = CreateEntity("Terrain");
            Terrain& t = ground.Add<Terrain>();
            t.generator.resolution = 129;
            t.generator.cellSize = 0.6f;
            t.generator.height = 3.6f;
            t.generator.ridge = 0.3f;
            t.generator.seed = 11;
            t.generator.flatRadius = 7.0f;
            t.generator.flatBlend = 6.0f;
            Entity rocks = CreateEntity("Rocks");
            rocks.SetParent(ground, false);
            FoliageScatter& f = rocks.Add<FoliageScatter>();
            f.mesh = "Foliage/Rock";
            f.scatter.count = 260;
            f.scatter.keepOutRadius = 12.0f;
            f.scatter.alignToNormal = true;
            f.scatter.maxSlopeDegrees = 45.0f;
            World::RegisterFoliageMeshes();
            // work cell
            Visual("WorkPad", "Showcase/Cylinder", "Robot/Pad", { 0.0f, 0.02f, 0.0f }, { 9.0f, 0.04f, 9.0f });
            Box("Pedestal", { 0.0f, 0.2f, 0.0f }, { 0.9f, 0.4f, 0.9f }, "Robot/Dark");
            Box("PickupTable", PICKUP + glm::vec3(0.0f, 0.35f, 0.0f), { 0.9f, 0.7f, 0.9f }, "Robot/Steel");
            Visual("SensorDome", "Sphere", "Robot/Dome", DOME, glm::vec3(DOME_R * 2.0f));
            Visual("SensorMast", "Showcase/Cylinder", "Robot/Steel", { DOME.x, DOME.y * 0.5f, DOME.z }, { 0.1f, DOME.y, 0.1f });
            Visual("GantryBeam", "Showcase/Cylinder", "Robot/Steel", (BEAM_A + BEAM_B) * 0.5f, { 0.24f, glm::length(BEAM_B - BEAM_A), 0.24f },
                   glm::quat(glm::vec3(0, 1, 0), glm::normalize(BEAM_B - BEAM_A)));
            Visual("GantryLegA", "Showcase/Cylinder", "Robot/Steel", { BEAM_A.x, BEAM_A.y * 0.5f, BEAM_A.z }, { 0.18f, BEAM_A.y, 0.18f });
            Visual("GantryLegB", "Showcase/Cylinder", "Robot/Steel", { BEAM_B.x, BEAM_B.y * 0.5f, BEAM_B.z }, { 0.18f, BEAM_B.y, 0.18f });
            // arm links (dynamic, gravity-compensated) chained by joints
            std::string parent = "Pedestal";
            for (int i = 0; i < JOINTS; ++i) {
                Entity link = Box(LINK_NAMES[i], LINK_CENTER[i], LINK_SIZE[i], i % 2 ? "Robot/Orange" : "Robot/Dark", LINK_MASS[i]);
                link.Get<Collider>().layer = ARM_LAYER;
                Rigidbody& rb = link.Get<Rigidbody>();
                rb.useGravity = false;
                rb.angularDamping = 2.0f;
                rb.linearDamping = 0.5f;
                Joint& j = link.Add<Joint>();
                j.type = i == JOINTS - 1 ? Physics::JointType::Prismatic : Physics::JointType::Hinge;
                j.connectedTo = parent;
                j.anchor = (JOINT_POS[i] - LINK_CENTER[i]) / LINK_SIZE[i];   // local units: scaled by the link size
                j.axis = JOINT_AXIS[i];
                j.enableMotor = true;
                j.maxMotorForce = i == JOINTS - 1 ? 200.0f : 600.0f;
                if (i == JOINTS - 1) { j.enableLimits = true; j.lowerLimit = -0.05f; j.upperLimit = 0.25f; }
                parent = LINK_NAMES[i];
            }
            Pedestal().Get<Collider>().layer = ARM_LAYER;
            SpawnCargo();
            // rover: 6 wheels on independent spring-dampers
            Entity chassis = Box("Rover", DOCK + glm::vec3(0.0f, 1.0f, 0.0f), { 1.6f, 0.4f, 2.6f }, "Robot/Rover", 320.0f);
            chassis.Get<Rigidbody>().angularDamping = 0.8f;
            chassis.Get<Collider>().layer = ROVER_LAYER;
            const glm::vec3 cs(1.6f, 0.4f, 2.6f);   // children are in the chassis' scaled local units
            Visual("RoverBedL", "Cube", "Robot/Dark", glm::vec3(-0.75f, 0.35f, 0.0f) / cs, glm::vec3(0.08f, 0.3f, 2.4f) / cs).SetParent(chassis, false);
            Visual("RoverBedR", "Cube", "Robot/Dark", glm::vec3(0.75f, 0.35f, 0.0f) / cs, glm::vec3(0.08f, 0.3f, 2.4f) / cs).SetParent(chassis, false);
            Visual("RoverMast", "Showcase/Cylinder", "Robot/Steel", glm::vec3(0.0f, 0.75f, -1.1f) / cs, glm::vec3(0.08f, 1.1f, 0.08f) / cs).SetParent(chassis, false);
            std::vector<WheelDesc> wheels;
            for (int i = 0; i < 6; ++i) {
                WheelDesc w;
                w.mount = glm::vec3(i % 2 ? 0.95f : -0.95f, -0.05f, -1.0f + 1.0f * float(i / 2));
                w.radius = 0.42f;
                w.steer = i < 2;   // front-wheel steering
                w.visual = "RoverWheel" + std::to_string(i);
                wheels.push_back(w);
                Visual(w.visual, "Showcase/Cylinder", "Robot/Tyre", chassis.WorldPosition() + w.mount, { 0.84f, 0.3f, 0.84f });
            }
            VehicleDesc vd;
            vd.restLength = 0.55f;
            vd.stiffness = 9000.0f;
            vd.damping = 900.0f;
            vd.engineForce = 3600.0f;
            vd.brakeForce = 4000.0f;
            vd.maxSpeed = 4.5f;
            vd.lateralStiffness = 3000.0f;
            vd.friction = 1.3f;
            vd.rollLift = 0.15f;
            vd.groundLayers = Physics::ALL_LAYERS & ~Physics::LayerBit(ARM_LAYER) & ~Physics::LayerBit(CARGO_LAYER);   // the wheels never stand on the arm or the crate
            rover.Setup(chassis, vd, wheels);
            stage = 0;
            stageTime = 0.0f;
            for (int i = 0; i < JOINTS; ++i) target[size_t(i)] = ikSolution[size_t(i)] = 0.0f;
            ctx.camera.SetOrbit(glm::vec3(0.5f, 1.6f, 0.5f), 9.0f, 0.6f, -0.3f);
        }

        Entity Pedestal() { return FindByName("Pedestal"); }

        void SpawnCargo() {
            if (Entity old = FindByName("Cargo"); old.Valid()) old.Destroy();
            Entity cargo = Box("Cargo", PICKUP + glm::vec3(0.0f, 0.92f, 0.0f), glm::vec3(0.42f), "Robot/Crate", 8.0f);
            cargo.Get<Collider>().shape.material.staticFriction = 1.0f;
            cargo.Get<Collider>().shape.material.dynamicFriction = 0.9f;
            cargo.Get<Collider>().layer = CARGO_LAYER;   // never collides with the arm links (gripper overlap)
            gripped = false;
        }

        // ---------------------------------------------------------------- IK (damped least squares)
        glm::vec3 TipPosition(const std::array<float, JOINTS>& q) const { return Forward(q).frames[JOINTS].Translation(); }
        glm::vec3 ToolAxis(const std::array<float, JOINTS>& q) const { return Forward(q).frames[JOINTS].Rotation() * glm::vec3(0, 1, 0); }

        // Sample points of the arm (links 1-4, plus the carried crate) in the FK model.
        static void ArmPoints(const std::array<float, JOINTS>& q, bool carrying, std::vector<glm::vec3>& points, std::vector<float>& radii) {
            const Fk fk = Forward(q);
            points.clear();
            radii.clear();
            for (int i = 1; i <= 4; ++i) {
                const glm::vec3 centre = LINK_CENTER[i] - JOINT_POS[i];
                for (int k = -2; k <= 2; ++k) {
                    points.push_back(fk.frames[i].TransformPoint(centre + glm::vec3(0.0f, LINK_SIZE[i].y * 0.25f * float(k), 0.0f)));
                    radii.push_back(0.15f);
                }
            }
            if (carrying) {
                points.push_back(fk.frames[JOINTS].Translation() + glm::vec3(0.0f, -0.21f, 0.0f));
                radii.push_back(0.3f);
            }
        }

        // Damped least squares, task-space rows: tip position (3) + tool direction (3) + one row per
        // arm / crate sample point closer than the margin to the SDF obstacles (pushed along the
        // gradient). Forward kinematics are composed from unit dual quaternions.
        void SolveIk(const glm::vec3& goal, const glm::vec3& toolDirection, float extension) {
            constexpr float MARGIN = 0.16f;   // beyond the link / crate radius
            std::array<float, JOINTS> q = ikSolution;
            q[JOINTS - 1] = extension;
            std::vector<glm::vec3> points, pointsDq;
            std::vector<float> radii, radiiDq;
            for (int iteration = 0; iteration < 12; ++iteration) {
                const glm::vec3 p = TipPosition(q), d = ToolAxis(q);
                std::vector<std::array<double, 5>> jac;
                std::vector<double> e = { goal.x - p.x, goal.y - p.y, goal.z - p.z, 0.5 * (toolDirection.x - d.x), 0.5 * (toolDirection.y - d.y), 0.5 * (toolDirection.z - d.z) };
                jac.resize(6);
                ArmPoints(q, gripped, points, radii);
                std::vector<size_t> active;
                std::vector<glm::vec3> normals;
                for (size_t k = 0; k < points.size(); ++k) {
                    const float dist = Obstacles(points[k]) - radii[k];
                    if (dist >= MARGIN) continue;
                    active.push_back(k);
                    normals.push_back(Math::SDF::Normal(Obstacles, points[k]));
                    e.push_back(2.0 * (MARGIN - dist));   // avoidance is weighted over the tip task
                    jac.push_back({});
                }
                for (int j = 0; j < 5; ++j) {
                    std::array<float, JOINTS> qq = q;
                    qq[size_t(j)] += 1e-3f;
                    const glm::vec3 dp = (TipPosition(qq) - p) / 1e-3f, dd = (ToolAxis(qq) - d) / 1e-3f * 0.5f;
                    const double col[6] = { dp.x, dp.y, dp.z, dd.x, dd.y, dd.z };
                    for (int r = 0; r < 6; ++r) jac[size_t(r)][size_t(j)] = col[r];
                    if (!active.empty()) {
                        ArmPoints(qq, gripped, pointsDq, radiiDq);
                        for (size_t a = 0; a < active.size(); ++a)
                            jac[6 + a][size_t(j)] = 2.0 * glm::dot(normals[a], (pointsDq[active[a]] - points[active[a]]) / 1e-3f);
                    }
                }
                // dq = J^T (J J^T + lambda^2 I)^-1 e
                const size_t m = e.size();
                std::vector<std::vector<double>> a(m, std::vector<double>(m + 1, 0.0));
                for (size_t r = 0; r < m; ++r) {
                    for (size_t c = 0; c < m; ++c) {
                        double sum = r == c ? 0.01 : 0.0;
                        for (int k = 0; k < 5; ++k) sum += jac[r][size_t(k)] * jac[c][size_t(k)];
                        a[r][c] = sum;
                    }
                    a[r][m] = e[r];
                }
                bool ok = true;
                for (size_t c = 0; c < m && ok; ++c) {   // Gaussian elimination (augmented matrix)
                    size_t piv = c;
                    for (size_t r = c + 1; r < m; ++r) if (std::abs(a[r][c]) > std::abs(a[piv][c])) piv = r;
                    if (std::abs(a[piv][c]) < 1e-12) { ok = false; break; }
                    std::swap(a[c], a[piv]);
                    for (size_t r = c + 1; r < m; ++r) {
                        const double f = a[r][c] / a[c][c];
                        for (size_t k = c; k <= m; ++k) a[r][k] -= f * a[c][k];
                    }
                }
                if (!ok) break;
                std::vector<double> y(m, 0.0);
                for (size_t r = m; r-- > 0;) {
                    double sum = a[r][m];
                    for (size_t k = r + 1; k < m; ++k) sum -= a[r][k] * y[k];
                    y[r] = sum / a[r][r];
                }
                for (int j = 0; j < 5; ++j) {
                    double dq = 0.0;
                    for (size_t r = 0; r < m; ++r) dq += jac[r][size_t(j)] * y[r];
                    q[size_t(j)] += float(std::clamp(dq, -0.3, 0.3));
                }
                q[1] = std::clamp(q[1], -2.2f, 2.2f);
                q[2] = std::clamp(q[2], -2.6f, 2.6f);
                q[4] = std::clamp(q[4], -2.2f, 2.2f);
            }
            ikSolution = q;
            ikError = glm::length(TipPosition(q) - goal);
            target = q;
        }

        // Transit path around the base, pushed out of the SDF obstacles to a 0.35 m margin.
        void PlanTransit(const glm::vec3& from, const glm::vec3& to) {
            transit.clear();
            const float a0 = std::atan2(from.z, from.x), a1 = std::atan2(to.z, to.x);
            float da = a1 - a0;
            if (da > 3.14159f) da -= 6.2831853f;
            if (da < -3.14159f) da += 6.2831853f;
            const float r0 = glm::length(glm::vec2(from.x, from.z)), r1 = glm::length(glm::vec2(to.x, to.z));
            for (int i = 0; i <= 40; ++i) {
                const float t = float(i) / 40.0f;
                const float a = a0 + da * t, r = glm::mix(r0, r1, t);
                glm::vec3 p(std::cos(a) * r, glm::mix(from.y, to.y, t) + 0.25f * std::sin(t * 3.14159f), std::sin(a) * r);
                for (int k = 0; k < 8; ++k) {   // sphere-trace style push-out along the SDF gradient
                    const float dist = Obstacles(p);
                    if (dist >= 0.5f) break;   // tool tip margin (the IK keeps the links and the crate clear)
                    p += Math::SDF::Normal(Obstacles, p) * (0.5f - dist + 0.01f);
                }
                transit.push_back(p);
            }
        }

        glm::vec3 RoverBed() const {
            const Entity c = rover.Chassis();
            return c.WorldPosition() + c.WorldRotation() * glm::vec3(0.0f, 0.52f, 0.2f);
        }

        void SetStage(int s) { stage = s; stageTime = 0.0f; }

        void Tick(GameContext& ctx, float dt) override {
            Entity cargo = FindByName("Cargo");
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            if (autoRun) stageTime += dt;
            const glm::vec3 grasp = cargo.Valid() ? cargo.WorldPosition() + glm::vec3(0.0f, 0.21f, 0.0f) : PICKUP + glm::vec3(0, 1.1f, 0);
            float extension = gripped ? 0.08f : 0.0f;
            auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
            switch (stage) {
                case 0:   // above the cargo
                    eeTarget = glm::mix(glm::vec3(0.0f, 2.6f, 1.2f), grasp + glm::vec3(0, 0.6f, 0), ease(stageTime / 2.5f));
                    if (stageTime > 3.0f) SetStage(1);
                    break;
                case 1:   // descend
                    eeTarget = grasp + glm::vec3(0, 0.6f * (1.0f - ease(stageTime / 1.5f)), 0);
                    if (stageTime > 1.8f) SetStage(2);
                    break;
                case 2:   // grip (slide out and attach)
                    eeTarget = grasp;
                    extension = 0.08f * ease(stageTime / 0.5f);
                    if (stageTime > 0.6f && cargo.Valid() && glm::length(Tip() - grasp) < 0.12f) {
                        gripped = true;
                        liftBase = grasp;
                        cargo.Get<Rigidbody>().type = Physics::BodyType::Kinematic;
                        cargo.MarkPhysicsDirty();
                        SetStage(3);
                    }
                    else if (stageTime > 3.0f) SetStage(0);
                    break;
                case 3:   // lift, plan the transit
                    eeTarget = liftBase + glm::vec3(0, 0.8f * ease(stageTime / 1.2f), 0);
                    if (stageTime > 1.4f) {
                        stageFrom = eeTarget;
                        stageTo = RoverBed() + glm::vec3(0.0f, 0.6f, 0.0f);
                        PlanTransit(stageFrom, stageTo);
                        SetStage(4);
                    }
                    break;
                case 4: {  // transit along the SDF-safe path
                    const float u = ease(stageTime / 4.0f) * float(transit.size() - 1);
                    const size_t i = std::min(size_t(u), transit.size() - 2);
                    eeTarget = glm::mix(transit[i], transit[i + 1], u - float(i));
                    if (stageTime > 4.3f) SetStage(5);
                    break;
                }
                case 5:   // lower into the rover bed
                    eeTarget = RoverBed() + glm::vec3(0.0f, 0.6f - 0.46f * ease(stageTime / 1.5f), 0.0f);   // crate ~2 cm above the bed
                    if (stageTime > 1.7f) SetStage(6);
                    break;
                case 6:   // release
                    eeTarget = RoverBed() + glm::vec3(0.0f, 0.14f, 0.0f);
                    if (gripped && cargo.Valid()) {
                        gripped = false;
                        cargo.Get<Rigidbody>().type = Physics::BodyType::Dynamic;
                        cargo.MarkPhysicsDirty();
                        ++deliveries;
                        ctx.PushEvent("Cargo loaded onto the rover");
                    }
                    extension = 0.08f * (1.0f - ease(stageTime / 0.4f));
                    if (stageTime > 1.0f) { PlanTransit(eeTarget, glm::vec3(0.0f, 2.6f, 1.2f)); SetStage(7); }
                    break;
                case 7: {  // retract along an SDF-safe path, the rover sets off
                    const float u = ease(stageTime / 2.5f) * float(transit.size() - 1);
                    const size_t i = std::min(size_t(u), transit.size() - 2);
                    eeTarget = glm::mix(transit[i], transit[i + 1], u - float(i));
                    if (stageTime > 2.7f) {
                        roverDriving = true;
                        roverWaypoint = 0;
                        roverStartY = rover.Chassis().WorldPosition().y;
                        roverLast = rover.Chassis().WorldPosition();
                        SetStage(8);
                    }
                    break;
                }
                default:  // idle until the rover is back; then the next cargo
                    eeTarget = glm::vec3(0.0f, 2.6f, 1.2f);
                    if (!roverDriving && stageTime > 1.0f) { SpawnCargo(); SetStage(0); }
                    break;
            }
            SolveIk(eeTarget, down, extension);
            if (gripped && cargo.Valid()) cargo.SetWorldPose(Tip() - glm::vec3(0.0f, 0.21f, 0.0f), glm::angleAxis(target[0], glm::vec3(0, 1, 0)));
            rover.UpdateVisuals(dt);
            // clearance of the links against the SDF (sampled along each link)
            minClearance = 1e9f;
            for (int i = 1; i < JOINTS; ++i) {
                Entity link = FindByName(LINK_NAMES[i]);
                if (!link.Valid()) continue;
                const glm::vec3 axis = link.WorldRotation() * glm::vec3(0, 1, 0);
                for (int k = -2; k <= 2; ++k) { const float cl = Obstacles(link.WorldPosition() + axis * (LINK_SIZE[i].y * 0.5f * float(k) / 2.0f)) - 0.15f; if (cl < minClearance) { minClearance = cl; worstLink = i; } }
            }
            if (Entity cargoNow = FindByName("Cargo"); gripped && cargoNow.Valid()) { const float cl = Obstacles(cargoNow.WorldPosition()) - 0.3f; if (cl < minClearance) { minClearance = cl; worstLink = 9; } }
            if (minClearance < runMinClearance) { runMinClearance = minClearance; runWorstLink = worstLink; runWorstStage = stage; }
            // holding torques (what the motors would hold against gravity without compensation)
            for (int j = 0; j < JOINTS; ++j) {
                Entity jl = FindByName(LINK_NAMES[j]);
                if (!jl.Valid()) continue;
                const glm::vec3 axisW = jl.WorldRotation() * JOINT_AXIS[j];
                const glm::vec3 pivot = jl.WorldPosition() + jl.WorldRotation() * (JOINT_POS[j] - LINK_CENTER[j]);
                float tau = 0.0f;
                for (int k = j; k < JOINTS; ++k) {
                    Entity l = FindByName(LINK_NAMES[k]);
                    if (!l.Valid()) continue;
                    const glm::vec3 weight(0.0f, -9.81f * LINK_MASS[k], 0.0f);
                    tau += j == JOINTS - 1 ? glm::dot(weight, axisW) : glm::dot(glm::cross(l.WorldPosition() - pivot, weight), axisW);
                }
                if (gripped && cargo.Valid()) {
                    const glm::vec3 weight(0.0f, -9.81f * 8.0f, 0.0f);
                    tau += j == JOINTS - 1 ? glm::dot(weight, axisW) : glm::dot(glm::cross(cargo.WorldPosition() - pivot, weight), axisW);
                }
                holdTorque[j] = tau;
            }
        }

        glm::vec3 Tip() {
            Entity g = FindByName(LINK_NAMES[JOINTS - 1]);
            return g.Valid() ? g.WorldPosition() + g.WorldRotation() * glm::vec3(0.0f, 0.15f, 0.0f) : glm::vec3(0.0f);
        }

        void FixedTick(GameContext&, float dt) override {
            // joint servos: motor speed = Kp * error (clamped)
            trackingError = 0.0f;
            for (int i = 0; i < JOINTS; ++i) {
                Entity link = FindByName(LINK_NAMES[i]);
                if (!link.Valid()) continue;
                const Joint& j = link.Get<Joint>();
                if (!Physics::IsValid(j.runtime)) continue;
                const Physics::JointState s = Physics::GetJointState(j.runtime);
                const float error = target[size_t(i)] - s.position;
                trackingError = std::max(trackingError, std::abs(error));
                const float vmax = i == JOINTS - 1 ? 0.6f : 1.6f;
                Physics::SetJointMotor(j.runtime, true, std::clamp(error * 9.0f, -vmax, vmax), i == JOINTS - 1 ? 200.0f : 600.0f);
            }
            // rover autopilot over the terrain loop
            if (roverDriving) {
                static const glm::vec3 route[] = { { 5.0f, 0, -9.0f }, { 13.0f, 0, -14.0f }, { 19.0f, 0, -4.0f }, { 14.0f, 0, 8.0f }, { 4.0f, 0, 10.0f }, DOCK + glm::vec3(0.0f, 0.0f, 5.0f), DOCK };
                const int count = int(sizeof(route) / sizeof(route[0]));
                const glm::vec3 p = rover.Chassis().WorldPosition();
                glm::vec3 fwd = rover.Chassis().WorldRotation() * glm::vec3(0, 0, -1);
                const glm::vec3 to = route[roverWaypoint] - p;
                const float heading = std::atan2(fwd.x, -fwd.z), want = std::atan2(to.x, -to.z);
                float diff = want - heading;
                while (diff > 3.14159f) diff -= 6.2831853f;
                while (diff < -3.14159f) diff += 6.2831853f;
                const float distance = glm::length(glm::vec2(to.x, to.z));
                const bool last = roverWaypoint == count - 1;
                const float throttle = last ? std::clamp(distance * 0.4f, 0.0f, 0.6f) : 0.85f;
                rover.FixedUpdate(dt, throttle, std::clamp(diff * 1.8f, -1.0f, 1.0f), last && distance < 0.6f ? 1.0f : 0.0f);
                roverDistance += glm::length(p - roverLast);
                roverLast = p;
                roverMaxClimb = std::max(roverMaxClimb, p.y - roverStartY);
                if (distance < (last ? 0.5f : 2.5f)) {
                    if (last) { roverDriving = false; Entity c = FindByName("Cargo"); if (c.Valid()) c.Destroy(); }
                    else ++roverWaypoint;
                }
            }
            else rover.FixedUpdate(dt, 0.0f, 0.0f, 1.0f);
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            const glm::vec3 r = rover.Chassis().WorldPosition();
            return {
                OrbitShot("Arm orbit", { 0.0f, 1.8f, 0.5f }, 6.5f, 1.2f, 16.0f),
                DollyShot("Gripper close-up", { -3.6f, 2.4f, 2.6f }, { -1.0f, 2.2f, 3.2f }, { -1.0f, 1.6f, 0.6f }, 8.0f),
                DollyShot("Obstacle pass", { 2.5f, 3.6f, 4.2f }, { -1.5f, 3.4f, 4.6f }, DOME, 9.0f),
                DollyShot("Rover chase", r + glm::vec3(-6.0f, 3.0f, 6.0f), r + glm::vec3(6.0f, 4.0f, 6.0f), r, 10.0f),
                OrbitShot("Terrain flyover", { 12.0f, 0.0f, -6.0f }, 26.0f, 14.0f, 26.0f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            Render::SdfPrimitive dome;
            dome.shape = Render::SdfShape::Sphere;
            dome.position = DOME;
            dome.params = glm::vec4(DOME_R + 0.35f, 0, 0, 0);   // inflated by the safety margin
            sdf.primitives.push_back(dome);
            Render::SdfPrimitive beam;
            beam.shape = Render::SdfShape::Capsule;
            const glm::vec3 a = BEAM_A, b = BEAM_B;
            beam.position = (a + b) * 0.5f;
            beam.rotation = glm::quat(glm::vec3(0, 1, 0), glm::normalize(b - a));
            beam.params = glm::vec4(glm::length(b - a) * 0.5f, 0.12f + 0.35f, 0, 0);
            beam.smoothness = 0.2f;
            sdf.primitives.push_back(beam);
            sdf.boundsMin = glm::vec3(-3.0f, 0.5f, -1.0f);
            sdf.boundsMax = glm::vec3(3.0f, 4.2f, 4.0f);
            sdf.color = glm::vec3(1.0f, 0.3f, 0.2f);
            sdf.roughness = 0.6f;
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(0.0f, 1.0f, 0.0f, 2.0f);
            // the planned transit as a polyline
            for (size_t i = 1; i < transit.size(); ++i) Render::DrawLine(transit[i - 1], transit[i], glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.5f, 0.0f, 0.5f, 5.0f); }
        const char* Controls() const override { return "RMB orbit, MMB pan, wheel zoom\nTimeline: pause, scrub, step, resume"; }

        void DrawPanel(GameContext& ctx) override {
            static const char* stages[] = { "APPROACH", "DESCEND", "GRIP", "LIFT", "TRANSIT", "LOWER", "RELEASE", "RETRACT", "ROVER" };
            char big[32], small[96];
            std::snprintf(big, sizeof(big), "%s", stages[std::min(stage, 8)]);
            std::snprintf(small, sizeof(small), "IK %.1f mm  track %.3f  clear %.2f m", double(ikError * 1000.0f), double(trackingError), double(minClearance));
            Retro::Lcd(big, small);
            static const char* names[JOINTS] = { "J1 YAW", "J2 SHLD", "J3 ELBW", "J4 ROLL", "J5 PTCH", "J6 SLID" };
            for (int i = 0; i < JOINTS; ++i) {
                Entity link = FindByName(LINK_NAMES[i]);
                Physics::JointState s;
                if (link.Valid() && Physics::IsValid(link.Get<Joint>().runtime)) s = Physics::GetJointState(link.Get<Joint>().runtime);
                char v[64];
                if (i == JOINTS - 1) std::snprintf(v, sizeof(v), "%+.3f m  %+.0f N", double(s.position), double(holdTorque[i]));
                else std::snprintf(v, sizeof(v), "%+5.1f deg %+6.0f Nm", double(glm::degrees(s.position)), double(holdTorque[i]));
                Retro::LedMeter(names[i], std::abs(holdTorque[i]) / (i == JOINTS - 1 ? 200.0f : 900.0f), v);
            }
            ImGui::TextDisabled("torque: gravity load the servo holds (links are gravity-compensated)");
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "deliveries %d  rover %.1f m driven, climbed %.2f m", deliveries, double(roverDistance), double(roverMaxClimb));
            Retro::LedToggle("AUTO", &autoRun);
            ImGui::SameLine();
            if (Retro::Button("NEXT CARGO")) { SpawnCargo(); SetStage(0); }
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.3f, 1), "TIMELINE");
            TimelineControls(ctx);
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("arm picks the cargo and loads the rover", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Frame() % 45 == 0) t.Screenshot("arm_" + std::to_string(t.Frame() / 45));
                t.Value("maxIk") = std::max(t.Value("maxIk"), double(stage == 1 || stage == 2 || stage == 5 || stage == 6 ? ikError : 0.0f));
                if (stage == 4) t.Value("transitIk") = std::max(t.Value("transitIk"), double(ikError));
                if (deliveries == 0 || stage < 8) return false;
                Entity cargo = FindByName("Cargo");
                const glm::vec3 bed = RoverBed();
                const bool onRover = cargo.Valid() && glm::length(glm::vec2(cargo.WorldPosition().x - bed.x, cargo.WorldPosition().z - bed.z)) < 0.9f && cargo.WorldPosition().y > bed.y - 0.3f;
                t.Expect("6 motorised joints created (5 hinges + prismatic)", Physics::GetStats().joints >= 6, Fmt("%.0f joints", Physics::GetStats().joints));
                t.Expect("dual-quaternion IK converges along the task", t.Value("maxIk") < 0.03,
                         Fmt("worst residual %.1f mm while gripping / placing, %.1f mm in transit (avoidance rows take priority)", t.Value("maxIk") * 1000.0, t.Value("transitIk") * 1000.0));
                t.Expect("cargo gripped, carried and released onto the rover bed", onRover, Fmt("cargo %.2f m from the bed centre, %.0f deliveries, ", cargo.Valid() ? glm::length(cargo.WorldPosition() - bed) : -1.0, deliveries) + Fmt("cargo at (%.2f, %.2f, ", cargo.Valid() ? cargo.WorldPosition().x : 0.0, cargo.Valid() ? cargo.WorldPosition().y : 0.0) + Fmt("%.2f), bed at (%.2f, %.2f, ", cargo.Valid() ? cargo.WorldPosition().z : 0.0, bed.x, bed.y) + Fmt("%.2f)", bed.z));
                t.Expect("SDF obstacle avoidance keeps every link clear", runMinClearance > 0.0f, Fmt("minimum link / cargo clearance %.3f m (dome + gantry beam; worst: link %.0f in stage ", runMinClearance, runWorstLink) + Fmt("%.0f)", runWorstStage));
                (void)c;
                return true;
            }, 60.0);
            test.AddStep("replay timeline scrubbing", [this](GameContext& c, ShowcaseTest& t) {
                Replay& replay = c.replay;
                if (!replay.Enabled()) { t.Expect("replay recording", false, "recording disabled"); return true; }
                if (t.Phase() == 0) {
                    if (replay.Size() < 120) return false;
                    c.state = GameState::Paused;
                    const size_t index = replay.Size() - 90;
                    replay.Restore(*this, index);
                    t.Value("index") = double(index);
                    t.Phase() = 1;
                    return false;
                }
                const ReplaySnapshot& snap = replay.At(size_t(t.Value("index")));
                Entity upper = FindByName("ArmUpper");
                float worst = 0.0f;
                for (const ReplayEntityState& e : snap.entities) {
                    Entity ent = FromId(e.entity);
                    if (ent.Valid() && ent.Has<Joint>()) worst = std::max(worst, glm::length(ent.Get<Transform>().position - e.position));
                }
                t.Expect("paused + restored 90 steps back: arm links match the snapshot", c.state == GameState::Paused && upper.Valid() && worst < 1e-4f,
                         Fmt("%.0f snapshots (%.1f MB), worst link error %.6f m", double(replay.Size()), double(replay.MemoryBytes()) / 1048576.0, worst));
                replay.ResumeFromScrub();
                c.state = GameState::Running;
                return true;
            }, 30.0);
            test.AddStep("rover drives the terrain loop with the cargo", [this](GameContext& c, ShowcaseTest& t) {
                if (roverDriving && t.StepSeconds(c) < 40.0) {
                    if (t.Frame() % 90 == 0) t.Screenshot("rover_" + std::to_string(t.Frame() / 90));
                    return false;
                }
                const World::Stats w = World::GetStats();
                t.Expect("rover over the fBm terrain (height-field collision)", roverDistance > 40.0f && roverMaxClimb > 0.4f && w.terrains == 1,
                         Fmt("%.1f m driven, climbed %.2f m, %.0f rocks scattered", roverDistance, roverMaxClimb, w.foliageInstances));
                t.Expect("rover returned to the dock", !roverDriving, Fmt("waypoint %.0f, %.1f s", roverWaypoint, t.StepSeconds(c)));
                return true;
            }, 60.0);
        }

        void OnExit(GameContext& ctx) override {
            Physics::SetLayersCollide(ARM_LAYER, ARM_LAYER, true);
            Physics::SetLayersCollide(ARM_LAYER, CARGO_LAYER, true);
            Physics::SetLayersCollide(ARM_LAYER, ROVER_LAYER, true);
            ShowcaseScene::OnExit(ctx);
        }
    };

    void RegisterRobotics(SceneManager& scenes) { scenes.Register("Robotics", [] { return std::make_unique<RoboticsScene>(); }); }
}
