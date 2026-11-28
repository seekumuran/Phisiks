#include "Phisiks/Core/Profiler.h"

#include <glad/gl.h>

namespace Phisiks {

    GpuTimer& GpuTimer::Get() {
        static GpuTimer timer;
        return timer;
    }

    void GpuTimer::BeginFrame() {
        ++m_frame;
        const size_t slotIndex = m_frame % LATENCY;
        for (auto& [name, track] : m_tracks) {
            // Opportunistically collect every slot whose result is ready (never blocks).
            for (Slot& slot : track.slots) {
                if (!slot.pending) continue;
                GLint available = 0;
                glGetQueryObjectiv(slot.end, GL_QUERY_RESULT_AVAILABLE, &available);
                if (!available) continue;
                GLuint64 t0 = 0, t1 = 0;
                glGetQueryObjectui64v(slot.begin, GL_QUERY_RESULT, &t0);   // available: no stall
                glGetQueryObjectui64v(slot.end, GL_QUERY_RESULT, &t1);
                track.lastMs = double(t1 - t0) / 1.0e6;
                slot.pending = false;
                ++m_collected;
                Profiler::Get().AddSample(("GPU: " + name).c_str(), track.lastMs);
            }
            // The slot we are about to overwrite must not still be in flight.
            if (!track.slots.empty() && track.slots[slotIndex].pending) {
                track.slots[slotIndex].pending = false;
                ++m_dropped;
            }
        }
    }

    void GpuTimer::Begin(const char* name) {
        Track& track = m_tracks[name];
        if (track.slots.empty()) {
            track.slots.resize(LATENCY);
            for (Slot& slot : track.slots) {
                glGenQueries(1, &slot.begin);
                glGenQueries(1, &slot.end);
            }
        }
        glQueryCounter(track.slots[m_frame % LATENCY].begin, GL_TIMESTAMP);
    }

    void GpuTimer::End(const char* name) {
        auto it = m_tracks.find(name);
        if (it == m_tracks.end() || it->second.slots.empty()) return;
        Slot& slot = it->second.slots[m_frame % LATENCY];
        glQueryCounter(slot.end, GL_TIMESTAMP);
        slot.pending = true;
    }

    void GpuTimer::Shutdown() {
        for (auto& [name, track] : m_tracks) {
            for (Slot& slot : track.slots) {
                glDeleteQueries(1, &slot.begin);
                glDeleteQueries(1, &slot.end);
            }
        }
        m_tracks.clear();
    }

    double GpuTimer::LastMs(const std::string& name) const {
        auto it = m_tracks.find(name);
        return it == m_tracks.end() ? -1.0 : it->second.lastMs;
    }
}
