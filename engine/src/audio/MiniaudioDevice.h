#pragma once
// Engine-private: portable audio output through miniaudio (vendor/miniaudio, public domain / MIT-0).
// Float32 stereo at the mixer rate, pulled by miniaudio's device thread.
#include <cstdint>

namespace Phisiks::Audio {
    class Mixer;

    class MiniaudioDevice {
    public:
        MiniaudioDevice() = default;
        ~MiniaudioDevice() { Close(); }
        MiniaudioDevice(const MiniaudioDevice&) = delete;
        MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;
        bool Open(Mixer& mixer, uint32_t rate);
        void Close();

    private:
        void* m_device = nullptr;   // ma_device
        Mixer* m_mixer = nullptr;
    };
}
