#include "LessonScenes.h"
#include "SceneBuilder.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Math/Integrators.h"
#include "Phisiks/Math/Rotor.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace Phisiks;

namespace Lab {
    namespace {
        constexpr const char* MAT_FLOOR = "Lesson/Floor";
        constexpr const char* MAT_EULER = "Lesson/Euler";
        constexpr const char* MAT_QUAT = "Lesson/Quaternion";
        constexpr const char* MAT_ROTOR = "Lesson/Rotor";
        constexpr const char* MAT_SUN = "Lesson/Sun";
        constexpr const char* MAT_CRATE = "Lesson/Crate";

        std::string F(const char* format, double a, double b = 0.0, double c = 0.0) {
            char text[256];
            std::snprintf(text, sizeof(text), format, a, b, c);
            return text;
        }

        void Caption(const std::string& line1, const std::string& line2) {
            const glm::ivec2 size = Render::GetFramebufferSize();
            Render::DrawScreenText(line1, glm::vec2(16.0f, float(size.y) - 58.0f), glm::vec4(0.6f, 0.9f, 1.0f, 1.0f), 2.0f);
            Render::DrawScreenText(line2, glm::vec2(16.0f, float(size.y) - 34.0f), glm::vec4(0.8f, 0.85f, 0.9f, 1.0f), 2.0f);
        }

        void Stage(Scene& scene, GameContext& ctx, const glm::vec3& target, float distance, float yaw, float pitch) {
            SceneBuilder::AddGround(scene, 30.0f, MAT_FLOOR, 1.0f);
            SceneBuilder::AddSun(scene, glm::vec3(-0.45f, -1.0f, -0.35f), glm::vec3(0.95f, 0.95f, 1.0f), 2.2f);
            ctx.camera.SetOrbit(target, distance, yaw, pitch);
        }

        Entity AddMesh(Scene& scene, const char* name, const char* mesh, const char* material, const glm::vec3& position, const glm::vec3& scale) {
            Entity e = scene.CreateEntity(name);
            e.Get<Transform>().position = position;
            e.Get<Transform>().scale = scale;
            e.Add<MeshRenderer>(MeshRenderer{ mesh, material });
            return e;
        }
    }

    // ==================================================================== Rotors
    void RotorsScene::OnEnter(GameContext& ctx) {
        Stage(*this, ctx, glm::vec3(0, 1.2f, 0), 7.0f, 0.0f, -0.25f);
        AddMesh(*this, "EulerCube", "Cube", MAT_EULER, glm::vec3(-2.5f, 1.2f, 0), glm::vec3(1.0f, 0.5f, 1.4f));
        AddMesh(*this, "QuaternionCube", "Cube", MAT_QUAT, glm::vec3(0.0f, 1.2f, 0), glm::vec3(1.0f, 0.5f, 1.4f));
        AddMesh(*this, "RotorCube", "Cube", MAT_ROTOR, glm::vec3(2.5f, 1.2f, 0), glm::vec3(1.0f, 0.5f, 1.4f));
    }

