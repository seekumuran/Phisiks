#include "Phisiks/Core/Time.h"

#include <algorithm>
#include <chrono>

namespace Phisiks {

    void FixedStepClock::Reset() {
        m_rawDelta = m_delta = m_total = m_accumulator = 0.0;
        m_stepsThisFrame = 0;
        m_totalSteps = m_discardedSteps = 0;
        m_started = false;
    }

    void FixedStepClock::Tick(double rawDeltaSeconds) {
        if (!m_started) {
            rawDeltaSeconds = m_config.fixedDeltaSeconds;
            m_started = true;
        }
        m_rawDelta = rawDeltaSeconds;
        m_delta = std::clamp(rawDeltaSeconds, 0.0, m_config.maxFrameDeltaSeconds);
        m_total += m_delta;
        m_stepsThisFrame = 0;

        // Never bank more than one frame's worth of steps; anything beyond would be dropped anyway.
        const double cap = m_config.fixedDeltaSeconds * m_config.maxStepsPerFrame;
        m_accumulator = std::min(m_accumulator + m_delta, cap);
    }

    bool FixedStepClock::ConsumeStep() {
        constexpr double epsilon = 1e-9;
        if (m_stepsThisFrame >= m_config.maxStepsPerFrame) {
            if (m_accumulator >= m_config.fixedDeltaSeconds - epsilon) {
                m_discardedSteps += static_cast<uint64_t>((m_accumulator + epsilon) / m_config.fixedDeltaSeconds);
            }
            m_accumulator = 0.0;
            return false;
        }
        if (m_accumulator + epsilon < m_config.fixedDeltaSeconds) return false;

        m_accumulator = std::max(0.0, m_accumulator - m_config.fixedDeltaSeconds);
        if (m_accumulator < epsilon) m_accumulator = 0.0;
        ++m_stepsThisFrame;
        ++m_totalSteps;
        return true;
    }

    double FixedStepClock::Alpha() const {
        return std::clamp(m_accumulator / m_config.fixedDeltaSeconds, 0.0, 1.0);
    }

    namespace Time {
        namespace {
            using SteadyClock = std::chrono::steady_clock;
            FixedStepClock g_clock;
            SteadyClock::time_point g_last;
            bool g_deterministic = false;
            int g_stepsPerFrame = 1;
        }

        void Init(FixedStepConfig config) {
            g_clock = FixedStepClock(config);
            g_last = SteadyClock::now();
        }

        void Update() {
            const SteadyClock::time_point now = SteadyClock::now();
            if (g_deterministic) g_clock.Tick(g_clock.Config().fixedDeltaSeconds * g_stepsPerFrame);
            else g_clock.Tick(std::chrono::duration<double>(now - g_last).count());
            g_last = now;
        }

        void SetDeterministic(bool enabled, int stepsPerFrame) {
            g_deterministic = enabled;
            g_stepsPerFrame = stepsPerFrame < 1 ? 1 : stepsPerFrame;
            if (enabled) {
                FixedStepConfig config = g_clock.Config();
                config.maxStepsPerFrame = config.maxStepsPerFrame > g_stepsPerFrame ? config.maxStepsPerFrame : g_stepsPerFrame;
                const double frame = config.fixedDeltaSeconds * g_stepsPerFrame;
                config.maxFrameDeltaSeconds = config.maxFrameDeltaSeconds > frame ? config.maxFrameDeltaSeconds : frame;
                g_clock = FixedStepClock(config);
            }
        }

        bool IsDeterministic() { return g_deterministic; }

        double NowSeconds() {
            return std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count();
        }

        FixedStepClock& Clock() {
            return g_clock;
        }
    }
}
