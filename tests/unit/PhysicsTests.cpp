// Headless Phisiks::Physics tests. Expectations are derived analytically (kinematics, Coulomb
// friction, restitution, geometry) and checked with tolerances that cover the solver's
// semi-implicit integration error at 240 Hz sub-steps.
#include "TestFramework.h"

#include "Phisiks/Physics/Physics.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace Phisiks;
using namespace Phisiks::Physics;

namespace {
    constexpr float DT = 1.0f / 60.0f;
    constexpr float G = 9.81f;

    struct Fixture {
        explicit Fixture(Settings settings = Settings()) { ok = Init(settings); }
        ~Fixture() { Shutdown(); }
        bool ok = false;
    };

    void Simulate(float seconds) {
        const int steps = int(std::lround(seconds / DT));
        for (int i = 0; i < steps; ++i) Step(DT);
    }

    ColliderDesc Box(glm::vec3 halfExtents, PhysicsMaterial material = PhysicsMaterial()) {
        ColliderDesc c; c.shape = ShapeType::Box; c.halfExtents = halfExtents; c.material = material; return c;
    }
    ColliderDesc Sphere(float radius, PhysicsMaterial material = PhysicsMaterial()) {
        ColliderDesc c; c.shape = ShapeType::Sphere; c.radius = radius; c.material = material; return c;
    }

    BodyId MakeBody(BodyType type, glm::vec3 position, ColliderDesc collider, float mass = 1.0f, glm::quat rotation = glm::quat(1, 0, 0, 0), Layer layer = LAYER_DEFAULT, uint64_t userData = 0) {
        BodyDesc desc;
        desc.type = type;
        desc.position = position;
        desc.rotation = rotation;
        desc.mass = mass;
        desc.layer = layer;
        desc.userData = userData;
        desc.colliders.push_back(std::move(collider));
        return CreateBody(desc);
    }

    // Static ground: top surface at y = 0.
    BodyId Ground(PhysicsMaterial material = PhysicsMaterial(), Layer layer = LAYER_DEFAULT) {
        return MakeBody(BodyType::Static, glm::vec3(0, -0.5f, 0), Box(glm::vec3(50, 0.5f, 50), material), 1.0f, glm::quat(1, 0, 0, 0), layer, 1);
    }
}

TEST_CASE("Physics: realistic default material (static 0.6, dynamic 0.5, restitution 0)") {
    const Settings settings;
    CHECK_NEAR(settings.defaultMaterial.staticFriction, 0.6, 1e-6);
    CHECK_NEAR(settings.defaultMaterial.dynamicFriction, 0.5, 1e-6);
    CHECK_NEAR(settings.defaultMaterial.restitution, 0.0, 1e-6);
    CHECK_NEAR(PhysicsMaterial().staticFriction, 0.6, 1e-6);
}

TEST_CASE("Physics: free fall matches y0 - g t^2 / 2") {
    Fixture f;
    CHECK(f.ok);
    const BodyId ball = MakeBody(BodyType::Dynamic, glm::vec3(0, 100, 0), Sphere(0.25f));
    Simulate(1.0f);
    CHECK_NEAR(GetPosition(ball).y, 100.0 - 0.5 * G * 1.0, 0.05);
    CHECK_NEAR(GetLinearVelocity(ball).y, -G, 0.05);
}

TEST_CASE("Physics: box comes to rest on static ground") {
    Fixture f;
    Ground();
    const BodyId box = MakeBody(BodyType::Dynamic, glm::vec3(0, 2, 0), Box(glm::vec3(0.5f)), 10.0f);
    Simulate(3.0f);
    CHECK_NEAR(GetPosition(box).y, 0.5, 0.02);
    CHECK(glm::length(GetLinearVelocity(box)) < 0.02f);
}

TEST_CASE("Physics: static friction holds below atan(0.6) = 31 deg and slides above") {
    auto slideDistance = [](float degrees) {
        Fixture f;
        const float theta = glm::radians(degrees);
        const glm::quat tilt = glm::angleAxis(theta, glm::vec3(0, 0, 1));
        const glm::vec3 normal = tilt * glm::vec3(0, 1, 0);
        MakeBody(BodyType::Static, glm::vec3(0), Box(glm::vec3(20, 0.1f, 2)), 1.0f, tilt);
        const glm::vec3 start = normal * (0.1f + 0.25f + 0.002f);
        const BodyId box = MakeBody(BodyType::Dynamic, start, Box(glm::vec3(0.25f)), 5.0f, tilt);
        Simulate(2.0f);
        return glm::length(GetPosition(box) - start);
    };
    const float held = slideDistance(20.0f);    // tan 20 = 0.36 < 0.6
    const float slid = slideDistance(40.0f);    // tan 40 = 0.84 > 0.6
    CHECK(held < 0.05f);
    // Sliding: a = g (sin t - mu_d cos t) = 2.55 m/s^2  ->  d = a t^2 / 2 = 5.1 m in 2 s.
    const double expected = 0.5 * G * (std::sin(glm::radians(40.0)) - 0.5 * std::cos(glm::radians(40.0))) * 4.0;
    CHECK(slid > 0.8 * expected && slid < 1.2 * expected);
}

