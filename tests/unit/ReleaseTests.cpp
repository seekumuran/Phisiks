// 1.1 release checks: the generated public version header, scene format version 2 (every
// component stored, stable ids, migration from version 1), entity / component JSON and snapshots.
#include "TestFramework.h"

#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/Version.h"
#include "Phisiks/World/Terrain.h"

#include <cmath>
#include <string>
#include <vector>

using namespace Phisiks;

// ============================================================================ version
TEST_CASE("Version: Phisiks/Version.h reports the CMake project version") {
    // PHISIKS_EXPECTED_* come from project(Phisiks VERSION ...) through the test target's definitions.
    CHECK(PHISIKS_VERSION_MAJOR == PHISIKS_EXPECTED_MAJOR);
    CHECK(PHISIKS_VERSION_MINOR == PHISIKS_EXPECTED_MINOR);
    CHECK(PHISIKS_VERSION_PATCH == PHISIKS_EXPECTED_PATCH);
    CHECK(std::string(PHISIKS_VERSION_STRING) == PHISIKS_EXPECTED_STRING);
    CHECK(std::string(PHISIKS_VERSION_STRING) == std::to_string(PHISIKS_VERSION_MAJOR) + "." + std::to_string(PHISIKS_VERSION_MINOR) + "." + std::to_string(PHISIKS_VERSION_PATCH));
    CHECK(PHISIKS_VERSION == PHISIKS_VERSION_MAJOR * 10000 + PHISIKS_VERSION_MINOR * 100 + PHISIKS_VERSION_PATCH);
}

// ============================================================================ what a save does not keep
TEST_CASE("SceneSerializer: UnsavedComponents is empty when every component is stored") {
    Scene scene("Stored");
    Entity e = scene.CreateEntity("Box");
    e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
    e.Add<Light>();
    scene.CreateEntity("Camera").Add<Camera>();
    scene.CreateEntity("J").Add<Joint>();
    scene.CreateEntity("Lua").Add<Script>();
    scene.CreateEntity("Target").Add<Health>();   // data only: stored
    CHECK(SceneSerializer::UnsavedComponents(scene).empty());
}

TEST_CASE("SceneSerializer: UnsavedComponents reports only C++ callbacks") {
    Scene scene("Callbacks");
    scene.CreateEntity("Dying").Add<Health>().onDeath = [](GameContext&, Entity, Entity) {};
    scene.CreateEntity("Button").Add<Interactable>().onInteract = [](GameContext&, Entity, Entity) {};
    const std::vector<SceneSerializer::UnsavedComponent> lost = SceneSerializer::UnsavedComponents(scene);
    CHECK(lost.size() == 2);
    CHECK(lost.size() == 2 && lost[0].type.find("Health.onDeath") == 0 && lost[0].count == 1);
    CHECK(lost.size() == 2 && lost[1].type.find("Interactable.onInteract") == 0 && lost[1].count == 1);
}

// ============================================================================ format version 2
namespace {
    void FullScene(Scene& s) {
        Entity a = s.CreateEntity("Arm");
        a.Get<Transform>().position = glm::vec3(1, 2, 3);
        a.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
        Collider c;
        c.shape.halfExtents = glm::vec3(0.2f, 1.0f, 0.2f);
        a.Add<Collider>(c);
        a.Add<Rigidbody>().mass = 3.0f;
        Joint j;
        j.type = Physics::JointType::Hinge;
        j.connectedTo = "Base";
        j.axis = glm::vec3(0, 0, 1);
        j.enableLimits = true;
        j.upperLimit = 1.2f;
        a.Add<Joint>(j);
        Entity base = s.CreateEntity("Base");
        a.SetParent(base, false);
        Script sc;
        sc.source = "function on_update(dt) end";
        base.Add<Script>(sc);
        Entity nav = s.CreateEntity("Nav");
        nav.Add<NavigationSurface>().desc.cellSize = 0.1f;
        nav.Add<NavAgent>().maxSpeed = 0.7f;
        Entity t = s.CreateEntity("Ground");
        t.Add<Terrain>().generator.seed = 42;
        t.Add<FoliageScatter>().scatter.count = 17;
        Entity v = s.CreateEntity("Fx");
        v.Add<Decal>().pattern = Render::DecalPattern::Hazard;
        v.Add<Trail>().lifetime = 2.5f;
        v.Add<Beam>().target = "Base";
        v.Add<OrbitPath>().radiusX = 4.0f;
        Entity m = s.CreateEntity("Walker");
        m.Add<SkinnedModel>().path = "models/walker.glb";
        CameraPath cp;
        cp.points = { { 0, 1, 0 }, { 2, 1, 0 }, { 2, 1, 2 } };
        cp.duration = 7.0f;
        m.Add<CameraPath>(cp);
        m.Add<Health>().max = 250.0f;
        m.Add<Interactable>().prompt = "open";
    }
}

