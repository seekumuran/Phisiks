// Headless Phisiks::Scene tests: hierarchy/transform propagation, entity and component lifecycle,
// physics body synchronisation, presentation sync (against a recording backend) and scene
// management. No window, renderer or audio device is involved.
#include "TestFramework.h"

#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"
#include "scene/SceneInternal.h"   // white-box: install a recording presentation backend

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>

using namespace Phisiks;

namespace {
    constexpr float EPS = 1e-4f;

    bool Near(const glm::vec3& a, const glm::vec3& b, float eps = EPS) { return glm::length(a - b) < eps; }
    bool Near(const glm::quat& a, const glm::quat& b, float eps = EPS) { return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < eps; }
    glm::quat Yaw(float radians) { return glm::angleAxis(radians, glm::vec3(0, 1, 0)); }

    struct PhysicsFixture {
        PhysicsFixture() { ok = Physics::Init(); }
        ~PhysicsFixture() { Physics::Shutdown(); }
        bool ok = false;
    };

    Collider BoxCollider(glm::vec3 halfExtents = glm::vec3(0.5f)) {
        Collider c;
        c.shape.shape = Physics::ShapeType::Box;
        c.shape.halfExtents = halfExtents;
        return c;
    }

    // Records every backend call; ids are never reused.
    struct RecordingRender final : SceneInternal::IRenderBackend {
        std::map<uint64_t, glm::mat4> meshes;
        std::map<uint64_t, glm::vec3> lights;
        int meshCreates = 0, meshUpdates = 0, meshDestroys = 0, lightUpdates = 0;
        bool camera = false;
        glm::mat4 view{ 1.0f };
        uint64_t next = 0;

        std::map<uint64_t, uint32_t> entityIds;
        glm::vec3 lastLightDirection{ 0.0f };
        uint64_t CreateMesh(const MeshRenderer&, const glm::mat4& world, uint32_t entityId) override { ++meshCreates; meshes[++next] = world; entityIds[next] = entityId; return next; }
        void UpdateMesh(uint64_t id, const MeshRenderer&, const glm::mat4& world) override { ++meshUpdates; meshes[id] = world; }
        void DestroyMesh(uint64_t id) override { ++meshDestroys; meshes.erase(id); }
        uint64_t CreateLight(const Light&, const glm::vec3& p, const glm::vec3& d) override { lights[++next] = p; lastLightDirection = d; return next; }
        void UpdateLight(uint64_t id, const Light&, const glm::vec3& p, const glm::vec3& d) override { ++lightUpdates; lights[id] = p; lastLightDirection = d; }
        void DestroyLight(uint64_t id) override { lights.erase(id); }
        std::vector<SceneInternal::CameraView> cameras;
        void SetCameras(const std::vector<SceneInternal::CameraView>& list) override {
            cameras = list;
            camera = !list.empty();
            if (camera) view = list.front().view;
        }
        RenderStats Stats() override { RenderStats s; s.meshObjects = uint32_t(meshes.size()); s.lights = uint32_t(lights.size()); s.cameraBound = camera; return s; }
    };

    struct RecordingAudio final : SceneInternal::IAudioBackend {
        std::map<uint64_t, std::string> playing;
        uint64_t next = 0;
        uint64_t Play(const std::string& clip, float, bool) override { playing[++next] = clip; return next; }
        void Stop(uint64_t voice) override { playing.erase(voice); }
        bool IsPlaying(uint64_t voice) override { return playing.count(voice) != 0; }
    };

    struct BackendFixture {
        RecordingRender render;
        RecordingAudio audio;
        BackendFixture() { SceneInternal::SetBackends(&render, &audio); }
        ~BackendFixture() { SceneInternal::SetBackends(nullptr, nullptr); }
    };
}

// ============================================================================ entities
TEST_CASE("Scene: entities get Name and Transform; ids are versioned") {
    Scene scene("Test");
    Entity a = scene.CreateEntity("A");
    CHECK(a.Valid());
    CHECK(a.Has<Name>() && a.Has<Transform>());
    CHECK(a.GetName() == "A");
    CHECK(scene.EntityCount() == 1);
    CHECK(scene.FindByName("A") == a);
    CHECK(scene.FromId(a.Id()) == a);

    const uint32_t staleId = a.Id();
    scene.DestroyImmediate(a);
    CHECK(!a.Valid());
    CHECK(scene.EntityCount() == 0);
    Entity b = scene.CreateEntity("B");          // recycles the slot with a new version
    CHECK(!scene.FromId(staleId).Valid());
    CHECK(b.Valid() && !a.Valid());
    CHECK(!scene.FindByName("A").Valid());
}

