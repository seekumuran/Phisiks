#include "render/RenderMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Phisiks::Render::RenderMath {

    // ====================================================================== primitives
    MeshData MakeCube() {
        MeshData mesh;
        struct Face { glm::vec3 n, u, v; };   // normal, right, up (right x up = normal)
        const Face faces[6] = {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
            { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
            { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
            { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        };
        for (const Face& f : faces) {
            const uint32_t base = uint32_t(mesh.vertices.size());
            const glm::vec2 corners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            for (const glm::vec2& c : corners) {
                const glm::vec3 p = 0.5f * f.n + (c.x - 0.5f) * f.u + (c.y - 0.5f) * f.v;
                mesh.vertices.push_back({ p, f.n, c });
            }
            for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) mesh.indices.push_back(base + i);
        }
        return mesh;
    }

    MeshData MakeSphere(int segments, int rings) {
        MeshData mesh;
        segments = std::max(segments, 3);
        rings = std::max(rings, 2);
        for (int r = 0; r <= rings; ++r) {
            const float v = float(r) / float(rings);
            const float phi = v * PI;                              // 0 at +Y
            for (int s = 0; s <= segments; ++s) {
                const float u = float(s) / float(segments);
                const float theta = u * 2.0f * PI;
                const glm::vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta));
                mesh.vertices.push_back({ 0.5f * n, n, { u, 1.0f - v } });
            }
        }
        const uint32_t stride = uint32_t(segments + 1);
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < segments; ++s) {
                const uint32_t a = r * stride + s, b = a + stride;
                // counter-clockwise seen from outside
                if (r != 0) { mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(a + 1); }
                if (r != rings - 1) { mesh.indices.push_back(a + 1); mesh.indices.push_back(b); mesh.indices.push_back(b + 1); }
            }
        }
        return mesh;
    }

    MeshData MakePlane() {
        MeshData mesh;
        const glm::vec3 n(0, 1, 0);
        mesh.vertices = {
            { { -0.5f, 0, 0.5f }, n, { 0, 0 } },
            { { 0.5f, 0, 0.5f }, n, { 1, 0 } },
            { { 0.5f, 0, -0.5f }, n, { 1, 1 } },
            { { -0.5f, 0, -0.5f }, n, { 0, 1 } },
        };
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        return mesh;
    }

    float BoundingRadius(const MeshData& mesh) {
        float r = 0.0f;
        for (const Vertex& v : mesh.vertices) r = std::max(r, glm::length(v.position));
        return r;
    }

    // ====================================================================== BRDF
    float D_GGX(float NoH, float alpha) {
        const float a2 = alpha * alpha;
        const float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
        return a2 / (PI * d * d);
    }

    float V_SmithGGXCorrelated(float NoV, float NoL, float alpha) {
        const float a2 = alpha * alpha;
        const float gv = NoL * std::sqrt(NoV * NoV * (1.0f - a2) + a2);
        const float gl = NoV * std::sqrt(NoL * NoL * (1.0f - a2) + a2);
        return 0.5f / std::max(gv + gl, 1e-6f);
    }

    glm::vec3 F_Schlick(float VoH, const glm::vec3& f0) {
        const float f = std::pow(1.0f - std::clamp(VoH, 0.0f, 1.0f), 5.0f);
        return f0 + (glm::vec3(1.0f) - f0) * f;
    }

    // ====================================================================== falloff
    float DistanceAttenuation(float distance, float range) {
        const float d2 = std::max(distance * distance, 1e-4f);
        const float ratio = distance / std::max(range, 1e-4f);
        const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        return window * window / d2;
    }

    float SpotFactor(float cosAngle, float cosInner, float cosOuter) {
        const float t = std::clamp((cosAngle - cosOuter) / std::max(cosInner - cosOuter, 1e-4f), 0.0f, 1.0f);
        return t * t;
    }

    // ====================================================================== fog
    float FogOpticalDepth(const glm::vec3& origin, const glm::vec3& dir, float distance, const FogSettings& fog) {
        if (!fog.enabled || distance <= 0.0f) return 0.0f;
        const float distanceDepth = fog.distanceDensity * distance;
        // integral_0^L hd * exp(-f (o.y + d.y t - base)) dt = hd * exp(-f (o.y - base)) * L * (1 - e^-k) / k,  k = f d.y L
        const float k = fog.heightFalloff * dir.y * distance;
        const float shape = std::fabs(k) > 1e-4f ? (1.0f - std::exp(-k)) / k : 1.0f - 0.5f * k;
        const float heightDepth = fog.heightDensity * std::exp(-fog.heightFalloff * (origin.y - fog.heightBase)) * distance * shape;
        return distanceDepth + heightDepth;
    }

    float FogAmount(const glm::vec3& origin, const glm::vec3& dir, float distance, const FogSettings& fog) {
        return std::min(1.0f - std::exp(-FogOpticalDepth(origin, dir, distance, fog)), fog.maxOpacity);
    }

    // ====================================================================== views
    std::array<glm::vec3, 8> FrustumCorners(const glm::vec3& position, const glm::quat& rotation, float verticalFov, float aspect, float nearDist, float farDist) {
        const glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
        const glm::vec3 right = rotation * glm::vec3(1, 0, 0);
        const glm::vec3 up = rotation * glm::vec3(0, 1, 0);
        const float t = std::tan(verticalFov * 0.5f);
        std::array<glm::vec3, 8> corners;
        int i = 0;
        for (float d : { nearDist, farDist }) {
            const float h = d * t, w = h * aspect;
            const glm::vec3 c = position + forward * d;
            corners[i++] = c - right * w - up * h;
            corners[i++] = c + right * w - up * h;
            corners[i++] = c + right * w + up * h;
            corners[i++] = c - right * w + up * h;
        }
        return corners;
    }

    glm::mat4 LookAtDirection(const glm::vec3& eye, const glm::vec3& direction) {
        const glm::vec3 d = glm::normalize(direction);
        const glm::vec3 up = std::fabs(d.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        return glm::lookAt(eye, eye + d, up);
    }

    glm::mat4 DirectionalShadowMatrix(const glm::vec3& lightDir, const std::array<glm::vec3, 8>& corners, int mapSize, float casterDepth) {
        glm::vec3 center(0.0f);
        for (const glm::vec3& c : corners) center += c;
        center /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& c : corners) radius = std::max(radius, glm::length(c - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantized: size never changes with rotation

        // Light view rotation only (no translation), then snap the centre to whole texels in light space.
        const glm::mat4 rotation = LookAtDirection(glm::vec3(0.0f), lightDir);
        const float texel = 2.0f * radius / float(std::max(mapSize, 1));
        glm::vec3 lightCenter = glm::vec3(rotation * glm::vec4(center, 1.0f));
        lightCenter.x = std::floor(lightCenter.x / texel) * texel;
        lightCenter.y = std::floor(lightCenter.y / texel) * texel;

        // Ortho box in light space: x/y around the snapped centre; z from in front of the slice
        // (towards the light, casterDepth) to behind it. Light view looks down -Z.
        const glm::mat4 projection = glm::ortho(lightCenter.x - radius, lightCenter.x + radius,
                                                lightCenter.y - radius, lightCenter.y + radius,
                                                -(lightCenter.z + radius + casterDepth), -(lightCenter.z - radius));
        return projection * rotation;
    }

    glm::mat4 SpotShadowMatrix(const glm::vec3& position, const glm::vec3& direction, float outerConeAngle, float range) {
        const float fov = std::min(2.0f * outerConeAngle + 0.1f, 3.0f);
        return glm::perspective(fov, 1.0f, 0.05f, std::max(range, 0.1f)) * LookAtDirection(position, direction);
    }

    std::array<glm::mat4, 6> PointShadowMatrices(const glm::vec3& p, float nearDist, float range) {
        const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, nearDist, std::max(range, nearDist * 2.0f));
        return {
            projection * glm::lookAt(p, p + glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)),
            projection * glm::lookAt(p, p + glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),
            projection * glm::lookAt(p, p + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),
            projection * glm::lookAt(p, p + glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)),
            projection * glm::lookAt(p, p + glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)),
            projection * glm::lookAt(p, p + glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)),
        };
    }

    bool Frustum::IntersectsSphere(const glm::vec3& center, float radius) const {
        for (const glm::vec4& p : planes) {
            if (glm::dot(glm::vec3(p), center) + p.w < -radius) return false;
        }
        return true;
    }

    Frustum ExtractFrustum(const glm::mat4& m) {
        // Rows of the matrix (glm is column-major: m[col][row]).
        auto row = [&](int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
        Frustum f;
        f.planes[0] = row(3) + row(0);   // left
        f.planes[1] = row(3) - row(0);   // right
        f.planes[2] = row(3) + row(1);   // bottom
        f.planes[3] = row(3) - row(1);   // top
        f.planes[4] = row(3) + row(2);   // near
        f.planes[5] = row(3) - row(2);   // far
        for (glm::vec4& p : f.planes) p /= glm::length(glm::vec3(p));
        return f;
    }

    // ====================================================================== font
    namespace Font {
        namespace {
            // Original 5x7 glyphs for ASCII 32..126, 7 rows each, bit 4 = leftmost pixel.
            constexpr uint8_t GLYPHS[95][7] = {
                { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // ' '
                { 0x04,0x04,0x04,0x04,0x04,0x00,0x04 }, // !
                { 0x0A,0x0A,0x00,0x00,0x00,0x00,0x00 }, // "
                { 0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00 }, // #
                { 0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04 }, // $
                { 0x18,0x19,0x02,0x04,0x08,0x13,0x03 }, // %
                { 0x08,0x14,0x14,0x08,0x15,0x12,0x0D }, // &
                { 0x04,0x04,0x00,0x00,0x00,0x00,0x00 }, // '
                { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 }, // (
                { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 }, // )
                { 0x00,0x04,0x15,0x0E,0x15,0x04,0x00 }, // *
                { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 }, // +
                { 0x00,0x00,0x00,0x00,0x06,0x04,0x08 }, // ,
                { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 }, // -
                { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C }, // .
                { 0x01,0x01,0x02,0x04,0x08,0x10,0x10 }, // /
                { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }, // 0
                { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, // 1
                { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F }, // 2
                { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E }, // 3
                { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, // 4
                { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E }, // 5
                { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E }, // 6
                { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, // 7
                { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }, // 8
                { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C }, // 9
                { 0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00 }, // :
                { 0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08 }, // ;
                { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 }, // <
                { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 }, // =
                { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 }, // >
                { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 }, // ?
                { 0x0E,0x11,0x17,0x15,0x17,0x10,0x0E }, // @
                { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, // A
                { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, // B
                { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E }, // C
                { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E }, // D
                { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, // E
                { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }, // F
                { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F }, // G
                { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }, // H
                { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }, // I
                { 0x07,0x02,0x02,0x02,0x02,0x12,0x0C }, // J
                { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, // K
                { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }, // L
                { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, // M
                { 0x11,0x11,0x19,0x15,0x13,0x11,0x11 }, // N
                { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, // O
                { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }, // P
                { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, // Q
                { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }, // R
                { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, // S
                { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }, // T
                { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, // U
                { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }, // V
                { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A }, // W
                { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }, // X
                { 0x11,0x11,0x11,0x0A,0x04,0x04,0x04 }, // Y
                { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }, // Z
                { 0x0E,0x08,0x08,0x08,0x08,0x08,0x0E }, // [
                { 0x10,0x10,0x08,0x04,0x02,0x01,0x01 }, // backslash
                { 0x0E,0x02,0x02,0x02,0x02,0x02,0x0E }, // ]
                { 0x04,0x0A,0x11,0x00,0x00,0x00,0x00 }, // ^
                { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F }, // _
                { 0x08,0x04,0x00,0x00,0x00,0x00,0x00 }, // `
                { 0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F }, // a
                { 0x10,0x10,0x16,0x19,0x11,0x11,0x1E }, // b
                { 0x00,0x00,0x0E,0x10,0x10,0x11,0x0E }, // c
                { 0x01,0x01,0x0D,0x13,0x11,0x11,0x0F }, // d
                { 0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E }, // e
                { 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 }, // f
                { 0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E }, // g
                { 0x10,0x10,0x16,0x19,0x11,0x11,0x11 }, // h
                { 0x04,0x00,0x0C,0x04,0x04,0x04,0x0E }, // i
                { 0x02,0x00,0x06,0x02,0x02,0x12,0x0C }, // j
                { 0x10,0x10,0x12,0x14,0x18,0x14,0x12 }, // k
                { 0x0C,0x04,0x04,0x04,0x04,0x04,0x0E }, // l
                { 0x00,0x00,0x1A,0x15,0x15,0x11,0x11 }, // m
                { 0x00,0x00,0x16,0x19,0x11,0x11,0x11 }, // n
                { 0x00,0x00,0x0E,0x11,0x11,0x11,0x0E }, // o
                { 0x00,0x00,0x1E,0x11,0x1E,0x10,0x10 }, // p
                { 0x00,0x00,0x0D,0x13,0x0F,0x01,0x01 }, // q
                { 0x00,0x00,0x16,0x19,0x10,0x10,0x10 }, // r
                { 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E }, // s
                { 0x08,0x08,0x1C,0x08,0x08,0x09,0x06 }, // t
                { 0x00,0x00,0x11,0x11,0x11,0x13,0x0D }, // u
                { 0x00,0x00,0x11,0x11,0x11,0x0A,0x04 }, // v
                { 0x00,0x00,0x11,0x11,0x15,0x15,0x0A }, // w
                { 0x00,0x00,0x11,0x0A,0x04,0x0A,0x11 }, // x
                { 0x00,0x00,0x11,0x11,0x0F,0x01,0x0E }, // y
                { 0x00,0x00,0x1F,0x02,0x04,0x08,0x1F }, // z
                { 0x02,0x04,0x04,0x08,0x04,0x04,0x02 }, // {
                { 0x04,0x04,0x04,0x04,0x04,0x04,0x04 }, // |
                { 0x08,0x04,0x04,0x02,0x04,0x04,0x08 }, // }
                { 0x00,0x00,0x08,0x15,0x02,0x00,0x00 }, // ~
            };
        }

        const uint8_t* Glyph(char c) {
            const int code = static_cast<unsigned char>(c);
            if (code < 32 || code > 126) return GLYPHS['?' - 32];
            return GLYPHS[code - 32];
        }

        std::vector<uint8_t> BuildAtlas() {
            std::vector<uint8_t> pixels(ATLAS_W * ATLAS_H, 0);
            for (int code = 32; code < 127; ++code) {
                const int cell = code - 32;
                const int cx = (cell % COLUMNS) * CELL_W, cy = (cell / COLUMNS) * CELL_H;
                const uint8_t* glyph = Glyph(char(code));
                for (int y = 0; y < GLYPH_H; ++y) {
                    for (int x = 0; x < GLYPH_W; ++x) {
                        if (glyph[y] & (0x10 >> x)) pixels[(cy + y) * ATLAS_W + cx + x] = 255;
                    }
                }
            }
            return pixels;
        }

        glm::vec2 Measure(const std::string& text, float scale) {
            float width = 0.0f, line = 0.0f;
            int lines = text.empty() ? 0 : 1;
            for (char c : text) {
                if (c == '\n') { width = std::max(width, line); line = 0.0f; ++lines; continue; }
                line += ADVANCE;
            }
            width = std::max(width, line);
            return glm::vec2(width, lines * LINE_HEIGHT) * scale;
        }
    }
}
