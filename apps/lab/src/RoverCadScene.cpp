#include "RoverCadScene.h"
#include "RoverAssembly.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <memory>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace RA = Lab::RoverAssembly;

namespace Lab {
    const CadWorkspace::Description& RoverCadScene::Workspace() const {
        static const Description d{
            "ROVER CAD", "Old-McDonald soil rover", ROOT, "projects/Old-McDonald/rover_cad.scene.json", "rover_cad",
            "Simulation > Projects > Old-McDonald",
            { "OLD-McDONALD SOIL ROVER", "Standalone CAD assembly (same assembly as the greenhouse mission)", "PHISIKS engineering visualisation",
              "Styled from the prototype photo; dimensions not measured from the prototype" },
            { { "Chassis", "Chassis + top plate" }, { "Bodywork", "Fenders + front panel" }, { "Drive system", "Drive (motors, wheels)" },
              { "Electronics", "Pico, driver, battery" }, { RA::LIDAR, "LiDAR" }, { RA::ARM, "Robotic arm" },
              { "End effector", "Wrist rotate + gripper" }, { RA::SENSORS, "Soil sensor head" }, { RA::WIRING, "Wiring" } },
            { 0.0f, 0.27f, 0.0f },   // centre: stowed arm reaches ~0.6 m
            1.55f, 1.6f, 0.80f, -0.42f,
            { 0.0f, 0.15f, 0.0f },
            { 0.0f, 0.10f, 0.0f }, 0.55f,
            { 0.0f, 0.0f, 0.0f } };
        return d;
    }

    CadWorkspace::Built RoverCadScene::BuildAssembly() {
        RA::RegisterMaterials();
        RA::Result r = RA::Build(*this, Entity(), ROOT, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0), 0);   // +X forward
        Built b;
        for (const RA::ExplodePart& p : r.explode) b.explode.push_back({ p.entity, p.offset });
        b.callouts = r.callouts;
        b.wiring = { RA::WIRING };
        return b;
    }

    void RoverCadScene::OnAssemblyBuilt() { RA::SetArm(*this, RA::StowPose()); }

    void RoverCadScene::DrawAssemblyPanel(GameContext&) {
        const ImVec4 heading(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f);
        ImGui::Spacing();
        ImGui::TextColored(heading, "ARM JOINTS (IK chain J1-J4)");
        RA::ArmPose pose = RA::GetArm(*this);
        bool changed = false;
        const char* labels[4] = { "J1 yaw", "J2 shoulder", "J3 elbow", "J4 wrist" };
        for (int i = 0; i < 4; ++i) {
            float deg = pose.q[i] * 180.0f / ProjectKit::PI;
            if (Retro::EqSlider(labels[i], &deg, -180.0f, 180.0f, "%.0f deg")) { pose.q[i] = deg * ProjectKit::PI / 180.0f; changed = true; }
        }
        if (changed) RA::SetArm(*this, pose);
        if (Retro::Button("Stow arm", 98.0f)) RA::SetArm(*this, RA::StowPose());
    }

    void RegisterRoverCadScene(SceneManager& scenes) {
        scenes.Register("RoverCAD", [] { return std::make_unique<RoverCadScene>(); });
    }
}
