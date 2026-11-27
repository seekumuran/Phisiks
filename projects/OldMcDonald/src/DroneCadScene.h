#pragma once
// Old-McDonald mapping drone: standalone CAD workspace. The same drone assembly as the greenhouse
// mission (DroneAssembly::Build, same meshes, materials and hierarchy), on its own in the shared CAD
// workspace (CadWorkspace), alongside the rover's. The greenhouse scene keeps its own instance of the
// drone for the scripted survey.
#include "CadWorkspace.h"

namespace Phisiks { class SceneManager; }

namespace Projects {
    class DroneCadScene : public CadWorkspace {
    public:
        static constexpr const char* ROOT = "Old-McDonald Mapping Drone";
        DroneCadScene() : CadWorkspace("DroneCAD") {}

    protected:
        const Description& Workspace() const override;
        Built BuildAssembly() override;
    };

    void RegisterDroneCadScene(Phisiks::SceneManager& scenes);
}
