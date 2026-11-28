#pragma once
// Renderer math with no GL dependency (unit-tested headless):
//   primitive meshes, PBR/fog/attenuation reference functions (the GLSL in engine/shaders mirrors
//   these exactly), shadow view fitting, frustum culling, and the built-in 5x7 font.

#include "Phisiks/Render/Renderer.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Phisiks::Render::RenderMath {

    // ------------------------------------------------------------------ primitives (unit sized)
    MeshData MakeCube();                                  // 1 m, 24 vertices (flat faces)
    MeshData MakeSphere(int segments = 48, int rings = 24);   // 1 m diameter
    MeshData MakePlane();                                 // 1x1 m in XZ, normal +Y
    float BoundingRadius(const MeshData& mesh);           // about the local origin

    // ------------------------------------------------------------------ BRDF (Cook-Torrance)
    constexpr float PI = 3.14159265358979f;
    float D_GGX(float NoH, float alpha);                                  // alpha = roughness^2
    float V_SmithGGXCorrelated(float NoV, float NoL, float alpha);        // includes 1/(4 NoV NoL)
    glm::vec3 F_Schlick(float VoH, const glm::vec3& f0);

    // ------------------------------------------------------------------ light falloff
    float DistanceAttenuation(float distance, float range);   // 1/d^2 with a smooth window to 0 at range
    float SpotFactor(float cosAngle, float cosInner, float cosOuter);

    // ------------------------------------------------------------------ fog
    // Optical depth of exponential distance fog + exponential height fog along a ray of
    // `distance` metres from `origin` in unit direction `dir` (closed form of the integral).
    float FogOpticalDepth(const glm::vec3& origin, const glm::vec3& dir, float distance, const FogSettings& fog);
    float FogAmount(const glm::vec3& origin, const glm::vec3& dir, float distance, const FogSettings& fog);   // 1 - transmittance, clamped

    // ------------------------------------------------------------------ views
    std::array<glm::vec3, 8> FrustumCorners(const glm::vec3& position, const glm::quat& rotation,
                                            float verticalFov, float aspect, float nearDist, float farDist);
    // Orthographic light view-projection covering the corners, stable under camera rotation
    // (bounding sphere) and snapped to whole shadow texels (no shimmering when the camera moves).
    // `casterDepth` extends the volume towards the light to include off-screen casters.
    glm::mat4 DirectionalShadowMatrix(const glm::vec3& lightDir, const std::array<glm::vec3, 8>& corners,
                                      int mapSize, float casterDepth = 60.0f);
    glm::mat4 SpotShadowMatrix(const glm::vec3& position, const glm::vec3& direction, float outerConeAngle, float range);
    // Cube faces in GL order (+X, -X, +Y, -Y, +Z, -Z).
    std::array<glm::mat4, 6> PointShadowMatrices(const glm::vec3& position, float nearDist, float range);
    glm::mat4 LookAtDirection(const glm::vec3& eye, const glm::vec3& direction);   // picks a safe up vector

    struct Frustum {
        glm::vec4 planes[6];   // normalized, inside: dot(n, p) + d >= 0
        bool IntersectsSphere(const glm::vec3& center, float radius) const;
    };
    Frustum ExtractFrustum(const glm::mat4& viewProjection);

    // ------------------------------------------------------------------ font
    namespace Font {
        constexpr int GLYPH_W = 5, GLYPH_H = 7;
        constexpr int CELL_W = 6, CELL_H = 8;              // glyph + 1 px spacing
        constexpr int COLUMNS = 16, ROWS = 6;              // ASCII 32..127
        constexpr int ATLAS_W = COLUMNS * CELL_W, ATLAS_H = ROWS * CELL_H;
        constexpr float ADVANCE = 6.0f, LINE_HEIGHT = 10.0f;   // in font pixels
        // Rows of the glyph, bit 4 = leftmost column; unknown characters map to '?'.
        const uint8_t* Glyph(char c);
        std::vector<uint8_t> BuildAtlas();                 // ATLAS_W x ATLAS_H, R8 (0/255), top row first
        glm::vec2 Measure(const std::string& text, float scale);
    }
}
