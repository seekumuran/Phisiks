// Headless tests of the P7 CPU references that the GPU kernels are checked against at runtime:
// force-field math, the spatial hash grid and the XPBD solver; plus a PhysX orbit driven by a
// planetary gravity field. Expectations are analytic (inverse-square law, alpha*m*g stretch,
// rest volumes) or exact (brute-force neighbour counts).
#include "TestFramework.h"

#include "Phisiks/Compute/Compute.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Physics/Xpbd.h"
#include "compute/SpatialHash.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace Phisiks;
using namespace Phisiks::Physics;

// ============================================================================ force fields
TEST_CASE("Fields: spherical gravity is GM/r^2 outside, GM r/R^3 inside, continuous at R") {
    ForceField f;
    f.type = FieldType::SphericalGravity;
    f.strength = 8.0f;
    f.radius = 1.0f;
    const glm::vec3 o(1.0f, 2.0f, 3.0f), axis(0, 1, 0);
    const glm::vec3 out = FieldMath::Acceleration(f, o, axis, o + glm::vec3(2.0f, 0, 0));
    CHECK_NEAR(out.x, -8.0f / 4.0f, 1e-5);
    CHECK_NEAR(out.y, 0.0f, 1e-6);
    const glm::vec3 in = FieldMath::Acceleration(f, o, axis, o + glm::vec3(0, -0.5f, 0));
    CHECK_NEAR(in.y, 8.0f * 0.5f, 1e-5);   // points back up towards the centre
    const float justIn = glm::length(FieldMath::Acceleration(f, o, axis, o + glm::vec3(0, 0, 0.9999f)));
    const float justOut = glm::length(FieldMath::Acceleration(f, o, axis, o + glm::vec3(0, 0, 1.0001f)));
    CHECK_NEAR(justIn, justOut, 2e-3);
    CHECK(glm::length(FieldMath::Acceleration(f, o, axis, o)) == 0.0f);
}

TEST_CASE("Fields: vortex is tangential, linear in the core, (core/r)^falloff outside") {
    ForceField f;
    f.type = FieldType::Vortex;
    f.strength = 5.0f;
    f.radius = 1.0f;
    f.falloff = 1.0f;
    const glm::vec3 axis(0, 1, 0);
    for (float r : { 0.5f, 1.0f, 2.0f, 4.0f }) {
        const glm::vec3 p(r * 0.6f, 3.0f, r * 0.8f);
        const glm::vec3 a = FieldMath::Acceleration(f, glm::vec3(0.0f), axis, p);
        CHECK_NEAR(glm::dot(a, glm::vec3(p.x, 0, p.z)), 0.0, 1e-5);   // no radial part
        CHECK_NEAR(a.y, 0.0, 1e-6);                                     // no axial part
        const float expected = r <= 1.0f ? 5.0f * r : 5.0f / r;
        CHECK_NEAR(glm::length(a), expected, 1e-4);
    }
    // counter-clockwise about +Y: at +X the flow goes towards -Z
    CHECK(FieldMath::Acceleration(f, glm::vec3(0.0f), axis, glm::vec3(2, 0, 0)).z < 0.0f);
}

TEST_CASE("Fields: influence radius fades smoothly over the last 10%") {
    CHECK_NEAR(FieldMath::InfluenceWeight(5.0f, 0.0f), 1.0, 0.0);   // unbounded
    CHECK_NEAR(FieldMath::InfluenceWeight(8.9f, 10.0f), 1.0, 1e-6);
    CHECK_NEAR(FieldMath::InfluenceWeight(9.5f, 10.0f), 0.5, 1e-5);
    CHECK_NEAR(FieldMath::InfluenceWeight(10.0f, 10.0f), 0.0, 1e-6);
    CHECK_NEAR(FieldMath::InfluenceWeight(12.0f, 10.0f), 0.0, 0.0);
    ForceField u;
    u.type = FieldType::Uniform;
    u.vector = glm::vec3(0, 0, 3.0f);
    u.influenceRadius = 10.0f;
    CHECK_NEAR(FieldMath::Acceleration(u, glm::vec3(0.0f), glm::vec3(0, 1, 0), glm::vec3(0, 9.5f, 0)).z, 1.5, 1e-4);
}

TEST_CASE("Fields: n-body accelerations conserve momentum and match two-body gravity") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(-3.0f, 3.0f), M(0.5f, 4.0f);
    std::vector<glm::vec3> p(40), a(40);
    std::vector<float> m(40);
    for (size_t i = 0; i < p.size(); ++i) { p[i] = glm::vec3(U(rng), U(rng), U(rng)); m[i] = M(rng); }
    FieldMath::NBodyAccelerations(p.data(), m.data(), p.size(), 0.7f, 0.1f, a.data());
    glm::dvec3 momentum(0.0), scale(0.0);
    for (size_t i = 0; i < p.size(); ++i) { momentum += glm::dvec3(a[i] * m[i]); scale += glm::abs(glm::dvec3(a[i] * m[i])); }
    CHECK(glm::length(momentum) < 1e-5 * glm::length(scale));
    const glm::vec3 two[2] = { glm::vec3(0.0f), glm::vec3(3.0f, 0, 0) };
    const float masses[2] = { 2.0f, 5.0f };
    glm::vec3 acc[2];
    FieldMath::NBodyAccelerations(two, masses, 2, 1.5f, 0.0f, acc);
    CHECK_NEAR(acc[0].x, 1.5 * 5.0 / 9.0, 1e-5);
    CHECK_NEAR(acc[1].x, -1.5 * 2.0 / 9.0, 1e-5);
}

