#include "DroneCadScene.h"
#include "DroneAssembly.h"
#include "Phisiks/Framework/SceneManager.h"

#include <memory>

using namespace Phisiks;
namespace DA = Projects::DroneAssembly;

namespace Projects {
    const CadWorkspace::Description& DroneCadScene::Workspace() const {
        static const Description d{
            "DRONE CAD", ROOT, "projects/OldMcDonald/scenes/drone_cad.scene.json", "drone_cad",
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
        DA::Result r = DA::Build(*this, Entity(), ROOT, glm::vec3(0.0f), glm::quat(1, 0, 0, 0));   // +X forward
        Built b;
        for (const DA::ExplodePart& p : r.explode) b.explode.push_back({ p.entity, p.offset });
        b.wiring = r.wiring;
        return b;
    }

    void RegisterDroneCadScene(SceneManager& scenes) {
        scenes.Register("DroneCAD", [] { return std::make_unique<DroneCadScene>(); });
    }
}
