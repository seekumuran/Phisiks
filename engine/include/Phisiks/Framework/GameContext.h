#pragma once
// Shared state handed to scenes and gameplay services each frame.

#include "Phisiks/Framework/CameraController.h"
#include "Phisiks/Framework/Gameplay.h"
#include "Phisiks/Framework/Input.h"
#include "Phisiks/Framework/PlayerController.h"
#include "Phisiks/Scene/Scene.h"

#include <cstdint>
#include <deque>
#include <string>

namespace Phisiks {
    class SceneManager;

    enum class GameState { Booting, Loading, Running, Paused, ShuttingDown };
    const char* ToString(GameState state);

    struct FrameStats {
        uint64_t frameIndex = 0;
        float deltaTime = 0.0f;           // clamped variable delta (Phisiks::Time::DeltaTime)
        int fixedStepsThisFrame = 0;
        uint64_t totalFixedSteps = 0;
        double simulationTime = 0.0;      // seconds of simulated (fixed-step) time while Running
        float gpuFrameMs = 0.0f;      // non-blocking GPU timer, a few frames old (-1 until available)
        float cpuFrameMs = 0.0f;      // whole frame on the CPU
        float cpuUpdateMs = 0.0f;     // gameplay + world update (excludes physics and rendering)
        float cpuPhysicsMs = 0.0f;    // fixed-step gameplay + physics steps this frame
        float cpuRenderMs = 0.0f;     // render submission on the CPU
    };

    struct GameContext {
        InputSystem input;
        PlayerController player;
        CameraController camera;
        SceneManager* scenes = nullptr;
        Scene* CurrentScene() const;      // the active scene (nullptr while loading/between scenes)

        GameState state = GameState::Booting;
        FrameStats stats;
        bool overlayVisible = true;
        bool physicsDebugDraw = false;
        bool quitRequested = false;

        std::deque<std::string> eventLog;  // recent gameplay events shown by the debug overlay
        void PushEvent(const std::string& message);
    };
}
