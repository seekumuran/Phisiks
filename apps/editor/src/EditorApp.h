#pragma once
// Launcher shared by every executable built on the PHISIKS editor (the editor itself, the sample
// browser, the example projects, the runtime test): command line, an IGame that hosts the editor,
// and Run().
//
//   Editor::Launch launch;
//   if (int code = Editor::ParseCommandLine(argc, argv, "Phisiks", launch)) return code == -1 ? 0 : code;
//   Editor::Host host;
//   host.registerScenes = [](Phisiks::SceneManager& s) { Editor::RegisterEmptyScene(s); };
//   return Editor::Run(launch, host);
#include "Editor.h"

#include "Phisiks/Runtime/Application.h"
#include "Phisiks/Runtime/Game.h"

#include <functional>
#include <memory>
#include <string>

namespace Editor {
    struct Launch {
        Phisiks::AppOptions app;
        int quality = -1;              // --quality low|medium|high
        bool ui = true;                // --no-ui
        bool autotest = false;         // --autotest (only executables with a test harness accept it)
        std::string open;              // --open <scene file>
        std::string project;           // --project <folder> (assets/ + scenes/)
    };

    // Options: --scene <name> --open <file> --fullscreen --run-dir <dir> --max-frames <n> --deterministic
    // --steps-per-frame <n> --headless --hidden --export-dir <dir> --export-frames --export-data
    // --export-vision --export-every <n> --gl41 --quality low|medium|high --no-ui --hot-reload [dir]
    // --assets <dir> (an extra asset root, e.g. projects/assets to open the example projects' scene files)
    // (--autotest when `allowAutotest`). Returns 0 to continue, -1 after --help, else an exit code (64).
    int ParseCommandLine(int argc, char** argv, const char* program, Launch& out, bool allowAutotest = false);

    // Adds an asset root searched before assets/ (resolved next to the executable, then the working
    // directory, like Assets::ResolveDirectory). Returns false if the folder does not exist.
    bool AddAssetDirectory(const std::string& directory);

    // An IGame that hosts the editor. Callbacks are optional.
    class Host : public Phisiks::IGame {
    public:
        std::function<void(Phisiks::SceneManager&)> registerScenes;
        std::function<void(Phisiks::GameContext&)> onStart;
        std::function<void(Phisiks::GameContext&, float)> onUpdate;
        // Return true when the callback drew the whole UI itself (the editor is then not drawn).
        std::function<bool(Phisiks::GameContext&)> onGui;
        int quality = -1;
        std::string openOnStart;

        void RegisterScenes(Phisiks::SceneManager& scenes) override;
        void OnStart(Phisiks::GameContext& ctx) override;
        void OnUpdate(Phisiks::GameContext& ctx, float dt) override;
        void OnGui(Phisiks::GameContext& ctx) override;
        bool OnQuitRequested(Phisiks::GameContext& ctx) override;

    private:
        bool m_opened = false;
    };

    // Applies the launch options to the host and runs the application.
    int Run(Launch& launch, Host& host, std::unique_ptr<Phisiks::ITestHarness> harness = nullptr);
}
