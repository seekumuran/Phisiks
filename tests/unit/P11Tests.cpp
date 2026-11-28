// P11 hero showcases: 6-DOF arm kinematics (dual-quaternion FK, DLS IK), pool water (Snell
// refraction, sphere cap volume, wave volume conservation, caustic energy) and the black-hole
// disk integrators (Euler drift vs Verlet / RK4), plus the see-through material default.
#include "TestFramework.h"

#include "01_robotics_6dof_arm/ArmKinematics.h"
#include "02_vfx_pool_fluid/PoolSim.h"
#include "03_science_black_hole/DiskSim.h"
#include "Phisiks/Render/Renderer.h"

#include <cmath>

using namespace Showcase;

TEST_CASE("Arm: zero pose FK and J1 rotation") {
    const Arm::Pose zero = Arm::Forward(Arm::Angles{});
    CHECK_NEAR(zero.TcpPosition().x, 1.24, 1e-4);
    CHECK_NEAR(zero.TcpPosition().y, 1.47, 1e-4);
    CHECK_NEAR(zero.Position(4).x, 0.95, 1e-5);   // wrist centre
    Arm::Angles q{};
    q[0] = 90.0f * Arm::DEG;
    const glm::vec3 p = Arm::Forward(q).TcpPosition();
    CHECK_NEAR(p.z, -1.24, 1e-3);
    CHECK_NEAR(p.x, 0.0, 1e-3);
}

TEST_CASE("Arm: DLS IK reaches a pose and a top-down grasp") {
    Arm::Angles goal = { 0.4f, 0.3f, -0.5f, 0.2f, 1.1f, -0.3f };
    const Arm::Pose target = Arm::Forward(goal);
    const Arm::IkResult r = Arm::SolveIk(Arm::Angles{}, target.TcpPosition(), target.TcpRotation(), true, 150);
    CHECK(r.positionError < 1e-3f);
    CHECK(r.angleError < 2e-3f);
    const glm::vec3 grasp(0.92f, 0.62f, 0.52f);
    const Arm::IkResult g = Arm::SolveIk({ 0.0f, 0.15f, -0.35f, 0.0f, 1.25f, 0.0f }, grasp, Arm::DownwardTool(std::atan2(-grasp.z, grasp.x)), true, 150);
    CHECK(g.converged);
    const glm::vec3 toolAxis = Arm::Forward(g.q).TcpRotation() * glm::vec3(1, 0, 0);
    CHECK_NEAR(toolAxis.y, -1.0, 2e-3);
}

TEST_CASE("Pool: Snell refraction, cap volume, volume-conserving waves") {
    const glm::vec3 in = glm::normalize(glm::vec3(0.5f, -0.8660254f, 0.0f));   // 30 deg incidence
    const glm::vec3 out = Pool::Refract(in, glm::vec3(0, 1, 0), 1.0f / Pool::WATER_IOR);
    CHECK_NEAR(0.5 / (out.x / glm::length(out)), Pool::WATER_IOR, 1e-4);
    const float r = 0.35f;
    CHECK_NEAR(Pool::CapVolume(r, r), 2.0 / 3.0 * Pool::PI * r * r * r, 1e-6);
    CHECK_NEAR(Pool::CapVolume(r, 2.0f * r), 4.0 / 3.0 * Pool::PI * r * r * r, 1e-6);
    Pool::HeightField w;
    w.Reset(64, 2.0f, 1.2f, 0.25f);
    w.AddDrop(0.3f, -0.4f, 0.3f, 0.1f);
    CHECK(w.MaxAbs() > 0.05f);
    for (int i = 0; i < 400; ++i) w.Step();
    CHECK(std::abs(w.MeanHeight()) < 1e-6);
    CHECK(w.MaxAbs() < 0.05f);   // the wave spreads and damps
    // a ball pushed into the water displaces it: volume conserved, ripples appear
    Pool::Sphere s{ glm::vec3(0.0f, 1.6f, 0.0f), r };
    w.Reset(64, 2.0f, 1.2f, 0.25f);
    for (int i = 0; i < 60; ++i) { s.centre.y -= 0.01f; w.CoupleSphere(s); w.Step(); }
    CHECK(std::abs(w.MeanHeight()) < 1e-5);
    CHECK(w.MaxAbs() > 0.003f);
}

TEST_CASE("Pool: caustics conserve light (flat water = 1 everywhere)") {
    Pool::HeightField w;
    w.Reset(48, 2.0f, 1.2f, 0.25f);
    Pool::Sphere away{ glm::vec3(0.0f, 50.0f, 0.0f), 0.1f };
    w.ComputeCaustics(glm::vec3(0, 1, 0), away);
    double mean = 0.0, minV = 1e9, maxV = 0.0;
    for (int j = 4; j < 44; ++j) for (int i = 4; i < 44; ++i) {
        const float v = w.caustics[size_t(j) * 48 + i];
        mean += v; minV = std::min(minV, double(v)); maxV = std::max(maxV, double(v));
    }
    mean /= 40.0 * 40.0;
    CHECK_NEAR(mean, 1.0, 0.02);
    CHECK(maxV - minV < 0.05);
    w.AddDrop(0.0f, 0.0f, 0.4f, 0.08f);   // a bump focuses light
    w.ComputeCaustics(glm::vec3(0, 1, 0), away);
    float peak = 0.0f;
    for (float v : w.caustics) peak = std::max(peak, v);
    CHECK(peak > 1.5f);
}

TEST_CASE("Disk: Euler gains energy, Verlet and RK4 hold circular orbits") {
    Disk::DiskSim d;
    d.Seed(600, 5);
    for (int s = 0; s < 1000; ++s) d.Step(Disk::Integrator::Rk4, true, 0.45);
    const glm::dvec3 drift = d.Drift();
    CHECK(drift.x > 1e-2);
    CHECK(drift.y < 2e-3);
    CHECK(drift.z < 2e-3);
    d.Bin();
    double total = 0.0;
    for (size_t i = 3; i < d.density.size(); i += 4) total += d.density[i];
    CHECK(total > 0.0);
}

TEST_CASE("Render: materials are opaque unless opacity is set") {
    Phisiks::Render::MaterialDesc m;
    CHECK(m.opacity == 1.0f);
    Phisiks::Render::PostPass p;
    CHECK(p.enabled && p.order == 0 && p.textures[0].empty());
}
