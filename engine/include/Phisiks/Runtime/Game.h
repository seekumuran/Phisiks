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
    };
}
