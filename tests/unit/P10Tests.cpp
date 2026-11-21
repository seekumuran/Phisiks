// P10: particle spawn shapes (disk on circular orbits, even fill), the non-blocking particle
// readback, entity-id false colours, the showcase raycast vehicle, the integrator split-test of the
// black-hole showcase, and the local (scaled) units of Joint anchors.
#include "TestFramework.h"

#include "Vehicle.h"

#include "compute/AdvancedWorld.h"
#include "Phisiks/Math/Integrators.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace Phisiks;

namespace {
    void StepScene(Scene& scene, int steps, float dt = 1.0f / 60.0f) {
        for (int i = 0; i < steps; ++i) {
            scene.SyncPhysicsBodies();
            scene.ApplyForceFields(dt);
            Physics::Step(dt);
            Advanced::Step(scene, dt);
            scene.PullPhysicsPoses();
        }
    }
}

TEST_CASE("Particles: disk spawn starts on circular orbits that a gravity field keeps (CPU backend)") {
    CHECK(Physics::Init());
    CHECK(Advanced::Init());
    {
        Scene scene("Disk");
        Entity hole = scene.CreateEntity("Hole");
        Physics::ForceField& f = hole.Add<Physics::ForceField>();
        f.type = Physics::FieldType::SphericalGravity;
        f.strength = 40.0f;
        f.radius = 1.0f;
        Entity disk = scene.CreateEntity("Disk");
        Physics::Advanced::ParticleSystem ps;
        ps.count = 2000;
        ps.spawnShape = Physics::Advanced::ParticleSpawn::Disk;
        ps.diskInnerRadius = 3.0f;
        ps.diskOuterRadius = 10.0f;
        ps.diskThickness = 0.2f;
        ps.orbitalGM = 40.0f;
        ps.useGravity = false;
        ps.radius = 0.01f;
        ps.friction = 0.0f;
        ps.damping = 0.0f;
        disk.Add<Physics::Advanced::ParticleSystem>(ps);
        StepScene(scene, 1);
        std::vector<glm::vec4> p0, p1, async;
        CHECK(Physics::Advanced::ReadParticles(disk, p0) && p0.size() == 2000);
        bool inside = true;
        for (const glm::vec4& p : p0) {
            const float r = glm::length(glm::vec2(p.x, p.z));
            inside = inside && r > 2.95f && r < 10.2f && std::abs(p.y) < 0.15f;
        }
        CHECK(inside);
        StepScene(scene, 119);   // 2 s: ~0.4 .. 2.4 rad of orbit
        CHECK(Physics::Advanced::ReadParticles(disk, p1));
        float worst = 0.0f;
        double angle = 0.0;
        for (size_t i = 0; i < p0.size(); ++i) {
            const float r0 = glm::length(glm::vec2(p0[i].x, p0[i].z)), r1 = glm::length(glm::vec2(p1[i].x, p1[i].z));
            worst = std::max(worst, std::abs(r1 - r0) / r0);
            angle += std::atan2(p0[i].z * p1[i].x - p0[i].x * p1[i].z, p0[i].x * p1[i].x + p0[i].z * p1[i].z);   // + = counter-clockwise from +Y
        }
        std::printf("    disk: worst radius change %.4f over 2 s, mean swept angle %.3f rad\n", worst, angle / double(p0.size()));
        CHECK(worst < 0.03f);
        CHECK(angle / double(p0.size()) > 0.3);   // they orbit (counter-clockwise seen from +Y)
        CHECK(Physics::Advanced::ReadParticlesAsync(disk, async) && async.size() == p1.size() && async[7] == p1[7]);   // CPU: immediate
    }
    Advanced::Shutdown();
    Physics::Shutdown();
}