TEST_CASE("Scene: component add / get / replace / remove") {
    Scene scene;
    Entity e = scene.CreateEntity();
    CHECK(!e.Has<MeshRenderer>());
    CHECK(e.TryGet<MeshRenderer>() == nullptr);
    e.Add<MeshRenderer>().mesh = "Sphere";
    CHECK(e.Has<MeshRenderer>() && e.Get<MeshRenderer>().mesh == "Sphere");
    e.Add<MeshRenderer>(MeshRenderer{ "Cube", "Wood" });   // replace
    CHECK(e.Get<MeshRenderer>().mesh == "Cube" && e.Get<MeshRenderer>().material == "Wood");
    e.Remove<MeshRenderer>();
    CHECK(!e.Has<MeshRenderer>());
    e.Remove<MeshRenderer>();                               // removing twice is harmless

    int count = 0;
    scene.CreateEntity().Add<Light>();
    scene.CreateEntity().Add<Light>();
    scene.Each<Light>([&](Entity, Light&) { ++count; });
    CHECK(count == 2);

    Entity invalid;
    CHECK(!invalid.Has<Transform>() && invalid.TryGet<Transform>() == nullptr);
}

// ============================================================================ hierarchy
TEST_CASE("Hierarchy: world transform composes parent TRS") {
    Scene scene;
    Entity parent = scene.CreateEntity("Parent");
    Entity child = scene.CreateEntity("Child");
    child.SetParent(parent, false);

    parent.Get<Transform>().position = glm::vec3(10, 0, 0);
    parent.Get<Transform>().rotation = Yaw(glm::half_pi<float>());   // +X -> -Z
    parent.Get<Transform>().scale = glm::vec3(2.0f);
    child.Get<Transform>().position = glm::vec3(1, 0, 0);

    CHECK(Near(child.WorldPosition(), glm::vec3(10, 0, -2)));
    CHECK(Near(child.WorldScale(), glm::vec3(2.0f)));
    CHECK(Near(child.WorldRotation(), Yaw(glm::half_pi<float>())));
    CHECK(Near(glm::vec3(child.WorldMatrix()[3]), glm::vec3(10, 0, -2)));

    // Three levels: changes to the root propagate through the chain.
    Entity grandchild = scene.CreateEntity("Grandchild");
    grandchild.SetParent(child, false);
    grandchild.Get<Transform>().position = glm::vec3(0, 1, 0);
    CHECK(Near(grandchild.WorldPosition(), glm::vec3(10, 2, -2)));
    parent.Get<Transform>().position = glm::vec3(0, 5, 0);
    CHECK(Near(grandchild.WorldPosition(), glm::vec3(0, 7, -2)));
    CHECK(grandchild.IsDescendantOf(parent));
    CHECK(!parent.IsDescendantOf(grandchild));
}

TEST_CASE("Hierarchy: SetParent keeps world pose; ClearParent restores it") {
    Scene scene;
    Entity parent = scene.CreateEntity();
    parent.Get<Transform>().position = glm::vec3(3, 1, -2);
    parent.Get<Transform>().rotation = Yaw(0.7f);
    parent.Get<Transform>().scale = glm::vec3(2.0f, 2.0f, 2.0f);

    Entity e = scene.CreateEntity();
    e.Get<Transform>().position = glm::vec3(1, 2, 3);
    e.Get<Transform>().rotation = glm::angleAxis(0.3f, glm::normalize(glm::vec3(1, 1, 0)));
    const glm::vec3 worldPos = e.WorldPosition();
    const glm::quat worldRot = e.WorldRotation();

    CHECK(e.SetParent(parent, true));
    CHECK(e.GetParent() == parent);
    CHECK(Near(e.WorldPosition(), worldPos));
    CHECK(Near(e.WorldRotation(), worldRot));
    CHECK(Near(e.WorldScale(), glm::vec3(1.0f)));
    CHECK(Near(e.Get<Transform>().scale, glm::vec3(0.5f)));   // local scale compensates the parent

    e.ClearParent(true);
    CHECK(!e.GetParent().Valid());
    CHECK(Near(e.Get<Transform>().position, worldPos));
    CHECK(Near(e.Get<Transform>().scale, glm::vec3(1.0f)));
    CHECK(parent.GetChildren().empty());
}

