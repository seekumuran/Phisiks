#pragma once
// Phisiks::Audio: clean-room audio output (WASAPI shared mode on Windows) with a software mixer.
// Clips (WAV) load on first use through Phisiks::Assets and stay cached. Without an output device
// (headless runs, no audio hardware) a null device advances voices in time so behaviour is the same.

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
