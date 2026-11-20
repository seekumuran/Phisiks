#include "audio/Mixer.h"

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
        m_voices.push_back({ id, std::move(clip), 0.0, 0.0, std::max(volume, 0.0f), loop });
        Voice& v = m_voices.back();
        v.step = double(v.clip->sampleRate) / double(m_outputRate) * pitch;
        return id;
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
            const float gain = v.volume * m_master;
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
                stereo[f * 2] += (l0 + (l1 - l0) * t) * gain;
                stereo[f * 2 + 1] += (r0 + (r1 - r0) * t) * gain;
                v.position += v.step;
            }
        }
        for (size_t i = 0; i < frames * 2; ++i) stereo[i] = std::clamp(stereo[i], -1.0f, 1.0f);
        m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                      [](const Voice& v) { return !v.loop && v.position >= double(v.clip->Frames()); }),
                       m_voices.end());
        m_framesMixed += frames;
    }
}
