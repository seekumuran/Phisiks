// PhisiksLab entry point: the interactive studio + sample scenes built on Phisiks.
//
// Usage: PhisiksLab [--scene <name>] [--fullscreen] [--autotest] [--run-dir <dir>] [--max-frames <n>]
//        [--deterministic] [--steps-per-frame <n>] [--headless | --hidden]
//        [--export-dir <dir> [--export-frames] [--export-data] [--export-vision] [--export-every <n>]]
//        [--gl41] [--quality low|medium|high] [--no-studio]
//   --gl41       force the OpenGL 4.1 feature tier (the macOS path: CPU culling + CPU compute)
//   --quality    scalability preset (Low = 720p internal -> FSR 1 upscale, for i3 / UHD laptops)
//   --no-studio  hide the Dear ImGui studio panels (on by default whenever a window exists)
//   --hot-reload [dir]  watch GLSL sources (default engine/shaders) and rebuild changed programs live
// Assets resolve next to the executable first (release package), then in the working directory (repository root).

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Runtime/Application.h"

#include "AutoTest.h"
#include "LessonScenes.h"
#include "P9Scenes.h"
#include "SandboxScenes.h"
#include "Showcase.h"
#include "Studio.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
    // The sample game: registers its scenes; gameplay lives in the scenes, the studio in Studio.cpp.
    class PhisiksLab : public Phisiks::IGame {
    public:
        int quality = -1;
        void RegisterScenes(Phisiks::SceneManager& scenes) override {
            Lab::RegisterScenes(scenes);
            Lab::RegisterLessonScenes(scenes);
            Lab::RegisterP9Scenes(scenes);
            Showcase::RegisterAll(scenes);   // the four P11 showcases (also standalone executables)
        }
        void OnStart(Phisiks::GameContext&) override {
            Lab::RegisterMaterials();
            Lab::RegisterLessonMaterials();
            Lab::RegisterP9Materials();
            if (quality >= 0) Phisiks::Render::ApplyQualityPreset(Phisiks::Render::QualityPreset(quality));
        }
        void OnUpdate(Phisiks::GameContext& ctx, float) override {
            if (ctx.CurrentScene() != m_lastScene) {
                m_lastScene = ctx.CurrentScene();
                Lab::Studio::OnSceneLoaded(ctx);
            }
            Showcase::HandleInput(ctx);   // keys 1-5 / 0 / M / F3 inside showcase scenes
            if (ctx.input.Pressed(Phisiks::Action::ToggleViewLayout) && ctx.CurrentScene() && !Showcase::Current(ctx)) {
                Lab::SetQuadView(*ctx.CurrentScene(), !Lab::IsQuadView(*ctx.CurrentScene()));
            }
        }
        void OnGui(Phisiks::GameContext& ctx) override { Lab::Studio::Draw(ctx); }

    private:
        Phisiks::Scene* m_lastScene = nullptr;
    };

    void Usage() {
        std::cerr << "Usage: PhisiksLab [--scene <name>] [--fullscreen] [--autotest] [--run-dir <dir>] [--max-frames <n>]\n"
                  << "       [--deterministic] [--steps-per-frame <n>] [--headless | --hidden]\n"
                  << "       [--export-dir <dir> [--export-frames] [--export-data] [--export-vision] [--export-every <n>]]\n"
                  << "       [--gl41] [--quality low|medium|high] [--no-studio]\n";
    }
}

int main(int argc, char** argv) {
    Phisiks::AppOptions options;
    options.windowTitle = "PhisiksLab";
    options.initialScene = "Sandbox";
    bool autotest = false;
    bool studio = true;
    int quality = -1;

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
        else if (arg == "--export-vision") options.exportVision = true;
        else if (arg == "--export-every" && hasValue) options.exportEvery = std::atoi(argv[++i]);
        else if (arg == "--gl41") options.maxGLVersion = 41;
        else if (arg == "--no-studio") studio = false;
        else if (arg == "--hot-reload") options.shaderHotReloadDirectory = (hasValue && argv[i + 1][0] != '-') ? argv[++i] : "engine/shaders";
        else if (arg == "--quality" && hasValue) {
            const std::string q = argv[++i];
            quality = q == "low" ? 0 : q == "medium" ? 1 : q == "high" ? 2 : -1;
            if (quality < 0) { Usage(); return 64; }
        }
        else {
            std::cerr << "Unknown argument '" << arg << "'\n";
            Usage();
            return 64;
        }
    }
    options.enableUI = studio && !options.headless;
    options.recordReplay = studio && !options.headless && !options.deterministic;

    PhisiksLab game;
    game.quality = quality;
    std::unique_ptr<Phisiks::ITestHarness> harness;
    if (autotest) harness = std::make_unique<Phisiks::AutoTest>(options.runDirectory);
    Showcase::SetTestMode(autotest);

    Phisiks::Application app;
    if (!app.Init(options, game, std::move(harness))) {
        app.Shutdown();
        return 1;
    }
    return app.Run();
}
