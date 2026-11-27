#include "EditorApp.h"

#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"

#include <cstdlib>
#include <iostream>

namespace Editor {
    namespace {
        void Usage(const char* program, bool autotest) {
            std::cerr << "Usage: " << program << " [--scene <name>] [--open <scene file>] [--fullscreen] [--run-dir <dir>] [--max-frames <n>]\n"
                      << "       [--deterministic] [--steps-per-frame <n>] [--headless | --hidden]\n"
                      << "       [--export-dir <dir> [--export-frames] [--export-data] [--export-vision] [--export-every <n>]]\n"
                      << "       [--gl41] [--quality low|medium|high] [--no-ui] [--hot-reload [dir]] [--assets <dir>]" << (autotest ? " [--autotest]" : "") << "\n"
                      << "  --gl41       force the OpenGL 4.1 feature tier (CPU culling + CPU compute; the macOS path)\n"
                      << "  --quality    scalability preset (low = reduced internal resolution + FSR 1, for integrated GPUs)\n"
                      << "  --no-ui      no editor panels (the viewport only)\n";
        }
    }

    int ParseCommandLine(int argc, char** argv, const char* program, Launch& out, bool allowAutotest) {
        Phisiks::AppOptions& o = out.app;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (arg == "--help" || arg == "-h") { Usage(program, allowAutotest); return -1; }
            else if (arg == "--autotest" && allowAutotest) { out.autotest = true; o.maxFrames = 20000; }
            else if (arg == "--fullscreen") o.fullscreen = true;
            else if (arg == "--scene" && hasValue) o.initialScene = argv[++i];
            else if (arg == "--open" && hasValue) out.open = argv[++i];
            else if (arg == "--run-dir" && hasValue) o.runDirectory = argv[++i];
            else if (arg == "--max-frames" && hasValue) o.maxFrames = std::strtoull(argv[++i], nullptr, 10);
            else if (arg == "--deterministic") o.deterministic = true;
            else if (arg == "--steps-per-frame" && hasValue) o.stepsPerFrame = std::atoi(argv[++i]);
            else if (arg == "--headless") o.headless = true;
            else if (arg == "--hidden") o.hiddenWindow = true;
            else if (arg == "--export-dir" && hasValue) o.exportDirectory = argv[++i];
            else if (arg == "--export-frames") o.exportFrames = true;
            else if (arg == "--export-data") o.exportData = true;
            else if (arg == "--export-vision") o.exportVision = true;
            else if (arg == "--export-every" && hasValue) o.exportEvery = std::atoi(argv[++i]);
            else if (arg == "--gl41") o.maxGLVersion = 41;
            else if (arg == "--no-ui" || arg == "--no-studio") out.ui = false;
            else if (arg == "--assets" && hasValue) {
                const char* dir = argv[++i];
                if (!AddAssetDirectory(dir)) { std::cerr << "Asset folder '" << dir << "' not found\n"; return 64; }
            }
            else if (arg == "--hot-reload") o.shaderHotReloadDirectory = (hasValue && argv[i + 1][0] != '-') ? argv[++i] : "engine/shaders";
            else if (arg == "--quality" && hasValue) {
                const std::string q = argv[++i];
                out.quality = q == "low" ? 0 : q == "medium" ? 1 : q == "high" ? 2 : -1;
                if (out.quality < 0) { Usage(program, allowAutotest); return 64; }
            }
            else {
                std::cerr << "Unknown argument '" << arg << "'\n";
                Usage(program, allowAutotest);
                return 64;
            }
        }
        return 0;
    }

    bool AddAssetDirectory(const std::string& directory) {
        const std::string resolved = Phisiks::Assets::ResolveDirectory(directory);
        if (resolved.empty()) return false;
        Phisiks::Assets::AddSearchPath(resolved);
        return true;
    }

    void Host::RegisterScenes(Phisiks::SceneManager& scenes) {
        RegisterEmptyScene(scenes, Config().emptyScene);
        if (registerScenes) registerScenes(scenes);
    }

    void Host::OnStart(Phisiks::GameContext& ctx) {
        if (onStart) onStart(ctx);
        if (quality >= 0) Phisiks::Render::ApplyQualityPreset(Phisiks::Render::QualityPreset(quality));
    }

    void Host::OnUpdate(Phisiks::GameContext& ctx, float dt) {
        if (!m_opened && ctx.CurrentScene()) {   // --open: load the file into the first scene
            m_opened = true;
            if (!openOnStart.empty()) {
                OnSceneLoaded(ctx);
                std::string status;
                if (!LoadScene(ctx, ResolveScenePath(openOnStart), &status)) Phisiks::Log::Error() << status;
            }
        }
        if (onUpdate) onUpdate(ctx, dt);
    }

    void Host::OnGui(Phisiks::GameContext& ctx) {
        if (onGui && onGui(ctx)) return;
        Draw(ctx);
    }

    bool Host::OnQuitRequested(Phisiks::GameContext& ctx) { return Editor::OnQuitRequested(ctx); }

    int Run(Launch& launch, Host& host, std::unique_ptr<Phisiks::ITestHarness> harness) {
        Phisiks::AppOptions& o = launch.app;
        if (o.initialScene.empty()) o.initialScene = Config().emptyScene;
        o.enableUI = launch.ui && !o.headless;
        o.recordReplay = launch.ui && !o.headless && !o.deterministic;
        o.captureCursor = false;   // editor: orbit camera and a free cursor (F4 / V switch to first person)
        o.sceneHotkeys = false;    // R / N would discard edits; the editor's Reload / scene menus ask first
        host.quality = launch.quality;
        host.openOnStart = launch.open;
        Phisiks::Application app;
        if (!app.Init(o, host, std::move(harness))) {
            app.Shutdown();
            return 1;
        }
        return app.Run();
    }
}
