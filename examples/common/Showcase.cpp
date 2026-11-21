#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/InputState.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Runtime/Application.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    void RegisterStuntCar(SceneManager& scenes);
    void RegisterFluidHarbor(SceneManager& scenes);
    void RegisterRobotics(SceneManager& scenes);
    void RegisterVilla(SceneManager& scenes);
    void RegisterVision(SceneManager& scenes);
    void RegisterBlackHole(SceneManager& scenes);

    namespace {
        bool g_testMode = false;                 // --autotest: hardware keys are ignored
        int g_quality = 1;
        std::array<float, 180> g_history{};
        size_t g_cursor = 0;
        float g_frameMs = 16.7f;
        std::chrono::steady_clock::time_point g_last{};

        ImVec4 Phosphor() { return ImVec4(Retro::PHOSPHOR[0], Retro::PHOSPHOR[1], Retro::PHOSPHOR[2], 1.0f); }
        ImVec4 Gold() { return ImVec4(Retro::GOLD[0], Retro::GOLD[1], Retro::GOLD[2], 1.0f); }

        Collider BoxCollider() {
            Collider c;
            c.shape.shape = Physics::ShapeType::Box;
            c.shape.halfExtents = glm::vec3(0.5f);
            return c;
        }
    }

    const char* ToString(ViewMode mode) {
        switch (mode) {
            case ViewMode::Final: return "FINAL RENDER";
            case ViewMode::Wireframe: return "WIREFRAME";
            case ViewMode::Fields: return "SDF + FIELDS";
            case ViewMode::Particles: return "RAW PARTICLES";
            case ViewMode::Cad: return "CAD 4-WAY";
            default: return "?";
        }
    }

    CameraShot OrbitShot(const std::string& name, const glm::vec3& centre, float radius, float height, float duration, float startAngle) {
        CameraShot s;
        s.name = name;
        s.duration = duration;
        s.closed = true;
        for (int i = 0; i < 8; ++i) {
            const float a = startAngle + float(i) / 8.0f * 6.2831853f;
            s.points.push_back(centre + glm::vec3(std::sin(a) * radius, height, std::cos(a) * radius));
            s.lookAt.push_back(centre);
        }
        return s;
    }

    CameraShot DollyShot(const std::string& name, const glm::vec3& a, const glm::vec3& b, const glm::vec3& target, float duration) {
        CameraShot s;
        s.name = name;
        s.duration = duration;
        for (int i = 0; i < 4; ++i) {
            const float t = float(i) / 3.0f;
            s.points.push_back(glm::mix(a, b, t));
            s.lookAt.push_back(target);
        }
        return s;
    }

    const std::vector<DemoInfo>& Demos() {
        static const std::vector<DemoInfo> demos = {
            { "StuntCar", "Stunt Car & Tower Demolition", "GAME DEV", "Spring-damper car, ramps, destructible crate / glass towers, guard bots, Lua boost pads, Doppler audio.", "Showcase01_StuntCar" },
            { "FluidHarbor", "Screen-Space Fluid Harbor", "REAL-TIME VFX", "4-pass SSF water, Beer-Lambert + Fresnel + foam, buoyant boat and crates, breakdown wipe.", "Showcase02_FluidHarbor" },
            { "Robotics", "6-DOF Arm & Terrain Rover", "ROBOTICS", "Motorised 6-axis arm, dual-quaternion IK, SDF obstacle avoidance, 6-wheel rover, replay scrubbing.", "Showcase03_Robotics" },
            { "Villa", "Cliffside Glass Villa", "ARCHITECTURE", "Glass-and-concrete villa, instanced landscaping, time-of-day sun, F3 CAD blueprint views.", "Showcase04_Villa" },
            { "Vision", "LiDAR & Segmentation Proving Ground", "AI / VISION", "Intersection traffic and pedestrians, 360-degree LiDAR, live instance mask, vision export.", "Showcase05_Vision" },
            { "BlackHole", "Black Hole Accretion Disk", "SCIENCE", "50k Doppler-shaded plasma particles, polar jets, Euler vs Verlet vs RK4 split-test.", "Showcase06_BlackHole" },
        };
        return demos;
    }

    void RegisterAll(SceneManager& scenes) {
        RegisterStuntCar(scenes);
        RegisterFluidHarbor(scenes);
        RegisterRobotics(scenes);
        RegisterVilla(scenes);
        RegisterVision(scenes);
        RegisterBlackHole(scenes);
    }

    ShowcaseScene* Current(GameContext& ctx) { return dynamic_cast<ShowcaseScene*>(ctx.CurrentScene()); }

    void RegisterShapes() {
        if (Render::HasMesh("Showcase/Cylinder")) return;
        auto revolve = [](float topRadius, int segments) {
            Render::MeshData m;
            // side
            const float slope = 0.5f - topRadius;   // radius change over the height 1 (normal tilt)
            for (int i = 0; i <= segments; ++i) {
                const float a = float(i) / float(segments) * 6.2831853f;
                const glm::vec3 radial(std::cos(a), 0.0f, std::sin(a));
                const glm::vec3 n = glm::normalize(radial + glm::vec3(0.0f, slope, 0.0f));
                m.vertices.push_back({ radial * 0.5f + glm::vec3(0, -0.5f, 0), n, { float(i) / float(segments), 0.0f } });
                m.vertices.push_back({ radial * topRadius + glm::vec3(0, 0.5f, 0), n, { float(i) / float(segments), 1.0f } });
            }
            for (int i = 0; i < segments; ++i) {
                const uint32_t b = uint32_t(i * 2);
                m.indices.insert(m.indices.end(), { b, b + 1, b + 3, b, b + 3, b + 2 });
            }
            // caps
            for (int cap = 0; cap < 2; ++cap) {
                const float y = cap == 0 ? -0.5f : 0.5f;
                const float r = cap == 0 ? 0.5f : topRadius;
                if (r <= 0.0f) continue;
                const glm::vec3 n(0.0f, cap == 0 ? -1.0f : 1.0f, 0.0f);
                const uint32_t centre = uint32_t(m.vertices.size());
                m.vertices.push_back({ glm::vec3(0, y, 0), n, { 0.5f, 0.5f } });
                for (int i = 0; i <= segments; ++i) {
                    const float a = float(i) / float(segments) * 6.2831853f;
                    m.vertices.push_back({ glm::vec3(std::cos(a) * r, y, std::sin(a) * r), n, { 0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a) } });
                }
                for (int i = 0; i < segments; ++i) {
                    const uint32_t a = centre + 1 + uint32_t(i), b = a + 1;
                    if (cap == 0) m.indices.insert(m.indices.end(), { centre, a, b });
                    else m.indices.insert(m.indices.end(), { centre, b, a });
                }
            }
            return m;
        };
        Render::RegisterMesh("Showcase/Cylinder", revolve(0.5f, 32));
        Render::RegisterMesh("Showcase/Cone", revolve(0.0f, 32));
    }

    void Material(const std::string& name, const glm::vec3& base, float roughness, float metallic, const glm::vec3& emissive,
                  Render::Pattern pattern, const glm::vec3& patternColor, float patternScale, float lineWidth) {
        Render::MaterialDesc m;
        m.baseColor = base;
        m.roughness = roughness;
        m.metallic = metallic;
        m.emissive = emissive;
        m.pattern = pattern;
        m.patternColor = patternColor;
        m.patternScale = patternScale;
        m.lineWidth = lineWidth;
        Render::RegisterMaterial(name, m);
    }
    void SetTestMode(bool testMode) { g_testMode = testMode; }
    namespace { std::string g_exportDirectory; bool g_exportVision = false; }
    const std::string& ExportDirectory() { return g_exportDirectory; }
    bool ExportVision() { return g_exportVision; }

    // ==================================================================== ShowcaseScene
    void ShowcaseScene::OnEnter(GameContext& ctx) {
        m_savedSettings = Render::Settings();
        m_mode = ViewMode::Final;
        m_shot = -1;
        m_time = 0.0f;
        m_hiddenMeshes.clear();
        Render::Settings().wireframe = false;
        Render::Settings().fluid = Render::FluidSettings();
        RegisterShapes();
        Build(ctx);
    }

    void ShowcaseScene::OnExit(GameContext&) {
        Render::SdfVisualization noSdf;
        Render::SetSdfVisualization(noSdf);
        Render::SetFieldVisualization(Render::FieldVisualization());
        Render::Settings() = m_savedSettings;
    }

    void ShowcaseScene::Update(GameContext& ctx, float dt) {
        m_time += dt;
        Tick(ctx, dt);
    }

    void ShowcaseScene::FixedUpdate(GameContext& ctx, float dt) { FixedTick(ctx, dt); }

    void ShowcaseScene::SetViewMode(ViewMode mode) {
        if (mode == m_mode) return;
        m_mode = mode;
        ApplyViewMode();
        OnViewModeChanged(mode);
    }

    void ShowcaseScene::ApplyViewMode() {
        Render::RenderSettings& r = Render::Settings();
        r.wireframe = m_mode == ViewMode::Wireframe;
        r.fluid.enabled = m_mode != ViewMode::Particles;
        // particles view: hide every mesh (restored afterwards)
        if (m_mode == ViewMode::Particles) {
            m_hiddenMeshes.clear();
            Each<MeshRenderer>([&](Entity e, MeshRenderer& m) {
                if (m.visible) { m.visible = false; m_hiddenMeshes.push_back(e.Id()); }
            });
        }
        else {
            for (uint32_t id : m_hiddenMeshes) if (Entity e = FromId(id); e.Valid()) if (auto* m = e.TryGet<MeshRenderer>()) m->visible = true;
            m_hiddenMeshes.clear();
        }
        Render::SdfVisualization sdf;
        Render::FieldVisualization field;
        if (m_mode == ViewMode::Fields) {
            BuildSdf(sdf);
            sdf.enabled = !sdf.primitives.empty();
            field.enabled = BuildFieldView(field);
        }
        Render::SetSdfVisualization(sdf);
        Render::SetFieldVisualization(field);
        SetCadViews(m_mode == ViewMode::Cad);
    }

    void ShowcaseScene::SetCadViews(bool enabled) {
        for (const char* name : { "CadTop", "CadFront", "CadSide" }) if (Entity e = FindByName(name); e.Valid()) DestroyImmediate(e);
        if (Entity main = FindByName("MainCamera"); main.Valid() && main.Has<Camera>())
            main.Get<Camera>().viewport = enabled ? glm::vec4(0.0f, 0.5f, 0.5f, 0.5f) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        if (!enabled) return;
        const glm::vec4 b = CadBounds();
        const glm::vec3 c(b);
        const float h = b.w;
        struct Def { const char* name; glm::vec3 offset, direction, up; glm::vec4 viewport; };
        const Def defs[] = {
            { "CadTop",   { 0.0f, h * 3.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.5f, 0.5f, 0.5f, 0.5f } },
            { "CadFront", { 0.0f, 0.0f, h * 3.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.5f, 0.5f } },
            { "CadSide",  { h * 3.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.0f, 0.5f, 0.5f } },
        };
        int order = 1;
        for (const Def& d : defs) {
            Entity e = CreateEntity(d.name);
            Transform& t = e.Get<Transform>();
            t.position = c + d.offset;
            t.rotation = glm::quatLookAt(d.direction, d.up);
            Camera& cam = e.Add<Camera>();
            cam.projection = Projection::Orthographic;
            cam.orthoHeight = h * 1.15f;
            cam.nearPlane = 0.5f;
            cam.farPlane = h * 6.0f;
            cam.viewport = d.viewport;
            cam.order = order++;
            cam.primary = false;
        }
    }

    void ShowcaseScene::DrawUI(GameContext&) {
        if (m_mode != ViewMode::Cad) return;
        // blueprint grid on the ground plane of the CAD bounds + quadrant labels
        const glm::vec4 b = CadBounds();
        const float h = b.w;
        const float step = h > 30.0f ? 5.0f : (h > 8.0f ? 1.0f : 0.5f);
        const glm::vec4 minor(0.15f, 0.45f, 1.0f, 0.5f), major(0.35f, 0.75f, 1.6f, 0.9f);
        const float y = 0.02f;
        int i = 0;
        for (float x = -h; x <= h + 1e-3f; x += step, ++i) {
            const glm::vec4& col = (i % 5 == 0) ? major : minor;
            Render::DrawLine(glm::vec3(b.x + x, y, b.z - h), glm::vec3(b.x + x, y, b.z + h), col);
            Render::DrawLine(glm::vec3(b.x - h, y, b.z + x), glm::vec3(b.x + h, y, b.z + x), col);
        }
        const glm::vec2 size = glm::vec2(Render::GetFramebufferSize());
        const glm::vec4 label(0.55f, 0.8f, 1.0f, 1.0f);
        char grid[48];
        std::snprintf(grid, sizeof(grid), "GRID %.1f M", double(step));
        Render::DrawScreenText("TOP (ORTHO)", glm::vec2(size.x * 0.5f + 12.0f, size.y * 0.5f - 30.0f), label);
        Render::DrawScreenText("FRONT (ORTHO)", glm::vec2(size.x * 0.5f - 200.0f, size.y - 30.0f), label);
        Render::DrawScreenText("SIDE (ORTHO)", glm::vec2(size.x * 0.5f + 12.0f, size.y - 30.0f), label);
        Render::DrawScreenText(grid, glm::vec2(size.x * 0.5f + 12.0f, size.y * 0.5f - 54.0f), label);
    }

    void ShowcaseScene::PlayShot(GameContext& ctx, int index) {
        std::vector<CameraShot> shots = Shots();
        if (index < 0 || index >= int(shots.size())) return;
        Entity camera = FindByName("MainCamera");
        if (!camera.Valid()) return;
        const CameraShot& s = shots[size_t(index)];
        CameraPath& path = camera.Add<CameraPath>();
        path = CameraPath();
        path.type = Animation::SplineType::CatmullRom;
        path.points = s.points;
        path.lookAt = s.lookAt;
        path.duration = s.duration;
        path.closed = s.closed;
        path.loop = true;
        path.playing = true;
        m_shot = index;
        ctx.PushEvent("Camera shot " + std::to_string(index + 1) + ": " + s.name);
    }

    void ShowcaseScene::StopShot(GameContext& ctx) {
        if (m_shot < 0) return;
        m_shot = -1;
        Entity camera = FindByName("MainCamera");
        if (!camera.Valid()) return;
        const glm::vec3 eye = camera.WorldPosition();
        const glm::vec3 forward = camera.WorldRotation() * glm::vec3(0.0f, 0.0f, -1.0f);
        camera.Remove<CameraPath>();
        const glm::vec3 target = eye + forward * 10.0f;
        ctx.camera.LookAt(eye, target);
        ctx.camera.SetOrbit(target, 10.0f, ctx.camera.GetYaw(), ctx.camera.GetPitch());
    }

    // ---- world-building helpers
    Entity ShowcaseScene::Visual(const std::string& name, const char* mesh, const std::string& material, const glm::vec3& position,
                                 const glm::vec3& scale, const glm::quat& rotation) {
        Entity e = CreateEntity(name);
        Transform& t = e.Get<Transform>();
        t.position = position;
        t.rotation = rotation;
        t.scale = scale;
        e.Add<MeshRenderer>(MeshRenderer{ mesh, material });
        return e;
    }

    Entity ShowcaseScene::Box(const std::string& name, const glm::vec3& centre, const glm::vec3& size, const std::string& material, float mass, const glm::quat& rotation) {
        Entity e = Visual(name, "Cube", material, centre, size, rotation);
        e.Add<Collider>(BoxCollider());
        if (mass > 0.0f) {
            Rigidbody body;
            body.mass = mass;
            e.Add<Rigidbody>(body);
        }
        return e;
    }

    Entity ShowcaseScene::Ball(const std::string& name, const glm::vec3& centre, float radius, const std::string& material, float mass) {
        Entity e = Visual(name, "Sphere", material, centre, glm::vec3(radius * 2.0f));
        Collider c;
        c.shape.shape = Physics::ShapeType::Sphere;
        c.shape.radius = 0.5f;
        e.Add<Collider>(c);
        if (mass > 0.0f) {
            Rigidbody body;
            body.mass = mass;
            e.Add<Rigidbody>(body);
        }
        return e;
    }

    Entity ShowcaseScene::Ground(float size, const std::string& material, float patternPerMetre) {
        Entity ground = CreateEntity("Ground");
        ground.Get<Transform>().scale = glm::vec3(size, 1.0f, size);
        MeshRenderer renderer{ "Plane", material };
        renderer.uvScale = glm::vec2(patternPerMetre);
        ground.Add<MeshRenderer>(renderer);
        Entity slab = CreateEntity("GroundCollider");
        slab.SetParent(ground, false);
        slab.Get<Transform>().position = glm::vec3(0.0f, -0.5f, 0.0f);
        slab.Add<Collider>(BoxCollider());
        return ground;
    }

    Entity ShowcaseScene::Sun(const glm::vec3& direction, const glm::vec3& color, float illuminance, bool shadows) {
        Entity e = CreateEntity("Sun");
        e.Get<Transform>().rotation = glm::quatLookAt(glm::normalize(direction), std::abs(glm::normalize(direction).y) > 0.99f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0));
        Light light;
        light.type = LightType::Directional;
        light.color = color;
        light.intensity = illuminance;
        light.castShadows = shadows;
        e.Add<Light>(light);
        return e;
    }

    Entity ShowcaseScene::PointLight(const std::string& name, const glm::vec3& position, const glm::vec3& color, float range, float intensity) {
        Entity e = CreateEntity(name);
        e.Get<Transform>().position = position;
        Light light;
        light.type = LightType::Point;
        light.color = color;
        light.range = range;
        light.intensity = intensity;
        e.Add<Light>(light);
        return e;
    }

    Entity ShowcaseScene::SpotLight(const std::string& name, const glm::vec3& position, const glm::vec3& target, const glm::vec3& color,
                                    float range, float intensity, float outerAngle, bool shadows) {
        Entity e = CreateEntity(name);
        Transform& t = e.Get<Transform>();
        t.position = position;
        const glm::vec3 dir = glm::normalize(target - position);
        t.rotation = glm::quatLookAt(dir, std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0));
        Light light;
        light.type = LightType::Spot;
        light.color = color;
        light.range = range;
        light.intensity = intensity;
        light.outerConeAngle = outerAngle;
        light.innerConeAngle = outerAngle * 0.6f;
        light.castShadows = shadows;
        e.Add<Light>(light);
        return e;
    }

    // ==================================================================== telemetry / input / HUD
    Telemetry GetTelemetry(GameContext& ctx) {
        Telemetry t;
        t.frameMs = g_frameMs;
        t.fps = g_frameMs > 0.0f ? 1000.0f / g_frameMs : 0.0f;
        t.cpuMs = ctx.stats.cpuFrameMs;
        t.gpuMs = std::max(ctx.stats.gpuFrameMs, 0.0f);
        const Physics::Advanced::Stats a = Physics::Advanced::GetStats();
        if (a.gpuAvailable) t.gpuParticles = a.particles;
        else t.cpuParticles = a.particles;
        if (ShowcaseScene* s = Current(ctx)) t.cpuParticles += s->CpuEffectParticles();
        t.ramMB = double(Platform::ProcessMemoryBytes()) / 1048576.0;
        const RenderStats r = GetRenderStats();
        t.drawn = r.instancesDrawn;
        t.culled = r.instancesCulled + r.occlusionCulled;
        t.cullingSavings = (t.drawn + t.culled) > 0 ? float(t.culled) / float(t.drawn + t.culled) : 0.0f;
        return t;
    }

    void HandleInput(GameContext& ctx) {
        const auto now = std::chrono::steady_clock::now();
        if (g_last.time_since_epoch().count() != 0) {
            const float ms = std::chrono::duration<float, std::milli>(now - g_last).count();
            g_frameMs = g_frameMs * 0.9f + ms * 0.1f;
            g_history[g_cursor] = ms / 33.3f;
            g_cursor = (g_cursor + 1) % g_history.size();
        }
        g_last = now;
        ShowcaseScene* s = Current(ctx);
        if (!s || g_testMode || UI::WantsKeyboard()) return;
        const InputState& keys = GetInputState();
        for (int i = 0; i < 5; ++i) if (keys.WasPressed(Key(int(Key::Num1) + i))) s->PlayShot(ctx, i);
        if (keys.WasPressed(Key::Num0)) s->StopShot(ctx);
        if (keys.WasPressed(Key::M)) s->SetViewMode(ViewMode((int(s->GetViewMode()) + 1) % int(ViewMode::Count)));
        if (ctx.input.Pressed(Action::ToggleViewLayout)) s->SetViewMode(s->GetViewMode() == ViewMode::Cad ? ViewMode::Final : ViewMode::Cad);
    }

    void TimelineControls(GameContext& ctx) {
        Scene* scene = ctx.CurrentScene();
        Replay& replay = ctx.replay;
        int index = int(replay.Scrubbed() ? replay.ScrubIndex() : (replay.Size() ? replay.Size() - 1 : 0));
        const int last = int(replay.Size()) - 1;
        const bool paused = ctx.state == GameState::Paused;
        auto restore = [&](int i) { if (scene && last >= 0) { ctx.state = GameState::Paused; replay.Restore(*scene, size_t(std::clamp(i, 0, last))); } };
        if (Retro::TransportButton("##sback", Retro::Glyph::StepBack, false, "Step back")) restore(index - 1);
        ImGui::SameLine();
        if (Retro::TransportButton("##splay", Retro::Glyph::Play, !paused, "Play (continues from the scrubbed step)")) ctx.state = GameState::Running;
        ImGui::SameLine();
        if (Retro::TransportButton("##spause", Retro::Glyph::Pause, paused, "Pause")) ctx.state = GameState::Paused;
        ImGui::SameLine();
        if (Retro::TransportButton("##sfwd", Retro::Glyph::StepForward, false, "Step forward")) restore(index + 1);
        ImGui::SameLine();
        ImGui::TextColored(Phosphor(), "%zu steps %.1f MB", replay.Size(), double(replay.MemoryBytes()) / 1048576.0);
        if (last >= 0) {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##sscrub", &index, 0, last, "step %d")) restore(index);
        }
        if (!replay.Enabled()) ImGui::TextDisabled("replay recording is off (--deterministic / --no-ui)");
    }

    void DrawGui(GameContext& ctx, const char* backToLab) {
        ShowcaseScene* s = Current(ctx);
        if (!s) return;
        const DemoInfo* info = nullptr;
        for (const DemoInfo& d : Demos()) if (s->Name() == d.scene) info = &d;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const Telemetry t = GetTelemetry(ctx);

        if (Retro::BeginPanel(info ? info->title : s->Name().c_str(), 10.0f, 10.0f, 330.0f)) {
            char big[32], small[96], value[48];
            std::snprintf(big, sizeof(big), "%5.1f FPS", double(t.fps));
            std::snprintf(small, sizeof(small), "%.2f ms  %s  %s", double(t.frameMs), ToString(s->GetViewMode()),
                          Render::ToString(Render::QualityPreset(g_quality)));
            Retro::Lcd(big, small);
            std::snprintf(value, sizeof(value), "%.1f ms", double(t.cpuMs));
            Retro::LedMeter("CPU", t.cpuMs / 16.7f, value);
            std::snprintf(value, sizeof(value), "%.1f ms", double(t.gpuMs));
            Retro::LedMeter("GPU", t.gpuMs / 16.7f, value);
            std::snprintf(value, sizeof(value), "%.0f / 2560 MB", t.ramMB);
            Retro::LedMeter("RAM", float(t.ramMB / RAM_BUDGET_MB), value);
            std::snprintf(value, sizeof(value), "%.0f %% culled", double(t.cullingSavings * 100.0f));
            Retro::LedMeter("CULL", t.cullingSavings, value);
            Retro::Analyzer("##showcaseframes", g_history.data(), int(g_history.size()), int(g_cursor), 34.0f);
            ImGui::TextColored(Phosphor(), "GPU particles %u  CPU particles %u", t.gpuParticles, t.cpuParticles);
            ImGui::TextColored(t.ramMB < RAM_BUDGET_MB ? Phosphor() : ImVec4(1, 0.3f, 0.2f, 1), "RAM budget %s  drawn %u  culled %u",
                               t.ramMB < RAM_BUDGET_MB ? "OK" : "OVER", t.drawn, t.culled);
            ImGui::Separator();
            ImGui::TextColored(Gold(), "VIEW (M cycles, F3 CAD)");
            for (int m = 0; m < int(ViewMode::Count); ++m) {
                if (m % 3 != 0) ImGui::SameLine();
                if (Retro::Button(ToString(ViewMode(m)), 100.0f, s->GetViewMode() == ViewMode(m) ? 1 : 0)) s->SetViewMode(ViewMode(m));
            }
            ImGui::TextColored(Gold(), "CINEMATIC CAMERA (1-5, 0 free)");
            const std::vector<CameraShot> shots = s->Shots();
            for (int i = 0; i < int(shots.size()); ++i) {
                if (i > 0) ImGui::SameLine();
                char label[8];
                std::snprintf(label, sizeof(label), "%d", i + 1);
                if (Retro::Button(label, 42.0f, s->ActiveShot() == i ? 1 : 0)) s->PlayShot(ctx, i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shots[size_t(i)].name.c_str());
            }
            ImGui::SameLine();
            if (Retro::Button("FREE", 52.0f, s->ActiveShot() < 0 ? 1 : 0)) s->StopShot(ctx);
            ImGui::TextColored(Gold(), "QUALITY");
            const char* names[3] = { "LOW", "MEDIUM", "HIGH" };
            for (int q = 0; q < 3; ++q) {
                if (q > 0) ImGui::SameLine();
                if (Retro::Button(names[q], 96.0f, g_quality == q ? 1 : 0)) { g_quality = q; Render::ApplyQualityPreset(Render::QualityPreset(q)); }
            }
            if (backToLab && Retro::Button("<< BACK TO LAB", 0.0f, 2)) ctx.scenes->Request(backToLab);
            if (*s->Controls()) ImGui::TextDisabled("%s", s->Controls());
            Retro::EndPanel();
        }
        if (Retro::BeginPanel(info ? info->industry : "DEMO", display.x - 370.0f, 10.0f, 360.0f)) {
            if (info) ImGui::TextWrapped("%s", info->blurb);
            ImGui::Separator();
            s->DrawPanel(ctx);
            Retro::EndPanel();
        }
    }

    // ==================================================================== executable entry point
    namespace {
        class ShowcaseGame : public IGame {
        public:
            int quality = 1;
            void RegisterScenes(SceneManager& scenes) override { RegisterAll(scenes); }
            void OnStart(GameContext&) override {
                g_quality = quality;
                Render::ApplyQualityPreset(Render::QualityPreset(quality));
            }
            void OnUpdate(GameContext& ctx, float) override { HandleInput(ctx); }
            void OnGui(GameContext& ctx) override { DrawGui(ctx); }
        };

        void Usage(const char* exe) {
            std::cerr << "Usage: " << exe << " [--autotest [--quick]] [--gl41] [--quality low|medium|high] [--width W --height H] [--fullscreen]\n"
                      << "       [--hidden | --headless] [--export-dir <dir> [--export-vision] [--export-frames] [--export-every N]]\n"
                      << "       [--max-frames N] [--run-dir <dir>] [--hot-reload [dir]] [--deterministic] [--no-ui]\n";
        }
    }

    int Run(int argc, char** argv, const char* sceneName, const char* windowTitle) {
        AppOptions options;
        options.windowTitle = windowTitle;
        options.initialScene = sceneName;
        options.runDirectory = std::string("Build/runs/") + sceneName;
        options.windowWidth = 1920;
        options.windowHeight = 1080;
        bool autotest = false, quick = false, ui = true;
        int quality = 1;   // Medium: the i3 / UHD laptop target
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (arg == "--autotest") { autotest = true; options.maxFrames = 30000; }
            else if (arg == "--quick") quick = true;
            else if (arg == "--gl41") options.maxGLVersion = 41;
            else if (arg == "--fullscreen") options.fullscreen = true;
            else if (arg == "--hidden") options.hiddenWindow = true;
            else if (arg == "--headless") options.headless = true;
            else if (arg == "--deterministic") options.deterministic = true;
            else if (arg == "--no-ui") ui = false;
            else if (arg == "--export-dir" && hasValue) options.exportDirectory = argv[++i];
            else if (arg == "--export-vision") options.exportVision = true;
            else if (arg == "--export-frames") options.exportFrames = true;
            else if (arg == "--export-data") options.exportData = true;
            else if (arg == "--export-every" && hasValue) options.exportEvery = std::atoi(argv[++i]);
            else if (arg == "--max-frames" && hasValue) options.maxFrames = std::strtoull(argv[++i], nullptr, 10);
            else if (arg == "--run-dir" && hasValue) options.runDirectory = argv[++i];
            else if (arg == "--width" && hasValue) options.windowWidth = std::atoi(argv[++i]);
            else if (arg == "--height" && hasValue) options.windowHeight = std::atoi(argv[++i]);
            else if (arg == "--hot-reload") options.shaderHotReloadDirectory = (hasValue && argv[i + 1][0] != '-') ? argv[++i] : "engine/shaders";
            else if (arg == "--quality" && hasValue) {
                const std::string q = argv[++i];
                quality = q == "low" ? 0 : q == "medium" ? 1 : q == "high" ? 2 : -1;
                if (quality < 0) { Usage(argv[0]); return 64; }
            }
            else { std::cerr << "Unknown argument '" << arg << "'\n"; Usage(argv[0]); return 64; }
        }
        // Vision export reads the GPU G-buffer, so "--headless --export-vision" renders into a hidden
        // window (no visible window, no UI) instead of skipping the renderer.
        if (options.headless && (options.exportVision || options.exportFrames)) {
            options.headless = false;
            options.hiddenWindow = true;
            ui = false;
            std::cerr << "Note: --headless with GPU exports renders into a hidden window\n";
        }
        if (options.exportVision && options.maxFrames == 0 && !autotest) options.maxFrames = 600;
        options.enableUI = ui && !options.headless && !options.hiddenWindow;
        options.recordReplay = !options.headless && !options.deterministic && std::string(sceneName) == "Robotics";   // timeline scrubbing
        g_testMode = autotest;
        g_exportDirectory = options.exportDirectory;
        g_exportVision = options.exportVision;

        ShowcaseGame game;
        game.quality = quality;
        std::unique_ptr<ITestHarness> harness;
        if (autotest) {
            ShowcaseTest::Options o;
            o.runDirectory = options.runDirectory;
            o.demo = sceneName;
            o.quick = quick;
            harness = std::make_unique<ShowcaseTest>(o);
        }
        Application app;
        if (!app.Init(options, game, std::move(harness))) {
            app.Shutdown();
            return 1;
        }
        return app.Run();
    }
}
