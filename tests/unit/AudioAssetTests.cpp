// Headless tests for Phisiks::Audio (WAV codec, mixer) and Phisiks::Assets (paths, images, OBJ).
// No audio device or GL context: the mixer is driven directly / through the null device.
#include "TestFramework.h"

#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Audio/Audio.h"
#include "audio/Mixer.h"
#include "audio/Wav.h"

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Phisiks;
using namespace Phisiks::Audio;

namespace {
    std::shared_ptr<ClipData> Sine(double freq, uint32_t rate, double seconds, uint16_t channels = 1, float amplitude = 0.5f) {
        auto clip = std::make_shared<ClipData>();
        clip->sampleRate = rate;
        clip->channels = channels;
        const size_t frames = size_t(rate * seconds);
        for (size_t i = 0; i < frames; ++i) {
            const float v = amplitude * float(std::sin(2.0 * glm::pi<double>() * freq * double(i) / rate));
            for (uint16_t c = 0; c < channels; ++c) clip->samples.push_back(c == 0 ? v : -v);
        }
        return clip;
    }

    // Upward zero crossings of the left channel -> frequency estimate.
    double MeasureFrequency(const std::vector<float>& stereo, uint32_t rate) {
        int crossings = 0;
        size_t first = 0, last = 0;
        for (size_t i = 1; i < stereo.size() / 2; ++i) {
            if (stereo[(i - 1) * 2] < 0.0f && stereo[i * 2] >= 0.0f) {
                if (crossings == 0) first = i;
                last = i;
                ++crossings;
            }
        }
        return crossings > 1 ? (crossings - 1) * double(rate) / double(last - first) : 0.0;
    }

    std::filesystem::path TempDir(const char* name) {
        auto dir = std::filesystem::temp_directory_path() / "phisiks_audio_asset_tests" / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }
}

// ============================================================================ WAV
TEST_CASE("Wav: PCM16 and float32 round trips") {
    auto clip = Sine(440.0, 22050, 0.05, 2);
    for (bool useFloat : { false, true }) {
        const std::vector<uint8_t> bytes = useFloat ? Wav::EncodeFloat32(*clip) : Wav::EncodePcm16(*clip);
        ClipData decoded;
        std::string error;
        CHECK(Wav::Decode(bytes.data(), bytes.size(), decoded, &error));
        CHECK(decoded.sampleRate == 22050 && decoded.channels == 2 && decoded.Frames() == clip->Frames());
        double maxError = 0.0;
        for (size_t i = 0; i < clip->samples.size(); ++i) maxError = std::max(maxError, double(std::abs(decoded.samples[i] - clip->samples[i])));
        CHECK(maxError < (useFloat ? 1e-7 : 1.0 / 32767.0 + 1e-6));
        CHECK_NEAR(decoded.Duration(), double(clip->Frames()) / 22050.0, 1e-9);
    }
}

TEST_CASE("Wav: 8/24/32-bit PCM, extensible header, odd chunks") {
    // Hand-built file: mono 8-bit PCM with an odd-sized LIST chunk before "data".
    std::vector<uint8_t> b = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                               'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x40,0x1F,0,0, 0x40,0x1F,0,0, 1,0, 8,0,
                               'L','I','S','T', 3,0,0,0, 'a','b','c', 0,
                               'd','a','t','a', 3,0,0,0, 0, 128, 255 };
    ClipData c;
    CHECK(Wav::Decode(b.data(), b.size(), c));
    CHECK(c.sampleRate == 8000 && c.Frames() == 3);
    CHECK_NEAR(c.samples[0], -1.0, 1e-6);
    CHECK_NEAR(c.samples[1], 0.0, 1e-6);
    CHECK_NEAR(c.samples[2], 127.0 / 128.0, 1e-6);

    // 24-bit extensible stereo: one frame (+0.5, -0.5)
    std::vector<uint8_t> e = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                               'f','m','t',' ', 40,0,0,0, 0xFE,0xFF, 2,0, 0x80,0xBB,0,0, 0,0,0,0, 6,0, 24,0,
                               22,0, 24,0, 3,0,0,0, 1,0,0,0, 0,0,0x10,0, 0x80,0,0,0xAA,0,0x38,0x9B,0x71,
                               'd','a','t','a', 6,0,0,0, 0,0,0x40, 0,0,0xC0 };
    CHECK(Wav::Decode(e.data(), e.size(), c));
    CHECK(c.channels == 2 && c.Frames() == 1);
    CHECK_NEAR(c.samples[0], 0.5, 1e-6);
    CHECK_NEAR(c.samples[1], -0.5, 1e-6);
}

