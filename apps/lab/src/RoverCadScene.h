#pragma once
// Old-McDonald rover: standalone CAD workspace. The same rover assembly as the greenhouse mission
// (RoverAssembly::Build, same meshes, materials and hierarchy), on its own in the shared CAD
// workspace (CadWorkspace: views, explode, visibility, selection, save / reopen). Adds the arm-joint
// section. The greenhouse scene is unchanged and keeps its own instance of the rover for the mission.
#include "CadWorkspace.h"

namespace Phisiks { class SceneManager; }

namespace Lab {
    class RoverCadScene : public CadWorkspace {
    public:
        static constexpr const char* ROOT = "Old-McDonald Soil Rover";
        RoverCadScene() : CadWorkspace("RoverCAD") {}

    protected:
        const Description& Workspace() const override;
        Built BuildAssembly() override;
        void OnAssemblyBuilt() override;
        void DrawAssemblyPanel(Phisiks::GameContext& ctx) override;
    };

    void RegisterRoverCadScene(Phisiks::SceneManager& scenes);
}