TEST_CASE("Hierarchy: reparenting and cycle rejection") {
    Scene scene;
    Entity a = scene.CreateEntity("A"), b = scene.CreateEntity("B"), c = scene.CreateEntity("C");
    CHECK(b.SetParent(a));
    CHECK(c.SetParent(b));
    CHECK(!a.SetParent(c));       // a -> c -> b -> a would be a cycle
    CHECK(!a.SetParent(a));
    CHECK(!a.GetParent().Valid());

    Scene other;
    CHECK(!c.SetParent(other.CreateEntity()));   // cross-scene parenting is rejected

    CHECK(c.SetParent(a));        // move c from b to a
    CHECK(b.GetChildren().empty());
    CHECK(a.GetChildren().size() == 2);
}

TEST_CASE("Hierarchy: SetWorldPose under a rotated, scaled parent") {
    Scene scene;
    Entity parent = scene.CreateEntity();
    parent.Get<Transform>().position = glm::vec3(1, 2, 3);
    parent.Get<Transform>().rotation = Yaw(1.1f);
    parent.Get<Transform>().scale = glm::vec3(3.0f);
    Entity child = scene.CreateEntity();
    child.SetParent(parent, false);

    const glm::vec3 target(-4, 0.5f, 7);
    const glm::quat targetRot = glm::angleAxis(0.4f, glm::vec3(1, 0, 0));
    child.SetWorldPose(target, targetRot);
    CHECK(Near(child.WorldPosition(), target, 1e-3f));
    CHECK(Near(child.WorldRotation(), targetRot));
}

// ============================================================================ destruction
TEST_CASE("Destroy: deferred, recursive, and safe to repeat") {
    Scene scene;
    Entity root = scene.CreateEntity("Root");
    Entity child = scene.CreateEntity("Child");
    Entity leaf = scene.CreateEntity("Leaf");
    Entity bystander = scene.CreateEntity("Bystander");
    child.SetParent(root);
    leaf.SetParent(child);

    root.Destroy();
    root.Destroy();                                      // queued once
    CHECK(root.Valid() && leaf.Valid());                 // deferred
    CHECK(scene.IsPendingDestroy(root));
    CHECK(scene.IsPendingDestroy(leaf));                 // descendants count as pending
    CHECK(!scene.FindByName("Root").Valid());            // pending entities are not found by name
    CHECK(scene.EntityCount() == 4);

    scene.FlushDestroyed();
    CHECK(!root.Valid() && !child.Valid() && !leaf.Valid());
    CHECK(bystander.Valid());
    CHECK(scene.EntityCount() == 1);
    scene.FlushDestroyed();                              // empty queue
    scene.DestroyImmediate(root);                        // stale handle: no-op
    CHECK(scene.EntityCount() == 1);
}

TEST_CASE("Destroy: destroying a child detaches it from its parent") {
    Scene scene;
    Entity parent = scene.CreateEntity();
    Entity a = scene.CreateEntity(), b = scene.CreateEntity();
    a.SetParent(parent);
    b.SetParent(parent);
    scene.DestroyImmediate(a);
    const auto children = parent.GetChildren();
    CHECK(children.size() == 1 && children[0] == b);
}

TEST_CASE("Scene: Clear destroys everything") {
    Scene scene;
    for (int i = 0; i < 10; ++i) scene.CreateEntity();
    Entity p = scene.CreateEntity();
    scene.CreateEntity().SetParent(p);
    p.Destroy();
    scene.Clear();
    CHECK(scene.EntityCount() == 0);
    scene.FlushDestroyed();   // the queue was cleared too
    CHECK(scene.EntityCount() == 0);
}

