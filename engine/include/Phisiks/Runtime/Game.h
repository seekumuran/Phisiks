#pragma once
// The interface a game implements to run on Phisiks.
//
//   class MyGame : public Phisiks::IGame {
//       void RegisterScenes(Phisiks::SceneManager& scenes) override { scenes.Register("Main", ...); }
//   };
//   int main() { MyGame game; Phisiks::Application app; app.Init(options, game); return app.Run(); }
//
// Call order per frame (Running state): OnUpdate -> scene Update -> [OnFixedUpdate -> scene
// FixedUpdate -> physics step] x N -> render. OnStart runs once after assets are loaded and the
// initial scene was requested; OnShutdown runs before the scene is unloaded.

namespace Phisiks {
    struct GameContext;
    class SceneManager;

    class IGame {
    public:
        virtual ~IGame() = default;
        virtual void RegisterScenes(SceneManager& scenes) = 0;
        virtual void OnStart(GameContext& /*ctx*/) {}
        virtual void OnUpdate(GameContext& /*ctx*/, float /*deltaTime*/) {}
        virtual void OnFixedUpdate(GameContext& /*ctx*/, float /*fixedDeltaTime*/) {}
        virtual void OnShutdown(GameContext& /*ctx*/) {}
        virtual void OnGui(GameContext& /*ctx*/) {}   // Dear ImGui windows (AppOptions::enableUI), after the frame was rendered
        // The user asked to quit (window close button, or Q while paused). Return false to keep running
        // (e.g. to ask about unsaved changes first); set GameContext::quitRequested later to quit.
        // Not called for --max-frames, test harness or RequestClose() quits.
        virtual bool OnQuitRequested(GameContext& /*ctx*/) { return true; }
    };
}
