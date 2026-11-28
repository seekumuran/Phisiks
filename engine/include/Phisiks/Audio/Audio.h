#pragma once
// Phisiks::Audio: clean-room audio output (WASAPI shared mode on Windows) with a software mixer.
// Clips (WAV) load on first use through Phisiks::Assets and stay cached. Without an output device
// (headless runs, no audio hardware) a null device advances voices in time so behaviour is the same.

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace Phisiks::Audio {
    using VoiceId = uint64_t;   // 0 = invalid

    struct InitOptions {
        bool useDevice = true;          // false: null device (deterministic / headless)
        uint32_t sampleRate = 48000;    // mix rate (the OS converts to the device format)
    };
    bool Init(const InitOptions& options = InitOptions());
    void Shutdown();
    void Update(float deltaSeconds);    // once per frame (drives the null device)

    VoiceId Play(const std::string& clipPath, float volume = 1.0f, bool loop = false, float pitch = 1.0f);
    void Stop(VoiceId voice);
    void StopAll();
    bool IsPlaying(VoiceId voice);
    void SetVolume(VoiceId voice, float volume);
    void SetMasterVolume(float volume);
    bool Preload(const std::string& clipPath);   // decode now instead of on first Play

    // 3D voices: equal-power stereo panning relative to the listener, inverse-square distance
    // attenuation (amplitude minDistance / d, silent at maxDistance) and Doppler pitch shift.
    VoiceId PlaySpatial(const std::string& clipPath, const glm::vec3& position, float volume = 1.0f, bool loop = false,
                        float minDistance = 1.0f, float maxDistance = 80.0f, float dopplerFactor = 1.0f);
    void SetVoicePosition(VoiceId voice, const glm::vec3& position, const glm::vec3& velocity = glm::vec3(0.0f));
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity = glm::vec3(0.0f));
    struct SpatialInfo {
        float gainLeft = 0.0f, gainRight = 0.0f, distanceGain = 0.0f, pan = 0.0f, dopplerPitch = 1.0f, distance = 0.0f;
    };
    bool GetSpatialInfo(VoiceId voice, SpatialInfo& out);   // what the next mixed block uses (tests / UI)
    double GetClipDuration(const std::string& clipPath);   // seconds, 0 if it cannot load

    struct Stats {
        bool deviceOpen = false;        // false = null device
        uint32_t sampleRate = 0;
        uint32_t activeVoices = 0;
        uint32_t clipsLoaded = 0;
        uint32_t clipLoadFailures = 0;
        uint64_t framesMixed = 0;
    };
    Stats GetStats();
}
