#pragma once
// Phisiks showcase framework (examples/common), shared by the four hero demos.
//
//   ShowcaseScene     a Phisiks::Scene with five cinematic spline camera shots (keys 1-5, 0 = free
//                     camera), a visualisation-mode switcher (M cycles, F3 toggles the CAD view):
//                       Final Render | Wireframe | 3D SDF & Vector Fields | Raw Compute Particles |
//                       CAD 4-way orthographic (perspective + top / front / side, blueprint grid)
//                     and the 1990s engineering-console HUD (RetroDeck skin): menu bar File | View |
//                     Simulation | CAD | Help, telemetry (frame time, FPS, GPU / CPU particles, RAM
//                     against the 2.5 GB budget, culling savings), the demo's control groups and a
//                     corner X / Y / Z orientation triad.
//   Run()             the executable entry point every demo's main() calls (argument parsing, the
//                     Medium quality preset at 1920x1080 by default, --autotest harness).
//   RegisterAll()     registers the four showcase scenes (hosted by the runtime tests as well).
//
// Demos only implement Build() and optional hooks; the framework owns modes, cameras and the HUD.
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Phisiks { class SceneManager; }

namespace Showcase {
    class ShowcaseTest;

    enum class ViewMode { Final, Wireframe, Fields, Particles, Cad, Count };
    const char* ToString(ViewMode mode);

    // A cinematic shot: Catmull-Rom path through `points`, looking at the matching `lookAt` points.
    struct CameraShot {
        std::string name;
        std::vector<glm::vec3> points;
        std::vector<glm::vec3> lookAt;
        float duration = 10.0f;
        bool closed = false;
    };

    // Orbit-camera helper shot: a circle of `radius` around `centre` at `height`, `turns` per duration.
    CameraShot OrbitShot(const std::string& name, const glm::vec3& centre, float radius, float height, float duration, float startAngle = 0.0f);
    // Dolly from a to b (both looking at `target`).
    CameraShot DollyShot(const std::string& name, const glm::vec3& a, const glm::vec3& b, const glm::vec3& target, float duration);

    struct DemoInfo {
        const char* scene;          // scene name
        const char* title;
        const char* industry;
        const char* blurb;
        const char* executable;
    };
    const std::vector<DemoInfo>& Demos();
    void RegisterAll(Phisiks::SceneManager& scenes);

    class ShowcaseScene : public Phisiks::Scene {
    public:
        explicit ShowcaseScene(const std::string& name) : Scene(name) {}

        // ---- demo hooks
        virtual void Build(Phisiks::GameContext& ctx) = 0;                 // create the world (OnEnter)
        virtual void Tick(Phisiks::GameContext& /*ctx*/, float /*dt*/) {}   // per frame while running
        virtual void FixedTick(Phisiks::GameContext& /*ctx*/, float /*dt*/) {}
        virtual std::vector<CameraShot> Shots() = 0;                       // five shots (keys 1-5)
        virtual void BuildSdf(Phisiks::Render::SdfVisualization& /*sdf*/) {}             // Fields mode
        virtual bool BuildFieldView(Phisiks::Render::FieldVisualization& /*field*/) { return false; }
        virtual glm::vec4 CadBounds() const { return glm::vec4(0.0f, 0.0f, 0.0f, 20.0f); }   // centre, half size (m)
        virtual void DrawPanel(Phisiks::GameContext& /*ctx*/) {}         // demo control groups (inside the right console panel)
        virtual void DrawMenu(Phisiks::GameContext& /*ctx*/) {}          // extra items in the Simulation menu
        virtual void AddTests(ShowcaseTest& /*test*/) {}                  // --autotest steps
        virtual uint32_t CpuEffectParticles() const { return 0; }        // CPU-side VFX particles (sparks, LiDAR points, ...)
        virtual const char* Controls() const { return ""; }
        virtual void OnViewModeChanged(ViewMode /*mode*/) {}

        // ---- framework
        void OnEnter(Phisiks::GameContext& ctx) final;
        void OnExit(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) final;
        void FixedUpdate(Phisiks::GameContext& ctx, float dt) final;
        void DrawUI(Phisiks::GameContext& ctx) override;                // CAD labels + blueprint grid

        void SetViewMode(ViewMode mode);
        ViewMode GetViewMode() const { return m_mode; }
        void PlayShot(Phisiks::GameContext& ctx, int index);             // 0..4
        void StopShot(Phisiks::GameContext& ctx);
        int ActiveShot() const { return m_shot; }
        float ElapsedTime() const { return m_time; }

