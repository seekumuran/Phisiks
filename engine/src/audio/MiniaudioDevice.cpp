#include "audio/MiniaudioDevice.h"
#include "Phisiks/Core/Log.h"
#include "audio/Mixer.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Phisiks::Audio {
    namespace {
        void OnData(ma_device* device, void* output, const void*, ma_uint32 frames) {
            static_cast<Mixer*>(device->pUserData)->Mix(static_cast<float*>(output), frames);
        }
    }

    bool MiniaudioDevice::Open(Mixer& mixer, uint32_t rate) {
        Close();
        m_mixer = &mixer;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = rate;
        config.periodSizeInMilliseconds = 10;
        config.dataCallback = OnData;
        config.pUserData = m_mixer;
        auto* device = new ma_device();
        if (ma_device_init(nullptr, &config, device) != MA_SUCCESS) {
            delete device;
            Log::Warning() << "Audio: miniaudio could not open the default playback device";
            return false;
        }
        if (ma_device_start(device) != MA_SUCCESS) {
            ma_device_uninit(device);
            delete device;
            return false;
        }
        Log::Info() << "Audio: miniaudio device '" << device->playback.name << "' (" << ma_get_backend_name(device->pContext->backend) << ", " << rate << " Hz)";
        m_device = device;
        return true;
    }

    void MiniaudioDevice::Close() {
        if (!m_device) return;
        auto* device = static_cast<ma_device*>(m_device);
        ma_device_uninit(device);
        delete device;
        m_device = nullptr;
    }
}