TEST_CASE("Particles: fill spawn is an even lattice from the bottom of the box") {
    CHECK(Physics::Init());
    CHECK(Advanced::Init());
    {
        Scene scene("Fill");
        Entity pool = scene.CreateEntity("Pool");
        pool.Get<Transform>().position = glm::vec3(0.0f, 5.0f, 0.0f);
        Physics::Advanced::ParticleSystem ps;
        ps.count = 1000;
        ps.spawnShape = Physics::Advanced::ParticleSpawn::Fill;
        ps.spawnSize = glm::vec3(2.0f, 1.0f, 1.0f);
        ps.radius = 0.05f;
        ps.useGravity = false;
        pool.Add<Physics::Advanced::ParticleSystem>(ps);
        StepScene(scene, 1, 1e-4f);
        std::vector<glm::vec4> p;
        CHECK(Physics::Advanced::ReadParticles(pool, p) && p.size() == 1000);
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, minZ = 1e9f, maxZ = -1e9f, nearest = 1e9f;
        for (size_t i = 0; i < p.size(); ++i) {
            minX = std::min(minX, p[i].x); maxX = std::max(maxX, p[i].x);
            minZ = std::min(minZ, p[i].z); maxZ = std::max(maxZ, p[i].z);
            minY = std::min(minY, p[i].y);
            for (size_t j = i + 1; j < p.size(); j += 7) nearest = std::min(nearest, glm::length(glm::vec3(p[i] - p[j])));
        }
        std::printf("    fill: x %.3f..%.3f, z %.3f..%.3f, lowest %.3f, nearest pair (sampled) %.4f\n", minX, maxX, minZ, maxZ, minY, nearest);
        CHECK(minX > -1.0f && maxX < 1.0f && minZ > -0.5f && maxZ < 0.5f);
        CHECK(std::abs(minY - (4.5f + 0.063f)) < 0.02f);   // first layer at half a spacing above the box bottom
        CHECK(nearest > 0.095f);                            // no overlapping particles (2 r = 0.1)
    }
    Advanced::Shutdown();
    Physics::Shutdown();
}

TEST_CASE("Render: entity-id false colours are deterministic and well separated") {
    std::set<uint32_t> colours;
    for (uint32_t id = 1; id <= 1000; ++id) {
        const glm::u8vec3 c = Render::EntityIdColor(id);
        CHECK(c.r >= 64 && c.g >= 64 && c.b >= 64);
        colours.insert(uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16);
    }
    CHECK(colours.size() >= 995);
    CHECK(Render::EntityIdColor(0) == glm::u8vec3(13, 13, 15));
    CHECK(Render::EntityIdColor(42) == Render::EntityIdColor(42));
}

TEST_CASE("Vehicle: raycast spring-damper suspension settles at m g / 4k and drives") {
    CHECK(Physics::Init());
    {
        Scene scene("Vehicle");
        Entity ground = scene.CreateEntity("Ground");
        ground.Get<Transform>().position = glm::vec3(0.0f, -0.5f, 0.0f);
        ground.Get<Transform>().scale = glm::vec3(200.0f, 1.0f, 200.0f);
        ground.Add<Collider>();
        Entity chassis = scene.CreateEntity("Car");
        chassis.Get<Transform>().position = glm::vec3(0.0f, 1.0f, 0.0f);
        chassis.Get<Transform>().scale = glm::vec3(1.9f, 0.5f, 4.2f);
        chassis.Add<Collider>();
        Rigidbody rb;
        rb.mass = 1200.0f;
        rb.angularDamping = 0.6f;
        chassis.Add<Rigidbody>(rb);
        scene.SyncPhysicsBodies();
        Showcase::Vehicle car;
        std::vector<Showcase::WheelDesc> wheels;
        for (int i = 0; i < 4; ++i) {
            Showcase::WheelDesc w;
            w.mount = glm::vec3(i % 2 ? 0.85f : -0.85f, -0.05f, i < 2 ? -1.35f : 1.35f);
            w.radius = 0.36f;
            w.steer = i < 2;
            wheels.push_back(w);
        }
        Showcase::VehicleDesc desc;
        car.Setup(chassis, desc, wheels);
        auto run = [&](int steps, float throttle, float steer) {
            for (int i = 0; i < steps; ++i) {
                scene.SyncPhysicsBodies();
                car.FixedUpdate(1.0f / 60.0f, throttle, steer, 0.0f);
                Physics::Step(1.0f / 60.0f);
                scene.PullPhysicsPoses();
            }
        };
        run(180, 0.0f, 0.0f);
        float mean = 0.0f;
        for (const Showcase::WheelState& w : car.Wheels()) mean += w.compression * 0.25f;
        const float expected = 1200.0f * 9.81f / (4.0f * desc.stiffness);
        std::printf("    vehicle: sag %.4f m (theory %.4f), %d wheels grounded\n", mean, expected, car.GroundedWheels());
        CHECK(car.GroundedWheels() == 4);
        CHECK(std::abs(mean - expected) < expected * 0.05f);
        run(120, 1.0f, 0.0f);
        CHECK(car.ForwardSpeed() > 5.0f);
        CHECK(chassis.WorldPosition().z < -3.0f);                     // forward is -Z
        const glm::vec3 before = chassis.WorldRotation() * glm::vec3(0, 0, -1);
        run(60, 0.5f, 1.0f);                                          // steer right
        const glm::vec3 after = chassis.WorldRotation() * glm::vec3(0, 0, -1);
        CHECK(glm::cross(before, after).y < -0.05f);                  // turned clockwise seen from above = right
    }
    Physics::Shutdown();
}