TEST_CASE("SceneSerializer v2: every component round-trips; ids are kept") {
    Scene a("Full");
    FullScene(a);
    const std::string json = SceneSerializer::ToJson(a);
    CHECK(json.find("\"version\": 2") != std::string::npos);
    Scene b("Full");
    std::string error;
    CHECK(SceneSerializer::FromJson(b, json, &error));
    CHECK(error.empty());
    CHECK(SceneSerializer::ToJson(b) == json);                                       // lossless
    CHECK(b.FindByName("Arm").GetGuid() == a.FindByName("Arm").GetGuid());           // stable identity
    CHECK(b.FindByName("Arm").GetParent() == b.FindByName("Base"));
    CHECK(b.FindByName("Arm").Get<Joint>().type == Physics::JointType::Hinge && b.FindByName("Arm").Get<Joint>().connectedTo == "Base");
    CHECK(std::fabs(b.FindByName("Nav").Get<NavAgent>().maxSpeed - 0.7f) < 1e-6f);
    CHECK(b.FindByName("Ground").Get<Terrain>().generator.seed == 42 && b.FindByName("Ground").Get<FoliageScatter>().scatter.count == 17);
    CHECK(b.FindByName("Fx").Get<Decal>().pattern == Render::DecalPattern::Hazard);
    CHECK(b.FindByName("Walker").Get<CameraPath>().points.size() == 3 && b.FindByName("Walker").Get<SkinnedModel>().path == "models/walker.glb");
    CHECK(std::fabs(b.FindByName("Walker").Get<Health>().max - 250.0f) < 1e-6f && b.FindByName("Walker").Get<Interactable>().prompt == "open");
    CHECK(SceneSerializer::UnsavedComponents(b).empty());
}

TEST_CASE("SceneSerializer v2: version 1 files load (migration) and get new ids") {
    const std::string v1 = R"({"format":"phisiks-scene","version":1,"name":"Old","materials":{},
        "entities":[{"name":"Root","parent":-1,"transform":{"position":[1,0,0],"rotation":[1,0,0,0],"scale":[1,1,1]},
                     "meshRenderer":{"mesh":"Sphere","material":"","uvScale":[1,1],"visible":true,"castShadows":true}},
                    {"name":"Child","parent":0,"transform":{"position":[0,2,0]}}]})";
    Scene s("Old");
    std::string error;
    CHECK(SceneSerializer::FromJson(s, v1, &error));
    CHECK(s.EntityCount() == 2);
    Entity root = s.FindByName("Root"), child = s.FindByName("Child");
    CHECK(root && child && child.GetParent() == root && root.Get<MeshRenderer>().mesh == "Sphere");
    CHECK(root.GetGuid() != 0 && child.GetGuid() != 0 && root.GetGuid() != child.GetGuid());
    const std::string rewritten = SceneSerializer::ToJson(s);
    CHECK(rewritten.find("\"version\": 2") != std::string::npos && rewritten.find("\"id\"") != std::string::npos);
    Scene t("NoVersion");   // no version key at all = version 1
    CHECK(SceneSerializer::FromJson(t, R"({"format":"phisiks-scene","entities":[{"name":"A"}]})", &error) && t.EntityCount() == 1);
}

TEST_CASE("SceneSerializer v2: a damaged component value or duplicate id changes nothing") {
    Scene s("Keep");
    s.CreateEntity("Survivor");
    std::string error;
    const std::string badValue = R"({"format":"phisiks-scene","version":2,"entities":[{"id":"00000000000000aa","name":"X","light":{"intensity":"bright"}}]})";
    CHECK(!SceneSerializer::FromJson(s, badValue, &error) && !error.empty() && s.FindByName("Survivor"));
    const std::string duplicate = R"({"format":"phisiks-scene","version":2,"entities":[{"id":"00000000000000aa","name":"X"},{"id":"00000000000000aa","name":"Y"}]})";
    CHECK(!SceneSerializer::FromJson(s, duplicate, &error) && error.find("duplicate") != std::string::npos && s.FindByName("Survivor"));
    CHECK(!SceneSerializer::FromJson(s, R"({"format":"phisiks-scene","version":3,"entities":[]})", &error) && s.FindByName("Survivor"));
    CHECK(!SceneSerializer::FromJson(s, R"({"format":"phisiks-scene","version":2,"entities":[{"id":"xyz"}]})", &error) && s.FindByName("Survivor"));
}