TEST_CASE("Physics: sliding stops after v^2 / (2 mu_d g)") {
    Fixture f;
    Ground();
    const BodyId slab = MakeBody(BodyType::Dynamic, glm::vec3(0, 0.1f, 0), Box(glm::vec3(0.5f, 0.1f, 0.5f)), 5.0f);
    Simulate(0.5f);                                    // settle
    const glm::vec3 start = GetPosition(slab);
    SetLinearVelocity(slab, glm::vec3(5, 0, 0));
    Simulate(3.0f);
    const double expected = 25.0 / (2.0 * 0.5 * G);   // 2.55 m
    const double travelled = GetPosition(slab).x - start.x;
    CHECK(travelled > 0.9 * expected && travelled < 1.1 * expected);
    CHECK(glm::length(GetLinearVelocity(slab)) < 0.02f);
}

TEST_CASE("Physics: restitution bounce height ~ e^2 h") {
    Fixture f;
    PhysicsMaterial bouncy; bouncy.restitution = 0.8f;
    Ground(bouncy);
    const float radius = 0.25f, dropHeight = 2.0f;     // bottom of the ball 2 m above ground
    const BodyId ball = MakeBody(BodyType::Dynamic, glm::vec3(0, dropHeight + radius, 0), Sphere(radius, bouncy));
    bool bounced = false;
    float apex = 0.0f;
    for (int i = 0; i < 180; ++i) {
        Step(DT);
        const float vy = GetLinearVelocity(ball).y;
        if (!bounced && vy > 0.5f) bounced = true;
        if (bounced) apex = std::max(apex, GetPosition(ball).y - radius);
        if (bounced && vy < 0.0f) break;
    }
    const float expected = 0.8f * 0.8f * dropHeight;   // 1.28 m
    CHECK(bounced);
    CHECK(apex > 0.8f * expected && apex < 1.15f * expected);
}

TEST_CASE("Physics: kinematic body follows targets, ignores gravity, pushes dynamics") {
    Fixture f;
    Ground();
    const BodyId pusher = MakeBody(BodyType::Kinematic, glm::vec3(0, 0.5f, 0), Box(glm::vec3(0.5f)));
    const BodyId crate = MakeBody(BodyType::Dynamic, glm::vec3(1.6f, 0.5f, 0), Box(glm::vec3(0.5f)), 2.0f);
    Simulate(0.3f);
    for (int i = 1; i <= 60; ++i) {
        SetKinematicTarget(pusher, glm::vec3(2.0f * i / 60.0f, 0.5f, 0), glm::quat(1, 0, 0, 0));
        Step(DT);
    }
    CHECK_NEAR(GetPosition(pusher).x, 2.0, 0.01);
    CHECK_NEAR(GetPosition(pusher).y, 0.5, 1e-4);
    CHECK(GetPosition(crate).x > 2.9f);                // pushed ahead of the pusher's front face
}

TEST_CASE("Physics: static bodies ignore impulses and velocity changes") {
    Fixture f;
    const BodyId wall = MakeBody(BodyType::Static, glm::vec3(3, 1, 0), Box(glm::vec3(0.5f)));
    AddImpulse(wall, glm::vec3(1000, 0, 0));
    SetLinearVelocity(wall, glm::vec3(10, 0, 0));
    Simulate(0.5f);
    CHECK_NEAR(GetPosition(wall).x, 3.0, 1e-6);
    CHECK(GetBodyType(wall) == BodyType::Static);
}

TEST_CASE("Physics: mass is honoured and impulse gives dv = J / m") {
    Fixture f;
    Settings s = GetSettings();
    SetGravity(glm::vec3(0));
    const BodyId body = MakeBody(BodyType::Dynamic, glm::vec3(0, 5, 0), Box(glm::vec3(0.5f)), 8.0f);
    CHECK_NEAR(GetMass(body), 8.0, 1e-4);
    AddImpulse(body, glm::vec3(16, 0, 0));
    Step(DT);
    CHECK_NEAR(GetLinearVelocity(body).x, 2.0, 1e-3);
    SetGravity(s.gravity);
}

