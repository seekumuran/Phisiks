#include "ShowcaseTest.h"
#include "Showcase.h"

#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Profiler.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Render/RenderStats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>

using namespace Phisiks;

namespace Showcase {
    namespace {
        std::string F(const char* format, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0) {
            char text[512];
            std::snprintf(text, sizeof(text), format, a, b, c, d, e);
            return text;
        }
        double WallSeconds(std::chrono::steady_clock::time_point start) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    ShowcaseTest::ShowcaseTest(Options options) : m_options(std::move(options)) {
        std::error_code ec;
        std::filesystem::create_directories(m_options.runDirectory, ec);
    }

    void ShowcaseTest::AddStep(const std::string& name, StepFunction run, double timeoutSeconds) {
        m_steps.push_back({ name, std::move(run), timeoutSeconds });
    }

    void ShowcaseTest::Expect(const std::string& name, bool condition, const std::string& detail) {
        const std::string step = m_step < m_steps.size() ? m_steps[m_step].name : "final";
        m_checks.push_back({ step, name, condition, detail });
        Log::Info() << "[" << (condition ? "PASS" : "FAIL") << "] " << step << ": " << name << " (" << detail << ")";
    }

    double ShowcaseTest::StepSeconds(const GameContext& ctx) const { return ctx.stats.simulationTime - m_stepStart; }

    ShowcaseScene* ShowcaseTest::Scene(GameContext& ctx) const { return Current(ctx); }

    double ShowcaseTest::MeanLuma(const Render::FrameCapture& c) {
        if (c.rgba.empty()) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i + 3 < c.rgba.size(); i += 4) sum += 0.2126 * c.rgba[i] + 0.7152 * c.rgba[i + 1] + 0.0722 * c.rgba[i + 2];
        return sum / double(c.rgba.size() / 4);
    }

