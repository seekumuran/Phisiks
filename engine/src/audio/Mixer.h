#pragma once
// Software mixer: any number of voices, each a shared clip played with volume, pitch and looping,
// resampled (linear interpolation) to the output rate and summed into stereo float frames.
// Thread-safe: the device thread calls Mix while the main thread starts/stops voices.

#include "audio/Wav.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Phisiks::Audio {
    class Mixer {
    public:
        explicit Mixer(uint32_t outputRate = 48000) : m_outputRate(outputRate) {}

        void SetOutputRate(uint32_t rate);
        uint32_t OutputRate() const { return m_outputRate; }

        uint64_t Play(std::shared_ptr<const ClipData> clip, float volume, bool loop, float pitch = 1.0f);   // 0 = rejected
        void Stop(uint64_t voice);
        void StopAll();
        bool IsPlaying(uint64_t voice) const;
        void SetVolume(uint64_t voice, float volume);
        void SetMasterVolume(float volume);

        // Overwrites `frames` stereo frames (2 floats each). Finished voices are removed.
        void Mix(float* stereo, size_t frames);
        // Advances voices without producing output (null device / deterministic runs).
        void Advance(size_t frames);

        size_t ActiveVoices() const;
        uint64_t FramesMixed() const;

    private:
        struct Voice {
            uint64_t id;
            std::shared_ptr<const ClipData> clip;
            double position;   // in source frames
            double step;       // source frames per output frame
            float volume;
            bool loop;
        };
        void MixLocked(float* stereo, size_t frames);

        mutable std::mutex m_mutex;
        std::vector<Voice> m_voices;
        uint32_t m_outputRate;
        float m_master = 1.0f;
        uint64_t m_nextId = 1;
        uint64_t m_framesMixed = 0;
    };
}