TEST_CASE("Physics: raycast returns the nearest hit with distance and normal; RaycastAll sorted") {
    Fixture f;
    const BodyId a = MakeBody(BodyType::Static, glm::vec3(2, 0, 0), Box(glm::vec3(0.5f)), 1, glm::quat(1, 0, 0, 0), LAYER_DEFAULT, 11);
    const BodyId b = MakeBody(BodyType::Static, glm::vec3(4, 0, 0), Box(glm::vec3(0.5f)), 1, glm::quat(1, 0, 0, 0), 3, 12);
    MakeBody(BodyType::Static, glm::vec3(6, 0, 0), Box(glm::vec3(0.5f)));
    const auto hit = Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f);
    CHECK(hit && hit->body == a);
    CHECK(hit && hit->userData == 11);
    CHECK(hit && std::fabs(hit->distance - 1.5f) < 1e-3f);
    CHECK(hit && std::fabs(hit->normal.x + 1.0f) < 1e-3f);
    const auto all = RaycastAll(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f);
    CHECK(all.size() == 3);
    CHECK(all.size() == 3 && all[0].distance < all[1].distance && all[1].distance < all[2].distance);
    QueryOptions onlyLayer3; onlyLayer3.layers = LayerBit(3);
    const auto filtered = Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f, onlyLayer3);
    CHECK(filtered && filtered->body == b);
    QueryOptions ignoreA; ignoreA.ignoreBody = a;
    const auto ignored = Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f, ignoreA);
    CHECK(ignored && ignored->body == b);
    CHECK(!Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 1.4f));   // max distance respected
}

TEST_CASE("Physics: triggers are skipped by queries unless requested") {
    Fixture f;
    ColliderDesc trigger = Box(glm::vec3(0.5f)); trigger.isTrigger = true;
    const BodyId zone = MakeBody(BodyType::Static, glm::vec3(1, 0, 0), trigger);
    const BodyId wall = MakeBody(BodyType::Static, glm::vec3(5, 0, 0), Box(glm::vec3(0.5f)));
    const auto normal = Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f);
    CHECK(normal && normal->body == wall);
    QueryOptions withTriggers; withTriggers.includeTriggers = true;
    const auto all = Raycast(glm::vec3(0), glm::vec3(1, 0, 0), 100.0f, withTriggers);
    CHECK(all && all->body == zone);
}

TEST_CASE("Physics: sphere/box/capsule sweeps stop at contact distance") {
    Fixture f;
    MakeBody(BodyType::Static, glm::vec3(5, 0, 0), Box(glm::vec3(0.5f, 3, 3)));   // face at x = 4.5
    const auto sphere = SphereCast(glm::vec3(0), 0.5f, glm::vec3(1, 0, 0), 100.0f);
    const auto box = BoxCast(glm::vec3(0), glm::vec3(0.25f), glm::quat(1, 0, 0, 0), glm::vec3(1, 0, 0), 100.0f);
    const auto capsule = CapsuleCast(glm::vec3(0), 0.3f, 0.5f, glm::vec3(1, 0, 0), 100.0f);
    CHECK(sphere && std::fabs(sphere->distance - 4.0f) < 0.01f);
    CHECK(box && std::fabs(box->distance - 4.25f) < 0.01f);
    CHECK(capsule && std::fabs(capsule->distance - 4.2f) < 0.01f);   // Y-up capsule: radius faces the wall
}

TEST_CASE("Physics: overlap queries find exactly the bodies inside the volume") {
    Fixture f;
    const BodyId nearBox = MakeBody(BodyType::Static, glm::vec3(1, 0, 0), Box(glm::vec3(0.5f)));
    MakeBody(BodyType::Static, glm::vec3(3, 0, 0), Box(glm::vec3(0.5f)));            // starts at x = 2.5
    const auto inside = OverlapSphere(glm::vec3(0), 2.0f);
    CHECK(inside.size() == 1 && inside[0] == nearBox);
    const auto boxed = OverlapBox(glm::vec3(2, 0, 0), glm::vec3(1.2f, 1, 1), glm::quat(1, 0, 0, 0));
    CHECK(boxed.size() == 2);
}

