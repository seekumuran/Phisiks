// CPU reference of the spatial upscaler (see Phisiks/Render/Upscaler.h). Mirrors
// engine/shaders/easu.frag.glsl and rcas.frag.glsl operation for operation.
#include "Phisiks/Render/Upscaler.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks::Render::Fsr {
    namespace {
        struct Image {
            const std::vector<uint8_t>& data;
            int w, h;
            glm::vec3 At(int x, int y) const {
                x = std::clamp(x, 0, w - 1);
                y = std::clamp(y, 0, h - 1);
                const uint8_t* p = data.data() + (size_t(y) * w + x) * 4;
                return glm::vec3(p[0], p[1], p[2]) / 255.0f;
            }
        };
        float Luma(const glm::vec3& c) { return c.g + 0.5f * (c.r + c.b); }
        uint8_t ToByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
        void Store(std::vector<uint8_t>& out, size_t index, const glm::vec3& c) {
            out[index * 4 + 0] = ToByte(c.r);
            out[index * 4 + 1] = ToByte(c.g);
            out[index * 4 + 2] = ToByte(c.b);
            out[index * 4 + 3] = 255;
        }

        void Accumulate(glm::vec2 offset, glm::vec2 dir, glm::vec2 len2, float lob, float clp, const glm::vec3& c, glm::vec3& acc, float& wsum) {
            const glm::vec2 v = glm::vec2(glm::dot(offset, dir), glm::dot(offset, glm::vec2(-dir.y, dir.x))) * len2;
            const float d2 = std::min(glm::dot(v, v), clp);
            float wB = 0.4f * d2 - 1.0f;
            float wA = lob * d2 - 1.0f;
            wB *= wB;
            wA *= wA;
            wB = 1.5625f * wB - 0.5625f;
            const float w = wB * wA;
            acc += c * w;
            wsum += w;
        }

        void Analyse(glm::vec2& dir, float& len, float w, float lL, float lR, float lD, float lU, float lC) {
            const float dx = lR - lL, dy = lU - lD;
            const float ex = std::clamp(std::fabs(dx) / std::max(std::max(std::fabs(lR - lC), std::fabs(lC - lL)), 1.0f / 65536.0f), 0.0f, 1.0f);
            const float ey = std::clamp(std::fabs(dy) / std::max(std::max(std::fabs(lU - lC), std::fabs(lC - lD)), 1.0f / 65536.0f), 0.0f, 1.0f);
            dir += glm::vec2(dx, dy) * w;
            len += (ex * ex + ey * ey) * w;
        }
    }

    void Easu(const std::vector<uint8_t>& in, int width, int height, int outWidth, int outHeight, std::vector<uint8_t>& out) {
        out.assign(size_t(outWidth) * outHeight * 4, 0);
        const Image img{ in, width, height };
        const glm::vec2 scale = glm::vec2(float(width), float(height)) / glm::vec2(float(outWidth), float(outHeight));
        for (int oy = 0; oy < outHeight; ++oy) {
            for (int ox = 0; ox < outWidth; ++ox) {
                const glm::vec2 pp = glm::vec2(float(ox) + 0.5f, float(oy) + 0.5f) * scale - 0.5f;
                const glm::vec2 fp = glm::floor(pp);
                const glm::vec2 f = pp - fp;
                const int bx = int(fp.x), by = int(fp.y);
                const glm::vec3 b = img.At(bx, by - 1), c = img.At(bx + 1, by - 1);
                const glm::vec3 e = img.At(bx - 1, by), ff = img.At(bx, by), g = img.At(bx + 1, by), hh = img.At(bx + 2, by);
                const glm::vec3 i = img.At(bx - 1, by + 1), j = img.At(bx, by + 1), k = img.At(bx + 1, by + 1), l = img.At(bx + 2, by + 1);
                const glm::vec3 n = img.At(bx, by + 2), o = img.At(bx + 1, by + 2);
                const float lb = Luma(b), lc = Luma(c), le = Luma(e), lf = Luma(ff), lg = Luma(g), lh = Luma(hh);
                const float li = Luma(i), lj = Luma(j), lk = Luma(k), ll = Luma(l), ln = Luma(n), lo = Luma(o);

                glm::vec2 dir(0.0f);
                float len = 0.0f;
                Analyse(dir, len, (1.0f - f.x) * (1.0f - f.y), le, lg, lb, lj, lf);
                Analyse(dir, len, f.x * (1.0f - f.y), lf, lh, lc, lk, lg);
                Analyse(dir, len, (1.0f - f.x) * f.y, li, lk, lf, ln, lj);
                Analyse(dir, len, f.x * f.y, lj, ll, lg, lo, lk);
                const float dir2 = glm::dot(dir, dir);
                dir = dir2 < 1.0f / 32768.0f ? glm::vec2(1.0f, 0.0f) : dir / std::sqrt(dir2);
                len = 0.5f * len;
                len *= len;
                const float stretch = 1.0f / std::max(std::fabs(dir.x), std::fabs(dir.y));
                const glm::vec2 len2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
                const float lob = 0.5f + (0.25f - 0.04f - 0.5f) * len;
                const float clp = 1.0f / lob;

                glm::vec3 acc(0.0f);
                float wsum = 0.0f;
                Accumulate(glm::vec2(0, -1) - f, dir, len2, lob, clp, b, acc, wsum);
                Accumulate(glm::vec2(1, -1) - f, dir, len2, lob, clp, c, acc, wsum);
                Accumulate(glm::vec2(-1, 0) - f, dir, len2, lob, clp, e, acc, wsum);
                Accumulate(glm::vec2(0, 0) - f, dir, len2, lob, clp, ff, acc, wsum);
                Accumulate(glm::vec2(1, 0) - f, dir, len2, lob, clp, g, acc, wsum);
                Accumulate(glm::vec2(2, 0) - f, dir, len2, lob, clp, hh, acc, wsum);
                Accumulate(glm::vec2(-1, 1) - f, dir, len2, lob, clp, i, acc, wsum);
                Accumulate(glm::vec2(0, 1) - f, dir, len2, lob, clp, j, acc, wsum);
                Accumulate(glm::vec2(1, 1) - f, dir, len2, lob, clp, k, acc, wsum);
                Accumulate(glm::vec2(2, 1) - f, dir, len2, lob, clp, l, acc, wsum);
                Accumulate(glm::vec2(0, 2) - f, dir, len2, lob, clp, n, acc, wsum);
                Accumulate(glm::vec2(1, 2) - f, dir, len2, lob, clp, o, acc, wsum);
                const glm::vec3 lo4 = glm::min(glm::min(ff, g), glm::min(j, k));
                const glm::vec3 hi4 = glm::max(glm::max(ff, g), glm::max(j, k));
                Store(out, size_t(oy) * outWidth + ox, glm::clamp(acc / wsum, lo4, hi4));
            }
        }
    }

    void Rcas(const std::vector<uint8_t>& in, int width, int height, float sharpnessStops, std::vector<uint8_t>& out) {
        out.assign(size_t(width) * height * 4, 0);
        const Image img{ in, width, height };
        const float sharpness = std::exp2(-sharpnessStops);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const glm::vec3 cN = img.At(x, y + 1), cW = img.At(x - 1, y), cC = img.At(x, y), cE = img.At(x + 1, y), cS = img.At(x, y - 1);
                const glm::vec3 mn4 = glm::min(glm::min(cN, cS), glm::min(cW, cE));
                const glm::vec3 mx4 = glm::max(glm::max(cN, cS), glm::max(cW, cE));
                const glm::vec3 hitMin = mn4 / glm::max(4.0f * mx4, glm::vec3(1.0f / 65536.0f));
                const glm::vec3 hitMax = (glm::vec3(1.0f) - mx4) / glm::min(4.0f * mn4 - 4.0f, glm::vec3(-1.0f / 65536.0f));
                const glm::vec3 lobeRGB = glm::max(-hitMin, hitMax);
                float lobe = std::max(-(0.25f - 1.0f / 16.0f), std::min(std::max(lobeRGB.r, std::max(lobeRGB.g, lobeRGB.b)), 0.0f)) * sharpness;
                const float lN = Luma(cN), lW = Luma(cW), lC = Luma(cC), lE = Luma(cE), lS = Luma(cS);
                const float range = std::max({ lN, lW, lE, lS, lC }) - std::min({ lN, lW, lE, lS, lC });
                const float nz = std::clamp(std::fabs(0.25f * (lN + lW + lE + lS) - lC) / std::max(range, 1.0f / 65536.0f), 0.0f, 1.0f);
                lobe *= 1.0f - 0.5f * nz;
                Store(out, size_t(y) * width + x, glm::clamp((lobe * (cN + cW + cE + cS) + cC) / (4.0f * lobe + 1.0f), 0.0f, 1.0f));
            }
        }
    }

    void Bilinear(const std::vector<uint8_t>& in, int width, int height, int outWidth, int outHeight, std::vector<uint8_t>& out) {
        out.assign(size_t(outWidth) * outHeight * 4, 0);
        const Image img{ in, width, height };
        const glm::vec2 scale = glm::vec2(float(width), float(height)) / glm::vec2(float(outWidth), float(outHeight));
        for (int oy = 0; oy < outHeight; ++oy) {
            for (int ox = 0; ox < outWidth; ++ox) {
                const glm::vec2 pp = glm::vec2(float(ox) + 0.5f, float(oy) + 0.5f) * scale - 0.5f;
                const glm::vec2 fp = glm::floor(pp);
                const glm::vec2 f = pp - fp;
                const int x = int(fp.x), y = int(fp.y);
                const glm::vec3 c = glm::mix(glm::mix(img.At(x, y), img.At(x + 1, y), f.x), glm::mix(img.At(x, y + 1), img.At(x + 1, y + 1), f.x), f.y);
                Store(out, size_t(oy) * outWidth + ox, c);
            }
        }
    }

    double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size() || a.empty()) return 0.0;
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                const double d = double(a[i + c]) - double(b[i + c]);
                sum += d * d;
                ++count;
            }
        }
        const double mse = sum / double(count);
        return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }
}
