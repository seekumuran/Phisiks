// Phisiks::Audio: clip cache + mixer + output device. Device backends (chosen at build time):
//   WASAPI     Windows default, event-driven shared mode (written against the Windows Core Audio docs)
//   miniaudio  macOS / Linux (CoreAudio, PulseAudio, ALSA, JACK), optional on Windows; audio/MiniaudioDevice.cpp
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "audio/Mixer.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <unordered_map>

#if defined(PHISIKS_AUDIO_MINIAUDIO)
#include "audio/MiniaudioDevice.h"
#elif defined(_WIN32)
#define PHISIKS_AUDIO_WASAPI 1
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace Phisiks::Audio {
    namespace {
#if defined(PHISIKS_AUDIO_MINIAUDIO)
        using Device = MiniaudioDevice;
#elif defined(PHISIKS_AUDIO_WASAPI)
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid linking ksuser.
        const GUID SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

        // WASAPI render thread. The mixer is fed in float32 stereo at the mix rate; the audio engine
        // converts to the device format (AUTOCONVERTPCM).
        class Device {
        public:
            bool Open(Mixer& mixer, uint32_t rate) {
                m_mixer = &mixer;
                m_rate = rate;
                std::atomic<int> result{ 0 };   // 0 pending, 1 ok, -1 failed
                m_thread = std::thread([this, &result] { Run(result); });
                while (result.load() == 0) std::this_thread::yield();
                if (result.load() < 0) {
                    m_thread.join();
                    return false;
                }
                return true;
            }

            void Close() {
                if (!m_thread.joinable()) return;
                m_stop = true;
                if (m_event) SetEvent(m_event);
                m_thread.join();
            }

        private:
            void Run(std::atomic<int>& result) {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                IMMDeviceEnumerator* enumerator = nullptr;
                IMMDevice* device = nullptr;
                IAudioClient* client = nullptr;
                IAudioRenderClient* render = nullptr;
                bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)));
                ok = ok && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
                ok = ok && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)));

                WAVEFORMATEXTENSIBLE format{};
                format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                format.Format.nChannels = 2;
                format.Format.nSamplesPerSec = m_rate;
                format.Format.wBitsPerSample = 32;
                format.Format.nBlockAlign = 8;
                format.Format.nAvgBytesPerSec = m_rate * 8;
                format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                format.Samples.wValidBitsPerSample = 32;
                format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                format.SubFormat = SUBTYPE_IEEE_FLOAT;
                const REFERENCE_TIME bufferDuration = 400000;   // 40 ms in 100 ns units
                ok = ok && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                        bufferDuration, 0, &format.Format, nullptr));
                if (ok) m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                ok = ok && m_event && SUCCEEDED(client->SetEventHandle(m_event));
                UINT32 bufferFrames = 0;
                ok = ok && SUCCEEDED(client->GetBufferSize(&bufferFrames));
                ok = ok && SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)));
                if (ok) {
                    BYTE* data = nullptr;
                    if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) {
                        m_mixer->Mix(reinterpret_cast<float*>(data), bufferFrames);
                        render->ReleaseBuffer(bufferFrames, 0);
                    }
                    ok = SUCCEEDED(client->Start());
                }
                result = ok ? 1 : -1;

                while (ok && !m_stop) {
                    WaitForSingleObject(m_event, 200);
                    if (m_stop) break;
                    UINT32 padding = 0;
                    if (FAILED(client->GetCurrentPadding(&padding))) break;   // device lost
                    const UINT32 available = bufferFrames - padding;
                    if (available == 0) continue;
                    BYTE* data = nullptr;
                    if (FAILED(render->GetBuffer(available, &data))) break;
                    m_mixer->Mix(reinterpret_cast<float*>(data), available);
                    render->ReleaseBuffer(available, 0);
                }

                if (client) client->Stop();
                if (render) render->Release();
                if (client) client->Release();
                if (device) device->Release();
                if (enumerator) enumerator->Release();
                if (m_event) { CloseHandle(m_event); m_event = nullptr; }
                CoUninitialize();
            }

            Mixer* m_mixer = nullptr;
            uint32_t m_rate = 48000;
            std::thread m_thread;
            std::atomic<bool> m_stop{ false };
            HANDLE m_event = nullptr;
        };
#else
        class Device {
        public:
            bool Open(Mixer&, uint32_t) { return false; }
            void Close() {}
        };
