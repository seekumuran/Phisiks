#pragma once
// Software mixer: any number of voices, each a shared clip played with volume, pitch and looping,
// resampled (linear interpolation) to the output rate and summed into stereo float frames.
// Spatial voices are mono-summed and placed relative to a listener: equal-power stereo panning,
// inverse-square distance attenuation (amplitude ~ minDistance / d, i.e. intensity ~ 1/d^2) with a
// smooth fade to silence at maxDistance, and a Doppler pitch shift from the source and listener
// velocities along the line between them. Gains and pitch are ramped across each mixed block so
// moving sources do not click.
// Thread-safe: the device thread calls Mix while the main thread starts/stops/moves voices.

#include "audio/Wav.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Phisiks::Audio {
    struct SpatialVoiceState {
        float gainLeft = 0.0f, gainRight = 0.0f;   // equal-power pan x distance gain (x voice volume x master)
        float distanceGain = 0.0f;
        float pan = 0.0f;                          // -1 left .. +1 right
        float dopplerPitch = 1.0f;
        float distance = 0.0f;
    };

    class Mixer {
    public:
        explicit Mixer(uint32_t outputRate = 48000) : m_outputRate(outputRate) {}

        void SetOutputRate(uint32_t rate);
        uint32_t OutputRate() const { return m_outputRate; }

        uint64_t Play(std::shared_ptr<const ClipData> clip, float volume, bool loop, float pitch = 1.0f);   // 0 = rejected
        uint64_t PlaySpatial(std::shared_ptr<const ClipData> clip, float volume, bool loop, const glm::vec3& position,
                             float minDistance, float maxDistance, float dopplerFactor);
        void SetVoicePosition(uint64_t voice, const glm::vec3& position, const glm::vec3& velocity);
        void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity);
        void SetSpeedOfSound(float metresPerSecond);
        bool GetSpatialState(uint64_t voice, SpatialVoiceState& out) const;   // gains the next block will use

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
            double step;       // source frames per output frame (pitch 1)
            float volume;
            bool loop;
            // spatial
            bool spatial = false;
            glm::vec3 sourcePosition{ 0.0f }, sourceVelocity{ 0.0f };
            float minDistance = 1.0f, maxDistance = 80.0f, doppler = 1.0f;
            float lastLeft = -1.0f, lastRight = -1.0f, lastPitch = 1.0f;   // ramp start (-1: none yet)
        };
        SpatialVoiceState Evaluate(const Voice& v) const;
        void MixLocked(float* stereo, size_t frames);

        mutable std::mutex m_mutex;
        std::vector<Voice> m_voices;
        uint32_t m_outputRate;
        float m_master = 1.0f;
        uint64_t m_nextId = 1;
        uint64_t m_framesMixed = 0;
        glm::vec3 m_listenerPosition{ 0.0f }, m_listenerVelocity{ 0.0f };
        glm::vec3 m_listenerRight{ 1.0f, 0.0f, 0.0f };
        float m_speedOfSound = 343.0f;
    };
}
