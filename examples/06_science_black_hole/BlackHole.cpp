// Showcase 06 (science & maths): black-hole accretion disk and an integrator split-test.
//
//   * 52,000 spatial-hash plasma particles (GPU compute, or the CPU worker pool on the 4.1 tier)
//     spawned on circular orbits in a Disk around a SphericalGravity field, shaded by the
//     relativistic Doppler factor of their line-of-sight velocity (engine/shaders/doppler.glsl:
//     blueshift + beaming on the approaching side, redshift on the receding side). The shader is
//     hot-reloadable: edit doppler.glsl while the demo runs (--hot-reload).
//   * Polar jets: helical HDR ribbons around a jittering beam core (bloom).
//   * Integrator split-test: three probes start on the same eccentric orbit in the Paczynski-Wiita
//     pseudo-Newtonian potential (phi = -GM / (r - rs): it reproduces the relativistic innermost
//     stable orbit and makes orbits precess). Explicit Euler, velocity Verlet and RK4 integrate them
//     with the same step: Euler gains energy and spirals out, Verlet and RK4 keep precessing rosettes.
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Math/Integrators.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/VFX/VFX.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace Phisiks;
using namespace Phisiks::Physics::Advanced;
namespace Retro = Phisiks::UI::Retro;
namespace Integrate = Phisiks::Math::Integrate;

namespace Showcase {
    namespace {
        constexpr float GM = 40.0f;          // m^3 / s^2 (disk field and probes)
        constexpr double RS = 1.0;           // "Schwarzschild radius" of the pseudo-Newtonian potential
        constexpr uint32_t DISK_PARTICLES = 52000;
        constexpr double PROBE_STEP = 0.1;   // s per integrator step (the same for all three)

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }

        glm::dvec3 PwAcceleration(const glm::dvec3& r) {
            const double d = glm::length(r);
            const double m = GM / ((d - RS) * (d - RS));
            return -r / d * m;
        }
        double PwEnergy(const glm::dvec3& r, const glm::dvec3& v) { return 0.5 * glm::dot(v, v) - GM / (glm::length(r) - RS); }

        struct Phase3 {   // (x, v) state for RK4
            glm::dvec3 x{ 0.0 }, v{ 0.0 };
            Phase3 operator+(const Phase3& o) const { return { x + o.x, v + o.v }; }
            Phase3 operator*(double s) const { return { x * s, v * s }; }
        };

