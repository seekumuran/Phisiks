#pragma once
// P9 classroom scenes.
//   Animation  glTF 2.0 walker (assets/models/walker.glb): dual-quaternion vs linear blend skinning
//              side by side, a cubic-spline clip, and a Catmull-Rom camera fly-through
//   VFX        HDR bloom, SSAO, motion trails + orbit paths, a raycast plasma beam, projected decals
//   Terrain    fBm heightmap terrain (splatmap, height-field collision), instanced foliage, a
//              navigation grid with an 8-agent crowd crossing, and a Doppler-shifted spatial drone
//   Joints     hinge door (limits), motorised windmill, ball-socket chain, prismatic elevator,
//              spring-damper bob, all driven or reacting through Lua scripts
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <string>

namespace Phisiks { class SceneManager; }

namespace Lab {
    class AnimationScene : public Phisiks::Scene {
    public:
        AnimationScene() : Scene("Animation") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
        bool cameraPathAttached = false;
    };

    class VfxScene : public Phisiks::Scene {
    public:
        VfxScene() : Scene("VFX") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
    private:
        float m_time = 0.0f;
    };

    class TerrainScene : public Phisiks::Scene {
    public:
        TerrainScene() : Scene("Terrain") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
        uint32_t crossings = 0;          // agent arrivals (targets swap on arrival)
    private:
        float m_time = 0.0f;
    };

    class JointsScene : public Phisiks::Scene {
    public:
        JointsScene() : Scene("Joints") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
    };

    void RegisterP9Scenes(Phisiks::SceneManager& scenes);
    void RegisterP9Materials();

    // Lesson caption: shown in the studio's classroom panel while the panels are visible (no bitmap
    // text behind them), else as overlay text at the bottom of the window.
    void ShowCaption(const std::string& line1, const std::string& line2);
}