// ============================================================================ physics sync
TEST_CASE("Physics sync: Collider -> static body, + Rigidbody -> dynamic, removal destroys") {
    PhysicsFixture physics;
    CHECK(physics.ok);
    Scene scene;
    Entity wall = scene.CreateEntity("Wall");
    wall.Add<Collider>(BoxCollider());
    Entity crate = scene.CreateEntity("Crate");
    crate.Get<Transform>().position = glm::vec3(0, 5, 0);
    crate.Add<Collider>(BoxCollider());
    crate.Add<Rigidbody>().mass = 4.0f;

    CHECK(!crate.GetBody().Valid());                     // created by the sync system
    scene.SyncPhysicsBodies();
    CHECK(Physics::GetBodyType(wall.GetBody()) == Physics::BodyType::Static);
    CHECK(Physics::GetBodyType(crate.GetBody()) == Physics::BodyType::Dynamic);
    CHECK_NEAR(Physics::GetMass(crate.GetBody()), 4.0f, 1e-4);
    CHECK(Physics::GetStats().staticBodies == 1 && Physics::GetStats().dynamicBodies == 1);
    CHECK(scene.FromBody(crate.GetBody()) == crate);     // user data maps back to the entity

    // Rigidbody removed -> rebuilt as a static body.
    crate.Remove<Rigidbody>();
    scene.SyncPhysicsBodies();
    CHECK(Physics::GetBodyType(crate.GetBody()) == Physics::BodyType::Static);
    CHECK(Physics::GetStats().staticBodies == 2 && Physics::GetStats().dynamicBodies == 0);

    // Collider removed -> no body.
    wall.Remove<Collider>();
    scene.SyncPhysicsBodies();
    CHECK(!wall.GetBody().Valid());
    CHECK(Physics::GetStats().staticBodies == 1);

    // Entity destruction releases its body.
    scene.DestroyImmediate(crate);
    CHECK(Physics::GetStats().staticBodies == 0);
}

TEST_CASE("Physics sync: collider scales with the world transform") {
    PhysicsFixture physics;
    Scene scene;
    Entity parent = scene.CreateEntity();
    parent.Get<Transform>().scale = glm::vec3(2.0f);
    Entity box = scene.CreateEntity();
    box.SetParent(parent, false);
    box.Get<Transform>().position = glm::vec3(0, 3, 0);  // world (0, 6, 0)
    box.Get<Transform>().scale = glm::vec3(1.0f, 0.5f, 3.0f);
    box.Add<Collider>(BoxCollider());
    scene.SyncPhysicsBodies();

    glm::vec3 lo, hi;
    Physics::GetBounds(box.GetBody(), lo, hi);
    // half extents 0.5 * (2, 1, 6) = (1, 0.5, 3) around (0, 6, 0); PhysX inflates world bounds by 1%.
    CHECK(Near(lo, glm::vec3(-1.0f, 5.5f, -3.0f), 0.06f));
    CHECK(Near(hi, glm::vec3(1.0f, 6.5f, 3.0f), 0.06f));
}

TEST_CASE("Physics sync: dynamic bodies drive transforms (in parent space)") {
    PhysicsFixture physics;
    Scene scene;
    Entity holder = scene.CreateEntity();
    holder.Get<Transform>().position = glm::vec3(100, 0, 0);
    Entity ball = scene.CreateEntity();
    ball.SetParent(holder, false);
    ball.Get<Transform>().position = glm::vec3(0, 10, 0);
    Collider c;
    c.shape.shape = Physics::ShapeType::Sphere;
    c.shape.radius = 0.5f;
    ball.Add<Collider>(c);
    ball.Add<Rigidbody>().linearDamping = 0.0f;
    scene.SyncPhysicsBodies();

    for (int i = 0; i < 30; ++i) Physics::Step(1.0f / 60.0f);   // 0.5 s free fall
    scene.PullPhysicsPoses();
    const float expectedY = 10.0f - 0.5f * 9.81f * 0.25f;        // 8.774 m
    CHECK_NEAR(ball.WorldPosition().y, expectedY, 0.02);
    CHECK_NEAR(ball.WorldPosition().x, 100.0, 1e-3);
    CHECK_NEAR(ball.Get<Transform>().position.x, 0.0, 1e-3);    // local stays relative to the holder
}

