// Showcase 03 (science): Schwarzschild black hole with ray-traced gravitational lensing.
//
//   * Post pass assets/shaders/black_hole.frag.glsl integrates every view ray as a null geodesic
//     (Binet equation, RK4 in the orbital angle): the event-horizon shadow, the Einstein photon
//     ring at the photon sphere, the lensed starfield and the far side of the accretion disk bent
//     over and under the shadow all come out of the same integration.
//   * The disk is 16,000 CPU particles in the Paczynski-Wiita potential (DiskSim.h), advanced by
//     Euler, velocity Verlet or RK4 (or all three in 120-degree sectors: the integrator
//     split-test), binned into a polar density texture every tick and shaded in the lens pass with
//     a thin-disk temperature profile, relativistic Doppler shift + beaming and gravitational
//     redshift (doppler.glsl, hot-reloadable with --hot-reload).
//   * Three probes start on the same eccentric orbit, one per integrator (glowing trails).
#include "DiskSim.h"
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr int PARTICLES = 16000;
        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[200];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }
    }

    class BlackHoleScene : public ShowcaseScene {
    public:
        BlackHoleScene() : ShowcaseScene("BlackHole") {}

        Disk::DiskSim disk;
        Disk::Integrator method = Disk::Integrator::Rk4;
        bool split = false;
        float mass = 1.0f, inclinationDeg = 6.0f, doppler = 1.0f, beaming = 3.0f, brightness = 1.0f, timeScale = 1.0f;
        float stars = 1.0f, ringGain = 0.9f;
        struct Probe { glm::dvec2 p, v; double e0; std::deque<glm::vec3> trail; Entity entity; };
        Probe probes[3];
        double simTime = 0.0;

        glm::vec3 Normal() const { const float i = glm::radians(inclinationDeg); return glm::vec3(0.0f, std::cos(i), std::sin(i)); }
        glm::vec3 Axis0() const { return glm::vec3(1.0f, 0.0f, 0.0f); }
        glm::vec3 ToWorld(const glm::dvec2& p) const {
            const glm::vec3 n = Normal(), b1 = Axis0(), b2 = glm::cross(n, b1);
            return (b1 * float(p.x) + b2 * float(p.y)) * mass;
        }

        void ResetProbes() {
            for (int k = 0; k < 3; ++k) {
                probes[k].p = glm::dvec2(11.0, 0.0);
                probes[k].v = glm::dvec2(0.0, Disk::CircularSpeed(11.0) * 0.86);
                probes[k].e0 = Disk::Energy(probes[k].p, probes[k].v);
                probes[k].trail.clear();
            }
        }

        void Build(GameContext& ctx) override {
            SpaceBackdrop();
            Material("BH/ProbeEuler", glm::vec3(0.2f), 0.4f, 0.0f, { 6.0f, 1.0f, 0.3f });
            Material("BH/ProbeVerlet", glm::vec3(0.2f), 0.4f, 0.0f, { 0.6f, 5.0f, 0.9f });
            Material("BH/ProbeRK4", glm::vec3(0.2f), 0.4f, 0.0f, { 0.6f, 1.6f, 6.0f });
            const char* mats[3] = { "BH/ProbeEuler", "BH/ProbeVerlet", "BH/ProbeRK4" };
            const char* names[3] = { "ProbeEuler", "ProbeVerlet", "ProbeRK4" };
            for (int k = 0; k < 3; ++k) probes[k].entity = Visual(names[k], "Sphere", mats[k], glm::vec3(11.0f, 0.0f, 0.0f), glm::vec3(0.32f));
            disk.Seed(PARTICLES);
            disk.Bin();
            ResetProbes();
            UpdatePass();
            ctx.camera.SetOrbit(glm::vec3(0.0f), 36.0f, 0.0f, -0.12f);
        }

        void FixedTick(GameContext&, float) override {
            const double h = 0.45 * double(timeScale);
            if (h <= 0.0) return;
            disk.Step(method, split, h);
            disk.Bin();
            Disk::Advance(Disk::Integrator::Euler, probes[0].p, probes[0].v, h);
            Disk::Advance(Disk::Integrator::Verlet, probes[1].p, probes[1].v, h);
            Disk::Advance(Disk::Integrator::Rk4, probes[2].p, probes[2].v, h);
            for (Probe& p : probes) {
                if (glm::length(p.p) > 40.0 || glm::length(p.p) < 2.2) { ResetProbes(); break; }
                p.trail.push_back(ToWorld(p.p));
                if (p.trail.size() > 260) p.trail.pop_front();
            }
            simTime += h;
        }

        void UpdatePass() {
            Render::SetDataTexture("bh_disk", disk.binsPhi, disk.binsR, 4, disk.density.data(), true, true);
            Render::PostPass pass;
            pass.name = "black_hole";
            pass.shader = "shaders/black_hole.frag.glsl";
            pass.textures[0] = "bh_disk";
            pass.params[0] = glm::vec4(0.0f, 0.0f, 0.0f, mass);
            pass.params[1] = glm::vec4(Normal(), doppler);
            pass.params[2] = glm::vec4(float(disk.rIn), float(disk.rOut), beaming, brightness);
            pass.params[3] = glm::vec4(split ? 1.0f : 0.0f, GetViewMode() == ViewMode::Particles ? 1.0f : 0.0f, stars, ringGain);
            pass.params[4] = glm::vec4(Axis0(), 0.0f);
            Render::SetPostPass(pass);
        }

        void Tick(GameContext&, float) override {
            UpdatePass();
            const glm::vec4 colors[3] = { { 3.0f, 0.6f, 0.15f, 1 }, { 0.3f, 2.6f, 0.5f, 1 }, { 0.3f, 0.9f, 3.2f, 1 } };
            for (int k = 0; k < 3; ++k) {
                Probe& p = probes[k];
                p.entity.SetWorldPosition(ToWorld(p.p));
                p.entity.Get<Transform>().scale = glm::vec3(0.32f * mass);
                if (p.trail.size() < 2) continue;
                std::vector<Render::RibbonPoint> pts;
                for (size_t i = 0; i < p.trail.size(); ++i) {
                    const float age = float(i) / float(p.trail.size() - 1);
                    pts.push_back({ p.trail[i], 0.06f * mass, colors[k] * (0.1f + 0.9f * age) });
                }
                Render::DrawRibbon(pts);
            }
        }

        std::vector<CameraShot> Shots() override {
            const float m = mass;
            return {
                OrbitShot("Orbit near the disk plane", glm::vec3(0.0f), 38.0f * m, 4.5f * m, 30.0f),
                DollyShot("Into the photon ring", glm::vec3(0.0f, 2.0f, 42.0f) * m, glm::vec3(0.0f, 1.0f, 22.0f) * m, glm::vec3(0.0f), 12.0f),
                DollyShot("Face-on from above", glm::vec3(0.5f, 46.0f, 4.0f) * m, glm::vec3(-0.5f, 40.0f, 8.0f) * m, glm::vec3(0.0f), 12.0f),
                DollyShot("Edge-on sweep", glm::vec3(-30.0f, 0.6f, 24.0f) * m, glm::vec3(30.0f, 0.6f, 24.0f) * m, glm::vec3(0.0f), 16.0f),
                DollyShot("Through the disk plane", glm::vec3(26.0f, -9.0f, 22.0f) * m, glm::vec3(20.0f, 9.0f, 26.0f) * m, glm::vec3(0.0f), 14.0f),
            };
        }
        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.0f, 0.0f, 24.0f * mass); }
        void BuildSdf(Render::SdfVisualization& sdf) override {
            Render::SdfPrimitive horizon;   // event horizon (r = 2M) and the photon sphere shell (r = 3M) as a torus slice
            horizon.shape = Render::SdfShape::Sphere;
            horizon.params = glm::vec4(2.0f * mass, 0, 0, 0);
            sdf.primitives.push_back(horizon);
            Render::SdfPrimitive isco;
            isco.shape = Render::SdfShape::Torus;
            isco.rotation = glm::angleAxis(glm::radians(inclinationDeg), glm::vec3(1, 0, 0));
            isco.params = glm::vec4(6.0f * mass, 0.15f * mass, 0, 0);
            sdf.primitives.push_back(isco);
            sdf.boundsMin = glm::vec3(-7.0f * mass);
            sdf.boundsMax = glm::vec3(7.0f * mass);
            sdf.color = { 0.9f, 0.5f, 0.2f };
        }
        const char* Controls() const override { return "Integrator buttons switch the disk solver; SPLIT TEST gives each third of the disk its own integrator."; }

        void DrawMenu(GameContext&) override {
            if (ImGui::MenuItem("Integrator split-test", nullptr, split)) split = !split;
            if (ImGui::MenuItem("Reset disk")) { disk.Seed(PARTICLES); ResetProbes(); }
            if (ImGui::MenuItem("Reload lens shader")) Render::ReloadShadersNow();
        }

        void DrawPanel(GameContext&) override {
            Retro::BeginGroup("Black Hole Parameters");
            Retro::EqSlider("MASS (m)", &mass, 0.4f, 3.0f, "%.2f");
            Retro::EqSlider("INCLINATION", &inclinationDeg, 0.0f, 85.0f, "%.1f deg");
            Retro::EqSlider("DOPPLER", &doppler, 0.0f, 1.5f, "%.2f");
            Retro::EqSlider("BEAMING", &beaming, 0.0f, 4.0f, "g^%.1f");
            Retro::EqSlider("DISK GLOW", &brightness, 0.0f, 3.0f);
            Retro::EqSlider("TIME SCALE", &timeScale, 0.0f, 3.0f);
            Retro::LcdField("SHADOW RADIUS", Fmt("%.2f m (3 sqrt3 M)", 5.196 * mass).c_str(), true);
            Retro::LcdField("ISCO / PHOTON", Fmt("%.1f m / %.1f m", 6.0 * mass, 3.0 * mass).c_str());
            Retro::EndGroup();
            Retro::BeginGroup("Integrator Split-Test");
            const char* names[3] = { "EULER", "VERLET", "RK4" };
            for (int k = 0; k < 3; ++k) {
                if (k) ImGui::SameLine();
                if (Retro::Button(names[k], 76.0f, !split && int(method) == k ? 1 : 0)) { method = Disk::Integrator(k); split = false; }
            }
            ImGui::SameLine();
            Retro::LedToggle("SPLIT", &split);
            if (Retro::Button("RESET DISK", -1.0f)) { disk.Seed(PARTICLES); ResetProbes(); }
            const glm::dvec3 drift = disk.Drift();
            Retro::LcdField("dE/E EULER", Fmt("%.2e  (%.0f lost)", drift.x, double(disk.reseeded[0])).c_str(), true);
            Retro::LcdField("dE/E VERLET", Fmt("%.2e  (%.0f lost)", drift.y, double(disk.reseeded[1])).c_str());
            Retro::LcdField("dE/E RK4", Fmt("%.2e  (%.0f lost)", drift.z, double(disk.reseeded[2])).c_str());
            Retro::LcdField("PROBE r E/V/R", Fmt("%.1f / %.1f / ", glm::length(probes[0].p), glm::length(probes[1].p)).append(Fmt("%.1f M", glm::length(probes[2].p))).c_str());
            Retro::EndGroup();
            Retro::BeginGroup("Lens");
            Retro::EqSlider("STARS", &stars, 0.0f, 3.0f);
            Retro::EqSlider("PHOTON RING", &ringGain, 0.0f, 3.0f);
            const Render::PostPassStatus st = Render::GetPostPassStatus("black_hole");
            Retro::LcdField("LENS SHADER", st.ready ? Fmt("built %.0fx, %.0f fails", st.builds, st.failures).c_str() : "NOT BUILT", !st.ready);
            Retro::EndGroup();
        }

        // Luma helpers on a capture (rows bottom-up).
        static double Luma(const Render::FrameCapture& c, int x, int y) {
            const uint8_t* p = &c.rgba[(size_t(y) * c.width + x) * 4];
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("integrator split-test (Euler drifts, Verlet / RK4 hold)", [](GameContext&, ShowcaseTest& t) {
                Disk::DiskSim d;
                d.Seed(3000, 9);
                for (int s = 0; s < 1500; ++s) d.Step(Disk::Integrator::Rk4, true, 0.45);
                const glm::dvec3 drift = d.Drift();
                t.Expect("Euler sector gains energy (|dE/E| > 1e-2)", drift.x > 1e-2, F("Euler %.3e, re-seeded %.0f", drift.x, double(d.reseeded[0])));
                t.Expect("Verlet and RK4 sectors stay bound (|dE/E| < 2e-3)", drift.y < 2e-3 && drift.z < 2e-3, F("Verlet %.3e, RK4 %.3e", drift.y, drift.z));
                t.Expect("RK4 more accurate than Euler by > 100x", drift.z * 100.0 < drift.x, F("ratio %.0f", drift.x / std::max(drift.z, 1e-12)));
                return true;
            });
            test.AddStep("lensing: shadow size, Doppler asymmetry, hot reload", [this](GameContext& c, ShowcaseTest& t) {
                switch (t.Phase()) {
                    case 0:   // face the hole from 36 M with the disk off: measure the shadow
                        StopShot(c);
                        c.camera.SetOrbit(glm::vec3(0.0f), 36.0f, 0.0f, 0.0f);
                        brightness = 0.0f; ringGain = 2.0f; stars = 0.0f;   // the photon ring marks the shadow edge
                        t.Value("f") = double(t.Frame());
                        t.Phase() = 1;
                        return false;
                    case 1: {
                        if (double(t.Frame()) - t.Value("f") == 6.0) t.RequestCapture();
                        if (double(t.Frame()) - t.Value("f") < 7.0 || !t.CaptureReady()) return false;
                        const Render::FrameCapture& cap = t.Capture();
                        const int cx = cap.width / 2, cy = cap.height / 2;
                        // shadow edge: first column from the centre where the photon ring lights up
                        int edge = 0;
                        for (int x = cx; x < cap.width - 8; ++x) {
                            double m = 0.0;
                            for (int y = cy - 40; y <= cy + 40; ++y) for (int k = 0; k < 8; ++k) m = std::max(m, Luma(cap, x + k, y));
                            if (m > 25.0) { edge = x - cx; break; }
                        }
                        Entity cam = FindByName("MainCamera");
                        const float fov = cam.Valid() ? cam.Get<Camera>().verticalFov : 1.0f;
                        const double dist = glm::length(cam.WorldPosition());
                        const double angle = std::asin(5.196 * mass / dist);   // critical impact parameter b = 3 sqrt(3) M
                        const double expected = std::tan(angle) / std::tan(fov * 0.5) * cap.height * 0.5;
                        t.Expect("shadow radius matches the critical impact parameter 3 sqrt(3) M", edge > 0 && std::abs(edge - expected) / expected < 0.12,
                                 F("measured %.0f px, expected %.0f px (camera %.1f M away)", edge, expected, dist / mass));
                        t.Expect("shadow is black", Luma(cap, cx, cy) < 4.0, F("centre luma %.1f", Luma(cap, cx, cy)));
                        brightness = 1.0f; ringGain = 0.9f; stars = 1.0f; inclinationDeg = 4.0f;
                        t.Value("f") = double(t.Frame());
                        t.Phase() = 2;
                        return false;
                    }
                    case 2: {
                        if (double(t.Frame()) - t.Value("f") == 6.0) t.RequestCapture();
                        if (double(t.Frame()) - t.Value("f") < 7.0 || !t.CaptureReady()) return false;
                        const Render::FrameCapture& cap = t.Capture();
                        double left = 0.0, right = 0.0;
                        for (int y = cap.height / 2 - 30; y < cap.height / 2 + 30; ++y) {
                            for (int x = 0; x < cap.width / 2; ++x) left += Luma(cap, x, y);
                            for (int x = cap.width / 2; x < cap.width; ++x) right += Luma(cap, x, y);
                        }
                        const double ratio = std::max(left, right) / std::max(std::min(left, right), 1.0);
                        t.Expect("Doppler beaming: approaching side of the disk brighter", ratio > 1.5, F("left / right brightness ratio %.2f", left / std::max(right, 1.0)));
                        t.Screenshot("bh_disk");
                        const uint32_t builds = Render::GetPostPassStatus("black_hole").builds;
                        const bool ok = Render::ReloadShadersNow();
                        t.Expect("lens shader + doppler.glsl rebuild (hot reload path)", ok && Render::GetPostPassStatus("black_hole").builds == builds + 1,
                                 F("builds %.0f -> %.0f", builds, Render::GetPostPassStatus("black_hole").builds));
                        inclinationDeg = 6.0f;
                        return true;
                    }
                }
                return true;
            }, 30.0);
        }
    };

    void RegisterBlackHole(SceneManager& scenes) { scenes.Register("BlackHole", [] { return std::make_unique<BlackHoleScene>(); }); }
}