TEST_CASE("SceneSerializer: entity trees, component JSON and validation") {
    Scene s("Trees");
    FullScene(s);
    Entity base = s.FindByName("Base");
    const uint64_t baseGuid = base.GetGuid(), armGuid = s.FindByName("Arm").GetGuid();
    const std::string tree = SceneSerializer::EntityToJson(base, true);
    const size_t before = s.EntityCount();
    // duplicate: new ids, same structure
    Entity copy = SceneSerializer::EntityFromJson(s, tree, Entity(), false);
    CHECK(copy && copy.GetGuid() != baseGuid && copy.GetChildren().size() == 1 && s.EntityCount() == before + 2);
    CHECK(copy.GetChildren().size() == 1 && copy.GetChildren()[0].Get<Joint>().type == Physics::JointType::Hinge);
    // delete + restore with the same ids
    s.DestroyImmediate(base);
    CHECK(!s.FindByGuid(baseGuid) && !s.FindByGuid(armGuid));
    Entity restored = SceneSerializer::EntityFromJson(s, tree, Entity(), true);
    CHECK(restored && restored.GetGuid() == baseGuid && s.FindByGuid(armGuid) && s.FindByGuid(armGuid).GetParent() == restored);
    // component JSON: a patch keeps the other fields; bad values are rejected without change
    Entity arm = s.FindByGuid(armGuid);
    CHECK(SceneSerializer::SetComponentJson(arm, "rigidbody", R"({"mass": 9.5})"));
    CHECK(std::fabs(arm.Get<Rigidbody>().mass - 9.5f) < 1e-6f && arm.Get<Rigidbody>().useGravity);
    std::string error;
    CHECK(!SceneSerializer::SetComponentJson(arm, "rigidbody", R"({"mass": "heavy"})", &error) && std::fabs(arm.Get<Rigidbody>().mass - 9.5f) < 1e-6f);
    CHECK(!SceneSerializer::SetComponentJson(arm, "warpDrive", R"({})", &error) && error.find("unknown") != std::string::npos);
    CHECK(SceneSerializer::SetComponentJson(arm, "light", R"({"type":"Spot","intensity":3})") && arm.Get<Light>().type == LightType::Spot);
    CHECK(SceneSerializer::RemoveComponent(arm, "light") && !arm.Has<Light>());
    // ApplyEntityJson: exact state (components absent from the JSON are removed)
    const std::string state = SceneSerializer::EntityToJson(arm, false);
    arm.Add<Decal>();
    arm.Get<Transform>().position = glm::vec3(9, 9, 9);
    CHECK(SceneSerializer::ApplyEntityJson(arm, state) && !arm.Has<Decal>() && arm.Get<Transform>().position == glm::vec3(1, 2, 3));
    CHECK(!SceneSerializer::ApplyEntityJson(arm, "{not json", &error) && arm.Has<Joint>());
    CHECK(SceneSerializer::ComponentKeys().size() == 24);   // 23 + cadModel (1.1 CAD)
}

TEST_CASE("SceneSerializer: RestoreSnapshot keeps handles, removes new entities, re-creates deleted ones") {
    Scene s("Play");
    FullScene(s);
    const std::string snapshot = SceneSerializer::ToJson(s, false);
    Entity arm = s.FindByName("Arm"), fx = s.FindByName("Fx");
    const uint64_t fxGuid = fx.GetGuid();
    const size_t count = s.EntityCount();
    // "play": move, re-parent, add components, create and destroy entities
    arm.Get<Transform>().position = glm::vec3(-5, 0, 0);
    arm.ClearParent(false);
    arm.Add<Light>();
    s.CreateEntity("Spawned").Add<MeshRenderer>();
    s.DestroyImmediate(fx);
    std::string error;
    CHECK(SceneSerializer::RestoreSnapshot(s, snapshot, &error));
    CHECK(arm.Valid() && arm.Get<Transform>().position == glm::vec3(1, 2, 3) && !arm.Has<Light>());   // same handle
    CHECK(arm.GetParent() == s.FindByName("Base"));
    CHECK(!s.FindByName("Spawned"));
    CHECK(s.FindByGuid(fxGuid) && std::fabs(s.FindByGuid(fxGuid).Get<Trail>().lifetime - 2.5f) < 1e-6f);
    CHECK(s.EntityCount() == count);
}

TEST_CASE("Scene: guids are unique, non-zero and settable") {
    Scene s("Ids");
    Entity a = s.CreateEntity("A"), b = s.CreateEntity("B");
    CHECK(a.GetGuid() != 0 && b.GetGuid() != 0 && a.GetGuid() != b.GetGuid());
    CHECK(s.FindByGuid(a.GetGuid()) == a && !s.FindByGuid(0));
    CHECK(!s.SetGuid(b, a.GetGuid()));   // taken
    CHECK(s.SetGuid(b, 0x1234) && b.GetGuid() == 0x1234 && s.FindByGuid(0x1234) == b);
}