TEST_CASE("Physics sync: kinematic bodies follow their transform; MarkPhysicsDirty rebuilds") {
    PhysicsFixture physics;
    Scene scene;
    Entity platform = scene.CreateEntity();
    platform.Add<Collider>(BoxCollider());
    platform.Add<Rigidbody>().type = Physics::BodyType::Kinematic;
    scene.SyncPhysicsBodies();
    const Physics::BodyId first = platform.GetBody();

    platform.Get<Transform>().position = glm::vec3(2, 0, 0);
    scene.SyncPhysicsBodies();
    Physics::Step(1.0f / 60.0f);
    CHECK(Near(Physics::GetPosition(platform.GetBody()), glm::vec3(2, 0, 0), 1e-3f));
    CHECK(platform.GetBody() == first);                  // a moved kinematic is not rebuilt

    platform.Get<Collider>().shape.halfExtents = glm::vec3(2.0f);   // in-place edit: no signal
    platform.MarkPhysicsDirty();
    scene.SyncPhysicsBodies();
    CHECK(!Physics::IsValid(first));
    CHECK(platform.GetBody().Valid() && !(platform.GetBody() == first));
}

TEST_CASE("Physics sync: SetWorldPose teleports the body; impulses reach it") {
    PhysicsFixture physics;
    Scene scene;
    Entity box = scene.CreateEntity();
    box.Add<Collider>(BoxCollider());
    Rigidbody rb;
    rb.mass = 2.0f;
    rb.useGravity = false;
    rb.linearDamping = 0.0f;
    box.Add<Rigidbody>(rb);
    scene.SyncPhysicsBodies();

    box.SetWorldPose(glm::vec3(5, 5, 5), glm::quat(1, 0, 0, 0));
    CHECK(Near(Physics::GetPosition(box.GetBody()), glm::vec3(5, 5, 5), 1e-4f));
    box.AddImpulse(glm::vec3(4, 0, 0));                  // dv = J / m = 2 m/s
    Physics::Step(1.0f / 60.0f);
    CHECK_NEAR(box.GetLinearVelocity().x, 2.0, 1e-3);
}

TEST_CASE("Physics sync: no-op without a physics world") {
    Scene scene;
    Entity e = scene.CreateEntity();
    e.Add<Collider>(BoxCollider());
    scene.SyncPhysicsBodies();                           // physics not initialised
    CHECK(!e.GetBody().Valid());
    scene.PullPhysicsPoses();
}

// ============================================================================ presentation
TEST_CASE("Presentation: render objects follow MeshRenderer lifecycle and transforms") {
    BackendFixture backend;
    Scene scene;
    Entity e = scene.CreateEntity();
    e.Add<MeshRenderer>();
    scene.SyncPresentation();
    CHECK(backend.render.meshes.size() == 1 && backend.render.meshCreates == 1);
    CHECK(backend.render.entityIds.begin()->second == e.Id());   // id buffer / picking key

    scene.SyncPresentation();                            // unchanged: no update
    CHECK(backend.render.meshUpdates == 0);

    Entity parent = scene.CreateEntity();
    e.SetParent(parent, false);
    parent.Get<Transform>().position = glm::vec3(0, 3, 0);
    scene.SyncPresentation();                            // parent moved -> child's world changed
    CHECK(backend.render.meshUpdates == 1);
    CHECK(Near(glm::vec3(backend.render.meshes.begin()->second[3]), glm::vec3(0, 3, 0)));

    e.Remove<MeshRenderer>();
    scene.SyncPresentation();
    CHECK(backend.render.meshes.empty());

    e.Add<MeshRenderer>();
    scene.SyncPresentation();
    scene.DestroyImmediate(parent);                      // destroys e with it
    CHECK(backend.render.meshes.empty());
    CHECK(backend.render.meshDestroys == 2);
}

