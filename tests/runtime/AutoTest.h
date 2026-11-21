#pragma once
// Scripted runtime verification. Runs inside the real application loop with the real renderer,
// PhysX and world; drives gameplay only through synthetic input actions; records measured
// values for every check; writes <runDir>/autotest_report.txt and screenshots.

#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Runtime/Application.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Phisiks {
    class AutoTest : public ITestHarness {
    public:
        explicit AutoTest(std::string runDirectory);

        void BeforeInput(GameContext& ctx) override;     // inject synthetic input for this frame
        void AfterSimulation(GameContext& ctx) override; // run the current step's logic/checks
        void AfterRender(GameContext& ctx) override;     // take requested screenshots (back buffer, before swap)
        void AfterUnload(GameContext& ctx) override;     // shutdown: verify the scene released all resources

        bool Finished() const override { return m_finished; }
        int ExitCode() const override;
        void WriteReport() const override;

    private:
        struct Check { std::string step; std::string name; bool passed; std::string detail; };
        struct Step {
            std::string name;
            std::function<bool(GameContext&)> run;  // returns true when the step is complete
            double timeoutSimSeconds = 20.0;
        };

        void Expect(const std::string& name, bool condition, const std::string& detail);
        void Screenshot(const std::string& name) { m_pendingScreenshots.push_back(name); }
        // Frame capture (final image + entity ids) of the frame rendered after this call.
        void RequestCapture() { m_captureRequested = true; m_captureReady = false; }
        double StepSimTime(const GameContext& ctx) const;
        void BuildSteps();

        std::string m_runDirectory;
        std::vector<Step> m_steps;
        std::vector<Check> m_checks;
        std::vector<std::string> m_pendingScreenshots;
        std::vector<std::string> m_writtenScreenshots;
        bool m_captureRequested = false;
        bool m_captureReady = false;
        Render::FrameCapture m_capture;      // latest requested capture
        Render::FrameCapture m_reference;    // kept for A/B comparisons
        size_t m_stepIndex = 0;
        bool m_stepStarted = false;
        double m_stepStartSim = 0.0;
        uint64_t m_stepStartFrame = 0;
        uint64_t m_stepFrames = 0;
        int m_phase = 0;                // per-step sub-state
        bool m_finished = false;

        // Values captured across steps.
        struct Baseline {
            size_t entities = 0;
            int rigidDynamics = 0;
            int rigidStatics = 0;
            size_t gameObjects = 0;
            size_t lights = 0;
            int characterControllers = 0;
        } m_baseline;
        float m_scratch[8]{};
        bool m_backbufferRequested = false, m_backbufferReady = false;
        Render::FrameCapture m_backbuffer;   // presented frame incl. the studio UI
        Render::RenderSettings m_savedSettings;
        std::string m_shaderOriginal;
        uint32_t m_scratchId = 0;
        double m_wallMark = 0.0;       // real-time marker (seconds)
    };
}
