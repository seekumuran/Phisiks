#include "Phisiks/Core/Profiler.h"

#include <algorithm>

namespace Phisiks {

    Profiler& Profiler::Get() {
        static Profiler profiler;
        return profiler;
    }

    void Profiler::BeginFrame() {
        for (auto& [name, zone] : m_zones) {
            zone.currentMs = 0.0;
            zone.currentCalls = 0;
        }
    }

    void Profiler::EndFrame() {
        for (auto& [name, zone] : m_zones) {
            if (zone.history.empty()) zone.history.assign(HISTORY, 0.0);
            zone.history[zone.head] = zone.currentMs;
            zone.head = (zone.head + 1) % HISTORY;
            zone.count = std::min<size_t>(zone.count + 1, HISTORY);

            double sum = 0.0, peak = 0.0;
            for (size_t i = 0; i < zone.count; ++i) {
                sum += zone.history[i];
                peak = std::max(peak, zone.history[i]);
            }
            zone.stats.lastMs = zone.currentMs;
            zone.stats.callsLastFrame = zone.currentCalls;
            zone.stats.averageMs = zone.count ? sum / double(zone.count) : 0.0;
            zone.stats.maxMs = peak;
        }
        ++m_frames;
    }

    void Profiler::AddSample(const char* name, double milliseconds) {
        Zone& zone = m_zones[name];
        if (zone.stats.name.empty()) zone.stats.name = name;
        zone.currentMs += milliseconds;
        zone.currentCalls++;
    }

    const ProfileZoneStats* Profiler::GetZone(const std::string& name) const {
        auto it = m_zones.find(name);
        return it == m_zones.end() ? nullptr : &it->second.stats;
    }

    std::vector<ProfileZoneStats> Profiler::GetZones() const {
        std::vector<ProfileZoneStats> zones;
        zones.reserve(m_zones.size());
        for (const auto& [name, zone] : m_zones) zones.push_back(zone.stats);
        std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) { return a.averageMs > b.averageMs; });
        return zones;
    }

    void Profiler::Clear() {
        m_zones.clear();
        m_frames = 0;
    }
}