TEST_CASE("Presentation: lights, camera and scene teardown") {
    BackendFixture backend;
    {
        Scene scene;
        Entity lamp = scene.CreateEntity();
        lamp.Get<Transform>().position = glm::vec3(1, 2, 3);
        lamp.Add<Light>();
        Entity cam = scene.CreateEntity("MainCamera");
        cam.Add<Camera>();
        cam.Get<Transform>().position = glm::vec3(0, 0, 5);
        scene.SyncPresentation();
        CHECK(backend.render.lights.size() == 1 && Near(backend.render.lights.begin()->second, glm::vec3(1, 2, 3)));
        CHECK(backend.render.camera);
        CHECK(scene.GetPrimaryCamera() == cam);
        // View matrix maps the camera position to the origin.
        CHECK(Near(glm::vec3(backend.render.view * glm::vec4(0, 0, 5, 1)), glm::vec3(0.0f)));

        lamp.Get<Light>().intensity = 3.0f;
        scene.SyncPresentation();
        CHECK(backend.render.lightUpdates == 1);

        // Direction is the entity's world -Z: rotating it re-syncs the light.
        lamp.Get<Transform>().rotation = glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1, 0, 0));   // -Z -> -Y
        scene.SyncPresentation();
        CHECK(backend.render.lightUpdates == 2);
        CHECK(Near(backend.render.lastLightDirection, glm::vec3(0, -1, 0)));

        cam.Get<Camera>().enabled = false;
        scene.SyncPresentation();
        CHECK(!backend.render.camera);
        cam.Get<Camera>().enabled = true;
        scene.SyncPresentation();
        CHECK(backend.render.camera);
    }   // scene destroyed
    CHECK(backend.render.lights.empty());
    CHECK(!backend.render.camera);
}

TEST_CASE("Presentation: AudioSource play / stop / playOnStart / release") {
    BackendFixture backend;
    Scene scene;
    Entity speaker = scene.CreateEntity();
    AudioSource& music = speaker.Add<AudioSource>();
    music.clip = "music.wav";
    music.loop = true;
    music.playOnStart = true;
    scene.SyncPresentation();
    CHECK(speaker.Get<AudioSource>().isPlaying && speaker.Get<AudioSource>().voice != 0);
    CHECK(backend.audio.playing.size() == 1);

    speaker.Get<AudioSource>().Stop();
    scene.SyncPresentation();
    CHECK(!speaker.Get<AudioSource>().isPlaying && backend.audio.playing.empty());

    speaker.Get<AudioSource>().Play();
    scene.SyncPresentation();
    CHECK(backend.audio.playing.size() == 1);
    scene.DestroyImmediate(speaker);                     // voice released with the entity
    CHECK(backend.audio.playing.empty());
}

TEST_CASE("Presentation: no backends (headless) is a no-op") {
    Scene scene;
    Entity e = scene.CreateEntity();
    e.Add<MeshRenderer>();
    e.Add<Light>();
    e.Add<AudioSource>().Play();
    scene.SyncPresentation();
    CHECK(!e.Get<AudioSource>().isPlaying);
    CHECK(GetRenderStats().meshObjects == 0);
}

TEST_CASE("Presentation: every enabled camera is synced, sorted by order, with its settings") {
    BackendFixture backend;
    Scene scene;
    Entity main = scene.CreateEntity("Main");
    main.Add<Camera>().order = 0;
    Entity top = scene.CreateEntity("Top");
    Camera& ortho = top.Add<Camera>();
    ortho.projection = Projection::Orthographic;
    ortho.orthoHeight = 24.0f;
    ortho.viewport = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
    ortho.order = -1;                                   // draws first
    ortho.primary = false;
    top.Get<Transform>().position = glm::vec3(0, 40, 0);
    Entity off = scene.CreateEntity("Off");
    off.Add<Camera>().enabled = false;

    scene.SyncPresentation();
    const auto& views = backend.render.cameras;
    CHECK(views.size() == 2);
    CHECK(views[0].entityId == top.Id() && views[1].entityId == main.Id());
    CHECK(views[0].camera.projection == Projection::Orthographic && views[0].camera.orthoHeight == 24.0f);
    CHECK(views[0].camera.viewport == glm::vec4(0.5f, 0.5f, 0.5f, 0.5f));
    CHECK(Near(glm::vec3(views[0].view * glm::vec4(0, 40, 0, 1)), glm::vec3(0.0f)));   // camera at the origin of its view
    CHECK(scene.GetPrimaryCamera() == main);
}
