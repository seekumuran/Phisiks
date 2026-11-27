// PhisiksSamples: the engine's sample scenes (physics, rendering, GPU compute, animation, VFX,
// terrain, navigation, joints + Lua) inside the PHISIKS editor, with a sample browser panel.
//
//   PhisiksSamples [--scene Sandbox|PhysicsTower|FieldLab|Rotors|Orbits|PBR|Fountain|Animation|VFX|Terrain|Joints] ...
//   PhisiksSamples --scene PhysicsTower --headless --deterministic --max-frames 600 --export-dir out --export-data
#include "EditorApp.h"
#include "SampleBrowser.h"

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "PHISIKS Samples";
    launch.app.initialScene = "Sandbox";
    if (const int code = Editor::ParseCommandLine(argc, argv, "PhisiksSamples", launch)) return code == -1 ? 0 : code;
    Editor::AppConfig config;
    config.sidePanelTitle = "SAMPLES";
    config.sidePanel = Samples::BrowserPanel;
    config.menus = Samples::SamplesMenu;
    Editor::Configure(config);
    Editor::Host host;
    host.registerScenes = Samples::RegisterAll;
    host.onStart = [](Phisiks::GameContext&) { Samples::RegisterAllMaterials(); };
    return Editor::Run(launch, host);
}