TEST_CASE("Physics: collision enter event with user data and impact speed sqrt(2 g h)") {
    Fixture f;
    const BodyId ground = Ground();
    const BodyId box = MakeBody(BodyType::Dynamic, glm::vec3(0, 1, 0), Box(glm::vec3(0.5f)), 3.0f, glm::quat(1, 0, 0, 0), LAYER_DEFAULT, 77);
    bool entered = false;
    for (int i = 0; i < 60 && !entered; ++i) {
        ClearEvents();
        Step(DT);
        for (const CollisionEvent& e : GetCollisionEvents()) {
            const bool pair = (e.a == box && e.b == ground) || (e.a == ground && e.b == box);
            if (e.kind == CollisionEvent::Kind::Enter && pair) {
                entered = true;
                CHECK((e.a == box ? e.userDataA : e.userDataB) == 77);
                // Dropped 0.5 m (box bottom from 0.5 to 0): v = sqrt(2 g h) = 3.13 m/s.
                CHECK(e.impactSpeed > 2.5f && e.impactSpeed < 3.6f);
            }
        }
    }
    CHECK(entered);
}

TEST_CASE("Physics: trigger enter/exit for a body falling through, and for a character") {
    Fixture f;
    Ground();
    ColliderDesc zone = Box(glm::vec3(1, 0.5f, 1)); zone.isTrigger = true;
    const BodyId trigger = MakeBody(BodyType::Static, glm::vec3(0, 3, 0), zone, 1, glm::quat(1, 0, 0, 0), LAYER_DEFAULT, 5);
    const BodyId ball = MakeBody(BodyType::Dynamic, glm::vec3(0, 6, 0), Sphere(0.2f));
    int enters = 0, exits = 0;
    for (int i = 0; i < 180; ++i) {
        ClearEvents();
        Step(DT);
        for (const TriggerEvent& e : GetTriggerEvents()) {
            if (e.trigger != trigger || e.other != ball) continue;
            CHECK(e.userDataTrigger == 5);
            (e.kind == TriggerEvent::Kind::Enter ? enters : exits)++;
        }
    }
    CHECK(enters == 1);
    CHECK(exits == 1);

    CharacterDesc cd; cd.footPosition = glm::vec3(0, 0, 4);
    const CharacterId walker = CreateCharacter(cd);
    ColliderDesc floorZone = Box(glm::vec3(1, 1, 1)); floorZone.isTrigger = true;
    const BodyId pad = MakeBody(BodyType::Static, glm::vec3(0, 1, 0), floorZone);
    bool characterEntered = false;
    for (int i = 0; i < 120 && !characterEntered; ++i) {
        ClearEvents();
        MoveCharacter(walker, glm::vec3(0, -0.05f, -4.0f * DT), DT);
        Step(DT);
        for (const TriggerEvent& e : GetTriggerEvents()) {
            if (e.trigger == pad && e.otherIsCharacter && e.kind == TriggerEvent::Kind::Enter) characterEntered = true;
        }
    }
    CHECK(characterEntered);
}

TEST_CASE("Physics: layers that do not collide pass through each other") {
    Fixture f;
    SetLayersCollide(1, 2, false);
    CHECK(!LayersCollide(1, 2));
    CHECK(LayersCollide(1, 1));
    Ground(PhysicsMaterial(), 1);
    const BodyId ghost = MakeBody(BodyType::Dynamic, glm::vec3(0, 1, 0), Box(glm::vec3(0.25f)), 1, glm::quat(1, 0, 0, 0), 2);
    const BodyId solid = MakeBody(BodyType::Dynamic, glm::vec3(2, 1, 0), Box(glm::vec3(0.25f)), 1, glm::quat(1, 0, 0, 0), 3);
    Simulate(1.5f);
    CHECK(GetPosition(ghost).y < -1.0f);
    CHECK_NEAR(GetPosition(solid).y, 0.25, 0.02);
}

TEST_CASE("Physics: capsule collider is Y-up") {
    Fixture f;
    ColliderDesc capsule; capsule.shape = ShapeType::Capsule; capsule.radius = 0.3f; capsule.halfHeight = 0.5f;
    MakeBody(BodyType::Static, glm::vec3(0, 2, 0), capsule);
    const auto top = Raycast(glm::vec3(0, 10, 0), glm::vec3(0, -1, 0), 20.0f);
    CHECK(top && std::fabs(top->point.y - (2.0f + 0.5f + 0.3f)) < 1e-3f);
    const auto side = Raycast(glm::vec3(-5, 2, 0), glm::vec3(1, 0, 0), 20.0f);
    CHECK(side && std::fabs(side->point.x + 0.3f) < 1e-3f);
}

