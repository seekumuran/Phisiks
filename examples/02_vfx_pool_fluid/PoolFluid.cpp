// Showcase 02 (real-time VFX): refractive tiled pool with a buoyant ball, on the CAD studio cyclorama.
//
//   * Open-top tiled basin (tools/assetgen) with cyan tiles and dark grout; static colliders for
//     the floor and walls; a white ball (dynamic rigid body, half the density of water).
//   * Water: 128 x 128 heightfield wave solver coupled two ways to the ball (PoolSim.h): the ball
//     displaces columns (ripples) and feels buoyancy rho g V(submerged cap) and water drag.
//   * Rendering: post pass assets/shaders/pool_water.frag.glsl: heightfield ray hit, Snell
//     refraction traced against the pool box and the ball, Fresnel sky / ball reflections,
//     Beer-Lambert absorption, projected floor caustics computed on the CPU from refracted sun
//     rays (with the ball's caustic shadow); breakdown slider Wireframe -> Depth -> Normals ->
//     Caustics & Refraction (fractions wipe between stages).
//   * Click the water to make ripples; DROP BALL lifts the ball and lets it fall in.
#include "PoolSim.h"
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Core/InputState.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr float RIM = 1.45f, LEVEL = 1.20f, FLOOR_Y = 0.25f, HALF = 2.0f;
        constexpr float BALL_R = 0.35f, WATER_DENSITY = 1000.0f, BALL_DENSITY = 500.0f, G = 9.81f;
        constexpr int GRID = 128;
        const glm::vec3 TO_SUN = glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[200];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }
    }

    class PoolScene : public ShowcaseScene {
    public:
        PoolScene() : ShowcaseScene("PoolFluid") {}

        Pool::HeightField water;
        Entity ball;
        float breakdown = 3.0f, causticStrength = 0.9f, refraction = 1.0f;
        bool rain = true;
        float rainTimer = 0.0f, buoyancyN = 0.0f, submergedFraction = 0.0f;
        int tick = 0, ripples = 0;
        std::mt19937 rng{ 11 };

        static float BallMass() { return BALL_DENSITY * 4.0f / 3.0f * Pool::PI * BALL_R * BALL_R * BALL_R; }

        void Build(GameContext& ctx) override {
            Render::MaterialDesc tiles;
            tiles.baseColor = { 0.20f, 0.62f, 0.86f };
            tiles.roughness = 0.25f;
            tiles.pattern = Render::Pattern::Tiles;
            tiles.patternColor = { 0.035f, 0.05f, 0.07f };
            tiles.patternScale = 4.0f;
            tiles.lineWidth = 0.05f;
            tiles.patternRoughness = 0.8f;
            Render::RegisterMaterial("Pool/Tiles", tiles);
            Material("Pool/Ball", { 0.92f, 0.92f, 0.90f }, 0.3f, 0.0f);
            CadStudio(*this, 0.7f, glm::vec3(0.0f, 0.0f, -2.5f));
            Sun(-TO_SUN, glm::vec3(1.0f, 0.97f, 0.92f), 2.6f, true);
            Render::Settings().shadowDistance = 18.0f;
            if (HeroMesh("pool_basin")) Visual("PoolBasin", HeroMeshName("pool_basin").c_str(), "Pool/Tiles", glm::vec3(0.0f, RIM, 0.0f), glm::vec3(1.0f));
            // colliders (invisible): floor slab and four walls
            auto solid = [&](const char* name, const glm::vec3& c, const glm::vec3& size) {
                Entity e = Box(name, c, size, "Pool/Tiles");
                e.Get<MeshRenderer>().visible = false;
            };
            solid("PoolFloor", { 0.0f, FLOOR_Y - 0.1f, 0.0f }, { 4.4f, 0.2f, 4.4f });
            solid("PoolWallE", { HALF + 0.1f, RIM * 0.5f, 0.0f }, { 0.2f, RIM, 4.4f });
            solid("PoolWallW", { -HALF - 0.1f, RIM * 0.5f, 0.0f }, { 0.2f, RIM, 4.4f });
            solid("PoolWallN", { 0.0f, RIM * 0.5f, HALF + 0.1f }, { 4.4f, RIM, 0.2f });
            solid("PoolWallS", { 0.0f, RIM * 0.5f, -HALF - 0.1f }, { 4.4f, RIM, 0.2f });
            ball = Ball("Ball", { 0.5f, LEVEL + 0.9f, -0.3f }, BALL_R, "Pool/Ball", BallMass());
            water.Reset(GRID, HALF, LEVEL, FLOOR_Y);
            Render::PostPass pass;
            pass.name = "pool_water";
            pass.shader = "shaders/pool_water.frag.glsl";
            pass.textures[0] = "pool_height";
            pass.textures[1] = "pool_caustics";
            Render::SetPostPass(pass);
            Upload();
            ctx.camera.SetOrbit(glm::vec3(0.0f, 0.9f, 0.0f), 7.2f, 0.65f, -0.42f);
        }

        Pool::Sphere BallSphere() const { return { ball.WorldPosition(), BALL_R }; }

        void Ripple(float x, float z, float amount = 0.06f) {
            water.AddDrop(x, z, 0.18f, amount);
            ++ripples;
        }

        void DropBall() {
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            ball.SetWorldPose(glm::vec3(u(rng) * 0.9f, LEVEL + 1.4f, u(rng) * 0.9f), glm::quat(1, 0, 0, 0));
        }

        void FixedTick(GameContext&, float dt) override {
            const Pool::Sphere s = BallSphere();
            // buoyancy + drag on the ball
            const Physics::BodyId body = ball.GetBody();
            if (body.Valid()) {
                const float vTotal = 4.0f / 3.0f * Pool::PI * BALL_R * BALL_R * BALL_R;
                const float vSub = water.Submerged(s);
                submergedFraction = vSub / vTotal;
                buoyancyN = WATER_DENSITY * G * vSub;
                const glm::vec3 v = Physics::GetLinearVelocity(body);
                const glm::vec3 drag = -v * (BallMass() * 2.2f * submergedFraction);
                Physics::AddForce(body, glm::vec3(0.0f, buoyancyN, 0.0f) + drag);
                Physics::AddTorque(body, -Physics::GetAngularVelocity(body) * (BallMass() * 0.05f * submergedFraction));
            }
            water.CoupleSphere(s);
            water.Step();
            water.Step();
            if (rain) {
                rainTimer += dt;
                if (rainTimer > 0.9f) {
                    rainTimer = 0.0f;
                    std::uniform_real_distribution<float> u(-1.7f, 1.7f);
                    Ripple(u(rng), u(rng), 0.025f);
                }
            }
            if (++tick % 2 == 0) water.ComputeCaustics(TO_SUN, s);
        }

        void Upload() {
            Render::SetDataTexture("pool_height", GRID, GRID, 1, water.h.data(), true, false);
            Render::SetDataTexture("pool_caustics", GRID, GRID, 1, water.caustics.data(), true, false);
            const Render::RenderSettings& r = Render::Settings();
            Render::PostPass pass;
            pass.name = "pool_water";
            pass.shader = "shaders/pool_water.frag.glsl";
            pass.textures[0] = "pool_height";
            pass.textures[1] = "pool_caustics";
            pass.params[0] = glm::vec4(LEVEL, HALF, FLOOR_Y, water.Cell());
            pass.params[1] = glm::vec4(TO_SUN, 2.6f);
            pass.params[2] = glm::vec4(ball.WorldPosition(), BALL_R);
            pass.params[3] = glm::vec4(breakdown, causticStrength, refraction, 0.0f);
            pass.params[4] = glm::vec4(r.skyColorZenith * 1.2f, 0.0f);
            pass.params[5] = glm::vec4(r.skyColorHorizon * 1.2f, 0.0f);
            pass.params[6] = glm::vec4(0.32f, 0.075f, 0.055f, 0.0f);
            pass.params[7] = glm::vec4(0.92f, 0.92f, 0.90f, 0.0f);
            pass.enabled = GetViewMode() != ViewMode::Particles;
            Render::SetPostPass(pass);
        }

        void Tick(GameContext&, float) override {
            const InputState& in = GetInputState();
            if (!UI::WantsMouse() && in.WasPressed(MouseButton::Left)) {
                glm::vec3 o, d;
                const glm::dvec2 c = in.CursorPosition();
                if (PixelRay(*this, glm::vec2(c), glm::vec2(Render::GetFramebufferSize()), o, d) && d.y < -1e-3f) {
                    const glm::vec3 p = o + d * ((LEVEL - o.y) / d.y);
                    if (std::abs(p.x) < HALF && std::abs(p.z) < HALF) Ripple(p.x, p.z);
                }
            }
            Upload();
            if (breakdown < 1.0f || GetViewMode() == ViewMode::Wireframe) {   // stage 0: the heightfield as a wire mesh
                const int stepCells = 4;
                const glm::vec4 col(0.05f, 0.25f, 0.65f, 1.0f);
                for (int j = 0; j < GRID; j += stepCells) for (int i = 0; i + stepCells < GRID; i += stepCells) {
                    const glm::vec2 a = water.CellCentre(i, j), b = water.CellCentre(i + stepCells, j);
                    Render::DrawLine({ a.x, LEVEL + water.At(i, j), a.y }, { b.x, LEVEL + water.At(i + stepCells, j), b.y }, col);
                    const glm::vec2 a2 = water.CellCentre(j, i), b2 = water.CellCentre(j, i + stepCells);
                    Render::DrawLine({ a2.x, LEVEL + water.At(j, i), a2.y }, { b2.x, LEVEL + water.At(j, i + stepCells), b2.y }, col);
                }
            }
        }

        std::vector<CameraShot> Shots() override {
            return {
                OrbitShot("Pool orbit", glm::vec3(0.0f, 1.0f, 0.0f), 6.6f, 3.2f, 26.0f, 0.4f),
                DollyShot("Ball close-up", { 2.4f, 2.1f, 2.2f }, { 1.6f, 1.75f, 1.4f }, { 0.3f, 1.2f, -0.2f }, 10.0f),
                DollyShot("Caustics from above", { 0.2f, 6.4f, 0.6f }, { -0.3f, 5.6f, 1.0f }, { 0.0f, 0.3f, 0.0f }, 12.0f),
                DollyShot("Low across the water", { -3.4f, 1.75f, 3.0f }, { 3.4f, 1.75f, 3.0f }, { 0.0f, 1.1f, 0.0f }, 14.0f),
                DollyShot("Cutaway corner", { 3.6f, 2.6f, -3.4f }, { 3.0f, 2.2f, -3.9f }, { 0.0f, 0.6f, 0.0f }, 12.0f),
            };
        }
        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.8f, 0.0f, 2.8f); }
        void BuildSdf(Render::SdfVisualization& sdf) override {
            Render::SdfPrimitive b;
            b.shape = Render::SdfShape::Sphere;
            b.position = ball.WorldPosition();
            b.params = glm::vec4(BALL_R, 0, 0, 0);
            sdf.primitives.push_back(b);
            Render::SdfPrimitive box;   // the water volume
            box.shape = Render::SdfShape::Box;
            box.position = { 0.0f, (LEVEL + FLOOR_Y) * 0.5f, 0.0f };
            box.params = glm::vec4(HALF, (LEVEL - FLOOR_Y) * 0.5f, HALF, 0);
            box.operation = Render::SdfOperation::Subtraction;
            box.smoothness = 0.05f;
            Render::SdfPrimitive slab = box;
            slab.operation = Render::SdfOperation::Union;
            sdf.primitives.insert(sdf.primitives.begin(), slab);
            sdf.boundsMin = glm::vec3(-HALF, FLOOR_Y, -HALF);
            sdf.boundsMax = glm::vec3(HALF, LEVEL + 1.0f, HALF);
            sdf.color = { 0.2f, 0.6f, 0.9f };
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        }
        const char* Controls() const override { return "Click the water to make ripples. DROP BALL drops the ball back in."; }

        void DrawMenu(GameContext&) override {
            if (ImGui::MenuItem("Drop ball")) DropBall();
            if (ImGui::MenuItem("Rain drops", nullptr, rain)) rain = !rain;
            if (ImGui::MenuItem("Calm water")) water.Reset(GRID, HALF, LEVEL, FLOOR_Y);
        }

        void DrawPanel(GameContext&) override {
            Retro::BeginGroup("Shader Passes");
            Retro::EqSlider("BREAKDOWN", &breakdown, 0.0f, 3.0f, "%.2f");
            const char* stages[4] = { "WIRE", "DEPTH", "NORMALS", "FINAL" };
            for (int k = 0; k < 4; ++k) {
                if (k) ImGui::SameLine();
                if (Retro::Button(stages[k], 80.0f, int(std::floor(breakdown + 1e-3f)) == k && breakdown - std::floor(breakdown) < 1e-3f ? 1 : 0)) breakdown = float(k);
            }
            Retro::LcdField("STAGE", breakdown < 1.0f ? "WIREFRAME" : breakdown < 2.0f ? "WATER DEPTH" : breakdown < 3.0f ? "NORMALS" : "CAUSTICS + REFRACTION", true);
            Retro::EndGroup();
            Retro::BeginGroup("Optics");
            Retro::EqSlider("CAUSTICS", &causticStrength, 0.0f, 1.5f);
            Retro::EqSlider("REFRACTION", &refraction, 0.0f, 1.5f);
            Retro::EndGroup();
            Retro::BeginGroup("Interaction");
            if (Retro::Button("DROP BALL", 120.0f, 2)) DropBall();
            ImGui::SameLine();
            Retro::LedToggle("RAIN", &rain);
            ImGui::SameLine();
            if (Retro::Button("CALM")) water.Reset(GRID, HALF, LEVEL, FLOOR_Y);
            ImGui::TextDisabled("click the water surface to make ripples");
            Retro::LcdField("BALL HEIGHT", Fmt("%+.3f m vs level", ball.WorldPosition().y - LEVEL).c_str());
            Retro::LcdField("SUBMERGED", Fmt("%.1f %%  (rho 0.5)", submergedFraction * 100.0f).c_str());
            Retro::LcdField("BUOYANCY", Fmt("%.0f N  (mg %.0f N)", buoyancyN, BallMass() * G).c_str(), true);
            Retro::LcdField("MAX WAVE", Fmt("%.1f mm", water.MaxAbs() * 1000.0f).c_str());
            Retro::LcdField("VOLUME DRIFT", Fmt("%.2e m", water.MeanHeight()).c_str());
            Retro::EndGroup();
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("water pass, Snell refraction, buoyant ball at rest", [this](GameContext& c, ShowcaseTest& t) {
                if (t.StepSeconds(c) < 9.0) return false;
                const Render::PostPassStatus st = Render::GetPostPassStatus("pool_water");
                t.Expect("pool water post pass built and running", st.ready && st.runs > 100, F("%.0f builds, %.0f view passes", st.builds, double(st.runs)) + (st.error.empty() ? "" : ", " + st.error));
                const glm::vec3 in = glm::normalize(glm::vec3(std::sin(0.7854f), -std::cos(0.7854f), 0.0f));
                const glm::vec3 out = Pool::Refract(in, glm::vec3(0, 1, 0), 1.0f / Pool::WATER_IOR);
                const float ratio = std::abs(in.x) / std::abs(out.x / glm::length(out));
                t.Expect("Snell's law: sin(45 deg) / sin(refracted) = 1.333", std::abs(ratio - Pool::WATER_IOR) < 1e-4f, F("ratio %.5f", ratio));
                const float y = ball.WorldPosition().y;
                t.Expect("ball (density 0.5) floats half submerged", std::abs(submergedFraction - 0.5f) < 0.08f && std::abs(y - LEVEL) < 0.05f,
                         F("submerged %.1f %%, centre %+.1f mm from the water level, buoyancy %.0f N", submergedFraction * 100.0f, (y - LEVEL) * 1000.0f, buoyancyN) + F(" vs weight %.0f N", BallMass() * G));
                t.Expect("heightfield conserves the water volume", std::abs(water.MeanHeight()) < 2e-4, F("mean height offset %.2e m", water.MeanHeight()));
                t.Screenshot("pool_rest");
                return true;
            }, 30.0);
            test.AddStep("ball drop splash and click ripple", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Phase() == 0) { rain = false; DropBall(); t.Phase() = 1; t.Value("t") = t.StepSeconds(c); t.Value("peak") = 0.0; return false; }
                t.Value("peak") = std::max(t.Value("peak"), double(water.MaxAbs()));
                if (t.Phase() == 1) {
                    if (t.StepSeconds(c) - t.Value("t") < 1.5) return false;
                    t.Expect("falling ball makes a splash (two-way coupling)", t.Value("peak") > 0.02, F("peak wave %.1f mm", t.Value("peak") * 1000.0));
                    const float before = water.Sample(-1.2f, 1.2f);
                    Ripple(-1.2f, 1.2f);
                    t.Expect("ripple input raises the surface at the click point", water.Sample(-1.2f, 1.2f) - before > 0.04f, F("+%.1f mm", (water.Sample(-1.2f, 1.2f) - before) * 1000.0f));
                    t.Phase() = 2;
                    return false;
                }
                if (t.Frame() < 120) return false;
                t.Screenshot("pool_splash");
                rain = true;
                return true;
            }, 30.0);
            test.AddStep("projected caustics", [this](GameContext&, ShowcaseTest& t) {
                double sum = 0.0, mx = 0.0;
                for (float v : water.caustics) { sum += v; mx = std::max(mx, double(v)); }
                const double mean = sum / double(water.caustics.size());
                // under the ball (sun-shadowed through the water)
                const glm::vec3 b = ball.WorldPosition();
                const float shift = (LEVEL - FLOOR_Y) * 0.75f * TO_SUN.x / TO_SUN.y;
                // darkest 5 x 5 patch around where the sun ray through the ball centre meets the floor
                const glm::vec2 foot(b.x - shift, b.z - shift * TO_SUN.z / TO_SUN.x);
                float shadow = 1e9f;
                for (int dj = -6; dj <= 6; ++dj) for (int di = -6; di <= 6; ++di) {
                    const int i = std::clamp(int((foot.x + HALF) / water.Cell()) + di, 2, GRID - 3), j = std::clamp(int((foot.y + HALF) / water.Cell()) + dj, 2, GRID - 3);
                    float patch = 0.0f;
                    for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) patch += water.caustics[size_t(j + y) * GRID + i + x] / 25.0f;
                    shadow = std::min(shadow, patch);
                }
                t.Expect("caustic network: bright focus lines over a mean of ~1", mx / mean > 1.5 && mean > 0.6 && mean < 1.1, F("mean %.3f, max %.2f (x%.2f)", mean, mx, mx / mean));
                t.Expect("the ball's caustic shadow on the floor", shadow < mean * 0.35, F("darkest 5x5 patch near the shadow %.3f (mean %.3f)", shadow, mean));
                return true;
            });
            test.AddStep("shader pass breakdown wipe", [this](GameContext& c, ShowcaseTest& t) {
                const float stages[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
                const int k = t.Phase() / 2;
                if (k == 0 && t.Phase() == 0) { StopShot(c); c.camera.SetOrbit(glm::vec3(0.0f, 0.9f, 0.0f), 6.0f, 0.65f, -0.6f); }
                if (t.Phase() == 9) { breakdown = 3.0f; return true; }
                if (k >= 4) {
                    double minDiff = 1e9;
                    for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b) minDiff = std::min(minDiff, ShowcaseTest::MeanAbsDiff(captures[a], captures[b]));
                    t.Expect("wireframe / depth / normals / final stages all distinct", minDiff > 1.0, F("smallest difference %.2f / 255", minDiff));
                    breakdown = 2.5f;
                    t.Screenshot("pool_wipe");
                    t.Phase() = 9;
                    return false;
                }
                if (t.Phase() % 2 == 0) { breakdown = stages[k]; t.Value("f") = double(t.Frame()); ++t.Phase(); return false; }
                if (double(t.Frame()) - t.Value("f") == 4.0) t.RequestCapture();
                if (double(t.Frame()) - t.Value("f") < 5.0 || !t.CaptureReady()) return false;
                captures[k] = t.Capture();
                t.Screenshot(std::string("pool_stage_") + char('0' + k));
                ++t.Phase();
                t.Value("f") = double(t.Frame());
                return false;
            }, 30.0);
        }
        Render::FrameCapture captures[4];
    };

    void RegisterPoolFluid(SceneManager& scenes) { scenes.Register("PoolFluid", [] { return std::make_unique<PoolScene>(); }); }
}