TEST_CASE("Integrators: pseudo-Newtonian split-test (Euler spirals out, Verlet and RK4 hold the orbit)") {
    constexpr double GM = 40.0, RS = 1.0, H = 0.1;
    auto accel = [](const glm::dvec3& r) { const double d = glm::length(r); return -r / d * (GM / ((d - RS) * (d - RS))); };
    auto energy = [](const glm::dvec3& r, const glm::dvec3& v) { return 0.5 * glm::dot(v, v) - GM / (glm::length(r) - RS); };
    struct S { glm::dvec3 x, v; S operator+(const S& o) const { return { x + o.x, v + o.v }; } S operator*(double s) const { return { x * s, v * s }; } };
    const double r0 = 8.0, vc = std::sqrt(GM * r0) / (r0 - RS);
    glm::dvec3 xe(r0, 0, 0), ve(0, 0, -0.92 * vc), xv = xe, vv = ve;
    S rk{ xe, ve };
    const double e0 = energy(xe, ve);
    double apoE = 0.0, apoR = 0.0, periR = 1e9;
    for (int i = 0; i < 1600; ++i) {
        Math::Integrate::ExplicitEuler(xe, ve, H, accel);
        Math::Integrate::VelocityVerlet(xv, vv, H, accel);
        rk = Math::Integrate::RK4(rk, 0.0, H, [&](double, const S& s) { return S{ s.v, accel(s.x) }; });
        apoE = std::max(apoE, glm::length(xe));
        apoR = std::max(apoR, glm::length(rk.x));
        periR = std::min(periR, glm::length(rk.x));
    }
    const double de = std::abs((energy(xe, ve) - e0) / e0), dv = std::abs((energy(xv, vv) - e0) / e0), dr = std::abs((energy(rk.x, rk.v) - e0) / e0);
    std::printf("    split-test: |dE/E| Euler %.3f, Verlet %.2e, RK4 %.2e; RK4 r %.2f..%.2f; Euler apoapsis %.1f\n", de, dv, dr, periR, apoR, apoE);
    CHECK(de > 0.1 && apoE > 20.0);
    CHECK(dv < 1e-4 && dr < 1e-4);
    CHECK(std::abs(apoR - 8.0) < 0.01 && periR > 4.5 && periR < 5.5);
}

TEST_CASE("Joints: Joint::anchor is in the entity's local (scaled) units") {
    CHECK(Physics::Init());
    {
        Scene scene("Anchor");
        Entity base = scene.CreateEntity("Base");
        base.Get<Transform>().position = glm::vec3(0.0f, 0.5f, 0.0f);
        base.Add<Collider>();
        Entity arm = scene.CreateEntity("Arm");
        arm.Get<Transform>().position = glm::vec3(0.0f, 2.0f, 0.0f);
        arm.Get<Transform>().scale = glm::vec3(0.2f, 2.0f, 0.2f);   // spans y = 1 .. 3
        arm.Add<Collider>();
        Rigidbody rb;
        rb.useGravity = false;
        arm.Add<Rigidbody>(rb);
        Joint& j = arm.Add<Joint>();
        j.type = Physics::JointType::Hinge;
        j.connectedTo = "Base";
        j.anchor = glm::vec3(0.0f, -0.5f, 0.0f);   // the bottom face of the unit box: y = 1 in the world
        j.axis = glm::vec3(1.0f, 0.0f, 0.0f);
        scene.SyncPhysicsBodies();
        scene.SyncPhysicsBodies();
        CHECK(Physics::IsValid(arm.Get<Joint>().runtime));
        const Physics::JointState s = Physics::GetJointState(arm.Get<Joint>().runtime);
        std::printf("    anchor world (%.3f, %.3f, %.3f)\n", s.anchorB.x, s.anchorB.y, s.anchorB.z);
        CHECK(glm::length(s.anchorB - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-3f);
    }
    Physics::Shutdown();
}
