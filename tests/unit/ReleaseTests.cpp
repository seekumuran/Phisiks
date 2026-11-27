// 1.1 release checks: the generated public version header, and the list of components that scene
// files do not store (the editor warns with it before a save would drop them).
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

// ============================================================================ components not stored in scene files
TEST_CASE("SceneSerializer: UnsavedComponents is empty when every component is stored") {
    Scene scene("Stored");
    Entity e = scene.CreateEntity("Box");
    e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
    e.Add<Light>();
    scene.CreateEntity("Camera").Add<Camera>();
    CHECK(SceneSerializer::UnsavedComponents(scene).empty());
}

TEST_CASE("SceneSerializer: UnsavedComponents lists every unstored type with its count, in a fixed order") {
    Scene scene("Unstored");
    scene.CreateEntity("J1").Add<Joint>();
    scene.CreateEntity("J2").Add<Joint>();
    scene.CreateEntity("J3").Add<Joint>();
    scene.CreateEntity("Lua").Add<Script>();
    scene.CreateEntity("Nav").Add<NavigationSurface>();
    scene.CreateEntity("Agent1").Add<NavAgent>();
    scene.CreateEntity("Agent2").Add<NavAgent>();
    scene.CreateEntity("Ground").Add<Terrain>();
    scene.CreateEntity("Trees").Add<FoliageScatter>();
    scene.CreateEntity("Scorch").Add<Decal>();
    scene.CreateEntity("Streak").Add<Trail>();
    scene.CreateEntity("Laser").Add<Beam>();
    scene.CreateEntity("Orbit").Add<OrbitPath>();
    scene.CreateEntity("Walker").Add<SkinnedModel>();
    scene.CreateEntity("Dolly").Add<CameraPath>();
    scene.CreateEntity("Target").Add<Health>();
    scene.CreateEntity("Button").Add<Interactable>();
    scene.CreateEntity("Plain").Add<MeshRenderer>(MeshRenderer{ "Cube", "" });   // stored: not listed

    const std::vector<SceneSerializer::UnsavedComponent> lost = SceneSerializer::UnsavedComponents(scene);
    const std::vector<std::pair<std::string, size_t>> expected = {
        { "Joint", 3 }, { "Script", 1 }, { "NavigationSurface", 1 }, { "NavAgent", 2 }, { "Terrain", 1 }, { "FoliageScatter", 1 },
        { "Decal", 1 }, { "Trail", 1 }, { "Beam", 1 }, { "OrbitPath", 1 }, { "SkinnedModel", 1 }, { "CameraPath", 1 },
        { "Health", 1 }, { "Interactable", 1 } };
    CHECK(lost.size() == expected.size());
    for (size_t i = 0; i < lost.size() && i < expected.size(); ++i) {
        CHECK(lost[i].type == expected[i].first);
        CHECK(lost[i].count == expected[i].second);
    }

    // What the warning is about: a round trip through the file keeps the entities but not those components.
    Scene reopened("Unstored");
    std::string error;
    CHECK(SceneSerializer::FromJson(reopened, SceneSerializer::ToJson(scene), &error));
    CHECK(reopened.EntityCount() == scene.EntityCount());
    CHECK(SceneSerializer::UnsavedComponents(reopened).empty());
    CHECK(reopened.FindByName("J1") && !reopened.FindByName("J1").Has<Joint>());
    CHECK(reopened.FindByName("Plain") && reopened.FindByName("Plain").Has<MeshRenderer>());
}
