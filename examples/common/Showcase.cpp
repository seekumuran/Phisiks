#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
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
    void RegisterRoboticArm(SceneManager& scenes);
    void RegisterPoolFluid(SceneManager& scenes);
    void RegisterBlackHole(SceneManager& scenes);
    void RegisterTurbineCad(SceneManager& scenes);

    namespace {
        bool g_testMode = false;                 // --autotest: hardware keys are ignored
        int g_quality = 1;
        std::array<float, 180> g_history{};
        size_t g_cursor = 0;
        float g_frameMs = 16.7f;
        std::chrono::steady_clock::time_point g_last{};

        // text on the dark console panels (amber)
        ImVec4 Phosphor() { return ImVec4(Retro::READOUT[0], Retro::READOUT[1], Retro::READOUT[2], 1.0f); }
        ImVec4 Gold() { return ImVec4(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f); }
        bool g_cadInk = false, g_cadInkUser = false;
        float g_inkStrength = 0.85f, g_inkWidth = 1.0f;

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
            { "RoboticArm", "6-DOF Industrial Robot Cell", "ROBOTICS", "Beveled 6-axis arm, joint frames J1-J6 + tool, dual-quaternion FK / DLS IK, joint & Cartesian jog, pick-and-place, replay timeline.", "Showcase01_RoboticArm" },
            { "PoolFluid", "Refractive Tiled Pool", "REAL-TIME VFX", "Heightfield ripples coupled to a buoyant sphere, Snell refraction, Fresnel sky, projected caustics, shader-pass breakdown.", "Showcase02_PoolFluid" },
            { "BlackHole", "Schwarzschild Black Hole", "SCIENCE", "Ray-traced gravitational lensing, Einstein photon ring, Doppler-beamed accretion disk on Euler / Verlet / RK4 integrators.", "Showcase03_BlackHole" },
            { "TurbineCad", "Turbine CAD Cutaway", "CAD / ENGINEERING", "Axial-centrifugal turbine assembly: exploded view, ghosted casing, CAD ink edges, rotor speed, 4-way blueprints.", "Showcase04_TurbineCad" },
        };
        return demos;
    }

    void RegisterAll(SceneManager& scenes) {
        RegisterRoboticArm(scenes);
        RegisterPoolFluid(scenes);
        RegisterBlackHole(scenes);
        RegisterTurbineCad(scenes);
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
    bool PixelRay(Scene& scene, const glm::vec2& pixel, const glm::vec2& windowSize, glm::vec3& origin, glm::vec3& direction) {
        Entity cam = scene.FindByName("MainCamera");
        if (!cam.Valid() || !cam.Has<Camera>() || windowSize.x <= 0.0f || windowSize.y <= 0.0f) return false;
        const Camera& c = cam.Get<Camera>();
        const glm::vec2 ndc(pixel.x / windowSize.x * 2.0f - 1.0f, 1.0f - pixel.y / windowSize.y * 2.0f);
        const float t = std::tan(c.verticalFov * 0.5f), aspect = windowSize.x / windowSize.y;
        const glm::quat r = cam.WorldRotation();
        origin = cam.WorldPosition();
        direction = glm::normalize(r * glm::vec3(ndc.x * t * aspect, ndc.y * t, -1.0f));
        return true;
    }

    std::string HeroMeshName(const std::string& file) { return "Hero/" + file; }
    bool HeroMesh(const std::string& file) {
        const std::string name = HeroMeshName(file);
        if (Render::HasMesh(name)) return true;
        Render::MeshData data;
        std::string error;
        if (!Assets::LoadObj("meshes/hero/" + file + ".obj", data, &error)) {
            Log::Error() << "Showcase: meshes/hero/" << file << ".obj: " << error << " (run assetgen)";
            return false;
        }
        return Render::RegisterMesh(name, data);
    }

    void CadStudio(ShowcaseScene& scene, float scale, const glm::vec3& centre) {
        Render::MaterialDesc cyc;
        cyc.baseColor = { 0.74f, 0.75f, 0.77f };
        cyc.roughness = 0.9f;
        cyc.pattern = Render::Pattern::Grid;
        cyc.patternColor = { 0.60f, 0.62f, 0.66f };
        cyc.lineWidth = 0.012f;
        Render::RegisterMaterial("Studio/Cyclorama", cyc);
        if (HeroMesh("cyclorama")) {
            Entity e = scene.Visual("Cyclorama", HeroMeshName("cyclorama").c_str(), "Studio/Cyclorama", centre, glm::vec3(scale));
            e.Get<MeshRenderer>().castShadows = false;
        }
        Entity floor = scene.CreateEntity("StudioFloor");   // physics floor under the cyclorama
        floor.Get<Transform>().position = centre + glm::vec3(0.0f, -0.5f, 0.0f);
        floor.Get<Transform>().scale = glm::vec3(40.0f * scale, 1.0f, 40.0f * scale);
        floor.Add<Collider>(BoxCollider());
        Render::RenderSettings& r = Render::Settings();
        r.skyColorZenith = { 0.56f, 0.58f, 0.62f };
        r.skyColorHorizon = { 0.66f, 0.67f, 0.70f };
        r.groundColor = { 0.50f, 0.51f, 0.53f };
        r.ambientUp = { 0.50f, 0.52f, 0.56f };
        r.ambientDown = { 0.30f, 0.30f, 0.31f };
        r.ambientIntensity = 1.0f;
        r.rimColor = { 0.9f, 0.92f, 1.0f };
        r.rimIntensity = 0.08f;
        r.fog.enabled = true;
        r.fog.color = r.skyColorHorizon;
        r.fog.distanceDensity = 0.022f / std::max(scale, 0.1f);
        r.fog.heightDensity = 0.0f;
        r.fog.maxOpacity = 0.85f;
        r.exposure = 1.0f;
        r.bloom.threshold = 2.0f;
        r.bloom.intensity = 0.03f;
        r.clearColor = glm::vec4(r.skyColorHorizon, 1.0f);
        r.shadowDistance = std::max(r.shadowDistance, 12.0f * scale);
    }

    void SpaceBackdrop() {
        Render::RenderSettings& r = Render::Settings();
        r.skyColorZenith = glm::vec3(0.0f);
        r.skyColorHorizon = glm::vec3(0.0f);
        r.groundColor = glm::vec3(0.0f);
        r.ambientUp = { 0.03f, 0.03f, 0.04f };
        r.ambientDown = { 0.01f, 0.01f, 0.015f };
        r.rimIntensity = 0.0f;
        r.fog.enabled = false;
        r.shadowsEnabled = false;
        r.clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        r.exposure = 1.0f;
        r.bloom.threshold = 1.0f;
        r.bloom.intensity = 0.10f;
    }

    namespace {
        void ApplyCadInk() {
            if (!g_cadInk) { Render::RemovePostPass("cad_ink"); return; }
            Render::PostPass pass;
            pass.name = "cad_ink";
            pass.shader = "shaders/cad_edges.frag.glsl";
            pass.order = 100;   // after the demo's own passes
            pass.params[0] = glm::vec4(g_inkStrength, g_inkWidth, 0.035f, 0.0f);
            pass.params[1] = glm::vec4(0.03f, 0.04f, 0.06f, 1.0f);
            Render::SetPostPass(pass);
        }
    }
    void SetCadInk(bool enabled, float strength, float width) {
        g_cadInk = enabled;
        g_cadInkUser = enabled;
        g_inkStrength = strength;
        g_inkWidth = width;
        ApplyCadInk();
    }
    bool CadInkEnabled() { return g_cadInk; }

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
        Render::ClearPostPasses();
        g_cadInk = g_cadInkUser = false;
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
        // the CAD 4-way view always shows ink outlines; other views keep the demo's choice
        g_cadInk = m_mode == ViewMode::Cad ? true : g_cadInkUser;
        ApplyCadInk();
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
        const glm::vec4 minor(0.10f, 0.25f, 0.55f, 0.6f), major(0.05f, 0.15f, 0.45f, 1.0f);
        const float y = 0.02f;
        int i = 0;
        for (float x = -h; x <= h + 1e-3f; x += step, ++i) {
            const glm::vec4& col = (i % 5 == 0) ? major : minor;
            Render::DrawLine(glm::vec3(b.x + x, y, b.z - h), glm::vec3(b.x + x, y, b.z + h), col);
            Render::DrawLine(glm::vec3(b.x - h, y, b.z + x), glm::vec3(b.x + h, y, b.z + x), col);
        }
        const glm::vec2 size = glm::vec2(Render::GetFramebufferSize());
        const glm::vec4 label(0.06f, 0.16f, 0.36f, 1.0f);
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

    namespace {
        // Corner X / Y / Z orientation triad of the main camera (bottom-left of the viewport).
        void OrientationTriad(GameContext& ctx, ShowcaseScene& s) {
            Entity cam = s.FindByName("MainCamera");
            if (!cam.Valid()) return;
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            const bool cad = s.GetViewMode() == ViewMode::Cad;
            const ImVec2 c(62.0f, (cad ? display.y * 0.5f : display.y) - 62.0f);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            // recessed obsidian well: black top / left, dark-steel bottom / right
            dl->AddRectFilled(ImVec2(c.x - 50, c.y - 50), ImVec2(c.x + 50, c.y + 50), IM_COL32(0x12, 0x13, 0x1A, 0xE8));
            dl->AddLine(ImVec2(c.x - 50, c.y - 50), ImVec2(c.x + 50, c.y - 50), IM_COL32(0x07, 0x08, 0x0B, 0xFF));
            dl->AddLine(ImVec2(c.x - 50, c.y - 50), ImVec2(c.x - 50, c.y + 50), IM_COL32(0x07, 0x08, 0x0B, 0xFF));
            dl->AddLine(ImVec2(c.x - 50, c.y + 50), ImVec2(c.x + 50, c.y + 50), IM_COL32(0x3E, 0x43, 0x56, 0xFF));
            dl->AddLine(ImVec2(c.x + 50, c.y - 50), ImVec2(c.x + 50, c.y + 50), IM_COL32(0x3E, 0x43, 0x56, 0xFF));
            const glm::quat inv = glm::inverse(cam.WorldRotation());
            struct AxisDraw { glm::vec3 v; ImU32 col; const char* label; };
            AxisDraw axes[3] = { { inv * glm::vec3(1, 0, 0), IM_COL32(0xFF, 0x40, 0x30, 0xFF), "X" },
                                 { inv * glm::vec3(0, 1, 0), IM_COL32(0x30, 0xE0, 0x50, 0xFF), "Y" },
                                 { inv * glm::vec3(0, 0, 1), IM_COL32(0x40, 0x80, 0xFF, 0xFF), "Z" } };
            std::sort(std::begin(axes), std::end(axes), [](const AxisDraw& a, const AxisDraw& b) { return a.v.z < b.v.z; });   // far first
            for (const AxisDraw& a : axes) {
                const ImVec2 e(c.x + a.v.x * 36.0f, c.y - a.v.y * 36.0f);
                dl->AddLine(c, e, a.col, 3.0f);
                dl->AddCircleFilled(e, 3.5f, a.col);
                dl->AddText(ImVec2(e.x + (a.v.x >= 0 ? 4.0f : -12.0f), e.y - 7.0f), a.col, a.label);
            }
            dl->AddCircleFilled(c, 3.0f, IM_COL32(0xE2, 0xE6, 0xF0, 0xFF));
            (void)ctx;
        }

        void ConsoleMenu(GameContext& ctx, ShowcaseScene& s, const char* backToLab) {
            if (!Retro::BeginMenuBar("PHISIKS SHOWCASE")) return;
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Restart demo", "R") && ctx.scenes) ctx.scenes->RequestReload();
                if (backToLab && ImGui::MenuItem("Back to PhisiksLab") && ctx.scenes) ctx.scenes->Request(backToLab);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                for (int m = 0; m < int(ViewMode::Count); ++m) {
                    if (ViewMode(m) == ViewMode::Cad) continue;
                    if (ImGui::MenuItem(ToString(ViewMode(m)), m == 0 ? "M cycles" : nullptr, s.GetViewMode() == ViewMode(m))) s.SetViewMode(ViewMode(m));
                }
                ImGui::Separator();
                const std::vector<CameraShot> shots = s.Shots();
                for (int i = 0; i < int(shots.size()); ++i) {
                    char key[4];
                    std::snprintf(key, sizeof(key), "%d", i + 1);
                    if (ImGui::MenuItem(shots[size_t(i)].name.c_str(), key, s.ActiveShot() == i)) s.PlayShot(ctx, i);
                }
                if (ImGui::MenuItem("Free camera", "0", s.ActiveShot() < 0)) s.StopShot(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem("Hide console panels", "Tab")) UI::SetPanelsVisible(false);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Simulation")) {
                const bool paused = ctx.state == GameState::Paused;
                if (ImGui::MenuItem(paused ? "Resume" : "Pause", "Esc")) ctx.state = paused ? GameState::Running : GameState::Paused;
                ImGui::Separator();
                const char* names[3] = { "Quality: Low", "Quality: Medium", "Quality: High" };
                for (int q = 0; q < 3; ++q) if (ImGui::MenuItem(names[q], nullptr, g_quality == q)) { g_quality = q; Render::ApplyQualityPreset(Render::QualityPreset(q)); }
                ImGui::Separator();
                s.DrawMenu(ctx);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("CAD")) {
                if (ImGui::MenuItem("4-way orthographic blueprints", "F3", s.GetViewMode() == ViewMode::Cad)) s.SetViewMode(s.GetViewMode() == ViewMode::Cad ? ViewMode::Final : ViewMode::Cad);
                bool ink = g_cadInkUser;
                if (ImGui::MenuItem("CAD ink outlines", nullptr, ink)) SetCadInk(!ink, g_inkStrength, g_inkWidth);
                if (ImGui::MenuItem("Wireframe", nullptr, s.GetViewMode() == ViewMode::Wireframe)) s.SetViewMode(s.GetViewMode() == ViewMode::Wireframe ? ViewMode::Final : ViewMode::Wireframe);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                ImGui::TextDisabled("1-5 camera shots   0 free camera   M view mode   F3 CAD 4-way   Tab panels");
                ImGui::TextDisabled("RMB drag orbit   MMB pan   wheel zoom   Esc pause");
                if (*s.Controls()) ImGui::TextDisabled("%s", s.Controls());
                ImGui::EndMenu();
            }
            const Telemetry t = GetTelemetry(ctx);
            char status[96];
            std::snprintf(status, sizeof(status), "   %s  |  %s  |  %.1f FPS", ToString(s.GetViewMode()), Render::ToString(Render::QualityPreset(g_quality)), double(t.fps));
            ImGui::TextDisabled("%s", status);
            Retro::EndMenuBar();
        }
    }

    void DrawGui(GameContext& ctx, const char* backToLab) {
        ShowcaseScene* s = Current(ctx);
        if (!s) return;
        const DemoInfo* info = nullptr;
        for (const DemoInfo& d : Demos()) if (s->Name() == d.scene) info = &d;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const Telemetry t = GetTelemetry(ctx);
        ConsoleMenu(ctx, *s, backToLab);
        const float top = Retro::MenuBarHeight() + 8.0f;

        if (Retro::BeginPanel("TELEMETRY", 10.0f, top, 318.0f)) {
            char big[32], small[96], value[48];
            std::snprintf(big, sizeof(big), "%5.1f FPS", double(t.fps));
            std::snprintf(small, sizeof(small), "%.2f ms  %s  %s", double(t.frameMs), ToString(s->GetViewMode()),
                          Render::ToString(Render::QualityPreset(g_quality)));
            Retro::Lcd(big, small);
            std::snprintf(value, sizeof(value), "%.1f ms", double(t.cpuMs));
            Retro::LedMeter("CPU", t.cpuMs / 16.7f, value);
            std::snprintf(value, sizeof(value), "%.1f ms", double(t.gpuMs));
            Retro::LedMeter("GPU", t.gpuMs / 16.7f, value);
            std::snprintf(value, sizeof(value), "%.0f MB", t.ramMB);
            Retro::LedMeter("RAM", float(t.ramMB / RAM_BUDGET_MB), value);
            std::snprintf(value, sizeof(value), "%.0f %%", double(t.cullingSavings * 100.0f));
            Retro::LedMeter("CULL", t.cullingSavings, value);
            Retro::Analyzer("##showcaseframes", g_history.data(), int(g_history.size()), int(g_cursor), 30.0f);
            std::snprintf(value, sizeof(value), "%u / %u", t.gpuParticles, t.cpuParticles);
            Retro::LcdField("PARTICLES GPU / CPU", value, true);
            std::snprintf(value, sizeof(value), "%u drawn %u culled", t.drawn, t.culled);
            Retro::LcdField("INSTANCES", value);
            std::snprintf(value, sizeof(value), "%s %.0f / 2560 MB", t.ramMB < RAM_BUDGET_MB ? "OK" : "OVER", t.ramMB);
            Retro::LcdField("RAM BUDGET", value, t.ramMB >= RAM_BUDGET_MB);
            Retro::BeginGroup("View Mode  (M, F3)");
            for (int m = 0; m < int(ViewMode::Count); ++m) {
                if (m % 2 != 0) ImGui::SameLine();
                if (Retro::Button(ToString(ViewMode(m)), 136.0f, s->GetViewMode() == ViewMode(m) ? 1 : 0)) s->SetViewMode(ViewMode(m));
            }
            Retro::EndGroup();
            Retro::BeginGroup("Camera Shots  (1-5, 0 free)");
            const std::vector<CameraShot> shots = s->Shots();
            for (int i = 0; i < int(shots.size()); ++i) {
                if (i > 0) ImGui::SameLine();
                char label[8];
                std::snprintf(label, sizeof(label), "%d", i + 1);
                if (Retro::Button(label, 36.0f, s->ActiveShot() == i ? 1 : 0)) s->PlayShot(ctx, i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shots[size_t(i)].name.c_str());
            }
            ImGui::SameLine();
            if (Retro::Button("FREE", 0.0f, s->ActiveShot() < 0 ? 1 : 0)) s->StopShot(ctx);
            Retro::EndGroup();
            if (backToLab && Retro::Button("<< BACK TO LAB", -1.0f, 2)) ctx.scenes->Request(backToLab);
            Retro::EndPanel();
        }
        if (Retro::BeginPanel(info ? info->title : s->Name().c_str(), display.x - 392.0f, top, 382.0f)) {
            if (info) {
                ImGui::TextColored(Gold(), "%s", info->industry);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(Retro::INK[0], Retro::INK[1], Retro::INK[2], 1.0f));
                ImGui::TextWrapped("%s", info->blurb);
                ImGui::PopStyleColor();
            }
            s->DrawPanel(ctx);
            Retro::EndPanel();
        }
        OrientationTriad(ctx, *s);
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
            void OnGui(GameContext& ctx) override {
                if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) UI::SetPanelsVisible(!UI::PanelsVisible());
                if (UI::PanelsVisible()) DrawGui(ctx);
            }
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
        options.recordReplay = false;   // the robot cell keeps its own joint-space timeline
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
