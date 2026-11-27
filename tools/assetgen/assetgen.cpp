// assetgen: generates the Phisiks sample assets procedurally (deterministic, no inputs).
// Everything it writes is original and released under CC0 (see assets/LICENSE.md).
//
//   assetgen [output-dir=assets]
//     textures/bricks.png       512x512 tileable running-bond bricks
//     textures/metal_plate.png  512x512 tileable brushed steel panels with rivets
//     audio/spawn_chime.wav     two-note chime, 44.1 kHz mono 16-bit
//     audio/impact.wav          short low thud, 44.1 kHz mono 16-bit
//     meshes/torus.obj          torus with normals and UVs (R 0.35 m, r 0.13 m)
//     audio/drone_hum.wav       2 s seamless hum loop (spatial-audio demo), 44.1 kHz mono 16-bit
//     models/walker.glb         skinned 4-bone tube, embedded PNG, clips "Twist" (linear) / "Sway" (cubic)
//     audio/engine_loop.wav     1 s seamless engine rumble loop (stunt-car showcase), 44.1 kHz mono 16-bit
//     audio/siren.wav           2 s seamless two-tone siren loop (guard bots), 44.1 kHz mono 16-bit
//     meshes/hero/*.obj         P11 hero showcase meshes (robot arm, turbine assembly, pool basin,
//                               studio cyclorama, frame arrow): see HeroMeshes.cpp
//     meshes/marina/*.obj       Project Marina engineering assembly parts and the tubing / wiring
//                               kit (PhisiksLab "ProjectMarina" scene): see MarinaMeshes.cpp
//     meshes/oldmcdonald/*.obj  Old-McDonald greenhouse, mapping drone and soil rover parts
//                               (PhisiksLab "OldMcDonald" scene): see OldMcDonaldMeshes.cpp

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

bool HeroMeshes(const fs::path& out);     // HeroMeshes.cpp
bool MarinaMeshes(const fs::path& out);   // MarinaMeshes.cpp
bool OldMcDonaldMeshes(const fs::path& out);   // OldMcDonaldMeshes.cpp

namespace {
    constexpr double PI = 3.14159265358979323846;

    // ------------------------------------------------------------------ noise (tileable)
    uint32_t Hash(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return x;
    }
    double Rand(int x, int y, int seed) { return Hash(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(seed) * 83492791u) / 4294967295.0; }

    // Value noise on a lattice of `period` cells that wraps (tileable across the texture).
    double Noise(double x, double y, int period, int seed) {
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        auto w = [&](int v) { return ((v % period) + period) % period; };
        const double a = Rand(w(x0), w(y0), seed), b = Rand(w(x0 + 1), w(y0), seed);
        const double c = Rand(w(x0), w(y0 + 1), seed), d = Rand(w(x0 + 1), w(y0 + 1), seed);
        const double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
        return (a + (b - a) * ux) * (1 - uy) + (c + (d - c) * ux) * uy;
    }
    double Fbm(double x, double y, int period, int seed, int octaves = 4) {
        double sum = 0, amp = 0.5;
        for (int i = 0; i < octaves; ++i) {
            sum += amp * Noise(x, y, period, seed + i);
            x *= 2; y *= 2; period *= 2; amp *= 0.5;
        }
        return sum;
    }

