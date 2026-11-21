#pragma once
// CPU reference of the renderer's spatial upscaler (FSR 1.0 algorithm: EASU edge-adaptive
// upsampling + RCAS robust contrast-adaptive sharpening; clean-room implementation, the GLSL in
// engine/shaders/easu.frag.glsl / rcas.frag.glsl mirrors it operation for operation).
// Images are RGBA8, rows bottom-up (OpenGL order); alpha is written as 255.

#include <cstdint>
#include <vector>

namespace Phisiks::Render::Fsr {
    void Easu(const std::vector<uint8_t>& in, int width, int height, int outWidth, int outHeight, std::vector<uint8_t>& out);
    void Rcas(const std::vector<uint8_t>& in, int width, int height, float sharpnessStops, std::vector<uint8_t>& out);
    // Bilinear reference (texture filtering of the same image), for comparisons.
    void Bilinear(const std::vector<uint8_t>& in, int width, int height, int outWidth, int outHeight, std::vector<uint8_t>& out);
    // Peak signal-to-noise ratio of the RGB channels (dB; 99 if identical).
    double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
}
