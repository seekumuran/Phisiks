// Showcase 04 (architecture & digital twin): modern glass cliffside villa.
//
//   * Multi-level glass-and-concrete villa (PBR materials: board-formed concrete, tinted glass,
//     oak, brushed steel), an infinity pool, a stone plateau over the sea.
//   * Instanced landscaping on a heightmap terrain: pines, bushes, grass and rocks (one GPU-instanced,
//     GPU-culled draw command per mesh).
//   * Time-of-day slider: the sun's elevation / colour, the sky gradient and exposure follow the
//     hour (golden hour -> night), and at dusk the interior lights, window glow and garden spotlights
//     switch on.
//   * F3: 4-way CAD view (perspective + orthographic top / front / side) with the blueprint grid and
//     dimension outlines of every slab.
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/World/Terrain.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }
        struct Slab { glm::vec3 centre, size; };   // blueprint outlines (CAD view)
    }

    class VillaScene : public ShowcaseScene {
    public:
        VillaScene() : ShowcaseScene("Villa") {}

        float hour = 17.6f;           // golden hour by default
        bool animateDay = false;
        bool lightsOn = false;
        std::vector<Slab> slabs;
        float sunElevation = 0.0f;

        void Build(GameContext& ctx) override {
            Material("Villa/Concrete", { 0.62f, 0.6f, 0.56f }, 0.8f, 0.0f, {}, Render::Pattern::Grid, { 0.5f, 0.48f, 0.45f }, 0.8f, 0.006f);
            Material("Villa/DarkConcrete", { 0.25f, 0.25f, 0.26f }, 0.75f, 0.0f, {}, Render::Pattern::Grid, { 0.2f, 0.2f, 0.21f }, 1.0f, 0.006f);
            Material("Villa/Oak", { 0.55f, 0.36f, 0.2f }, 0.55f, 0.0f, {}, Render::Pattern::Wood, { 0.38f, 0.24f, 0.12f }, 3.0f);
            Material("Villa/Steel", { 0.12f, 0.12f, 0.13f }, 0.35f, 0.9f);
            Material("Villa/Stone", { 0.42f, 0.38f, 0.33f }, 0.9f, 0.0f, {}, Render::Pattern::Tiles, { 0.3f, 0.27f, 0.24f }, 0.5f, 0.02f);
            Material("Villa/Cliff", { 0.36f, 0.33f, 0.29f }, 0.95f, 0.0f, {}, Render::Pattern::Tiles, { 0.27f, 0.25f, 0.22f }, 0.25f, 0.03f);
            Material("Villa/Sea", { 0.02f, 0.08f, 0.12f }, 0.08f);
            Material("Villa/Pool", { 0.05f, 0.35f, 0.45f }, 0.05f, 0.0f, { 0.02f, 0.12f, 0.16f });
            SetGlass(false);

            Sun(glm::vec3(-0.5f, -0.3f, -0.8f), glm::vec3(1.0f, 0.7f, 0.45f), 2.5f);
            // plateau, cliff and the sea below
            Box("Plateau", { 0.0f, -6.0f, 2.0f }, { 70.0f, 12.0f, 28.0f }, "Villa/Cliff");
            Visual("Sea", "Plane", "Villa/Sea", { 0.0f, -11.5f, 60.0f }, { 400.0f, 1.0f, 90.0f });
            Visual("SeaFar", "Plane", "Villa/Sea", { 0.0f, -11.5f, -40.0f }, { 400.0f, 1.0f, 110.0f });
            Entity terrain = CreateEntity("Hills");
            terrain.Get<Transform>().position = glm::vec3(0.0f, -0.02f, -34.0f);
            Terrain& t = terrain.Add<Terrain>();
            t.generator.height = 9.0f;
            t.generator.flatRadius = 14.0f;
            t.generator.flatBlend = 10.0f;
            t.generator.seed = 5;
            t.generator.resolution = 97;
            t.generator.cellSize = 0.75f;
            World::RegisterFoliageMeshes();
            struct S { const char* mesh; uint32_t count; float spacing, minS, maxS; bool align; };
            const S scatters[] = { { "Foliage/Pine", 200, 2.8f, 1.0f, 1.7f, false }, { "Foliage/Bush", 260, 1.3f, 0.7f, 1.3f, false },
                                   { "Foliage/Grass", 300, 0.7f, 0.9f, 1.5f, false }, { "Foliage/Rock", 90, 1.5f, 0.5f, 1.6f, true } };
            int k = 0;
            for (const S& sc : scatters) {
                Entity e = CreateEntity(std::string("Scatter") + std::to_string(k));
                e.SetParent(terrain, false);
                FoliageScatter& f = e.Add<FoliageScatter>();
                f.mesh = sc.mesh;
                f.scatter.count = sc.count;
                f.scatter.seed = 17 + uint32_t(k);
                f.scatter.minSpacing = sc.spacing;
                f.scatter.minScale = sc.minS;
                f.scatter.maxScale = sc.maxS;
                f.scatter.keepOutRadius = 16.0f;
                f.scatter.alignToNormal = sc.align;
                f.scatter.maxSlopeDegrees = 35.0f;
                f.castShadows = k == 0;   // pines cast shadows; bushes, grass and rocks do not (cost)
                ++k;
            }
            // the villa
            auto slab = [&](const std::string& name, const glm::vec3& c, const glm::vec3& size, const char* material) {
                Box(name, c, size, material);
                slabs.push_back({ c, size });
            };
            slab("Terrace", { 0.0f, 0.15f, 3.0f }, { 26.0f, 0.3f, 14.0f }, "Villa/Stone");
            slab("GroundFloorSlab", { -2.0f, 0.45f, 0.0f }, { 15.0f, 0.3f, 8.0f }, "Villa/Concrete");
            slab("GroundCore", { -8.0f, 2.2f, 0.0f }, { 3.0f, 3.2f, 8.0f }, "Villa/Concrete");
            slab("UpperSlab", { 2.5f, 3.95f, 1.5f }, { 17.0f, 0.4f, 9.0f }, "Villa/Concrete");
            slab("UpperCore", { 9.5f, 5.6f, 1.5f }, { 3.0f, 2.9f, 9.0f }, "Villa/DarkConcrete");
            slab("Roof", { 2.0f, 7.25f, 1.5f }, { 18.5f, 0.4f, 10.0f }, "Villa/Concrete");
            slab("Chimney", { -4.0f, 8.4f, -1.5f }, { 1.2f, 2.0f, 1.2f }, "Villa/DarkConcrete");
            // glazing (ground floor and the cantilevered upper storey)
            Box("GlassFrontLow", { -2.0f, 2.2f, 3.95f }, { 9.0f, 3.2f, 0.06f }, "Villa/Glass");
            Box("GlassSideLow", { 5.45f, 2.2f, 0.0f }, { 0.06f, 3.2f, 8.0f }, "Villa/Glass");
            Box("GlassBackLow", { -2.0f, 2.2f, -3.95f }, { 9.0f, 3.2f, 0.06f }, "Villa/Glass");
            Box("GlassFrontHigh", { 1.0f, 5.6f, 5.95f }, { 14.0f, 2.9f, 0.06f }, "Villa/Glass");
            Box("GlassSideHigh", { -5.95f, 5.6f, 1.5f }, { 0.06f, 2.9f, 9.0f }, "Villa/Glass");
            Box("GlassBackHigh", { 1.0f, 5.6f, -2.95f }, { 14.0f, 2.9f, 0.06f }, "Villa/Glass");
            for (int i = 0; i < 6; ++i) Visual("Mullion" + std::to_string(i), "Cube", "Villa/Steel", { -5.5f + 2.6f * float(i), 5.6f, 6.0f }, { 0.08f, 2.9f, 0.1f });
            Box("Deck", { 6.5f, 0.35f, 6.5f }, { 7.0f, 0.1f, 6.0f }, "Villa/Oak");
            Box("PoolRim", { -4.5f, 0.32f, 7.6f }, { 12.0f, 0.06f, 4.0f }, "Villa/Stone");
            Visual("PoolWater", "Cube", "Villa/Pool", { -4.5f, 0.3f, 7.6f }, { 11.4f, 0.08f, 3.4f });
            Visual("Stair", "Cube", "Villa/Oak", { 7.5f, 2.1f, -2.0f }, { 1.2f, 0.12f, 3.6f }, glm::angleAxis(0.75f, glm::vec3(1, 0, 0)));
            for (int i = 0; i < 4; ++i) Box("Column" + std::to_string(i), { 9.5f, 1.9f, -1.5f + 2.6f * float(i) }, { 0.3f, 3.0f, 0.3f }, "Villa/Steel");
            // interior furniture blocks (visible through the glass)
            Visual("Sofa", "Cube", "Villa/DarkConcrete", { -3.0f, 0.9f, 1.5f }, { 3.0f, 0.6f, 1.0f });
            Visual("Table", "Cube", "Villa/Oak", { 1.0f, 1.1f, 0.0f }, { 2.4f, 0.08f, 1.1f });
            Visual("Bed", "Cube", "Villa/Oak", { 4.0f, 4.45f, 0.5f }, { 2.2f, 0.5f, 2.4f });
            ctx.camera.SetOrbit(glm::vec3(1.0f, 3.0f, 1.0f), 30.0f, 0.55f, -0.18f);
            ApplyTimeOfDay();
        }

        void SetGlass(bool night) {
            // tinted, reflective glazing; at night the warm interior shows through as emission
            Material("Villa/Glass", { 0.05f, 0.08f, 0.09f }, 0.04f, 0.85f, night ? glm::vec3(1.4f, 0.95f, 0.55f) : glm::vec3(0.0f));
        }

        void SetLights(bool on) {
            if (on == lightsOn && (on ? FindByName("InteriorLow").Valid() : !FindByName("InteriorLow").Valid())) return;
            lightsOn = on;
            SetGlass(on);
            for (const char* name : { "InteriorLow", "InteriorHigh", "GardenSpotA", "GardenSpotB" }) if (Entity e = FindByName(name); e.Valid()) DestroyImmediate(e);
            if (!on) return;
            PointLight("InteriorLow", { -1.5f, 2.8f, 0.0f }, { 1.0f, 0.75f, 0.45f }, 12.0f, 30.0f);
            PointLight("InteriorHigh", { 2.0f, 6.4f, 1.5f }, { 1.0f, 0.75f, 0.45f }, 12.0f, 30.0f);
            SpotLight("GardenSpotA", { 12.0f, 0.4f, 9.0f }, { 6.0f, 5.0f, 5.0f }, { 0.8f, 0.85f, 1.0f }, 22.0f, 60.0f, 0.55f);
            SpotLight("GardenSpotB", { -12.0f, 0.4f, 9.0f }, { -6.0f, 5.0f, 4.0f }, { 0.8f, 0.85f, 1.0f }, 22.0f, 60.0f, 0.55f);
        }

        // Sun / sky / exposure from the hour (simple analytic model: elevation from a sine day arc).
        void ApplyTimeOfDay() {
            const float h = std::fmod(hour + 24.0f, 24.0f);
            sunElevation = glm::radians(62.0f) * std::sin(3.14159265f * (h - 6.0f) / 12.0f);
            const float azimuth = glm::radians(-90.0f + 180.0f * (h - 6.0f) / 12.0f);
            const float day = glm::smoothstep(-0.12f, 0.25f, sunElevation);         // 0 night .. 1 day
            const float golden = 1.0f - glm::smoothstep(0.05f, 0.45f, std::abs(sunElevation));   // near the horizon
            Render::RenderSettings& r = Render::Settings();
            const glm::vec3 nightZenith(0.004f, 0.006f, 0.02f), dayZenith(0.12f, 0.28f, 0.62f);
            const glm::vec3 nightHorizon(0.01f, 0.015f, 0.035f), dayHorizon(0.55f, 0.65f, 0.75f), goldHorizon(1.0f, 0.45f, 0.18f);
            r.skyColorZenith = glm::mix(nightZenith, dayZenith, day);
            r.skyColorHorizon = glm::mix(glm::mix(nightHorizon, dayHorizon, day), goldHorizon, golden * day);
            r.ambientUp = glm::mix(glm::vec3(0.02f, 0.03f, 0.06f), glm::vec3(0.32f, 0.36f, 0.45f), day);
            r.ambientDown = glm::mix(glm::vec3(0.01f), glm::vec3(0.06f, 0.05f, 0.04f), day);
            r.fog.color = glm::mix(glm::vec3(0.01f, 0.012f, 0.03f), glm::mix(glm::vec3(0.55f, 0.6f, 0.68f), glm::vec3(0.8f, 0.45f, 0.25f), golden), day);
            r.fog.distanceDensity = 0.006f;
            r.exposure = glm::mix(2.2f, 1.0f, day);
            if (Entity sun = FindByName("Sun"); sun.Valid()) {
                const float elevation = std::max(sunElevation, glm::radians(-4.0f));
                const glm::vec3 toSun(std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth));
                const glm::vec3 dir = -glm::normalize(toSun + glm::vec3(0.0f, 0.02f, 0.0f));
                sun.Get<Transform>().rotation = glm::quatLookAt(dir, glm::vec3(0, 1, 0));
                Light& light = sun.Get<Light>();
                light.color = glm::mix(glm::vec3(1.0f, 0.97f, 0.92f), glm::vec3(1.0f, 0.55f, 0.25f), golden);
                light.intensity = sunElevation > 0.0f ? 3.0f * glm::smoothstep(0.0f, 0.18f, sunElevation) : 0.0f;
                if (sunElevation <= 0.0f) { light.color = glm::vec3(0.45f, 0.55f, 0.9f); light.intensity = 0.12f; }   // moonlight
            }
            SetLights(sunElevation < glm::radians(4.0f));
        }

        void Tick(GameContext&, float dt) override {
            if (animateDay) { hour = std::fmod(hour + dt * 0.8f, 24.0f); ApplyTimeOfDay(); }
            if (GetViewMode() == ViewMode::Cad) {   // dimension outlines of every slab
                const glm::vec4 c(0.4f, 0.8f, 1.8f, 1.0f);
                for (const Slab& s : slabs) Render::DrawAABB(s.centre - s.size * 0.5f, s.centre + s.size * 0.5f, c);
            }
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            return {
                OrbitShot("Golden-hour orbit", { 1.0f, 3.0f, 1.0f }, 28.0f, 6.0f, 30.0f, 0.4f),
                DollyShot("Glass facade", { -14.0f, 5.0f, 14.0f }, { 14.0f, 5.5f, 14.0f }, { 0.0f, 4.0f, 2.0f }, 12.0f),
                DollyShot("From the sea", { 8.0f, -6.0f, 46.0f }, { -6.0f, 2.0f, 30.0f }, { 0.0f, 3.0f, 2.0f }, 12.0f),
                DollyShot("Pool terrace", { -12.0f, 1.6f, 11.0f }, { -2.0f, 1.4f, 11.5f }, { 1.0f, 2.5f, 0.0f }, 9.0f),
                DollyShot("Site plan", { 0.0f, 70.0f, 8.0f }, { 0.0f, 55.0f, 6.0f }, { 0.0f, 0.0f, -6.0f }, 10.0f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            // the massing model: slabs as smooth-unioned rounded boxes
            for (size_t i = 0; i < slabs.size() && i < 7; ++i) {
                Render::SdfPrimitive p;
                p.shape = Render::SdfShape::RoundBox;
                p.position = slabs[i].centre;
                p.params = glm::vec4(slabs[i].size * 0.5f, 0.15f);
                p.smoothness = i == 0 ? 0.0f : 0.5f;
                sdf.primitives.push_back(p);
            }
            Render::SdfPrimitive pool;
            pool.shape = Render::SdfShape::Box;
            pool.position = { -4.5f, 0.4f, 7.6f };
            pool.params = glm::vec4(5.7f, 0.5f, 1.7f, 0.0f);
            pool.operation = Render::SdfOperation::Subtraction;
            pool.smoothness = 0.2f;
            sdf.primitives.push_back(pool);
            sdf.boundsMin = glm::vec3(-14.0f, -0.5f, -5.0f);
            sdf.boundsMax = glm::vec3(14.0f, 10.0f, 11.0f);
            sdf.color = glm::vec3(0.85f, 0.85f, 0.82f);
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(0.0f, 1.0f, 0.0f, 2.5f);
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 3.0f, 2.0f, 14.0f); }
        const char* Controls() const override { return "RMB orbit, MMB pan, wheel zoom\nF3 CAD 4-way blueprint, time slider in the panel"; }

        void DrawPanel(GameContext&) override {
            char big[32], small[96];
            const int hh = int(hour), mm = int((hour - float(hh)) * 60.0f);
            std::snprintf(big, sizeof(big), "%02d:%02d", hh, mm);
            std::snprintf(small, sizeof(small), "SUN %+.1f deg  %s", double(glm::degrees(sunElevation)), lightsOn ? "LIGHTS ON" : "DAYLIGHT");
            Retro::Lcd(big, small);
            if (Retro::EqSlider("time of day (h)", &hour, 0.0f, 24.0f, "%.1f")) ApplyTimeOfDay();
            const struct { const char* label; float h; } presets[] = { { "DAWN", 6.4f }, { "NOON", 12.0f }, { "GOLDEN", 17.6f }, { "NIGHT", 21.5f } };
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (Retro::Button(presets[i].label, 74.0f, std::abs(hour - presets[i].h) < 0.2f ? 1 : 0)) { hour = presets[i].h; ApplyTimeOfDay(); }
            }
            Retro::LedToggle("ANIMATE DAY", &animateDay);
            const World::Stats w = World::GetStats();
            const RenderStats r = GetRenderStats();
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "landscaping: %u instances in %u batches", w.foliageInstances, w.foliageBatches);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "%u draw commands, %u instances drawn", r.drawCommands, r.instancesDrawn);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "%u lights, %u slabs in the blueprint", r.lights, uint32_t(slabs.size()));
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("time of day: noon, golden hour, night", [this](GameContext&, ShowcaseTest& t) {
                static const float hours[3] = { 12.0f, 17.6f, 21.5f };
                static const char* names[3] = { "noon", "golden", "night" };
                const int i = t.Phase() / 3;
                if (i >= 3) {
                    const double rNoon = t.Value("r0") / std::max(t.Value("b0"), 1.0), rGold = t.Value("r1") / std::max(t.Value("b1"), 1.0);
                    t.Expect("noon is brighter than golden hour, which is brighter than night", t.Value("l0") > t.Value("l1") && t.Value("l1") > t.Value("l2"),
                             Fmt("mean luma noon %.1f, golden %.1f, night %.1f", t.Value("l0"), t.Value("l1"), t.Value("l2")));
                    t.Expect("golden hour is warmer (higher red / blue) than noon", rGold > rNoon * 1.1, Fmt("R/B noon %.2f, golden %.2f", rNoon, rGold));
                    t.Expect("night switches the interior + garden lights on", t.Value("lightsNight") >= 4 && t.Value("lightsNoon") <= 1,
                             Fmt("lights at noon %.0f, at night %.0f (plus window glow)", t.Value("lightsNoon"), t.Value("lightsNight")));
                    hour = 17.6f;
                    ApplyTimeOfDay();
                    return true;
                }
                switch (t.Phase() % 3) {
                    case 0: hour = hours[i]; ApplyTimeOfDay(); t.Value("w") = 0; ++t.Phase(); return false;
                    case 1: if (++t.Value("w") < 8) return false; t.RequestCapture(); ++t.Phase(); return false;
                    default: {
                        if (!t.CaptureReady()) return false;
                        const Render::FrameCapture& c = t.Capture();
                        double r = 0, b = 0;
                        for (size_t p = 0; p + 3 < c.rgba.size(); p += 4) { r += c.rgba[p]; b += c.rgba[p + 2]; }
                        t.Value("l" + std::to_string(i)) = ShowcaseTest::MeanLuma(c);
                        t.Value("r" + std::to_string(i)) = r;
                        t.Value("b" + std::to_string(i)) = b;
                        if (i == 0) t.Value("lightsNoon") = GetRenderStats().lights;
                        if (i == 2) t.Value("lightsNight") = GetRenderStats().lights;
                        t.Screenshot(std::string("time_") + names[i]);
                        ++t.Phase();
                        return false;
                    }
                }
            }, 30.0);
            test.AddStep("instanced landscaping", [](GameContext&, ShowcaseTest& t) {
                if (t.Frame() < 5) return false;
                const World::Stats w = World::GetStats();
                const RenderStats r = GetRenderStats();
                t.Expect("foliage scattered and drawn as GPU instances (one batch per mesh)", w.foliageInstances > 700 && w.foliageBatches == 4 && r.drawCommands < 40,
                         Fmt("%.0f instances in %.0f batches, %.0f draw commands this frame", w.foliageInstances, w.foliageBatches, r.drawCommands));
                return true;
            });
            test.AddStep("F3 CAD blueprint views", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Frame() == 0) { SetViewMode(ViewMode::Cad); return false; }
                if (t.Frame() == 6) t.RequestCapture();
                if (t.Frame() < 7 || !t.CaptureReady()) return false;
                Entity top = FindByName("CadTop");
                const Render::FrameCapture& cap = t.Capture();
                // the four quadrants show different projections
                auto quadLuma = [&](int qx, int qy) {
                    double sum = 0; size_t n = 0;
                    for (int y = qy * cap.height / 2; y < (qy + 1) * cap.height / 2; y += 4) for (int x = qx * cap.width / 2; x < (qx + 1) * cap.width / 2; x += 4) {
                        const size_t p = (size_t(y) * size_t(cap.width) + size_t(x)) * 4;
                        sum += 0.2126 * cap.rgba[p] + 0.7152 * cap.rgba[p + 1] + 0.0722 * cap.rgba[p + 2];
                        ++n;
                    }
                    return n ? sum / double(n) : 0.0;
                };
                const double q[4] = { quadLuma(0, 1), quadLuma(1, 1), quadLuma(0, 0), quadLuma(1, 0) };
                const double spread = *std::max_element(q, q + 4) - *std::min_element(q, q + 4);
                t.Expect("perspective + orthographic top / front / side", top.Valid() && top.Get<Camera>().projection == Projection::Orthographic && GetRenderStats().views == 4 && spread > 2.0,
                         Fmt("4 views; quadrant luma persp %.1f, top %.1f, ", q[0], q[1]) + Fmt("front %.1f, side %.1f", q[2], q[3]));
                t.Screenshot("cad_blueprint");
                SetViewMode(ViewMode::Final);
                (void)c;
                return true;
            });
        }
    };

    void RegisterVilla(SceneManager& scenes) { scenes.Register("Villa", [] { return std::make_unique<VillaScene>(); }); }
}
