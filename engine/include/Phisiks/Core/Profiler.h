#pragma once
// CPU profiling zones (headless) and non-blocking GPU timers (OpenGL).
//
//   void Update() { PHISIKS_PROFILE_SCOPE("Update"); ... }
//   Profiler::Get().GetZone("Update")->lastMs / averageMs
//
// GPU timers record GL_TIMESTAMP queries into a ring of frames and only read results that the
// driver reports as available, so the CPU never waits for the GPU to finish.

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Phisiks {

    struct ProfileZoneStats {
        std::string name;
        double lastMs = 0.0;       // total time in the zone during the last completed frame
        double averageMs = 0.0;    // rolling average over HISTORY completed frames
        double maxMs = 0.0;        // max over the history window
        uint32_t callsLastFrame = 0;
    };

    class Profiler {
    public:
        static constexpr int HISTORY = 120;

        static Profiler& Get();

        void BeginFrame();
        void EndFrame();
        void AddSample(const char* zone, double milliseconds);   // also used by GPU timers / tests

        const ProfileZoneStats* GetZone(const std::string& name) const;
        std::vector<ProfileZoneStats> GetZones() const;          // sorted by averageMs, descending
        uint64_t FrameCount() const { return m_frames; }
        void Clear();

    private:
        struct Zone {
            ProfileZoneStats stats;
            double currentMs = 0.0;
            uint32_t currentCalls = 0;
            std::vector<double> history;   // ring buffer
            size_t head = 0;
            size_t count = 0;
        };
        std::unordered_map<std::string, Zone> m_zones;
        uint64_t m_frames = 0;
    };

    class ScopedProfileZone {
    public:
        explicit ScopedProfileZone(const char* name) : m_name(name), m_start(std::chrono::steady_clock::now()) {}
        ~ScopedProfileZone() {
            Profiler::Get().AddSample(m_name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count());
        }
        ScopedProfileZone(const ScopedProfileZone&) = delete;
        ScopedProfileZone& operator=(const ScopedProfileZone&) = delete;
    private:
        const char* m_name;
        std::chrono::steady_clock::time_point m_start;
    };

    // Non-blocking GPU timer (OpenGL GL_TIMESTAMP queries). One Begin/End pair per name per frame.
    // Results arrive LATENCY frames later and are fed into Profiler under "GPU: <name>".
    class GpuTimer {
    public:
        static constexpr int LATENCY = 4;   // frames of query ring

        static GpuTimer& Get();

        void BeginFrame();                   // collects every result that is ready; never waits
        void Begin(const char* name);
        void End(const char* name);
        void Shutdown();                     // deletes GL queries (call while the context is current)

        double LastMs(const std::string& name) const;   // most recent completed measurement (-1 = none yet)
        uint64_t SamplesCollected() const { return m_collected; }
        uint64_t SamplesDropped() const { return m_dropped; }   // slots reused before results were ready

    private:
        struct Slot { uint32_t begin = 0, end = 0; bool pending = false; };
        struct Track { std::vector<Slot> slots; double lastMs = -1.0; };
        std::unordered_map<std::string, Track> m_tracks;
        uint64_t m_frame = 0;
        uint64_t m_collected = 0;
        uint64_t m_dropped = 0;
    };
}

#define PHISIKS_PROFILE_CONCAT_INNER(a, b) a##b
#define PHISIKS_PROFILE_CONCAT(a, b) PHISIKS_PROFILE_CONCAT_INNER(a, b)
#define PHISIKS_PROFILE_SCOPE(name) ::Phisiks::ScopedProfileZone PHISIKS_PROFILE_CONCAT(phisiksProfileZone_, __LINE__)(name)
