#pragma once
// Old-McDonald soil rover: the ONE rover assembly, built from ordinary scene entities. Used by
//   - the Old-McDonald greenhouse scene (OldMcDonaldScene.cpp), where it drives the A* mission, and
//   - the standalone rover CAD workspace (RoverCadScene.cpp), where it is inspected on its own.
// Both call Build(); geometry comes from assets/meshes/oldmcdonald (tools/assetgen/OldMcDonaldMeshes.cpp),
// dimensions from OldMcDonaldSpec.h. Styling follows docs/reference/old mcdonald/rover-reference.jpeg.
#include "ProjectKit.h"

#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Lab::RoverAssembly {
    // Entity names shared by both scenes (and the autotest).
    inline constexpr const char* BODY = "Rover body";
    inline constexpr const char* ARM = "Sensor-positioning arm";
    inline constexpr const char* LIDAR = "LiDAR";
    inline constexpr const char* SENSORS = "Soil sensor head";
    inline constexpr const char* WIRING = "Rover wiring";
    inline constexpr const char* JOINTS[4] = { "J1 Base yaw", "J2 Shoulder", "J3 Elbow", "J4 Wrist" };
    inline constexpr const char* WHEELS[4] = { "Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR" };

    struct ExplodePart { std::string entity; glm::vec3 offset; };   // local offset at explode = 1
    struct Result {
        Phisiks::Entity root, body;
        std::vector<ExplodePart> explode;
        std::vector<ProjectKit::Callout> callouts;
    };

    void RegisterMaterials();   // every OMD/* material the rover uses (shared with the drone where names match)

    // Builds `name` under `parent` at `position` / `rotation`, with the rover body (+X forward, ground
    // contact at y = 0, local `bodyRotation`) as its child. Callouts are tagged with `calloutGroup`.
    Result Build(Phisiks::Scene& scene, Phisiks::Entity parent, const std::string& name, const glm::vec3& position,
                 const glm::quat& rotation, const glm::quat& bodyRotation, int calloutGroup);

    // Arm joint angles (J1 yaw about Y, J2-J4 about Z, radians).
    struct ArmPose { float q[4]; };
    ArmPose StowPose();
    void SetArm(Phisiks::Scene& scene, const ArmPose& pose);
    ArmPose GetArm(Phisiks::Scene& scene);
}
