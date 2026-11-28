#pragma once
// Frame timing and fixed-timestep accumulation.
//
// FixedStepClock is a pure value type (no OS calls) so it can be unit-tested deterministically.
// Phisiks::Time wraps one global clock driven by the OS monotonic timer.

#include <cstdint>

namespace Phisiks {

    struct FixedStepConfig {
        double fixedDeltaSeconds = 1.0 / 60.0;   // simulation step
        double maxFrameDeltaSeconds = 0.1;        // larger frame deltas are clamped (debugger pauses, hitches)
        int maxStepsPerFrame = 5;                 // spiral-of-death guard
    };

    class FixedStepClock {
    public:
        explicit FixedStepClock(FixedStepConfig config = {}) : m_config(config) {}

        void Reset();
        // Advance by a raw (unclamped) frame delta in seconds. The first tick after Reset() is
        // treated as exactly one fixed step, because there is no previous frame to measure.
        void Tick(double rawDeltaSeconds);
        // Returns true and consumes one fixed step if enough time has accumulated. After
        // maxStepsPerFrame steps in one frame, the remaining backlog is discarded.
        bool ConsumeStep();

        double RawDelta() const        { return m_rawDelta; }
        double Delta() const           { return m_delta; }          // clamped
        double TotalTime() const       { return m_total; }          // sum of clamped deltas
        double Accumulator() const     { return m_accumulator; }
        double Alpha() const;                                       // accumulator / fixed step, [0,1]
        int StepsThisFrame() const     { return m_stepsThisFrame; }
        uint64_t TotalSteps() const    { return m_totalSteps; }
        uint64_t DiscardedSteps() const { return m_discardedSteps; }
        const FixedStepConfig& Config() const { return m_config; }

    private:
        FixedStepConfig m_config;
        double m_rawDelta = 0.0;
        double m_delta = 0.0;
        double m_total = 0.0;
        double m_accumulator = 0.0;
        int m_stepsThisFrame = 0;
        uint64_t m_totalSteps = 0;
        uint64_t m_discardedSteps = 0;
        bool m_started = false;
    };

    namespace Time {
        void Init(FixedStepConfig config = {});
        void Update();                 // once per frame
        // Deterministic stepping: every Update advances exactly `stepsPerFrame` fixed steps,
        // independent of the wall clock (reproducible simulations, recording, headless runs).
        void SetDeterministic(bool enabled, int stepsPerFrame = 1);
        bool IsDeterministic();
        double NowSeconds();           // monotonic seconds since an arbitrary epoch
        FixedStepClock& Clock();

        inline float DeltaTime()      { return float(Clock().Delta()); }
        inline float RawDeltaTime()   { return float(Clock().RawDelta()); }
        inline float FixedDeltaTime() { return float(Clock().Config().fixedDeltaSeconds); }
        inline float TotalTime()      { return float(Clock().TotalTime()); }
        inline bool ConsumeFixedStep() { return Clock().ConsumeStep(); }
    }
}
