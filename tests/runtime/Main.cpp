// PhisiksRuntimeTests: the runtime verification harness (AutoTest.cpp) in the real application loop
// with the real renderer, physics and audio. It hosts every scene the checks need: the editor's
// empty document, the engine samples (examples/samples), the four showcases (examples/) and the
// example projects (projects/).
//
//   PhisiksRuntimeTests --autotest [--run-dir <dir>] [--gl41] [--hidden]   (exit code = failed checks)
//   PhisiksRuntimeTests --scene <name> ...                                  (any hosted scene, interactively)
#include "AutoTest.h"
#include "DroneCadScene.h"
#include "EditorApp.h"
#include "MarinaScene.h"
#include "OldMcDonaldScene.h"
#include "RoverCadScene.h"
#include "SampleBrowser.h"
#include "Showcase.h"

#include "Phisiks/Framework/GameContext.h"

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "PHISIKS runtime tests";
    launch.app.initialScene = "Sandbox";
    if (const int code = Editor::ParseCommandLine(argc, argv, "PhisiksRuntimeTests", launch, true)) return code == -1 ? 0 : code;
    if (launch.autotest) launch.app.maxFrames = 60000;   // safety cap; each step has its own budget
    Editor::AddAssetDirectory("projects/assets");
    Editor::AppConfig config;
    config.insetViewport = false;          // pixel checks measure the full frame (one step tests the inset viewport)
    config.sidePanelTitle = "SAMPLES";
    config.sidePanel = Samples::BrowserPanel;
    config.menus = Samples::SamplesMenu;
    // as PhisiksSamples: samples and the mission start playing; the CAD workspaces and the empty
    // document start in edit mode (as in the OldMcDonald and PHISIKS programs)
    config.playOnLoad = [](const std::string& name) { return name != "RoverCAD" && name != "DroneCAD" && name != "Untitled"; };
    Editor::Configure(config);
    Editor::Host host;
    host.registerScenes = [](Phisiks::SceneManager& scenes) {
        Samples::RegisterAll(scenes);
        Projects::RegisterMarinaScene(scenes);
        Projects::RegisterOldMcDonaldScene(scenes);
        Projects::RegisterRoverCadScene(scenes);
        Projects::RegisterDroneCadScene(scenes);
        Showcase::RegisterAll(scenes);
    };
    host.onStart = [](Phisiks::GameContext&) { Samples::RegisterAllMaterials(); };
    host.onUpdate = [](Phisiks::GameContext& ctx, float) { Showcase::HandleInput(ctx); };   // keys 1-5 / 0 / M / F3 in showcase scenes
    host.onGui = [](Phisiks::GameContext& ctx) {
        if (!Showcase::Current(ctx)) return false;
        Showcase::DrawGui(ctx, "Sandbox");   // showcase scenes bring their own console HUD
        return true;
    };
    Showcase::SetTestMode(launch.autotest);
    std::unique_ptr<Phisiks::ITestHarness> harness;
    if (launch.autotest) harness = std::make_unique<Phisiks::AutoTest>(launch.app.runDirectory);
    return Editor::Run(launch, host, std::move(harness));
}
