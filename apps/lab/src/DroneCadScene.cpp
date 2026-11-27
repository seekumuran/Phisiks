#include "DroneCadScene.h"
#include "DroneAssembly.h"
#include "Phisiks/Framework/SceneManager.h"

#include <memory>

using namespace Phisiks;
namespace DA = Lab::DroneAssembly;

namespace Lab {
    const CadWorkspace::Description& DroneCadScene::Workspace() const {
        static const Description d{
            "DRONE CAD", "Old-McDonald mapping drone", ROOT, "projects/Old-McDonald/drone_cad.scene.json", "drone_cad",
            "Simulation > Projects > Old-McDonald",
            { "OLD-McDONALD MAPPING DRONE", "Standalone CAD assembly (same assembly as the greenhouse mission)", "PHISIKS engineering visualisation",
              "Styled from the prototype photo; Pixhawk 2.4.8 + ReadyToSky GPS documented, frame F450-class" },
            { { DA::FRAME, "Frame (plates, arms)" }, { DA::PROPULSION, "Motors" }, { DA::PROPELLERS, "Propellers" },
              { DA::ELECTRONICS, "Flight controller + ESCs" }, { DA::WIRING, "Wiring" }, { DA::BATTERY, "Battery" },
              { DA::GPS, "GPS (avionics)" }, { DA::GEAR, "Landing gear" } },
            { 0.0f, 0.12f, 0.0f },   // centre: skids on the ground, GPS puck ~0.25 m
            1.05f, 1.15f, 0.80f, -0.50f,
            { 0.0f, 0.10f, 0.0f },
            { 0.0f, 0.04f, 0.0f }, 0.35f,
            { 0.0f, 0.20f, 0.0f } };   // lift while exploded: battery and landing gear move down by up to 0.2 m
        return d;
    }

    CadWorkspace::Built DroneCadScene::BuildAssembly() {
        DA::RegisterMaterials();
        DA::Result r = DA::Build(*this, Entity(), ROOT, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0);   // +X forward
        Built b;
        for (const DA::ExplodePart& p : r.explode) b.explode.push_back({ p.entity, p.offset });
        b.callouts = r.callouts;
        b.wiring = r.wiring;
        return b;
    }

    void RegisterDroneCadScene(SceneManager& scenes) {
        scenes.Register("DroneCAD", [] { return std::make_unique<DroneCadScene>(); });
    }
}