TEST_CASE("Wav: malformed files are rejected with a reason") {
    std::string error;
    ClipData c;
    const uint8_t junk[] = { 'R','I','F','F', 0,0,0,0, 'A','V','I',' ' };
    CHECK(!Wav::Decode(junk, sizeof(junk), c, &error) && !error.empty());
    auto clip = Sine(100.0, 8000, 0.01);
    std::vector<uint8_t> bytes = Wav::EncodePcm16(*clip);
    bytes[20] = 2;   // format tag 2 (ADPCM): unsupported
    CHECK(!Wav::Decode(bytes.data(), bytes.size(), c, &error) && error.find("unsupported") != std::string::npos);
    std::vector<uint8_t> noData(bytes.begin(), bytes.begin() + 36);
    CHECK(!Wav::Decode(noData.data(), noData.size(), c, &error));
}

// ============================================================================ mixer
TEST_CASE("Mixer: resamples to the output rate and honours pitch") {
    Mixer mixer(48000);
    const uint64_t voice = mixer.Play(Sine(1000.0, 44100, 0.5), 1.0f, false);
    CHECK(voice != 0);
    std::vector<float> out(48000 / 4 * 2);
    mixer.Mix(out.data(), out.size() / 2);
    CHECK_NEAR(MeasureFrequency(out, 48000), 1000.0, 2.0);

    Mixer fast(48000);
    fast.Play(Sine(500.0, 48000, 1.0), 1.0f, false, 2.0f);   // pitch x2 -> 1 kHz
    fast.Mix(out.data(), out.size() / 2);
    CHECK_NEAR(MeasureFrequency(out, 48000), 1000.0, 2.0);
}

TEST_CASE("Mixer: volume, master volume, mono to both channels, stereo kept, sum clipped") {
    auto dc = std::make_shared<ClipData>();
    dc->sampleRate = 48000;
    dc->channels = 1;
    dc->samples.assign(4800, 0.4f);
    Mixer mixer(48000);
    mixer.Play(dc, 0.5f, false);
    float frame[2 * 16];
    mixer.Mix(frame, 16);
    CHECK_NEAR(frame[0], 0.2, 1e-6);
    CHECK_NEAR(frame[1], 0.2, 1e-6);
    mixer.SetMasterVolume(0.5f);
    mixer.Mix(frame, 16);
    CHECK_NEAR(frame[0], 0.1, 1e-6);

    Mixer loud(48000);
    for (int i = 0; i < 4; ++i) loud.Play(dc, 1.0f, false);   // 4 x 0.4 = 1.6 -> clipped
    loud.Mix(frame, 16);
    CHECK(frame[0] == 1.0f);

    Mixer stereo(48000);
    stereo.Play(Sine(1000.0, 48000, 0.1, 2), 1.0f, false);
    std::vector<float> out(200);
    stereo.Mix(out.data(), 100);
    CHECK_NEAR(out[20], -out[21], 1e-6);   // right channel is the inverted left
}

TEST_CASE("Mixer: one-shot voices end, looping voices wrap, stop and ids") {
    Mixer mixer(1000);
    auto clip = Sine(10.0, 1000, 0.1);   // 100 frames
    const uint64_t once = mixer.Play(clip, 1.0f, false);
    const uint64_t loop = mixer.Play(clip, 1.0f, true);
    CHECK(once != loop && mixer.ActiveVoices() == 2);
    std::vector<float> out(2 * 60);
    mixer.Mix(out.data(), 60);
    CHECK(mixer.IsPlaying(once));
    mixer.Mix(out.data(), 60);                      // 120 frames > 100
    CHECK(!mixer.IsPlaying(once));
    CHECK(mixer.IsPlaying(loop));
    mixer.Advance(10000);
    CHECK(mixer.IsPlaying(loop));
    mixer.Stop(loop);
    CHECK(!mixer.IsPlaying(loop) && mixer.ActiveVoices() == 0);
    CHECK(mixer.FramesMixed() == 120 + 10000);
    CHECK(mixer.Play(nullptr, 1.0f, false) == 0);
}

