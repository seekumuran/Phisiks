#pragma once
// Old-McDonald mapping drone: the ONE drone assembly, built from ordinary scene entities. Used by
//   - the Old-McDonald greenhouse scene (OldMcDonaldScene.cpp), where it flies the scripted survey, and
//   - the standalone drone CAD workspace (DroneCadScene.cpp), where it is inspected on its own.
// Geometry: projects/assets/meshes/oldmcdonald/drone_*.obj (projects/tools/OldMcDonaldMeshes.cpp); dimensions:
// OldMcDonaldSpec.h. Styling follows docs/reference/old mcdonald/drone-reference.jpeg. Documented
// hardware: Pixhawk 2.4.8 flight controller, ReadyToSky GPS; the rest is a generic F450-class build.
#include "ProjectKit.h"

#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Projects::DroneAssembly {
    // Subsystem groups (direct children of the drone root) and names used by the mission / autotest.
    inline constexpr const char* FRAME = "Frame (F450 class)";
    inline constexpr const char* PROPULSION = "Propulsion";
    inline constexpr const char* PROPELLERS = "Propellers";
    inline constexpr const char* ELECTRONICS = "Electronics";
    inline constexpr const char* FC = "Flight controller - Pixhawk 2.4.8";
    inline constexpr const char* GPS = "GPS - ReadyToSky";
    inline constexpr const char* BATTERY = "Battery - LiPo";
    inline constexpr const char* GEAR = "Landing gear";
    inline constexpr const char* WIRING = "Drone wiring";

    struct ExplodePart { std::string entity; glm::vec3 offset; };   // local offset at explode = 1
    struct Result {
        Phisiks::Entity root;
        std::vector<ExplodePart> explode;
        std::vector<std::string> wiring;   // groups shown only while assembled
    };

    void RegisterMaterials();

    // Builds `name` under `parent` at `position` / `rotation`; frame centre over the origin, +X forward,
    // landing skids on y = 0.
    Result Build(Phisiks::Scene& scene, Phisiks::Entity parent, const std::string& name, const glm::vec3& position,
                 const glm::quat& rotation);
}
