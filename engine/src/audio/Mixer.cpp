#include "audio/Mixer.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks::Audio {
    void Mixer::SetOutputRate(uint32_t rate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const double ratio = double(m_outputRate) / double(std::max(rate, 1u));
        m_outputRate = std::max(rate, 1u);
        for (Voice& v : m_voices) v.step *= ratio;
    }

    uint64_t Mixer::Play(std::shared_ptr<const ClipData> clip, float volume, bool loop, float pitch) {
        if (!clip || clip->Frames() == 0 || clip->channels == 0 || pitch <= 0.0f) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t id = m_nextId++;
        Voice v{ id, std::move(clip), 0.0, 0.0, std::max(volume, 0.0f), loop };
        v.step = double(v.clip->sampleRate) / double(m_outputRate) * pitch;
        m_voices.push_back(std::move(v));
        return id;
    }

    uint64_t Mixer::PlaySpatial(std::shared_ptr<const ClipData> clip, float volume, bool loop, const glm::vec3& position,
                                float minDistance, float maxDistance, float dopplerFactor) {
        const uint64_t id = Play(std::move(clip), volume, loop, 1.0f);
        if (!id) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Voice& v : m_voices) {
            if (v.id != id) continue;
            v.spatial = true;
            v.sourcePosition = position;
            v.minDistance = std::max(minDistance, 0.01f);
            v.maxDistance = std::max(maxDistance, v.minDistance + 0.01f);
            v.doppler = std::max(dopplerFactor, 0.0f);
        }
        return id;
    }

    void Mixer::SetVoicePosition(uint64_t voice, const glm::vec3& position, const glm::vec3& velocity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Voice& v : m_voices) if (v.id == voice) { v.sourcePosition = position; v.sourceVelocity = velocity; }
    }

    void Mixer::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listenerPosition = position;
        m_listenerVelocity = velocity;
        const glm::vec3 right = glm::cross(forward, up);
        if (glm::length(right) > 1e-6f) m_listenerRight = glm::normalize(right);
    }

    void Mixer::SetSpeedOfSound(float metresPerSecond) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_speedOfSound = std::max(metresPerSecond, 1.0f);
    }

    SpatialVoiceState Mixer::Evaluate(const Voice& v) const {
        SpatialVoiceState s;
        const glm::vec3 toSource = v.sourcePosition - m_listenerPosition;
        const float d = glm::length(toSource);
        s.distance = d;
        // Inverse-square law: intensity falls with 1/d^2, so the amplitude goes with minDistance / d.
        s.distanceGain = v.minDistance / std::max(d, v.minDistance);
        const float fadeStart = 0.8f * v.maxDistance;
        if (d > fadeStart) {
            const float t = std::clamp((d - fadeStart) / (v.maxDistance - fadeStart), 0.0f, 1.0f);
            s.distanceGain *= 1.0f - t * t * (3.0f - 2.0f * t);
        }
        const glm::vec3 u = d > 1e-5f ? toSource / d : glm::vec3(0.0f);
        s.pan = std::clamp(glm::dot(u, m_listenerRight), -1.0f, 1.0f);
        const float angle = (s.pan + 1.0f) * 0.25f * 3.14159265f;   // equal power: L^2 + R^2 = 1
        const float gain = v.volume * m_master * s.distanceGain;
        s.gainLeft = std::cos(angle) * gain;
        s.gainRight = std::sin(angle) * gain;
        // Doppler: f' = f (c + v_listener . u) / (c + v_source . u), u from listener to source.
        if (v.doppler > 0.0f && d > 1e-5f) {
            const float c = m_speedOfSound;
            const float vl = std::clamp(glm::dot(m_listenerVelocity, u) * v.doppler, -0.9f * c, 0.9f * c);
            const float vs = std::clamp(glm::dot(v.sourceVelocity, u) * v.doppler, -0.9f * c, 0.9f * c);
            s.dopplerPitch = std::clamp((c + vl) / (c + vs), 0.25f, 4.0f);
        }
        return s;
    }

    bool Mixer::GetSpatialState(uint64_t voice, SpatialVoiceState& out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Voice& v : m_voices) {
            if (v.id != voice || !v.spatial) continue;
            out = Evaluate(v);
            return true;
        }
        return false;
    }

    void Mixer::Stop(uint64_t voice) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [&](const Voice& v) { return v.id == voice; }), m_voices.end());
    }

    void Mixer::StopAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_voices.clear();
    }

    bool Mixer::IsPlaying(uint64_t voice) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::any_of(m_voices.begin(), m_voices.end(), [&](const Voice& v) { return v.id == voice; });
    }

    void Mixer::SetVolume(uint64_t voice, float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Voice& v : m_voices) if (v.id == voice) v.volume = std::max(volume, 0.0f);
    }

    void Mixer::SetMasterVolume(float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_master = std::max(volume, 0.0f);
    }

    size_t Mixer::ActiveVoices() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_voices.size();
    }

    uint64_t Mixer::FramesMixed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_framesMixed;
    }

    void Mixer::Mix(float* stereo, size_t frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        MixLocked(stereo, frames);
    }

    void Mixer::Advance(size_t frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<float> scratch(std::min<size_t>(frames, 4096) * 2);
        while (frames > 0) {
            const size_t n = std::min<size_t>(frames, 4096);
            MixLocked(scratch.data(), n);
            frames -= n;
        }
    }

    void Mixer::MixLocked(float* stereo, size_t frames) {
        std::fill(stereo, stereo + frames * 2, 0.0f);
        for (Voice& v : m_voices) {
            const ClipData& c = *v.clip;
            const size_t clipFrames = c.Frames();
            const int ch = c.channels;
            // Per-block gains (ramped from the previous block's values for spatial voices).
            float left0 = v.volume * m_master, right0 = left0, left1 = left0, right1 = right0, pitch0 = 1.0f, pitch1 = 1.0f;
            if (v.spatial) {
                const SpatialVoiceState s = Evaluate(v);
                left1 = s.gainLeft; right1 = s.gainRight; pitch1 = s.dopplerPitch;
                if (v.lastLeft < 0.0f) { v.lastLeft = left1; v.lastRight = right1; v.lastPitch = pitch1; }
                left0 = v.lastLeft; right0 = v.lastRight; pitch0 = v.lastPitch;
                v.lastLeft = left1; v.lastRight = right1; v.lastPitch = pitch1;
            }
            const float invFrames = frames > 1 ? 1.0f / float(frames - 1) : 0.0f;
            for (size_t f = 0; f < frames; ++f) {
                if (v.position >= double(clipFrames)) {
                    if (!v.loop) break;
                    v.position = std::fmod(v.position, double(clipFrames));
                }
                const size_t i0 = size_t(v.position);
                size_t i1 = i0 + 1;
                if (i1 >= clipFrames) i1 = v.loop ? 0 : i0;
                const float t = float(v.position - double(i0));
                const float l0 = c.samples[i0 * ch], l1 = c.samples[i1 * ch];
                const float r0 = ch > 1 ? c.samples[i0 * ch + 1] : l0;
                const float r1 = ch > 1 ? c.samples[i1 * ch + 1] : l1;
                const float l = l0 + (l1 - l0) * t, r = r0 + (r1 - r0) * t;
                const float k = float(f) * invFrames;
                if (v.spatial) {
                    const float mono = 0.5f * (l + r);
                    stereo[f * 2] += mono * (left0 + (left1 - left0) * k);
                    stereo[f * 2 + 1] += mono * (right0 + (right1 - right0) * k);
                    v.position += v.step * double(pitch0 + (pitch1 - pitch0) * k);
                }
                else {
                    stereo[f * 2] += l * left1;
                    stereo[f * 2 + 1] += r * right1;
                    v.position += v.step;
                }
            }
        }
        for (size_t i = 0; i < frames * 2; ++i) stereo[i] = std::clamp(stereo[i], -1.0f, 1.0f);
        m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                      [](const Voice& v) { return !v.loop && v.position >= double(v.clip->Frames()); }),
                       m_voices.end());
        m_framesMixed += frames;
    }
}
