#pragma once
// RIFF/WAVE decoding and encoding (no device dependency; unit-tested headless).
// Decodes PCM 8/16/24/32-bit, IEEE float 32/64 and WAVE_FORMAT_EXTENSIBLE into interleaved floats.

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Audio {
    struct ClipData {
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        std::vector<float> samples;   // interleaved, [-1, 1]
        size_t Frames() const { return channels ? samples.size() / channels : 0; }
        double Duration() const { return sampleRate ? double(Frames()) / sampleRate : 0.0; }
    };

    namespace Wav {
        bool Decode(const uint8_t* data, size_t size, ClipData& out, std::string* error = nullptr);
        std::vector<uint8_t> EncodePcm16(const ClipData& clip);
        std::vector<uint8_t> EncodeFloat32(const ClipData& clip);
    }
}