    double ShowcaseTest::MeanAbsDiff(const Render::FrameCapture& a, const Render::FrameCapture& b) {
        if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) return -1.0;
        double sum = 0.0;
        for (size_t i = 0; i < a.rgba.size(); ++i) if (i % 4 != 3) sum += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
        return sum / double(a.rgba.size() / 4 * 3);
    }

    void ShowcaseTest::BuildFrameworkSteps(GameContext& ctx) {
        m_built = true;
        std::vector<Step> demoSteps;
        m_steps.swap(demoSteps);

        AddStep("boot", [](GameContext& c, ShowcaseTest& t) {
            if (t.Frame() < 30) return false;
            ShowcaseScene* s = t.Scene(c);
            const Render::RendererInfo info = Render::GetRendererInfo();
            const RenderStats r = GetRenderStats();
            const Render::RenderSettings& rs = Render::Settings();
            t.Expect("showcase scene running", s != nullptr && c.state == GameState::Running, s ? s->Name() : std::string("none"));
            t.Expect("renderer tier", info.contextVersion >= 41, F("GL %.0f, ", info.contextVersion) + (info.compatTier ? "4.1 tier (CPU culling + CPU compute)" : "4.6 tier (GPU compute)") + ", " + info.renderer);
            t.Expect("quality preset applied", rs.renderScale > 0.5f && rs.renderScale <= 1.0f,
                     F("render scale %.2f, MSAA %.0fx, bloom %.0f mips, SSAO ", rs.renderScale, rs.msaaSamples, rs.bloom.mipCount) + (rs.ssao.enabled ? (rs.ssao.halfResolution ? "half-res" : "full-res") : "off"));
            t.Expect("window output size", r.outputWidth > 0 && r.outputHeight > 0, F("%.0f x %.0f output, internal %.0f x %.0f", r.outputWidth, r.outputHeight, r.internalWidth, r.internalHeight));
            t.Screenshot("boot");
            return true;
        });
        for (Step& s : demoSteps) m_steps.push_back(std::move(s));

        AddStep("cinematic camera shots 1-5", [](GameContext& c, ShowcaseTest& t) {
            ShowcaseScene* s = t.Scene(c);
            if (!s) return true;
            const int shot = t.Phase();
            if (shot >= 5) { s->StopShot(c); return true; }
            Entity cam = s->FindByName("MainCamera");
            if (t.Value("shotFrame") == 0.0) {
                s->PlayShot(c, shot);
                t.Value("shotX") = cam.WorldPosition().x; t.Value("shotY") = cam.WorldPosition().y; t.Value("shotZ") = cam.WorldPosition().z;
            }
            t.Value("shotFrame") += 1.0;
            if (t.Value("shotFrame") < 50.0) return false;
            const glm::vec3 start(float(t.Value("shotX")), float(t.Value("shotY")), float(t.Value("shotZ")));
            const glm::vec3 now = cam.WorldPosition();
            const CameraPath* path = cam.TryGet<CameraPath>();
            const std::vector<CameraShot> shots = s->Shots();
            t.Expect("shot " + std::to_string(shot + 1) + " plays (" + shots[size_t(shot)].name + ")", path && path->playing && s->ActiveShot() == shot && glm::length(now - start) > 0.05f,
                     F("camera moved %.2f m along a %.1f m spline in 50 frames", glm::length(now - start), path ? path->positionSpline.Length() : 0.0));
            t.Screenshot("shot_" + std::to_string(shot + 1));
            t.Value("shotFrame") = 0.0;
            ++t.Phase();
            return false;
        }, 60.0);

        AddStep("view modes", [](GameContext& c, ShowcaseTest& t) {
            ShowcaseScene* s = t.Scene(c);
            if (!s) return true;
            static const ViewMode order[] = { ViewMode::Final, ViewMode::Wireframe, ViewMode::Fields, ViewMode::Particles, ViewMode::Cad, ViewMode::Final };
            const int i = t.Phase() / 2;
            if (i >= 6) return true;
            if (t.Phase() % 2 == 0) {
                s->SetViewMode(order[i]);
                t.Value("modeFrame") = 0.0;
                ++t.Phase();
                return false;
            }
            t.Value("modeFrame") += 1.0;
            if (t.Value("modeFrame") == 6.0) t.RequestCapture();
            if (t.Value("modeFrame") < 7.0 || !t.CaptureReady()) return false;
            const RenderStats r = GetRenderStats();
            const double diff = i == 0 ? 0.0 : MeanAbsDiff(t.Capture(), t.m_reference);
            const std::string name = ToString(order[i]);
            switch (order[i]) {
                case ViewMode::Final:
                    if (i == 0) { t.m_reference = t.Capture(); t.Value("finalDrawn") = r.instancesDrawn; t.Expect("final render", MeanLuma(t.Capture()) > 3.0, F("mean luma %.1f, %.0f instances drawn", MeanLuma(t.Capture()), r.instancesDrawn)); }
                    else t.Expect("back to final render", !Render::Settings().wireframe && r.views >= 1 && !Render::GetSdfVisualization().enabled, F("%.0f view(s), mean luma %.1f", r.views, MeanLuma(t.Capture())));
                    break;
                case ViewMode::Wireframe:
                    t.Expect("wireframe", Render::Settings().wireframe && diff > 0.5, F("image differs from the final render by %.2f / 255 per channel", diff));
                    break;
                case ViewMode::Fields:
                    t.Expect("3D SDF & vector fields", (Render::GetSdfVisualization().enabled || r.views > 0) && diff > 0.3,
                             F("SDF primitives %.0f, image difference %.2f", double(Render::GetSdfVisualization().primitives.size()), diff) + (Render::GetSdfVisualization().enabled ? " (raymarched)" : ""));
                    break;
                case ViewMode::Particles:
                    t.Expect("raw compute particles (meshes hidden, SSF off)", !Render::Settings().fluid.enabled && r.instancesDrawn < uint32_t(t.Value("finalDrawn")) && diff > 0.5,
                             F("%.0f -> %.0f instances drawn, %.0f particles drawn, difference %.2f", t.Value("finalDrawn"), r.instancesDrawn, r.particlesDrawn, diff));
                    break;
                case ViewMode::Cad:
                    t.Expect("CAD 4-way orthographic (persp + top / front / side)", r.views >= 4 && diff > 0.5, F("%.0f views rendered, difference %.2f", r.views, diff));
                    break;
                default: break;
            }
            t.Screenshot("mode_" + std::to_string(i) + "_" + std::string(ToString(order[i])).substr(0, 4));
            ++t.Phase();
            return false;
        }, 60.0);

        if (!m_options.quick) {
            AddStep("frame budget (Medium preset)", [](GameContext& c, ShowcaseTest& t) {
                ShowcaseScene* s = t.Scene(c);
                if (!s) return true;
                if (t.Phase() == 0) { s->PlayShot(c, 0); t.Phase() = 1; t.Value("warm") = 0; return false; }
                if (t.Phase() == 1) { if (++t.Value("warm") < 90) return false; t.Phase() = 2; t.m_frameTimes.clear(); t.m_gpuTimes.clear(); t.m_cpuTimes.clear(); return false; }
                if (int(t.m_frameTimes.size()) < t.m_options.measureFrames) return false;
                std::vector<double> f = t.m_frameTimes;
                std::sort(f.begin(), f.end());
                const double avg = std::accumulate(f.begin(), f.end(), 0.0) / double(f.size());
                const double p95 = f[size_t(double(f.size() - 1) * 0.95)];
                const double gpu = t.m_gpuTimes.empty() ? 0.0 : std::accumulate(t.m_gpuTimes.begin(), t.m_gpuTimes.end(), 0.0) / double(t.m_gpuTimes.size());
                const double cpu = std::accumulate(t.m_cpuTimes.begin(), t.m_cpuTimes.end(), 0.0) / double(std::max<size_t>(t.m_cpuTimes.size(), 1));
                const RenderStats r = GetRenderStats();
                t.Value("avgFrameMs") = avg;
                auto zone = [](const char* name) { const ProfileZoneStats* z = Profiler::Get().GetZone(std::string("GPU: ") + name); return z ? z->averageMs : 0.0; };
                auto cpuZone = [](const char* name) { const ProfileZoneStats* z = Profiler::Get().GetZone(name); return z ? z->averageMs : 0.0; };
                t.Expect("GPU time per zone (average of the last 120 frames)", true,
                         F("prepass %.2f ms, scene %.2f ms, post %.2f ms (SSF %.2f ms), output %.2f ms, ", zone("Prepass"), zone("Scene"), zone("Post"), zone("SSF"), zone("Output")) + F("UI %.2f ms, ", zone("UI")) + F("simulation %.2f ms; CPU zones: ", zone("Simulation")) + F("update %.2f ms, physics %.2f ms, render %.2f ms", cpuZone("Update"), cpuZone("Physics"), cpuZone("Render")));
                t.Expect("60+ FPS average at the window size (16.7 ms budget)", avg <= 1000.0 / 60.0,
                         F("avg %.2f ms (%.1f FPS), p95 %.2f ms, max %.2f ms, ", avg, 1000.0 / avg, p95, f.back()) +
                         F("GPU %.2f ms, CPU %.2f ms, output %.0f x %.0f", gpu, cpu, r.outputWidth, r.outputHeight));
                s->StopShot(c);
                return true;
            }, 120.0);
        }

        AddStep("budgets", [](GameContext&, ShowcaseTest& t) {
            t.Expect("0 OpenGL errors", Render::GetGLErrorCount() == 0, F("%.0f GL errors", Render::GetGLErrorCount()));
            t.Expect("RAM below the 2.5 GB budget", t.m_peakRamMB > 0.0 && t.m_peakRamMB < RAM_BUDGET_MB, F("peak working set %.0f MB of 2560 MB", t.m_peakRamMB));
            return true;
        });
        (void)ctx;
    }

    void ShowcaseTest::BeforeInput(GameContext&) {}

    void ShowcaseTest::AfterSimulation(GameContext& ctx) {
        if (m_finished) return;
        if (!m_built) {
            if (!Current(ctx)) return;
            if (ShowcaseScene* s = Current(ctx)) s->AddTests(*this);
            BuildFrameworkSteps(ctx);
        }
        m_peakRamMB = std::max(m_peakRamMB, double(Platform::ProcessMemoryBytes()) / 1048576.0);
        if (m_step >= m_steps.size()) { m_finished = true; return; }
        Step& step = m_steps[m_step];
        if (!m_stepStarted) {
            m_stepStarted = true;
            m_stepStart = ctx.stats.simulationTime;
            m_stepWallStart = WallSeconds(m_start);
            m_stepFrames = 0;
            m_phase = 0;
            m_values.clear();
            Log::Info() << "Showcase test step: " << step.name;
        }
        bool done = step.run(ctx, *this);
        ++m_stepFrames;
        if (!done && WallSeconds(m_start) - m_stepWallStart > step.timeout) {
            Expect("step completes within " + F("%.0f s", step.timeout), false, F("timed out in phase %.0f after %.0f frames", m_phase, double(m_stepFrames)));
            done = true;
        }
        if (done) {
            ++m_step;
            m_stepStarted = false;
            if (m_step >= m_steps.size()) m_finished = true;
        }
    }

    void ShowcaseTest::AfterRender(GameContext& ctx) {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastFrame.time_since_epoch().count() != 0) {
            m_frameTimes.push_back(std::chrono::duration<double, std::milli>(now - m_lastFrame).count());
            if (ctx.stats.gpuFrameMs > 0.0f) m_gpuTimes.push_back(ctx.stats.gpuFrameMs);
            m_cpuTimes.push_back(ctx.stats.cpuFrameMs);
            if (m_frameTimes.size() > 4000) { m_frameTimes.clear(); m_gpuTimes.clear(); m_cpuTimes.clear(); }
        }
        m_lastFrame = now;
        if (m_captureRequested) {
            m_captureRequested = false;
            m_captureReady = Render::CaptureFrame(m_capture);
        }
        for (const std::string& name : m_pendingScreenshots) {
            const std::string path = m_options.runDirectory + "/" + name + ".png";
            if (CaptureScreenshot(path)) m_written.push_back(path);
        }
        m_pendingScreenshots.clear();
    }

    void ShowcaseTest::AfterUnload(GameContext& ctx) {
        const Physics::Stats p = Physics::GetStats();
        m_step = m_steps.size();
        Expect("scene unload releases every body and joint", p.dynamicBodies == 0 && p.staticBodies == 0 && p.kinematicBodies == 0 && p.joints == 0,
               F("%.0f dynamic, %.0f static, %.0f kinematic bodies, %.0f joints left", p.dynamicBodies, p.staticBodies, p.kinematicBodies, p.joints));
        (void)ctx;
    }

    int ShowcaseTest::ExitCode() const {
        if (m_checks.empty()) return 3;
        for (const Check& c : m_checks) if (!c.passed) return 1;
        return 0;
    }

    void ShowcaseTest::WriteReport() const {
        std::ofstream f(m_options.runDirectory + "/showcase_report.txt", std::ios::binary | std::ios::trunc);
        int passed = 0;
        for (const Check& c : m_checks) passed += c.passed ? 1 : 0;
        const Render::RendererInfo info = Render::GetRendererInfo();
        f << "Phisiks showcase autotest: " << m_options.demo << "\n";
        f << "Platform: " << Platform::OperatingSystem() << ", " << Platform::HardwareThreads() << " threads, "
          << (Platform::SystemMemoryBytes() >> 20) << " MB RAM, GL " << info.contextVersion << (info.compatTier ? " (4.1 tier)" : " (4.6 tier)") << ", " << info.renderer
          << ", physics " << Physics::BackendName() << "\n";
        f << "Result: " << passed << "/" << m_checks.size() << " checks passed" << (m_options.quick ? " (quick: no frame-budget step)" : "") << "\n\n";
        for (const Check& c : m_checks) f << (c.passed ? "[PASS] " : "[FAIL] ") << c.step << ": " << c.name << "  --  " << c.detail << "\n";
        f << "\nScreenshots:\n";
        for (const std::string& s : m_written) f << "  " << s << "\n";
        Log::Info() << "Showcase autotest " << m_options.demo << ": " << passed << "/" << m_checks.size() << " passed";
        std::printf("Showcase autotest %s: %d/%zu checks passed\n", m_options.demo.c_str(), passed, m_checks.size());
    }
}