// ============================================================================ spatial hash
TEST_CASE("SpatialHash: grid neighbour counts equal brute force exactly") {
    namespace SH = Compute::SpatialHash;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> q(0, 4 * 64 - 1);   // 1/64 m lattice inside a 4 m cube
    std::vector<glm::vec3> p(1500);
    for (glm::vec3& v : p) v = glm::vec3(float(q(rng)), float(q(rng)), float(q(rng))) / 64.0f;
    p[1] = p[0];                                           // coincident pair
    p[2] = p[0] + glm::vec3(0.25f, 0, 0);                  // exactly at the radius: not a neighbour
    const SH::Grid grid = SH::Build(p, 0.25f, 1024);
    CHECK(grid.cellStart.size() == 1025);
    CHECK(grid.cellStart.back() == p.size());
    uint32_t mismatches = 0, total = 0;
    for (uint32_t i = 0; i < p.size(); ++i) {
        const uint32_t n = SH::CountNeighbours(grid, p, i, 0.25f);
        mismatches += n != SH::CountNeighboursBruteForce(p, i, 0.25f);
        total += n;
    }
    CHECK(mismatches == 0);
    CHECK(total > 0);
    CHECK(!SH::Within(p[0], p[2], 0.25f));
    CHECK(SH::Hash(glm::ivec3(-1, 2, -3), 1024) < 1024);
}

// ============================================================================ XPBD
namespace {
    Xpbd::Collider GroundPlane() { return Xpbd::Collider{}; }   // plane through the origin, normal +Y
    bool AllFinite(const Xpbd::Model& m) {
        for (const glm::vec4& x : m.x) if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) return false;
        return true;
    }
}

TEST_CASE("XPBD: graph colouring is valid for rope, cloth and soft box") {
    Xpbd::Model rope = Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(0, 1, 0), 20, 1.0f, 0.0f, true);
    Xpbd::Model cloth = Xpbd::MakeCloth(glm::vec3(-1, 2, 0), glm::vec3(2, 0, 0), glm::vec3(0, -2, 0), 12, 12, 1.0f, 0.0f, 1e-3f, true);
    Xpbd::Model box = Xpbd::MakeSoftBox(glm::vec3(0, 1, 0), glm::vec3(1.0f), 3, 1.0f, 0.0f, 0.0f);
    for (Xpbd::Model* m : { &rope, &cloth, &box }) {
        Xpbd::Colorize(*m);
        CHECK(ValidateColoring(*m));
        CHECK(m->colorStart.size() >= 2);
        CHECK(m->colorStart.back() == m->constraints.size());
    }
    CHECK(rope.colorStart.size() - 1 == 2);   // a chain needs exactly two colours
}

TEST_CASE("XPBD: stiff hanging rope keeps its length") {
    Xpbd::Model rope = Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(2, 3, 0), 16, 1.0f, 0.0f, true);   // horizontal: swings
    Xpbd::Colorize(rope);
    Xpbd::StepParams sp;
    sp.substeps = 20;
    float worst = 0.0f;
    for (int i = 0; i < 240; ++i) { Xpbd::Step(rope, sp); worst = std::max(worst, Xpbd::MaxRelativeStretch(rope)); }
    CHECK(AllFinite(rope));
    CHECK(worst < 0.01f);
    CHECK(rope.x[0] == glm::vec4(0, 3, 0, 0));                 // pinned end did not move
    CHECK(rope.x.back().y < 2.0f);                              // swung down
}

TEST_CASE("XPBD: compliant link settles at the analytic stretch alpha*m*g") {
    const float alpha = 1e-3f, mass = 2.0f;   // two particles: the free one carries mass/2
    Xpbd::Model link = Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(0, 2, 0), 1, mass, alpha, true);
    Xpbd::Colorize(link);
    link.damping = 2.0f;
    Xpbd::StepParams sp;
    for (int i = 0; i < 60 * 10; ++i) Xpbd::Step(link, sp);
    const float length = glm::length(glm::vec3(link.x[1]) - glm::vec3(link.x[0]));
    const double expected = double(alpha) * (mass / 2.0) * 9.81;
    CHECK_NEAR(length - 1.0f, expected, 0.02 * expected);
}

