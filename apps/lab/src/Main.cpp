// PhisiksLab entry point: a sample game built on Phisiks.
//
// Usage: PhisiksLab [--scene <name>] [--fullscreen] [--autotest] [--run-dir <dir>] [--max-frames <n>]
//        [--deterministic] [--steps-per-frame <n>] [--headless | --hidden]
//        [--export-dir <dir> [--export-frames] [--export-data] [--export-every <n>]]
// Run from the repository root (asset paths are relative to it).

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Runtime/Application.h"

#include "AutoTest.h"
#include "SandboxScenes.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
    // The sample game: registers its scenes; all gameplay lives in the scenes (SandboxScenes.cpp).
    class PhisiksLab : public Phisiks::IGame {
    public:
        void RegisterScenes(Phisiks::SceneManager& scenes) override { Lab::RegisterScenes(scenes); }
        void OnStart(Phisiks::GameContext&) override { Lab::RegisterMaterials(); }
        void OnUpdate(Phisiks::GameContext& ctx, float) override {
            if (ctx.input.Pressed(Phisiks::Action::ToggleViewLayout) && ctx.CurrentScene()) {
                Lab::SetQuadView(*ctx.CurrentScene(), !Lab::IsQuadView(*ctx.CurrentScene()));
            }
        }
    };
}

int main(int argc, char** argv) {
    Phisiks::AppOptions options;
    options.windowTitle = "PhisiksLab";
    options.initialScene = "Sandbox";
    bool autotest = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--autotest") { autotest = true; options.maxFrames = 20000; }
        else if (arg == "--fullscreen") options.fullscreen = true;
        else if (arg == "--scene" && hasValue) options.initialScene = argv[++i];
        else if (arg == "--run-dir" && hasValue) options.runDirectory = argv[++i];
        else if (arg == "--max-frames" && hasValue) options.maxFrames = std::strtoull(argv[++i], nullptr, 10);
        else if (arg == "--deterministic") options.deterministic = true;
        else if (arg == "--steps-per-frame" && hasValue) options.stepsPerFrame = std::atoi(argv[++i]);
        else if (arg == "--headless") options.headless = true;
        else if (arg == "--hidden") options.hiddenWindow = true;
        else if (arg == "--export-dir" && hasValue) options.exportDirectory = argv[++i];
        else if (arg == "--export-frames") options.exportFrames = true;
        else if (arg == "--export-data") options.exportData = true;
        else if (arg == "--export-every" && hasValue) options.exportEvery = std::atoi(argv[++i]);
        else {
            std::cerr << "Unknown argument '" << arg << "'\n"
                      << "Usage: PhisiksLab [--scene <name>] [--fullscreen] [--autotest] [--run-dir <dir>] [--max-frames <n>]\n"
                      << "       [--deterministic] [--steps-per-frame <n>] [--headless | --hidden]\n"
                      << "       [--export-dir <dir> [--export-frames] [--export-data] [--export-every <n>]]\n";
            return 64;
        }
    }

    PhisiksLab game;
    std::unique_ptr<Phisiks::ITestHarness> harness;
    if (autotest) harness = std::make_unique<Phisiks::AutoTest>(options.runDirectory);

    Phisiks::Application app;
    if (!app.Init(options, game, std::move(harness))) {
        app.Shutdown();
        return 1;
    }
    return app.Run();
}