TEST_CASE("Audio: null device plays clips from files and finishes them in time") {
    const auto dir = TempDir("clips");
    auto clip = Sine(440.0, 48000, 0.25);
    const std::vector<uint8_t> bytes = Wav::EncodePcm16(*clip);
    std::ofstream(dir / "beep.wav", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Assets::AddSearchPath(dir.string());

    Audio::Init(Audio::InitOptions{ false });
    CHECK(!Audio::GetStats().deviceOpen);
    const VoiceId voice = Audio::Play("beep.wav", 0.8f);
    CHECK(voice != 0 && Audio::IsPlaying(voice));
    CHECK_NEAR(Audio::GetClipDuration("beep.wav"), 0.25, 1e-6);
    Audio::Update(0.2f);
    CHECK(Audio::IsPlaying(voice));
    Audio::Update(0.1f);                           // 0.3 s > 0.25 s
    CHECK(!Audio::IsPlaying(voice));
    CHECK(Audio::Play("missing.wav") == 0);
    CHECK(Audio::GetStats().clipsLoaded == 1 && Audio::GetStats().clipLoadFailures == 1);
    Audio::Shutdown();
}

// ============================================================================ assets
TEST_CASE("Assets: resolve through search paths, read bytes, async image decode") {
    const auto dir = TempDir("images");
    Assets::Image img;
    img.width = 4;
    img.height = 2;
    for (int i = 0; i < 8; ++i) img.rgba.insert(img.rgba.end(), { uint8_t(i * 30), 10, 200, 255 });
    CHECK(Assets::WritePng((dir / "tex" / "small.png").string(), img));
    Assets::AddSearchPath(dir.string());
    CHECK(!Assets::Resolve("tex/small.png").empty());
    CHECK(Assets::Resolve("tex/nope.png").empty());

    Assets::Image sync;
    CHECK(Assets::LoadImageFile("tex/small.png", sync));
    CHECK(sync.width == 4 && sync.height == 2 && sync.rgba == img.rgba);

    const Assets::Ticket ok = Assets::RequestImage("tex/small.png");
    const Assets::Ticket bad = Assets::RequestImage("tex/nope.png");
    Assets::Image out;
    Assets::Status s1 = Assets::Status::Pending, s2 = Assets::Status::Pending;
    for (int i = 0; i < 500 && (s1 == Assets::Status::Pending || s2 == Assets::Status::Pending); ++i) {
        if (s1 == Assets::Status::Pending) s1 = Assets::PollImage(ok, out);
        Assets::Image ignored;
        if (s2 == Assets::Status::Pending) s2 = Assets::PollImage(bad, ignored);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(s1 == Assets::Status::Ready && out.rgba == img.rgba);
    CHECK(s2 == Assets::Status::Failed);
    CHECK(Assets::PollImage(ok, out) == Assets::Status::Failed);   // results are handed out once
    Assets::Shutdown();
}

TEST_CASE("Assets: bilinear resize keeps constant images and corners") {
    Assets::Image in;
    in.width = in.height = 2;
    in.rgba = { 0,0,0,255, 255,255,255,255, 0,0,0,255, 255,255,255,255 };
    Assets::Image out;
    Assets::ResizeImage(in, 4, 4, out);
    CHECK(out.width == 4 && out.rgba.size() == 64);
    CHECK(out.rgba[0] == 0 && out.rgba[3 * 4] == 255);       // left edge black, right edge white
    CHECK(out.rgba[1 * 4] > 0 && out.rgba[1 * 4] < 255);     // interpolated in between
}

TEST_CASE("Assets: OBJ parser (quads, relative indices, missing normals, errors)") {
    Render::MeshData mesh;
    std::string error;
    const char* quad =
        "# unit quad facing +Y\n"
        "v -0.5 0 0.5\nv 0.5 0 0.5\nv 0.5 0 -0.5\nv -0.5 0 -0.5\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "f 1/1 2/2 3/3 4/4\n";
    CHECK(Assets::ParseObj(quad, mesh, &error));
    CHECK(mesh.vertices.size() == 4 && mesh.indices.size() == 6);
    for (const Render::Vertex& v : mesh.vertices) CHECK_NEAR(v.normal.y, 1.0, 1e-6);   // generated, facing +Y
    CHECK_NEAR(mesh.vertices[2].uv.x, 1.0, 1e-6);

    const char* relative = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf -3//-1 -2//-1 -1//-1\n";
    CHECK(Assets::ParseObj(relative, mesh, &error));
    CHECK(mesh.indices.size() == 3 && mesh.vertices[0].normal.z == 1.0f);

    CHECK(!Assets::ParseObj("v 0 0 0\nf 1 2 3\n", mesh, &error) && error.find("range") != std::string::npos);
    CHECK(!Assets::ParseObj("v 0 0 0\n", mesh, &error) && error.find("no faces") != std::string::npos);
    CHECK(!Assets::ParseObj("v 0 0\nf 1 1 1\n", mesh, &error));
}