        struct Probe {
            const char* name;
            glm::vec3 color;
            glm::dvec3 x, v;
            double e0 = 0.0, maxRadius = 0.0, minRadius = 1e9;
            std::deque<glm::vec3> history;
        };
    }

    class BlackHoleScene : public ShowcaseScene {
    public:
        BlackHoleScene() : ShowcaseScene("BlackHole") {}

        Probe probes[3] = { { "EULER", { 4.0f, 0.5f, 0.3f }, {}, {} }, { "VERLET", { 3.5f, 2.6f, 0.4f }, {}, {} }, { "RK4", { 0.4f, 2.4f, 4.0f }, {}, {} } };
        int stepsPerFrame = 4;
        double probeTime = 0.0;
        bool jets = true;
        glm::quat probePlane = glm::angleAxis(glm::radians(62.0f), glm::vec3(1.0f, 0.0f, 0.3f));
        float jetTime = 0.0f;
        std::string shaderDirectory;

        void ResetProbes() {
            const double r0 = 8.0;
            const double vCircular = std::sqrt(GM * r0) / (r0 - RS);
            for (Probe& p : probes) {
                p.x = glm::dvec3(r0, 0.0, 0.0);
                p.v = glm::dvec3(0.0, 0.0, -0.92 * vCircular);   // eccentric: periapsis ~ 5.1 rs (outside the ISCO)
                p.e0 = PwEnergy(p.x, p.v);
                p.maxRadius = p.minRadius = r0;
                p.history.clear();
            }
            probeTime = 0.0;
        }

        void Build(GameContext& ctx) override {
            Material("BH/Horizon", glm::vec3(0.0f), 1.0f);
            Material("BH/ProbeEuler", glm::vec3(0.2f), 0.4f, 0.0f, { 6.0f, 0.8f, 0.4f });
            Material("BH/ProbeVerlet", glm::vec3(0.2f), 0.4f, 0.0f, { 5.0f, 3.6f, 0.5f });
            Material("BH/ProbeRK4", glm::vec3(0.2f), 0.4f, 0.0f, { 0.5f, 3.4f, 6.0f });
            Render::RenderSettings& r = Render::Settings();
            r.skyColorZenith = { 0.002f, 0.002f, 0.006f };
            r.skyColorHorizon = { 0.006f, 0.005f, 0.012f };
            r.groundColor = { 0.002f, 0.002f, 0.004f };
            r.ambientUp = { 0.02f, 0.02f, 0.03f };
            r.ambientDown = { 0.01f, 0.01f, 0.015f };
            r.fog.enabled = false;
            r.bloom.intensity = 0.12f;
            r.bloom.threshold = 0.8f;
            r.exposure = 1.2f;
            r.shadowsEnabled = false;

            Entity hole = Visual("EventHorizon", "Sphere", "BH/Horizon", glm::vec3(0.0f), glm::vec3(float(RS) * 2.0f));
            Physics::ForceField& field = hole.Add<Physics::ForceField>();
            field.type = Physics::FieldType::SphericalGravity;
            field.strength = GM;
            field.radius = float(RS);
            field.affectsRigidBodies = false;
            OrbitPath& photon = hole.Add<OrbitPath>();   // photon ring at 1.5 rs
            photon.radiusX = photon.radiusZ = 1.5f * float(RS);
            photon.width = 0.06f;
            photon.color = glm::vec4(4.0f, 2.6f, 1.2f, 1.0f);
            photon.segments = 128;
            Entity isco = CreateEntity("ISCO");   // innermost stable circular orbit of the PW potential: 3 rs
            OrbitPath& io = isco.Add<OrbitPath>();
            io.radiusX = io.radiusZ = 3.0f * float(RS);
            io.color = glm::vec4(0.5f, 0.6f, 1.0f, 0.4f);
            io.dash = 0.015f;
            io.width = 0.02f;
            // the disk
            Entity disk = CreateEntity("AccretionDisk");
            ParticleSystem& ps = disk.Add<ParticleSystem>();
            ps.count = DISK_PARTICLES;
            ps.spawnShape = ParticleSpawn::Disk;
            ps.diskInnerRadius = 3.0f;
            ps.diskOuterRadius = 13.0f;
            ps.diskThickness = 0.35f;
            ps.orbitalGM = GM;
            ps.radius = 0.035f;
            ps.mass = 0.001f;
            ps.friction = 0.0f;
            ps.damping = 0.0f;
            ps.useGravity = false;
            ps.pushRigidBodies = false;
            ps.colorMode = ParticleColor::Doppler;
            ps.color = { 1.0f, 0.55f, 0.22f };
            ps.glow = 1.5f;
            ps.dopplerSpeed = 6.0f;
            ps.seed = 21;
            // probes for the integrator split-test
            const char* materials[3] = { "BH/ProbeEuler", "BH/ProbeVerlet", "BH/ProbeRK4" };
            for (int i = 0; i < 3; ++i) Visual(std::string("Probe") + probes[i].name, "Sphere", materials[i], glm::vec3(8.0f, 0.0f, 0.0f), glm::vec3(0.35f));
            ResetProbes();
            ctx.camera.SetOrbit(glm::vec3(0.0f), 26.0f, 0.3f, -0.32f);
        }

        void StepProbes() {
            for (int s = 0; s < stepsPerFrame; ++s) {
                Integrate::ExplicitEuler(probes[0].x, probes[0].v, PROBE_STEP, PwAcceleration);
                Integrate::VelocityVerlet(probes[1].x, probes[1].v, PROBE_STEP, PwAcceleration);
                Phase3 y{ probes[2].x, probes[2].v };
                y = Integrate::RK4(y, probeTime, PROBE_STEP, [](double, const Phase3& q) { return Phase3{ q.v, PwAcceleration(q.x) }; });
                probes[2].x = y.x;
                probes[2].v = y.v;
                probeTime += PROBE_STEP;
                for (Probe& p : probes) {
                    const double r = glm::length(p.x);
                    p.maxRadius = std::max(p.maxRadius, r);
                    p.minRadius = std::min(p.minRadius, r);
                }
            }
        }

        double EnergyDrift(const Probe& p) const { return std::abs((PwEnergy(p.x, p.v) - p.e0) / p.e0); }

        void Tick(GameContext&, float dt) override {
            StepProbes();
            for (int i = 0; i < 3; ++i) {
                Probe& p = probes[i];
                const glm::vec3 world = probePlane * glm::vec3(p.x);
                if (Entity e = FindByName(std::string("Probe") + p.name); e.Valid()) e.Get<Transform>().position = world;
                if (glm::length(p.x) < 60.0) {
                    p.history.push_back(world);
                    if (p.history.size() > 1800) p.history.pop_front();
                }
                std::vector<Render::RibbonPoint> line;
                line.reserve(p.history.size());
                const size_t n = p.history.size();
                for (size_t k = 0; k < n; k += 2) line.push_back({ p.history[k], 0.05f, glm::vec4(p.color * (0.25f + 0.75f * float(k) / float(std::max<size_t>(n, 1))), 1.0f) });
                if (line.size() > 1) Render::DrawRibbon(line);
            }
            // polar jets: two helical strands per jet around a jittering beam core
            jetTime += dt;
            if (jets) {
                for (int sign = -1; sign <= 1; sign += 2) {
                    for (int strand = 0; strand < 2; ++strand) {
                        std::vector<Render::RibbonPoint> helix;
                        for (int k = 0; k <= 80; ++k) {
                            const float h = float(k) / 80.0f * 16.0f;
                            const float a = h * 1.4f - jetTime * 9.0f + float(strand) * 3.14159f;
                            const float radius = 0.15f + 0.09f * h;
                            helix.push_back({ glm::vec3(std::cos(a) * radius, float(sign) * (1.2f + h), std::sin(a) * radius), 0.08f + 0.02f * h,
                                              glm::vec4(glm::vec3(0.6f, 1.4f, 4.5f) * (1.0f - float(k) / 90.0f), 1.0f) });
                        }
                        Render::DrawRibbon(helix);
                    }
                    std::vector<Render::RibbonPoint> core;
                    for (int k = 0; k <= 40; ++k) {
                        const float h = float(k) / 40.0f * 14.0f;
                        const float j = 0.05f * std::sin(jetTime * 37.0f + h * 5.0f);
                        core.push_back({ glm::vec3(j, float(sign) * (1.1f + h), -j), 0.14f * (1.0f - float(k) / 44.0f), glm::vec4(2.5f, 3.5f, 8.0f, 1.0f) });
                    }
                    Render::DrawRibbon(core);
                }
            }
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            return {
                OrbitShot("Disk orbit", glm::vec3(0.0f), 24.0f, 7.0f, 30.0f),
                DollyShot("Edge-on (Doppler)", { -26.0f, 0.6f, 6.0f }, { -26.0f, 1.2f, -6.0f }, glm::vec3(0.0f), 12.0f),
                DollyShot("Jet flyby", { 4.0f, 26.0f, 10.0f }, { 6.0f, 8.0f, 14.0f }, glm::vec3(0.0f, 4.0f, 0.0f), 10.0f),
                DollyShot("Pole view", { 0.5f, 34.0f, 0.5f }, { 0.5f, 26.0f, 0.5f }, glm::vec3(0.0f), 8.0f),
                OrbitShot("Probe orbits", glm::vec3(0.0f), 18.0f, -9.0f, 20.0f, 1.2f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            Render::SdfPrimitive horizon;
            horizon.shape = Render::SdfShape::Sphere;
            horizon.params = glm::vec4(float(RS), 0, 0, 0);
            sdf.primitives.push_back(horizon);
            Render::SdfPrimitive photon;
            photon.shape = Render::SdfShape::Torus;
            photon.params = glm::vec4(1.5f * float(RS), 0.08f, 0, 0);
            photon.smoothness = 0.3f;
            sdf.primitives.push_back(photon);
            Render::SdfPrimitive isco = photon;
            isco.params = glm::vec4(3.0f * float(RS), 0.05f, 0, 0);
            sdf.primitives.push_back(isco);
            sdf.boundsMin = glm::vec3(-4.0f, -1.5f, -4.0f);
            sdf.boundsMax = glm::vec3(4.0f, 1.5f, 4.0f);
            sdf.color = glm::vec3(0.9f, 0.6f, 0.3f);
            sdf.metallic = 1.0f;
            sdf.roughness = 0.2f;
        }

        bool BuildFieldView(Render::FieldVisualization& field) override {
            field.boundsMin = glm::vec3(-14.0f, -0.5f, -14.0f);
            field.boundsMax = glm::vec3(14.0f, 0.5f, 14.0f);
            field.resolution = glm::ivec3(16, 1, 16);
            field.arrowScale = 0.25f;
            field.maxLength = 1.4f;
            return true;
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.0f, 0.0f, 16.0f); }
        uint32_t CpuEffectParticles() const override { return 0; }
        const char* Controls() const override { return "RMB orbit, MMB pan, wheel zoom\n--hot-reload: edit engine/shaders/doppler.glsl live"; }

        void DrawPanel(GameContext&) override {
            char big[32], small[96];
            std::snprintf(big, sizeof(big), "T+%6.1f s", probeTime);
            std::snprintf(small, sizeof(small), "step %.2f s x %d / frame  GM %.0f  rs %.1f", PROBE_STEP, stepsPerFrame, double(GM), RS);
            Retro::Lcd(big, small);
            for (const Probe& p : probes) {
                char v[64];
                const double drift = EnergyDrift(p);
                std::snprintf(v, sizeof(v), "dE %.2e  r %.1f..%.1f", drift, p.minRadius, p.maxRadius);
                Retro::LedMeter(p.name, float(std::min(1.0, std::log10(1.0 + drift * 1e4) / 4.0)), v);
            }
            ImGui::TextDisabled("energy drift |E - E0| / |E0| (log scale), radius range");
            Retro::EqSliderInt("steps / frame", &stepsPerFrame, 1, 16);
            if (Retro::Button("RESET PROBES")) ResetProbes();
            ImGui::SameLine();
            Retro::LedToggle("JETS", &jets);
            const Render::ShaderReloadStatus hs = Render::GetShaderReloadStatus();
            if (Retro::Button(hs.enabled ? "RELOAD DOPPLER.GLSL" : "WATCH SHADERS", 0.0f, hs.enabled ? (hs.failures ? 2 : 1) : 0)) {
                if (!hs.enabled) Render::EnableShaderHotReload("engine/shaders");
                else Render::ReloadShadersNow();
            }
            const Stats s = GetStats();
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "%u plasma particles (%s), %u reloads", s.particles, s.gpuAvailable ? "GPU" : "CPU", hs.reloads);
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("accretion disk: 52k particles on stable orbits", [](GameContext& c, ShowcaseTest& t) {
                if (t.StepSeconds(c) < 5.0) return false;
                const Stats s = GetStats();
                Entity disk = c.CurrentScene()->FindByName("AccretionDisk");
                std::vector<glm::vec4> positions;
                ReadParticles(disk, positions);
                size_t inDisk = 0;
                for (const glm::vec4& p : positions) {
                    const float r = glm::length(glm::vec2(p.x, p.z));
                    if (r > 2.5f && r < 14.5f && std::abs(p.y) < 1.0f) ++inDisk;
                }
                const double fraction = double(inDisk) / double(std::max<size_t>(positions.size(), 1));
                t.Expect("disk particles keep their orbits (spatial hash + field on the " + std::string(s.gpuAvailable ? "GPU" : "CPU") + ")",
                         s.particles >= 50000 && fraction > 0.95, Fmt("%.0f particles, %.4f still in the disk after 5 s", s.particles, fraction));
                return true;
            }, 30.0);
            test.AddStep("Doppler: approaching side blueshifted", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Frame() == 0) { PlayShot(c, 1); return false; }
                if (t.Frame() == 30) t.RequestCapture();
                if (t.Frame() < 31 || !t.CaptureReady()) return false;
                // camera on -X looking at +X, disk turning counter-clockwise (seen from +Y): the
                // near (+z... ) half moves towards the camera on screen-left / away on screen-right
                const Render::FrameCapture& cap = t.Capture();
                double br[2] = { 0, 0 }, rr[2] = { 0, 0 };
                for (int y = cap.height * 47 / 100; y < cap.height * 53 / 100; ++y) for (int x = 0; x < cap.width; ++x) {   // the disk band
                    const size_t p = (size_t(y) * size_t(cap.width) + size_t(x)) * 4;
                    const int half = x < cap.width / 2 ? 0 : 1;
                    rr[half] += cap.rgba[p];
                    br[half] += cap.rgba[p + 2];
                }
                const double left = br[0] / std::max(rr[0], 1.0), right = br[1] / std::max(rr[1], 1.0);
                t.Expect("blue / red ratio differs between the approaching and the receding side", std::max(left, right) > 1.3 * std::min(left, right),
                         Fmt("B/R left %.2f, right %.2f", left, right));
                t.Screenshot("doppler_edge_on");
                StopShot(c);
                return true;
            });
            test.AddStep("integrator split-test (Euler / Verlet / RK4)", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Frame() == 0) { ResetProbes(); stepsPerFrame = 8; }
                if (probeTime < 160.0) return false;
                const double e = EnergyDrift(probes[0]), v = EnergyDrift(probes[1]), r = EnergyDrift(probes[2]);
                t.Expect("explicit Euler gains energy and spirals out", e > 0.1 && probes[0].maxRadius > 1.3 * probes[1].maxRadius,
                         Fmt("Euler |dE/E| %.3f, apoapsis %.1f vs Verlet %.1f", e, probes[0].maxRadius, probes[1].maxRadius));
                t.Expect("velocity Verlet and RK4 hold the orbit", v < 0.02 && r < 0.01 && std::abs(probes[2].maxRadius - 8.0) < 0.2,
                         Fmt("Verlet |dE/E| %.2e, RK4 |dE/E| %.2e, ", v, r) + Fmt("RK4 apoapsis %.3f (start 8.000)", probes[2].maxRadius));
                t.Expect("orbits precess (pseudo-Newtonian potential)", probes[2].minRadius > 3.0 && probes[2].minRadius < 6.0,
                         Fmt("RK4 periapsis %.2f rs (ISCO 3 rs), %.0f s simulated", probes[2].minRadius, probeTime));
                t.Screenshot("split_test");
                stepsPerFrame = 4;
                (void)c;
                return true;
            }, 60.0);
            test.AddStep("Doppler shader hot reload", [this](GameContext& c, ShowcaseTest& t) {
                namespace fs = std::filesystem;
                const std::string dir = t.RunDirectory() + "/shaders";
                if (t.Phase() == 0) {
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    for (const auto& entry : fs::directory_iterator("engine/shaders", ec)) fs::copy_file(entry.path(), dir + "/" + entry.path().filename().string(), fs::copy_options::overwrite_existing, ec);
                    Render::EnableShaderHotReload(dir);
                    PlayShot(c, 1);
                    t.Phase() = 1;
                    t.Value("w") = 0;
                    return false;
                }
                if (t.Phase() == 1) {
                    if (++t.Value("w") < 20) return false;
                    t.RequestCapture();
                    t.Phase() = 2;
                    return false;
                }
                if (t.Phase() == 2) {
                    if (!t.CaptureReady()) return false;
                    t.stored = t.Capture();
                    std::ifstream in(dir + "/doppler.glsl", std::ios::binary);
                    std::stringstream ss;
                    ss << in.rdbuf();
                    std::string src = ss.str();
                    const std::string marker = "    return baseColor * tint * min(D * D * D, 8.0);";
                    const size_t at = src.find(marker);
                    if (at != std::string::npos) src.replace(at, marker.size(), "    return vec3(0.1, 3.0, 0.2) * min(D * D * D, 8.0);   // edited live by the autotest");
                    std::ofstream(dir + "/doppler.glsl", std::ios::binary | std::ios::trunc) << src;
                    t.Value("reloads") = Render::GetShaderReloadStatus().reloads;
                    t.Phase() = 3;
                    t.Value("w") = 0;
                    return false;
                }
                if (t.Phase() == 3) {
                    if (Render::GetShaderReloadStatus().reloads <= uint32_t(t.Value("reloads")) && ++t.Value("w") < 300) return false;
                    t.Value("w") = 0;
                    t.Phase() = 4;
                    return false;
                }
                if (t.Phase() == 4) {
                    if (++t.Value("w") < 10) return false;
                    t.RequestCapture();
                    t.Phase() = 5;
                    return false;
                }
                if (!t.CaptureReady()) return false;
                auto green = [](const Render::FrameCapture& cap) { double g = 0, other = 0; for (size_t p = 0; p + 3 < cap.rgba.size(); p += 4) { g += cap.rgba[p + 1]; other += cap.rgba[p] + cap.rgba[p + 2]; } return g / std::max(other * 0.5, 1.0); };
                const Render::ShaderReloadStatus s = Render::GetShaderReloadStatus();
                const double before = green(t.stored), after = green(t.Capture());
                t.Expect("edited doppler.glsl rebuilt live (disk turns green)", s.reloads > uint32_t(t.Value("reloads")) && s.failures == 0 && after > before * 1.3,
                         Fmt("%.0f reloads, %.0f programs rebuilt, green ratio %.2f -> ", s.reloads, s.programsRebuilt, before) + Fmt("%.2f", after));
                t.Screenshot("doppler_hot_reload");
                Render::DisableShaderHotReload();
                StopShot(c);
                return true;
            }, 30.0);
        }
    };

    void RegisterBlackHole(SceneManager& scenes) { scenes.Register("BlackHole", [] { return std::make_unique<BlackHoleScene>(); }); }
}
