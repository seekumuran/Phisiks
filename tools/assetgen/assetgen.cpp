// assetgen: generates the Phisiks sample assets procedurally (deterministic, no inputs).
// Everything it writes is original and released under CC0 (see assets/LICENSE.md).
//
//   assetgen [output-dir=assets]
//     textures/bricks.png       512x512 tileable running-bond bricks
//     textures/metal_plate.png  512x512 tileable brushed steel panels with rivets
//     audio/spawn_chime.wav     two-note chime, 44.1 kHz mono 16-bit
//     audio/impact.wav          short low thud, 44.1 kHz mono 16-bit
//     meshes/torus.obj          torus with normals and UVs (R 0.35 m, r 0.13 m)

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
}

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("assets");
    bool ok = true;
    ok &= Save(Bricks(), out / "textures/bricks.png");
    ok &= Save(MetalPlate(), out / "textures/metal_plate.png");
    ok &= SaveWav(Chime(44100), 44100, out / "audio/spawn_chime.wav");
    ok &= SaveWav(Impact(44100), 44100, out / "audio/impact.wav");
    ok &= Torus(out / "meshes/torus.obj", 0.35, 0.13, 48, 24);
    std::printf("assetgen: %s -> %s\n", ok ? "ok" : "FAILED", out.string().c_str());
    return ok ? 0 : 1;
}
