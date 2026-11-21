#pragma once
// Classroom scenes for the PhisiksLab studio (each one teaches one engine topic).
//   Rotors     the same rotation as Euler angles, a quaternion and a geometric-algebra rotor
//   Orbits     explicit Euler vs RK4 on a Kepler orbit (energy drift, trails)
//   PBR        roughness x metallic sphere grid under key / rim / point lights
//   Fountain   particle emitter (rate, lifetime, cone) whose particles push dynamic crates (two-way coupling)
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <vector>

namespace Phisiks { class SceneManager; }

namespace Lab {
    class RotorsScene : public Phisiks::Scene {
    public:
        RotorsScene() : Scene("Rotors") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
        float maxDeviationDegrees = 0.0f;   // between the three representations (should be ~0)
    private:
        float m_time = 0.0f;
    };

    class OrbitsScene : public Phisiks::Scene {
    public:
        OrbitsScene() : Scene("Orbits") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void FixedUpdate(Phisiks::GameContext& ctx, float dt) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
        double eulerEnergyDrift = 0.0, rk4EnergyDrift = 0.0;   // relative to the initial energy
        float timeScale = 4.0f;
    private:
        struct Body { glm::dvec3 r{ 0.0 }, v{ 0.0 }; std::vector<glm::vec3> trail; };
        Body m_euler, m_rk4;
        double m_energy0 = 0.0;
    };

    class PbrScene : public Phisiks::Scene {
    public:
        PbrScene() : Scene("PBR") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
    };

    class FountainScene : public Phisiks::Scene {
    public:
        FountainScene() : Scene("Fountain") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
    };

    void RegisterLessonScenes(Phisiks::SceneManager& scenes);
    void RegisterLessonMaterials();
}