        // Helpers for building worlds (materials are plain Render::MaterialDesc).
        Phisiks::Entity Box(const std::string& name, const glm::vec3& centre, const glm::vec3& size, const std::string& material,
                            float mass = 0.0f, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
        Phisiks::Entity Ball(const std::string& name, const glm::vec3& centre, float radius, const std::string& material, float mass = 0.0f);
        Phisiks::Entity Visual(const std::string& name, const char* mesh, const std::string& material, const glm::vec3& position,
                               const glm::vec3& scale, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
        Phisiks::Entity Ground(float size, const std::string& material, float patternPerMetre = 1.0f);
        Phisiks::Entity Sun(const glm::vec3& direction, const glm::vec3& color, float illuminance, bool shadows = true);
        Phisiks::Entity PointLight(const std::string& name, const glm::vec3& position, const glm::vec3& color, float range, float intensity);
        Phisiks::Entity SpotLight(const std::string& name, const glm::vec3& position, const glm::vec3& target, const glm::vec3& color,
                                  float range, float intensity, float outerAngle, bool shadows = false);

    private:
        void ApplyViewMode();
        void SetCadViews(bool enabled);

        ViewMode m_mode = ViewMode::Final;
        int m_shot = -1;
        float m_time = 0.0f;
        Phisiks::Render::RenderSettings m_savedSettings;
        std::vector<uint32_t> m_hiddenMeshes;                 // entity ids hidden by the particles view
    };

    // Shared assets: "Showcase/Cylinder" (radius 0.5, height 1 along Y, capped) and "Showcase/Cone"
    // (base radius 0.5 at y = -0.5, apex at +0.5). Registered once.
    void RegisterShapes();
    // Loads assets/meshes/hero/<file>.obj (tools/assetgen) as mesh "Hero/<file>" once; false if missing.
    bool HeroMesh(const std::string& file);
    std::string HeroMeshName(const std::string& file);   // "Hero/<file>"

    // Studio environments: the light-grey 1990s CAD cyclorama (floor + cove + back wall, faint grid,
    // soft key light, matched fog) scaled by `scale` (1 = 40 m wide), or the deep-space black backdrop.
    void CadStudio(ShowcaseScene& scene, float scale = 1.0f, const glm::vec3& centre = glm::vec3(0.0f));
    void SpaceBackdrop();
    // Screen-space CAD ink: dark silhouette / part edges from entity-id and depth discontinuities
    // (post pass assets/shaders/cad_edges.frag.glsl). `strength` 0..1, `width` in pixels (1..2).
    void SetCadInk(bool enabled, float strength = 0.85f, float width = 1.0f);
    bool CadInkEnabled();
    // Registers (or replaces) a plain PBR material.
    void Material(const std::string& name, const glm::vec3& base, float roughness, float metallic = 0.0f,
                  const glm::vec3& emissive = glm::vec3(0.0f), Phisiks::Render::Pattern pattern = Phisiks::Render::Pattern::None,
                  const glm::vec3& patternColor = glm::vec3(0.2f), float patternScale = 1.0f, float lineWidth = 0.04f);
    // World-space ray through a window pixel of the main camera (mouse picking). False without a camera.
    bool PixelRay(Phisiks::Scene& scene, const glm::vec2& pixel, const glm::vec2& windowSize, glm::vec3& origin, glm::vec3& direction);

    ShowcaseScene* Current(Phisiks::GameContext& ctx);       // nullptr unless the current scene is a showcase

    // Per-frame framework input (keys 1-5 / 0 / M / F3); the showcase app and the runtime tests call it in OnUpdate.
    void HandleInput(Phisiks::GameContext& ctx);
    // RetroDeck HUD + demo panel (inside IGame::OnGui). `backToLab` adds a button that requests that scene.
    void DrawGui(Phisiks::GameContext& ctx, const char* backToLab = nullptr);
    // Replay transport (step back / play / pause / step forward + scrub slider) for demo panels.
    void TimelineControls(Phisiks::GameContext& ctx);
    void SetTestMode(bool testMode);                         // automated runs ignore hardware keys
    // --export-dir / --export-vision of this run (demos add their own exports next to the engine's).
    const std::string& ExportDirectory();
    bool ExportVision();

    // Telemetry shared by the HUD and the test harness.
    struct Telemetry {
        float frameMs = 0.0f, fps = 0.0f, cpuMs = 0.0f, gpuMs = 0.0f;
        uint32_t gpuParticles = 0, cpuParticles = 0;
        double ramMB = 0.0;
        float cullingSavings = 0.0f;                          // culled / (drawn + culled)
        uint32_t drawn = 0, culled = 0;
    };
    Telemetry GetTelemetry(Phisiks::GameContext& ctx);
    constexpr double RAM_BUDGET_MB = 2560.0;

    // Executable entry point. Options: --autotest [--quick] --gl41 --quality low|medium|high
    // --hidden --headless (= hidden window: vision export needs the GPU) --export-dir <dir>
    // --export-vision --export-frames --export-every <n> --width <w> --height <h> --fullscreen
    // --max-frames <n> --run-dir <dir> --hot-reload [dir] --deterministic --no-ui
    int Run(int argc, char** argv, const char* sceneName, const char* windowTitle);
}
