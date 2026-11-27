// OldMcDonald: the Old-McDonald agricultural robotics project, a separate application built on the
// PHISIKS engine and editor (projects/OldMcDonald): the greenhouse mission scene and the standalone
// rover / drone CAD workspaces (Workspace menu). Meshes: projects/assets.
#include "DroneCadScene.h"
#include "EditorApp.h"
#include "OldMcDonaldScene.h"
#include "RoverCadScene.h"

#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Framework/GameContext.h"

#include <imgui.h>

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "Old-McDonald - PHISIKS";
    launch.app.initialScene = "OldMcDonald";
    if (const int code = Editor::ParseCommandLine(argc, argv, "OldMcDonald", launch)) return code == -1 ? 0 : code;
    Editor::AddAssetDirectory("projects/assets");   // next to the executable (package) or in the working directory
    Editor::AppConfig config;
    config.menus = [](Phisiks::GameContext& ctx) {
        if (!ImGui::BeginMenu("Workspace")) return;
        const Phisiks::Scene* scene = ctx.CurrentScene();
        const char* items[][2] = { { "Greenhouse mission", "OldMcDonald" }, { "Rover CAD", "RoverCAD" }, { "Drone CAD", "DroneCAD" } };
        for (const auto& item : items)
            if (ImGui::MenuItem(item[0], nullptr, scene && scene->Name() == item[1])) Editor::RequestScene(ctx, item[1]);
        ImGui::EndMenu();
    };
    Editor::Configure(config);
    Editor::Host host;
    host.registerScenes = [](Phisiks::SceneManager& scenes) {
        Projects::RegisterOldMcDonaldScene(scenes);
        Projects::RegisterRoverCadScene(scenes);
        Projects::RegisterDroneCadScene(scenes);
    };
    return Editor::Run(launch, host);
}