TEST_CASE("XPBD: soft box has the rest volume and keeps it after landing") {
    Xpbd::Model box = Xpbd::MakeSoftBox(glm::vec3(0, 1.5f, 0), glm::vec3(0.8f, 0.6f, 1.0f), 3, 2.0f, 1e-4f, 0.0f);
    CHECK_NEAR(Xpbd::TotalVolume(box), 0.8 * 0.6 * 1.0, 1e-5);
    for (const Xpbd::Constraint& c : box.constraints) {
        if (c.type == Xpbd::Volume) CHECK(c.rest > 0.0f);   // consistently oriented tetrahedra
    }
    Xpbd::Colorize(box);
    const Xpbd::Collider ground = GroundPlane();
    Xpbd::StepParams sp;
    sp.substeps = 16;
    sp.colliders = &ground;
    sp.colliderCount = 1;
    for (int i = 0; i < 180; ++i) Xpbd::Step(box, sp);
    CHECK(AllFinite(box));
    float minY = 1e9f;
    for (const glm::vec4& x : box.x) minY = std::min(minY, x.y);
    CHECK_NEAR(minY, box.particleRadius, 0.01);                 // landed and resting on its particle radius
    CHECK_NEAR(Xpbd::TotalVolume(box), 0.48, 0.05 * 0.48);
}

TEST_CASE("XPBD: cloth hangs from its pinned corners without tearing") {
    Xpbd::Model cloth = Xpbd::MakeCloth(glm::vec3(-1, 3, 0), glm::vec3(2, 0, 0), glm::vec3(0, -2, 0), 16, 16, 1.0f, 0.0f, 1e-2f, true);
    Xpbd::Colorize(cloth);
    std::vector<glm::vec4> pinned;
    for (const glm::vec4& x : cloth.x) if (x.w == 0.0f) pinned.push_back(x);
    CHECK(pinned.size() == 2);
    CHECK(!cloth.triangles.empty());
    Xpbd::StepParams sp;
    sp.substeps = 15;
    sp.gravity = glm::vec3(0, -9.81f, -3.0f);   // wind-like pull out of plane
    for (int i = 0; i < 180; ++i) Xpbd::Step(cloth, sp);
    CHECK(AllFinite(cloth));
    size_t k = 0;
    for (const glm::vec4& x : cloth.x) if (x.w == 0.0f) CHECK(x == pinned[k++]);
    CHECK(Xpbd::MaxRelativeStretch(cloth) < 0.05f);
}

// ============================================================================ fields on PhysX bodies
TEST_CASE("Fields: rigid body on a circular orbit around a planetary field keeps its radius") {
    Settings settings;
    settings.gravity = glm::vec3(0.0f);
    CHECK(Init(settings));
    ForceField planet;
    planet.type = FieldType::SphericalGravity;
    planet.strength = 6.0f;
    planet.radius = 0.5f;
    const float r0 = 2.0f;
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.position = glm::vec3(r0, 0, 0);
    desc.mass = 0.5f;
    ColliderDesc c;
    c.shape = ShapeType::Sphere;
    c.radius = 0.1f;
    desc.colliders.push_back(c);
    const BodyId moon = CreateBody(desc);
    SetLinearVelocity(moon, glm::vec3(0, 0, -std::sqrt(planet.strength / r0)));
    const float period = glm::two_pi<float>() * std::sqrt(r0 * r0 * r0 / planet.strength);
    const float dt = 1.0f / 60.0f;
    float minR = r0, maxR = r0;
    for (int i = 0; i < int(period / dt); ++i) {
        const glm::vec3 p = FieldMath::SamplePosition(GetPosition(moon), GetLinearVelocity(moon), dt, GetSettings().substeps);
        AddForce(moon, FieldMath::Acceleration(planet, glm::vec3(0.0f), glm::vec3(0, 1, 0), p) * GetMass(moon));
        Step(dt);
        const float r = glm::length(GetPosition(moon));
        minR = std::min(minR, r);
        maxR = std::max(maxR, r);
    }
    CHECK(maxR - minR < 0.01f * r0);                                       // ~0.4% expected (see SamplePosition)
    CHECK(glm::length(GetPosition(moon) - glm::vec3(r0, 0, 0)) < 0.05f * r0);  // back near the start after one period
    Shutdown();
}

// ============================================================================ compute backends
TEST_CASE("Compute: OpenGL backend needs the renderer; OpenCL and SYCL are hooks; CPU is always there") {
    using namespace Phisiks::Compute;
    CHECK(GetBackend(BackendType::OpenGL) == nullptr);   // headless: no GL context
    CHECK(GetBackend(BackendType::OpenCL) == nullptr);
    CHECK(GetBackend(BackendType::SYCL) == nullptr);
    CHECK(GetBackend(BackendType::CPU) != nullptr);
    const std::vector<DeviceInfo> all = EnumerateBackends();
    CHECK(all.size() == 4);
    for (size_t i = 0; i < 3 && i < all.size(); ++i) {
        CHECK(!all[i].available);
        CHECK(!all[i].note.empty());
    }
    CHECK(all.size() == 4 && all[3].available && all[3].backend == BackendType::CPU);
    CHECK(std::string(ToString(BackendType::SYCL)) == "SYCL");
    CHECK(std::string(ToString(BackendType::CPU)) == "CPU");
}
