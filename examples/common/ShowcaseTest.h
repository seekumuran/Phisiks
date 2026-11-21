#pragma once
// Runtime verification harness for the showcase executables (--autotest). Runs inside the real
// application loop: framework steps (boot, the demo's own steps, five camera shots, five view
// modes, a 1080p frame-budget measurement on the Medium preset) and the final gates (0 OpenGL
// errors, RAM below the 2.5 GB budget). Writes <runDir>/showcase_report.txt and screenshots.
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Runtime/Application.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Showcase {
    class ShowcaseScene;

    class ShowcaseTest : public Phisiks::ITestHarness {
    public:
        struct Options {
            std::string runDirectory;
            std::string demo;
            bool quick = false;            // skip the frame-budget measurement (ctest)
            int measureFrames = 300;
        };
        explicit ShowcaseTest(Options options);

        // A step returns true when it is complete. `t.Frame()` counts frames since the step started.
        using StepFunction = std::function<bool(Phisiks::GameContext&, ShowcaseTest&)>;
        void AddStep(const std::string& name, StepFunction run, double timeoutSeconds = 30.0);

        void Expect(const std::string& name, bool condition, const std::string& detail);
        Phisiks::Render::FrameCapture stored;   // scratch capture for A/B comparisons in demo steps
        uint64_t Frame() const { return m_stepFrames; }
        double StepSeconds(const Phisiks::GameContext& ctx) const;   // simulated seconds since the step started
        int& Phase() { return m_phase; }
        double& Value(const std::string& key) { return m_values[key]; }
        void Screenshot(const std::string& name) { m_pendingScreenshots.push_back(name); }
        void RequestCapture() { m_captureRequested = true; m_captureReady = false; }
        bool CaptureReady() const { return m_captureReady; }
        const Phisiks::Render::FrameCapture& Capture() const { return m_capture; }
        static double MeanLuma(const Phisiks::Render::FrameCapture& c);
        static double MeanAbsDiff(const Phisiks::Render::FrameCapture& a, const Phisiks::Render::FrameCapture& b);
        ShowcaseScene* Scene(Phisiks::GameContext& ctx) const;
        const std::string& RunDirectory() const { return m_options.runDirectory; }

        // ITestHarness
        void BeforeInput(Phisiks::GameContext& ctx) override;
        void AfterSimulation(Phisiks::GameContext& ctx) override;
        void AfterRender(Phisiks::GameContext& ctx) override;
        void AfterUnload(Phisiks::GameContext& ctx) override;
        void WriteReport() const override;
        bool Finished() const override { return m_finished; }
        int ExitCode() const override;

    private:
        struct Step { std::string name; StepFunction run; double timeout; };
        struct Check { std::string step, name; bool passed; std::string detail; };
        void BuildFrameworkSteps(Phisiks::GameContext& ctx);

        Options m_options;
        std::vector<Step> m_steps;
        std::vector<Check> m_checks;
        std::map<std::string, double> m_values;
        std::vector<std::string> m_pendingScreenshots, m_written;
        Phisiks::Render::FrameCapture m_capture, m_reference;
        std::vector<double> m_frameTimes, m_gpuTimes, m_cpuTimes;   // ms per presented frame (wall clock), GPU, CPU
        std::chrono::steady_clock::time_point m_lastFrame{};
        bool m_captureRequested = false, m_captureReady = false;
        bool m_built = false, m_finished = false, m_stepStarted = false;
        size_t m_step = 0;
        uint64_t m_stepFrames = 0;
        double m_stepStart = 0.0, m_stepWallStart = 0.0;
        int m_phase = 0;
        double m_peakRamMB = 0.0;
        std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    };
}