    struct Image {
        int w, h;
        std::vector<uint8_t> px;
        Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 255) {}
        void Set(int x, int y, double r, double g, double b) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(std::clamp(r, 0.0, 255.0));
            p[1] = uint8_t(std::clamp(g, 0.0, 255.0));
            p[2] = uint8_t(std::clamp(b, 0.0, 255.0));
            p[3] = 255;
        }
    };

    bool Save(const Image& image, const fs::path& path) {
        fs::create_directories(path.parent_path());
        return stbi_write_png(path.string().c_str(), image.w, image.h, 4, image.px.data(), image.w * 4) != 0;
    }

    // ------------------------------------------------------------------ textures
    Image Bricks() {
        const int size = 512, rows = 8, perRow = 4;
        const double brickH = double(size) / rows, brickW = double(size) / perRow, mortar = 5.0;
        Image img(size, size);
        for (int y = 0; y < size; ++y) {
            const int row = int(y / brickH);
            const double offset = (row % 2) ? brickW * 0.5 : 0.0;
            for (int x = 0; x < size; ++x) {
                const double bx = std::fmod(x + offset, double(size));
                const int col = int(bx / brickW);
                const double lx = bx - col * brickW, ly = y - row * brickH;
                const double edge = std::min({ lx, brickW - lx, ly, brickH - ly });
                const double n = Fbm(x / 32.0, y / 32.0, size / 32, 11);
                if (edge < mortar * 0.5) {
                    const double m = 160 + 30 * (n - 0.5);
                    img.Set(x, y, m, m * 0.97, m * 0.9);
                    continue;
                }
                const double tone = 0.8 + 0.4 * Rand(col, row, 3);          // per-brick variation
                const double speckle = 0.85 + 0.3 * Fbm(x / 6.0, y / 6.0, size / 6 + 1, 5);
                const double bevel = std::clamp(edge / 6.0, 0.6, 1.0);       // darker near the mortar
                const double k = tone * speckle * bevel;
                img.Set(x, y, 152 * k, 64 * k, 46 * k);
            }
        }
        return img;
    }

    Image MetalPlate() {
        const int size = 512, panel = 256;
        Image img(size, size);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const double brushed = Fbm(x / 64.0, y / 1.5, size / 64, 21, 3);   // streaks along X
                double v = 120 + 50 * brushed;
                const int lx = x % panel, ly = y % panel;
                const int seam = std::min({ lx, panel - 1 - lx, ly, panel - 1 - ly });
                if (seam < 2) v *= 0.45;                                          // panel gap
                else if (seam < 5) v *= 1.12;                                     // bevel highlight
                // Rivets 18 px in from each panel corner
                for (int cy : { 18, panel - 18 }) {
                    for (int cx : { 18, panel - 18 }) {
                        const double dx = lx - cx, dy = ly - cy, d = std::sqrt(dx * dx + dy * dy);
                        if (d < 7.0) v = 95 + 110 * std::clamp(1.0 - (dx + dy + 7.0) / 14.0, 0.0, 1.0);   // lit from top-left
                    }
                }
                img.Set(x, y, v * 0.96, v, v * 1.04);
            }
        }
        return img;
    }

    // ------------------------------------------------------------------ audio
    bool SaveWav(const std::vector<double>& samples, int rate, const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        const uint32_t bytes = uint32_t(samples.size() * 2);
        f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
        f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
        f.write("data", 4); u32(bytes);
        for (double s : samples) u16(uint16_t(int16_t(std::lround(std::clamp(s, -1.0, 1.0) * 32767.0))));
        return bool(f);
    }

    std::vector<double> Chime(int rate) {
        std::vector<double> out(size_t(rate * 0.8), 0.0);
        auto note = [&](double start, double freq, double amp) {
            for (size_t i = size_t(start * rate); i < out.size(); ++i) {
                const double t = double(i) / rate - start;
                const double env = std::min(t / 0.004, 1.0) * std::exp(-t / 0.16);
                out[i] += amp * env * (std::sin(2 * PI * freq * t) + 0.3 * std::sin(2 * PI * freq * 2.0 * t));
            }
        };
        note(0.0, 880.0, 0.32);
        note(0.11, 1318.5, 0.28);   // a fifth above
        return out;
    }

    std::vector<double> Impact(int rate) {
        std::vector<double> out(size_t(rate * 0.3), 0.0);
        double lowpass = 0.0;
        for (size_t i = 0; i < out.size(); ++i) {
            const double t = double(i) / rate;
            const double noise = Rand(int(i), 7, 9) * 2 - 1;
            lowpass += (noise - lowpass) * 0.08;
            out[i] = 0.7 * std::exp(-t / 0.06) * std::sin(2 * PI * 70.0 * t) + 0.5 * std::exp(-t / 0.02) * lowpass;
        }
        return out;
    }

    // ------------------------------------------------------------------ mesh
    bool Torus(const fs::path& path, double R, double r, int major, int minor) {
        fs::create_directories(path.parent_path());
        std::ofstream f(path);
        f << "# Torus generated by tools/assetgen (CC0)\n";
        char line[160];
        for (int i = 0; i <= major; ++i) {
            const double u = 2 * PI * i / major;
            for (int j = 0; j <= minor; ++j) {
                const double v = 2 * PI * j / minor;
                const double cx = std::cos(u), cz = std::sin(u);
                const double nx = std::cos(v) * cx, ny = std::sin(v), nz = std::cos(v) * cz;
                std::snprintf(line, sizeof(line), "v %.6f %.6f %.6f\nvn %.6f %.6f %.6f\nvt %.6f %.6f\n",
                              (R + r * std::cos(v)) * cx, r * std::sin(v), (R + r * std::cos(v)) * cz, nx, ny, nz, double(i) / major, double(j) / minor);
                f << line;
            }
        }
        const int stride = minor + 1;
        for (int i = 0; i < major; ++i) {
            for (int j = 0; j < minor; ++j) {
                const int a = i * stride + j + 1, b = (i + 1) * stride + j + 1;
                // counter-clockwise seen from outside
                std::snprintf(line, sizeof(line), "f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d\n", a, a, a, a + 1, a + 1, a + 1, b + 1, b + 1, b + 1, b, b, b);
                f << line;
            }
        }
        return bool(f);
    }

    // ------------------------------------------------------------------ audio (loop)
    // 2 s seamless hum (whole periods only) for the spatial-audio demo: 110 Hz + harmonics + slow beat.
    std::vector<double> DroneHum(int rate) {
        std::vector<double> out(size_t(rate) * 2, 0.0);
        for (size_t i = 0; i < out.size(); ++i) {
            const double t = double(i) / rate;
            const double beat = 0.8 + 0.2 * std::sin(2 * PI * 1.0 * t);
            out[i] = beat * (0.45 * std::sin(2 * PI * 110.0 * t) + 0.25 * std::sin(2 * PI * 220.0 * t) + 0.12 * std::sin(2 * PI * 330.0 * t)
                             + 0.06 * std::sin(2 * PI * 550.0 * t));
        }
        return out;
    }

    // Engine: 55 Hz firing fundamental with odd/even harmonics and a 6 Hz cylinder "chug"; every
    // component completes whole cycles in 1 s, so the loop is seamless.
    std::vector<double> EngineLoop(int rate) {
        std::vector<double> out(size_t(rate), 0.0);
        for (size_t i = 0; i < out.size(); ++i) {
            const double t = double(i) / rate;
            const double chug = 0.75 + 0.25 * std::sin(2 * PI * 6.0 * t);
            double v = 0.0;
            for (int h = 1; h <= 8; ++h) v += std::sin(2 * PI * 55.0 * h * t + 0.7 * h) / (h * (h % 2 ? 1.0 : 1.6));
            out[i] = 0.42 * chug * v;
        }
        return out;
    }

    // Siren: 650 Hz / 850 Hz alternating every 0.5 s (whole cycles per half: phase-continuous).
    std::vector<double> Siren(int rate) {
        std::vector<double> out(size_t(rate) * 2, 0.0);
        double phase = 0.0;
        for (size_t i = 0; i < out.size(); ++i) {
            const double t = double(i) / rate;
            const double f = std::fmod(t, 1.0) < 0.5 ? 650.0 : 850.0;
            phase += 2 * PI * f / rate;
            out[i] = 0.35 * (std::sin(phase) + 0.3 * std::sin(2.0 * phase) + 0.12 * std::sin(3.0 * phase));
        }
        return out;
    }

    // ------------------------------------------------------------------ glTF 2.0 binary (GLB)
    // models/walker.glb: a 2 m segmented tube skinned to a 4-bone chain, with an embedded PNG
    // (stripes) and two clips: "Twist" (LINEAR: each bone twists 70 deg about Y and sways) and
    // "Sway" (CUBICSPLINE bend about X). Written to exercise the importer and dual-quaternion skinning.
    struct Blob {
        std::vector<uint8_t> bytes;
        size_t Add(const void* data, size_t size) {
            while (bytes.size() % 4) bytes.push_back(0);
            const size_t offset = bytes.size();
            const uint8_t* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
            return offset;
        }
    };

    void PngToMemory(void* context, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out->insert(out->end(), p, p + size);
    }

    bool WalkerGlb(const fs::path& path) {
        constexpr int RINGS = 40, SIDES = 24, BONES = 4;
        constexpr double HEIGHT = 2.0, RADIUS = 0.25, SEGMENT = HEIGHT / BONES;
        std::vector<float> positions, normals, uvs, weights;
        std::vector<uint16_t> joints;
        std::vector<uint32_t> indices;
        for (int r = 0; r <= RINGS; ++r) {
            const double y = HEIGHT * r / RINGS;
            const double b = std::min(y / SEGMENT, double(BONES) - 1e-9);
            const int bone = std::min(int(std::floor(b - 0.5 < 0 ? 0 : b - 0.5)), BONES - 1);
            // blend between the bone below and above the segment centre
            const double centre = (bone + 0.5) * SEGMENT;
            double f = (y - centre) / SEGMENT;
            int j0 = bone, j1 = std::min(bone + 1, BONES - 1);
            if (f < 0) { j1 = j0; f = 0; }
            f = std::clamp(f, 0.0, 1.0);
            const double smooth = f * f * (3 - 2 * f);
            for (int s = 0; s <= SIDES; ++s) {
                const double a = 2 * PI * s / SIDES;
                positions.insert(positions.end(), { float(RADIUS * std::cos(a)), float(y), float(RADIUS * std::sin(a)) });
                normals.insert(normals.end(), { float(std::cos(a)), 0.0f, float(std::sin(a)) });
                uvs.insert(uvs.end(), { float(double(s) / SIDES), float(1.0 - y / HEIGHT) });
                joints.insert(joints.end(), { uint16_t(j0), uint16_t(j1), 0, 0 });
                weights.insert(weights.end(), { float(1.0 - smooth), float(smooth), 0.0f, 0.0f });
            }
        }
        for (int r = 0; r < RINGS; ++r)
            for (int s = 0; s < SIDES; ++s) {
                const uint32_t a = uint32_t(r * (SIDES + 1) + s), b = a + SIDES + 1;
                indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });   // counter-clockwise from outside
            }
        const size_t vertexCount = positions.size() / 3;
        // inverse bind matrices (column-major): bone i at y = i * SEGMENT
        std::vector<float> inverseBind;
        for (int i = 0; i < BONES; ++i) {
            const float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, float(-i * SEGMENT), 0, 1 };
            inverseBind.insert(inverseBind.end(), m, m + 16);
        }
        // clip "Twist" (linear): 9 keys over 2 s
        std::vector<float> twistTimes;
        std::vector<std::vector<float>> twistRot(BONES);
        for (int k = 0; k <= 8; ++k) {
            const double t = 2.0 * k / 8;
            twistTimes.push_back(float(t));
            const double twist = (70.0 * PI / 180.0) * std::sin(2 * PI * t / 2.0);
            const double sway = (12.0 * PI / 180.0) * std::sin(2 * PI * t / 2.0 + 0.6);
            for (int b = 0; b < BONES; ++b) {
                // q = qz(sway) * qy(twist)
                const double cy = std::cos(twist * 0.5), sy = std::sin(twist * 0.5), cz = std::cos(sway * 0.5), sz = std::sin(sway * 0.5);
                const double x = -sz * sy, y = cz * sy, z = sz * cy, w = cz * cy;
                twistRot[b].insert(twistRot[b].end(), { float(x), float(y), float(z), float(w) });
            }
        }
        // clip "Sway" (cubic spline, in-tangent / value / out-tangent per key): bend about X
        std::vector<float> swayTimes = { 0.0f, 1.0f, 2.0f };
        std::vector<float> swayRot;
        const double angles[3] = { 0.0, 25.0 * PI / 180.0, 0.0 };
        for (int k = 0; k < 3; ++k) {
            const double h = angles[k] * 0.5;
            const float zero[4] = { 0, 0, 0, 0 };
            const float value[4] = { float(std::sin(h)), 0.0f, 0.0f, float(std::cos(h)) };
            swayRot.insert(swayRot.end(), zero, zero + 4);
            swayRot.insert(swayRot.end(), value, value + 4);
            swayRot.insert(swayRot.end(), zero, zero + 4);
        }
        // texture: horizontal stripes (rings) with a darker seam
        Image tex(128, 128);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const bool stripe = (y / 16) % 2 == 0;
                const double shade = x < 4 ? 0.6 : 1.0;
                tex.Set(x, y, (stripe ? 230 : 60) * shade, (stripe ? 150 : 170) * shade, (stripe ? 40 : 220) * shade);
            }
        std::vector<uint8_t> png;
        stbi_write_png_to_func(PngToMemory, &png, tex.w, tex.h, 4, tex.px.data(), tex.w * 4);

        Blob bin;
        struct View { size_t offset, length; };
        std::vector<View> views;
        auto view = [&](const void* data, size_t size) { views.push_back({ bin.Add(data, size), size }); return int(views.size() - 1); };
        const int vPos = view(positions.data(), positions.size() * 4), vNor = view(normals.data(), normals.size() * 4), vUv = view(uvs.data(), uvs.size() * 4);
        const int vJoint = view(joints.data(), joints.size() * 2), vWeight = view(weights.data(), weights.size() * 4), vIndex = view(indices.data(), indices.size() * 4);
        const int vIbm = view(inverseBind.data(), inverseBind.size() * 4), vTwistT = view(twistTimes.data(), twistTimes.size() * 4);
        std::vector<int> vTwistR;
        for (int b = 0; b < BONES; ++b) vTwistR.push_back(view(twistRot[b].data(), twistRot[b].size() * 4));
        const int vSwayT = view(swayTimes.data(), swayTimes.size() * 4), vSwayR = view(swayRot.data(), swayRot.size() * 4);
        const int vPng = view(png.data(), png.size());
        while (bin.bytes.size() % 4) bin.bytes.push_back(0);

        std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Phisiks assetgen (CC0)\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
        json += "\"nodes\":[{\"name\":\"Walker\",\"children\":[1,5]},";
        json += "{\"name\":\"Bone0\",\"children\":[2]},{\"name\":\"Bone1\",\"translation\":[0,0.5,0],\"children\":[3]},";
        json += "{\"name\":\"Bone2\",\"translation\":[0,0.5,0],\"children\":[4]},{\"name\":\"Bone3\",\"translation\":[0,0.5,0]},";
        json += "{\"name\":\"Body\",\"mesh\":0,\"skin\":0}],";
        json += "\"skins\":[{\"name\":\"Spine\",\"joints\":[1,2,3,4],\"inverseBindMatrices\":6}],";
        json += "\"meshes\":[{\"name\":\"Body\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5,\"material\":0}]}],";
        json += "\"materials\":[{\"name\":\"Stripes\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0.0,\"roughnessFactor\":0.55,\"baseColorTexture\":{\"index\":0}}}],";
        json += "\"textures\":[{\"source\":0}],\"images\":[{\"name\":\"stripes\",\"mimeType\":\"image/png\",\"bufferView\":" + std::to_string(vPng) + "}],";
        char text[512];
        std::string accessors = "\"accessors\":[";
        auto accessor = [&](int bufferView, int componentType, size_t count, const char* type, const std::string& extra = std::string()) {
            std::snprintf(text, sizeof(text), "%s{\"bufferView\":%d,\"componentType\":%d,\"count\":%zu,\"type\":\"%s\"%s}", accessors.back() == '[' ? "" : ",",
                          bufferView, componentType, count, type, extra.c_str());
            accessors += text;
        };
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (size_t i = 0; i < vertexCount; ++i) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], positions[i * 3 + k]); hi[k] = std::max(hi[k], positions[i * 3 + k]); }
        std::snprintf(text, sizeof(text), ",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        accessor(vPos, 5126, vertexCount, "VEC3", text);                 // 0
        accessor(vNor, 5126, vertexCount, "VEC3");                       // 1
        accessor(vUv, 5126, vertexCount, "VEC2");                        // 2
        accessor(vJoint, 5123, vertexCount, "VEC4");                     // 3
        accessor(vWeight, 5126, vertexCount, "VEC4");                    // 4
        accessor(vIndex, 5125, indices.size(), "SCALAR");                // 5
        accessor(vIbm, 5126, BONES, "MAT4");                             // 6
        accessor(vTwistT, 5126, twistTimes.size(), "SCALAR", ",\"min\":[0],\"max\":[2]");   // 7
        for (int b = 0; b < BONES; ++b) accessor(vTwistR[b], 5126, twistTimes.size(), "VEC4");   // 8..11
        accessor(vSwayT, 5126, swayTimes.size(), "SCALAR", ",\"min\":[0],\"max\":[2]");     // 12
        accessor(vSwayR, 5126, swayRot.size() / 4, "VEC4");             // 13
        accessors += "],";
        json += accessors;
        json += "\"animations\":[{\"name\":\"Twist\",\"samplers\":[";
        for (int b = 0; b < BONES; ++b) { std::snprintf(text, sizeof(text), "%s{\"input\":7,\"output\":%d,\"interpolation\":\"LINEAR\"}", b ? "," : "", 8 + b); json += text; }
        json += "],\"channels\":[";
        for (int b = 0; b < BONES; ++b) { std::snprintf(text, sizeof(text), "%s{\"sampler\":%d,\"target\":{\"node\":%d,\"path\":\"rotation\"}}", b ? "," : "", b, 1 + b); json += text; }
        json += "]},{\"name\":\"Sway\",\"samplers\":[{\"input\":12,\"output\":13,\"interpolation\":\"CUBICSPLINE\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}],";
        json += "\"bufferViews\":[";
        for (size_t i = 0; i < views.size(); ++i) { std::snprintf(text, sizeof(text), "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", i ? "," : "", views[i].offset, views[i].length); json += text; }
        json += "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.bytes.size()) + "}]}";
        while (json.size() % 4) json += ' ';

        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        u32(0x46546C67u); u32(2); u32(uint32_t(12 + 8 + json.size() + 8 + bin.bytes.size()));
        u32(uint32_t(json.size())); u32(0x4E4F534Au); f.write(json.data(), std::streamsize(json.size()));
        u32(uint32_t(bin.bytes.size())); u32(0x004E4942u); f.write(reinterpret_cast<const char*>(bin.bytes.data()), std::streamsize(bin.bytes.size()));
        return bool(f);
    }
}

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("assets");
    bool ok = true;
    ok &= Save(Bricks(), out / "textures/bricks.png");
    ok &= Save(MetalPlate(), out / "textures/metal_plate.png");
    ok &= SaveWav(Chime(44100), 44100, out / "audio/spawn_chime.wav");
    ok &= SaveWav(Impact(44100), 44100, out / "audio/impact.wav");
    ok &= Torus(out / "meshes/torus.obj", 0.35, 0.13, 48, 24);
    ok &= SaveWav(DroneHum(44100), 44100, out / "audio/drone_hum.wav");
    ok &= WalkerGlb(out / "models/walker.glb");
    ok &= SaveWav(EngineLoop(44100), 44100, out / "audio/engine_loop.wav");
    ok &= SaveWav(Siren(44100), 44100, out / "audio/siren.wav");
    ok &= HeroMeshes(out);
    ok &= MarinaMeshes(out);
    ok &= OldMcDonaldMeshes(out);
    std::printf("assetgen: %s -> %s\n", ok ? "ok" : "FAILED", out.string().c_str());
    return ok ? 0 : 1;
}
