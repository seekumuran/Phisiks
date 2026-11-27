// ProjectMarina: the Project Marina engineering scene, a separate application built on the PHISIKS
// engine and editor (projects/ProjectMarina). Meshes: projects/assets.
#include "EditorApp.h"
#include "MarinaScene.h"

#include "Phisiks/Assets/Assets.h"

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "Project Marina - PHISIKS";
    launch.app.initialScene = "ProjectMarina";
    if (const int code = Editor::ParseCommandLine(argc, argv, "ProjectMarina", launch)) return code == -1 ? 0 : code;
    Editor::AddAssetDirectory("projects/assets");   // next to the executable (package) or in the working directory
    Editor::Configure(Editor::AppConfig{});
    Editor::Host host;
    host.registerScenes = Projects::RegisterMarinaScene;
    return Editor::Run(launch, host);
}