#endif

        struct State {
            Mixer mixer;
            std::unique_ptr<Device> device;
            bool deviceOpen = false;
            double nullFrameDebt = 0.0;
            std::unordered_map<std::string, std::shared_ptr<const ClipData>> clips;
            uint32_t loadFailures = 0;
        };
        std::unique_ptr<State> g;

        State& Get() {
            if (!g) Init(InitOptions{ false });   // lazily: null device
            return *g;
        }

        std::shared_ptr<const ClipData> LoadClip(const std::string& path) {
            State& s = Get();
            auto it = s.clips.find(path);
            if (it != s.clips.end()) return it->second;
            std::vector<uint8_t> bytes;
            auto clip = std::make_shared<ClipData>();
            std::string error;
            if (!Assets::ReadBytes(path, bytes)) error = "file not found";
            else if (!Wav::Decode(bytes.data(), bytes.size(), *clip, &error)) {}
            if (!error.empty()) {
                Log::Error() << "Audio: cannot load '" << path << "': " << error;
                ++s.loadFailures;
                s.clips[path] = nullptr;   // do not retry every frame
                return nullptr;
            }
            Log::Info() << "Audio: loaded '" << path << "' (" << clip->sampleRate << " Hz, " << clip->channels << " ch, "
                        << int(clip->Duration() * 1000.0) << " ms)";
            s.clips[path] = clip;
            return clip;
        }
    }

    bool Init(const InitOptions& options) {
        if (g) return true;
        g = std::make_unique<State>();
        g->mixer.SetOutputRate(options.sampleRate);
        if (options.useDevice) {
            g->device = std::make_unique<Device>();
            g->deviceOpen = g->device->Open(g->mixer, options.sampleRate);
            if (!g->deviceOpen) g->device.reset();
        }
        Log::Info() << "Audio: " << (g->deviceOpen ? "WASAPI output open" : "null output (no device)") << ", mixing at " << options.sampleRate << " Hz";
        return true;
    }

    void Shutdown() {
        if (!g) return;
        if (g->device) g->device->Close();
        g.reset();
    }

    void Update(float deltaSeconds) {
        if (!g || g->deviceOpen) return;
        // Null device: consume output time so voices finish when they would have on a device.
        g->nullFrameDebt += double(std::max(deltaSeconds, 0.0f)) * g->mixer.OutputRate();
        const size_t frames = size_t(g->nullFrameDebt);
        g->nullFrameDebt -= double(frames);
        if (frames) g->mixer.Advance(frames);
    }

    VoiceId Play(const std::string& clipPath, float volume, bool loop, float pitch) {
        auto clip = LoadClip(clipPath);
        return clip ? Get().mixer.Play(clip, volume, loop, pitch) : 0;
    }
    void Stop(VoiceId voice) { if (g) g->mixer.Stop(voice); }
    void StopAll() { if (g) g->mixer.StopAll(); }
    bool IsPlaying(VoiceId voice) { return g && voice != 0 && g->mixer.IsPlaying(voice); }
    void SetVolume(VoiceId voice, float volume) { if (g) g->mixer.SetVolume(voice, volume); }
    void SetMasterVolume(float volume) { Get().mixer.SetMasterVolume(volume); }

    VoiceId PlaySpatial(const std::string& clipPath, const glm::vec3& position, float volume, bool loop, float minDistance, float maxDistance, float dopplerFactor) {
        auto clip = LoadClip(clipPath);
        return clip ? Get().mixer.PlaySpatial(clip, volume, loop, position, minDistance, maxDistance, dopplerFactor) : 0;
    }
    void SetVoicePosition(VoiceId voice, const glm::vec3& position, const glm::vec3& velocity) { if (g) g->mixer.SetVoicePosition(voice, position, velocity); }
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity) {
        Get().mixer.SetListener(position, forward, up, velocity);
    }
    bool GetSpatialInfo(VoiceId voice, SpatialInfo& out) {
        SpatialVoiceState s;
        if (!g || !g->mixer.GetSpatialState(voice, s)) return false;
        out = SpatialInfo{ s.gainLeft, s.gainRight, s.distanceGain, s.pan, s.dopplerPitch, s.distance };
        return true;
    }
    bool Preload(const std::string& clipPath) { return LoadClip(clipPath) != nullptr; }
    double GetClipDuration(const std::string& clipPath) {
        auto clip = LoadClip(clipPath);
        return clip ? clip->Duration() : 0.0;
    }

    Stats GetStats() {
        Stats s;
        if (!g) return s;
        s.deviceOpen = g->deviceOpen;
        s.sampleRate = g->mixer.OutputRate();
        s.activeVoices = uint32_t(g->mixer.ActiveVoices());
        for (const auto& [path, clip] : g->clips) if (clip) ++s.clipsLoaded;
        s.clipLoadFailures = g->loadFailures;
        s.framesMixed = g->mixer.FramesMixed();
        return s;
    }
}
