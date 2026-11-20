#include "audio/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Phisiks::Audio::Wav {
    namespace {
        constexpr uint16_t FORMAT_PCM = 1;
        constexpr uint16_t FORMAT_FLOAT = 3;
        constexpr uint16_t FORMAT_EXTENSIBLE = 0xFFFE;

        uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        bool Fail(std::string* error, const char* message) {
            if (error) *error = message;
            return false;
        }

        void Put16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
        void Put32(std::vector<uint8_t>& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }
        void PutTag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

        std::vector<uint8_t> Encode(const ClipData& clip, uint16_t format, uint16_t bits) {
            const uint32_t dataBytes = uint32_t(clip.samples.size() * (bits / 8));
            std::vector<uint8_t> out;
            out.reserve(44 + dataBytes);
            PutTag(out, "RIFF");
            Put32(out, 36 + dataBytes);
            PutTag(out, "WAVE");
            PutTag(out, "fmt ");
            Put32(out, 16);
            Put16(out, format);
            Put16(out, clip.channels);
            Put32(out, clip.sampleRate);
            Put32(out, clip.sampleRate * clip.channels * (bits / 8));
            Put16(out, uint16_t(clip.channels * (bits / 8)));
            Put16(out, bits);
            PutTag(out, "data");
            Put32(out, dataBytes);
            for (float s : clip.samples) {
                if (format == FORMAT_FLOAT) {
                    uint32_t bitsValue;
                    std::memcpy(&bitsValue, &s, 4);
                    Put32(out, bitsValue);
                }
                else {
                    const float c = std::clamp(s, -1.0f, 1.0f);
                    Put16(out, uint16_t(int16_t(std::lround(c * 32767.0f))));
                }
            }
            return out;
        }
    }

    bool Decode(const uint8_t* data, size_t size, ClipData& out, std::string* error) {
        out = {};
        if (!data || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return Fail(error, "not a RIFF/WAVE file");

        uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
        uint32_t rate = 0;
        const uint8_t* samples = nullptr;
        size_t sampleBytes = 0;
        size_t offset = 12;
        while (offset + 8 <= size) {
            const uint8_t* chunk = data + offset;
            const uint32_t chunkSize = U32(chunk + 4);
            const size_t bodyAvailable = size - offset - 8;
            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                if (chunkSize < 16 || bodyAvailable < 16) return Fail(error, "truncated fmt chunk");
                format = U16(chunk + 8);
                channels = U16(chunk + 10);
                rate = U32(chunk + 12);
                blockAlign = U16(chunk + 20);
                bits = U16(chunk + 22);
                if (format == FORMAT_EXTENSIBLE) {
                    if (chunkSize < 40 || bodyAvailable < 40) return Fail(error, "truncated extensible fmt chunk");
                    format = U16(chunk + 8 + 24);   // first two bytes of the SubFormat GUID
                }
            }
            else if (std::memcmp(chunk, "data", 4) == 0) {
                samples = chunk + 8;
                sampleBytes = std::min<size_t>(chunkSize, bodyAvailable);   // tolerate a truncated last chunk
            }
            offset += 8 + size_t(chunkSize) + (chunkSize & 1u);              // chunks are word aligned
        }
        if (!rate || !channels || !bits) return Fail(error, "missing fmt chunk");
        if (!samples) return Fail(error, "missing data chunk");
        const bool pcm = format == FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        const bool flt = format == FORMAT_FLOAT && (bits == 32 || bits == 64);
        if (!pcm && !flt) return Fail(error, "unsupported sample format");
        const size_t bytesPerSample = bits / 8;
        if (blockAlign != channels * bytesPerSample) return Fail(error, "inconsistent block alignment");

        const size_t count = sampleBytes / bytesPerSample / channels * channels;
        out.sampleRate = rate;
        out.channels = channels;
        out.samples.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = samples + i * bytesPerSample;
            float v = 0.0f;
            if (flt && bits == 32) { float f; std::memcpy(&f, p, 4); v = f; }
            else if (flt) { double d; std::memcpy(&d, p, 8); v = float(d); }
            else if (bits == 8) v = (float(p[0]) - 128.0f) / 128.0f;
            else if (bits == 16) v = float(int16_t(U16(p))) / 32768.0f;
            else if (bits == 24) v = float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0f;
            else v = float(double(int32_t(U32(p))) / 2147483648.0);
            out.samples[i] = v;
        }
        return true;
    }

    std::vector<uint8_t> EncodePcm16(const ClipData& clip) { return Encode(clip, FORMAT_PCM, 16); }
    std::vector<uint8_t> EncodeFloat32(const ClipData& clip) { return Encode(clip, FORMAT_FLOAT, 32); }
}