    void RotorsScene::Update(GameContext&, float dt) {
        m_time += dt;
        // yaw-pitch-roll angles as three representations of one rotation
        const float yaw = 0.7f * m_time, pitch = 0.4f * std::sin(0.9f * m_time), roll = 0.3f * m_time;
        const glm::mat4 euler = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)) * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0)) *
                                glm::rotate(glm::mat4(1.0f), roll, glm::vec3(0, 0, 1));
        const glm::quat q = glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch, glm::vec3(1, 0, 0)) * glm::angleAxis(roll, glm::vec3(0, 0, 1));
        using Math::Rotor3;
        const Rotor3 r = Rotor3::FromQuat(glm::angleAxis(yaw, glm::vec3(0, 1, 0))) * Rotor3::FromQuat(glm::angleAxis(pitch, glm::vec3(1, 0, 0))) *
                         Rotor3::FromQuat(glm::angleAxis(roll, glm::vec3(0, 0, 1)));
        if (Entity e = FindByName("EulerCube")) e.Get<Transform>().rotation = glm::quat_cast(glm::mat3(euler));
        if (Entity e = FindByName("QuaternionCube")) e.Get<Transform>().rotation = q;
        if (Entity e = FindByName("RotorCube")) e.Get<Transform>().rotation = glm::normalize(r.ToQuat());
        // deviation of the rotated x axis between the representations
        const glm::vec3 ex = glm::mat3(euler) * glm::vec3(1, 0, 0), qx = q * glm::vec3(1, 0, 0), rx = r.Rotate(glm::vec3(1, 0, 0));
        const float d1 = glm::degrees(std::acos(std::clamp(glm::dot(ex, qx), -1.0f, 1.0f)));
        const float d2 = glm::degrees(std::acos(std::clamp(glm::dot(qx, rx), -1.0f, 1.0f)));
        maxDeviationDegrees = std::max(maxDeviationDegrees, std::max(d1, d2));
    }

    void RotorsScene::DrawUI(GameContext&) {
        Caption("Transforms & rotors: Euler angles (left), quaternion (middle), GA rotor (right)",
                F("same yaw-pitch-roll composed three ways; max axis deviation %.4f deg", maxDeviationDegrees));
    }

    // ==================================================================== Orbits
    void OrbitsScene::OnEnter(GameContext& ctx) {
        Stage(*this, ctx, glm::vec3(0, 1.5f, 0), 14.0f, 0.4f, -0.8f);
        AddMesh(*this, "Sun", "Sphere", MAT_SUN, glm::vec3(0, 1.5f, 0), glm::vec3(0.8f));
        AddMesh(*this, "EulerMoon", "Sphere", MAT_EULER, glm::vec3(4, 1.5f, 0), glm::vec3(0.3f));
        AddMesh(*this, "Rk4Moon", "Sphere", MAT_ROTOR, glm::vec3(4, 1.5f, 0), glm::vec3(0.3f));
        const Math::Integrate::KeplerOrbit orbit{ 16.0 };
        m_euler.r = m_rk4.r = glm::dvec3(4.0, 0.0, 0.0);
        m_euler.v = m_rk4.v = glm::dvec3(0.0, 0.0, -orbit.CircularSpeed(4.0)) * 1.1;   // slightly elliptic
        m_energy0 = orbit.Energy(m_euler.r, m_euler.v);
    }

    void OrbitsScene::FixedUpdate(GameContext&, float dt) {
        const Math::Integrate::KeplerOrbit orbit{ 16.0 };
        struct S { glm::dvec3 r, v; S operator+(const S& o) const { return { r + o.r, v + o.v }; } S operator*(double s) const { return { r * s, v * s }; } };
        auto f = [&](double, const S& y) { return S{ y.v, orbit.Acceleration(y.r) }; };
        const double h = double(dt) * timeScale;
        S e{ m_euler.r, m_euler.v }, k{ m_rk4.r, m_rk4.v };
        e = Math::Integrate::Euler(e, 0.0, h, f);
        k = Math::Integrate::RK4(k, 0.0, h, f);
        m_euler.r = e.r; m_euler.v = e.v;
        m_rk4.r = k.r; m_rk4.v = k.v;
        eulerEnergyDrift = (orbit.Energy(m_euler.r, m_euler.v) - m_energy0) / std::fabs(m_energy0);
        rk4EnergyDrift = (orbit.Energy(m_rk4.r, m_rk4.v) - m_energy0) / std::fabs(m_energy0);
        for (Body* b : { &m_euler, &m_rk4 }) {
            b->trail.push_back(glm::vec3(b->r) + glm::vec3(0, 1.5f, 0));
            if (b->trail.size() > 600) b->trail.erase(b->trail.begin());
        }
    }

    void OrbitsScene::Update(GameContext&, float) {
        if (glm::length(m_euler.r) < 40.0) if (Entity e = FindByName("EulerMoon")) e.Get<Transform>().position = glm::vec3(m_euler.r) + glm::vec3(0, 1.5f, 0);
        if (Entity e = FindByName("Rk4Moon")) e.Get<Transform>().position = glm::vec3(m_rk4.r) + glm::vec3(0, 1.5f, 0);
        for (size_t i = 1; i < m_euler.trail.size(); ++i) Render::DrawLine(m_euler.trail[i - 1], m_euler.trail[i], glm::vec4(1.0f, 0.5f, 0.2f, 1.0f));
        for (size_t i = 1; i < m_rk4.trail.size(); ++i) Render::DrawLine(m_rk4.trail[i - 1], m_rk4.trail[i], glm::vec4(0.3f, 0.8f, 1.0f, 1.0f));
    }

    void OrbitsScene::DrawUI(GameContext&) {
        Caption("RK4 vs explicit Euler on a Kepler orbit (orange: Euler, blue: RK4)",
                F("energy drift: Euler %+.3f%%, RK4 %+.6f%%", eulerEnergyDrift * 100.0, rk4EnergyDrift * 100.0));
    }

    // ==================================================================== PBR
    void PbrScene::OnEnter(GameContext& ctx) {
        Stage(*this, ctx, glm::vec3(0, 1.6f, 0), 9.0f, 0.0f, -0.15f);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 5; ++col) {
                Render::MaterialDesc m;
                m.baseColor = row < 3 ? glm::vec3(0.75f, 0.12f, 0.08f) : glm::vec3(0.95f, 0.72f, 0.35f);
                m.roughness = 0.08f + 0.23f * float(col);
                m.metallic = float(row) / 4.0f;
                const std::string name = "Lesson/PBR_" + std::to_string(row) + "_" + std::to_string(col);
                Render::RegisterMaterial(name, m);
                AddMesh(*this, ("Sphere_" + std::to_string(row) + "_" + std::to_string(col)).c_str(), "Sphere", name.c_str(),
                        glm::vec3(-2.4f + 1.2f * float(col), 0.5f + 1.1f * float(row), 0.0f), glm::vec3(0.9f));
            }
        }
        SceneBuilder::AddPointLight(*this, "WarmLight", glm::vec3(-3, 4, 3), glm::vec3(1.0f, 0.8f, 0.6f), 12.0f, 30.0f);
        SceneBuilder::AddPointLight(*this, "CoolLight", glm::vec3(3, 2, 3), glm::vec3(0.5f, 0.7f, 1.0f), 12.0f, 20.0f);
    }

    void PbrScene::DrawUI(GameContext&) {
        Caption("PBR: roughness increases left -> right, metallic bottom -> top",
                "Cook-Torrance GGX + hemisphere ambient + rim; try exposure / quality presets in the studio");
    }

    // ==================================================================== Fountain
    void FountainScene::OnEnter(GameContext& ctx) {
        Stage(*this, ctx, glm::vec3(0, 1.0f, 0), 9.0f, 0.6f, -0.35f);
        Entity emitter = CreateEntity("Fountain");
        emitter.Get<Transform>().position = glm::vec3(0, 0.2f, 0);
        Physics::Advanced::ParticleSystem ps;
        ps.count = 6000;
        ps.emissionRate = 900.0f;
        ps.lifetime = 5.0f;
        ps.spawnSize = glm::vec3(0.2f);
        ps.emitDirection = glm::vec3(0.35f, 1.0f, 0.0f);
        ps.emitSpeed = 7.0f;
        ps.emitSpread = 0.15f;
        ps.radius = 0.05f;
        ps.mass = 0.02f;
        ps.color = glm::vec3(0.3f, 0.7f, 1.0f);
        emitter.Add<Physics::Advanced::ParticleSystem>(ps);
        for (int i = 0; i < 3; ++i) {
            SceneBuilder::AddDynamicBox(*this, "Crate" + std::to_string(i), glm::vec3(2.2f + 0.9f * float(i), 0.3f + 0.61f * float(i % 2), 0.0f), glm::vec3(0.6f), 0.6f, MAT_CRATE);
        }
    }

    void FountainScene::DrawUI(GameContext&) {
        const Physics::Advanced::Stats s = Physics::Advanced::GetStats();
        Caption("Particle emitter + two-way coupling: the jet pushes the dynamic crates",
                F("%.0f live particles, %.0f bodies pushed, %.3f N*s last step", double(s.particlesAlive), double(s.coupledBodies), double(s.coupledImpulse)));
    }

    // ==================================================================== registration
    void RegisterLessonScenes(SceneManager& scenes) {
        scenes.Register("Rotors", [] { return std::make_unique<RotorsScene>(); });
        scenes.Register("Orbits", [] { return std::make_unique<OrbitsScene>(); });
        scenes.Register("PBR", [] { return std::make_unique<PbrScene>(); });
        scenes.Register("Fountain", [] { return std::make_unique<FountainScene>(); });
    }

    void RegisterLessonMaterials() {
        auto make = [](glm::vec3 base, float roughness, Render::Pattern pattern = Render::Pattern::None, glm::vec3 accent = glm::vec3(0.0f), float scale = 1.0f) {
            Render::MaterialDesc m;
            m.baseColor = base;
            m.roughness = roughness;
            m.pattern = pattern;
            m.patternColor = accent;
            m.patternScale = scale;
            return m;
        };
        Render::RegisterMaterial(MAT_FLOOR, make({ 0.10f, 0.10f, 0.12f }, 0.9f, Render::Pattern::Grid, { 0.25f, 0.30f, 0.40f }, 1.0f));
        Render::RegisterMaterial(MAT_EULER, make({ 0.85f, 0.40f, 0.12f }, 0.5f));
        Render::RegisterMaterial(MAT_QUAT, make({ 0.75f, 0.75f, 0.78f }, 0.4f));
        Render::RegisterMaterial(MAT_ROTOR, make({ 0.20f, 0.55f, 0.95f }, 0.4f));
        Render::MaterialDesc sun = make({ 1.0f, 0.8f, 0.3f }, 0.5f);
        sun.emissive = glm::vec3(2.0f, 1.5f, 0.5f);
        Render::RegisterMaterial(MAT_SUN, sun);
        Render::RegisterMaterial(MAT_CRATE, make({ 0.55f, 0.33f, 0.15f }, 0.6f, Render::Pattern::Wood, { 0.33f, 0.18f, 0.07f }, 2.0f));
    }
}