TEST_CASE("Physics: convex hull and triangle mesh colliders") {
    Fixture f;
    ColliderDesc ground; ground.shape = ShapeType::TriangleMesh;
    ground.vertices = { { -10, 0, -10 }, { 10, 0, -10 }, { 10, 0, 10 }, { -10, 0, 10 } };
    ground.indices = { 0, 2, 1, 0, 3, 2 };   // counter-clockwise seen from above (normal +Y)
    CHECK(MakeBody(BodyType::Static, glm::vec3(0), ground).Valid());
    ColliderDesc hull; hull.shape = ShapeType::ConvexMesh;
    for (int i = 0; i < 8; ++i) hull.vertices.push_back(glm::vec3(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f, i & 4 ? 0.5f : -0.5f));
    const BodyId cube = MakeBody(BodyType::Dynamic, glm::vec3(0, 2, 0), hull, 4.0f);
    CHECK(cube.Valid());
    Simulate(2.5f);
    CHECK_NEAR(GetPosition(cube).y, 0.5, 0.03);
    ColliderDesc badMesh; badMesh.shape = ShapeType::TriangleMesh; badMesh.vertices = ground.vertices; badMesh.indices = ground.indices;
    CHECK(!MakeBody(BodyType::Dynamic, glm::vec3(0, 5, 0), badMesh).Valid());   // dynamic triangle meshes are rejected
}

TEST_CASE("Physics: character controller grounding, wall blocking and step offset") {
    Fixture f;
    Ground();
    MakeBody(BodyType::Static, glm::vec3(0, 1, -5), Box(glm::vec3(3, 1, 0.25f)));     // wall face at z = -4.75
    MakeBody(BodyType::Static, glm::vec3(5, 0.1f, 0), Box(glm::vec3(0.5f, 0.1f, 2)));  // 0.2 m step at x 4.5..5.5
    MakeBody(BodyType::Static, glm::vec3(-5, 0.3f, 0), Box(glm::vec3(0.5f, 0.3f, 2))); // 0.6 m block at x -5.5..-4.5
    CharacterDesc cd; cd.footPosition = glm::vec3(0, 0.05f, 0); cd.radius = 0.3f; cd.stepOffset = 0.3f;
    const CharacterId c = CreateCharacter(cd);
    CHECK(c.Valid());
    auto walk = [&](glm::vec3 direction, float seconds) {
        CharacterMoveResult last;
        for (int i = 0; i < int(seconds / DT); ++i) last = MoveCharacter(c, direction * 3.0f * DT + glm::vec3(0, -0.1f, 0), DT);
        return last;
    };
    CHECK(walk(glm::vec3(0), 0.2f).grounded);
    walk(glm::vec3(0, 0, -1), 3.0f);
    const float z = GetCharacterFootPosition(c).z;
    CHECK(z > -4.75f + 0.3f - 0.05f && z < -4.75f + 0.3f + 0.05f);   // stopped by the wall at its radius

    SetCharacterFootPosition(c, glm::vec3(2, 0.02f, 0));
    walk(glm::vec3(1, 0, 0), 1.0f);                                      // 3 m: ends on the step (x 4.5..5.5)
    CHECK(GetCharacterFootPosition(c).x > 4.6f);                         // climbed the 0.2 m step
    CHECK(GetCharacterFootPosition(c).y > 0.15f);

    SetCharacterFootPosition(c, glm::vec3(-2, 0.02f, 0));
    walk(glm::vec3(-1, 0, 0), 1.5f);
    CHECK(GetCharacterFootPosition(c).x > -4.5f + 0.3f - 0.05f);         // blocked by the 0.6 m block
}

TEST_CASE("Physics: destroyed ids are stale; stats count live objects") {
    Fixture f;
    const BodyId a = MakeBody(BodyType::Dynamic, glm::vec3(0, 5, 0), Box(glm::vec3(0.5f)));
    MakeBody(BodyType::Static, glm::vec3(0), Box(glm::vec3(0.5f)));
    MakeBody(BodyType::Kinematic, glm::vec3(3, 0, 0), Box(glm::vec3(0.5f)));
    CharacterDesc cd;
    const CharacterId ch = CreateCharacter(cd);
    Stats stats = GetStats();
    CHECK(stats.dynamicBodies == 1 && stats.staticBodies == 1 && stats.kinematicBodies == 1 && stats.characters == 1);
    DestroyBody(a);
    CHECK(!IsValid(a));
    const BodyId b = MakeBody(BodyType::Dynamic, glm::vec3(0, 5, 0), Box(glm::vec3(0.5f)));   // reuses the slot
    CHECK(IsValid(b) && b != a && !IsValid(a));
    DestroyBody(a);                                       // stale: no effect on b
    CHECK(IsValid(b));
    DestroyCharacter(ch);
    CHECK(!IsValid(ch));
    DestroyAll();
    stats = GetStats();
    CHECK(stats.dynamicBodies == 0 && stats.staticBodies == 0 && stats.kinematicBodies == 0 && stats.characters == 0);
}
