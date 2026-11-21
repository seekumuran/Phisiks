#pragma once
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Runtime/Game.h"

#include <functional>
#include <memory>
#include <string>

namespace Phisiks {
    // Optional hooks for automated runtime verification. The engine knows nothing about specific
    // tests; a game or test target supplies an implementation (see tests/runtime/AutoTest).
    class ITestHarness {
    public:
        virtual ~ITestHarness() = default;
        virtual void BeforeInput(GameContext& ctx) = 0;      // inject synthetic input for this frame
        virtual void AfterSimulation(GameContext& ctx) = 0;  // run checks after physics/camera
        virtual void AfterRender(GameContext& ctx) = 0;      // back buffer is valid (before present)
        virtual void AfterUnload(GameContext& ctx) = 0;      // shutdown: scene already unloaded
        virtual void WriteReport() const = 0;
        virtual bool Finished() const = 0;
        virtual int ExitCode() const = 0;
    };

    struct AppOptions {
        std::string windowTitle = "Phisiks";
        std::string initialScene;                          // must be registered by the game
        std::string runDirectory = "Build/runs/latest";    // logs, screenshots, reports, crash dumps
        bool fullscreen = false;
        uint64_t maxFrames = 0;                            // 0 = unlimited (safety cap for automated runs)

        // Simulation / recording
        bool deterministic = false;    // each frame advances exactly `stepsPerFrame` fixed steps (wall clock ignored)
        int stepsPerFrame = 1;
        bool headless = false;         // no window, renderer or audio device: pure simulation
        bool hiddenWindow = false;     // render without showing the window (frame export)
        std::string exportDirectory;   // empty = no export
        bool exportFrames = false;     // PNG per exported frame (needs rendering)
        bool exportData = false;       // CSV of dynamic bodies per exported frame
        bool exportVision = false;     // synthetic vision: aligned RGB + linear depth + normals + entity-id mask + camera CSV
        int exportEvery = 1;

        // Studio / platform
        bool enableUI = false;         // Dear ImGui (IGame::OnGui)
        int maxGLVersion = 46;         // 41 = force the OpenGL 4.1 feature tier (macOS path) on any GPU
        bool recordReplay = false;     // GameContext::replay records every fixed step
        size_t replayCapacity = 1800;
        std::string shaderHotReloadDirectory;   // non-empty: watch these GLSL files (Render::EnableShaderHotReload)
    };

    class Application {
    public:
        Application();
        ~Application();

        // `game` must outlive the Application. A test harness (optional) disables hardware input.
        bool Init(const AppOptions& options, IGame& game, std::unique_ptr<ITestHarness> harness = nullptr);
        int Run();          // returns the process exit code
        void Shutdown();

    private:
        void RunGameFrame();
        void HandleGlobalActions();
        void SetState(GameState state);
        void SetCursor(Platform::CursorMode mode);   // no-op without a visible window

        AppOptions m_options;
        IGame* m_game = nullptr;
        GameContext m_ctx;
        SceneManager m_scenes;
        std::unique_ptr<ITestHarness> m_harness;
        bool m_initialized = false;
        bool m_shutDown = false;
        bool m_rendererReady = false;
        std::unique_ptr<class Recorder> m_recorder;
    };
}
