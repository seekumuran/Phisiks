#pragma once
// Phisiks::Render: forward PBR renderer (OpenGL 4.6).
//
// Scenes feed it automatically (MeshRenderer, Light, Camera components). Games use this header to
// register meshes and materials, tune the environment (exposure, ambient, fog, shadows) and draw
// debug lines / overlay text. Everything here must be called on the main (GL) thread.

#include "Phisiks/Render/RenderStats.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Render {

    // ---------------------------------------------------------------- environment
    // Defaults: the dark "studio" look (charcoal backdrop, soft hemisphere ambient, cool rim light),
    // which keeps HUD text, lighting and shadows high-contrast.
    struct FogSettings {
        bool enabled = true;
        glm::vec3 color{ 0.030f, 0.033f, 0.042f };   // linear; matches the backdrop
        float distanceDensity = 0.012f;              // per metre (exponential distance fog)
        float heightDensity = 0.0f;                  // per metre at heightBase (exponential height fog)
        float heightFalloff = 0.35f;                 // per metre (density halves every ln2/falloff m)
        float heightBase = 0.0f;                     // world Y of the densest layer
        float maxOpacity = 0.97f;
    };

    struct RenderSettings {
        float exposure = 1.0f;
        glm::vec3 skyColorZenith{ 0.010f, 0.011f, 0.016f };   // backdrop (linear)
        glm::vec3 skyColorHorizon{ 0.040f, 0.044f, 0.055f };
        glm::vec3 groundColor{ 0.015f, 0.015f, 0.018f };      // backdrop below the horizon
        glm::vec3 ambientUp{ 0.11f, 0.12f, 0.15f };           // hemisphere ambient from above
        glm::vec3 ambientDown{ 0.025f, 0.025f, 0.03f };       // and from below
        float ambientIntensity = 1.0f;
        glm::vec3 rimColor{ 0.45f, 0.55f, 0.80f };            // rim light on vertical surfaces
        float rimIntensity = 0.35f;
        float rimPower = 3.0f;
        FogSettings fog;
        bool shadowsEnabled = true;
        float shadowDistance = 40.0f;      // directional shadow coverage from the camera (m)
        int shadowMapSize = 2048;          // directional; spot/point use half of it
        int msaaSamples = 4;               // 1 = off; applied when the swap-chain size changes or on Init
        glm::vec4 clearColor{ 0.012f, 0.013f, 0.018f, 1.0f };
    };
    RenderSettings& Settings();

    // ---------------------------------------------------------------- materials
    // Parametric PBR materials with optional procedural surface patterns and an optional albedo
    // texture (loaded on demand). Patterns and textures are mapped tri-planar in object space
    // scaled to metres, times MeshRenderer::uvScale.
    enum class Pattern : uint32_t {
        None = 0,
        Checker = 1,          // two-colour checker, 1 cell per metre
        Grid = 2,             // lines at integer coordinates (lineWidth), 10x10 sub-grid
        Noise = 3,            // value noise blend between baseColor and patternColor
        Wood = 4,             // ring/grain bands
        Tiles = 5,            // tiles with grout (patternColor) of lineWidth
        CalibrationGrid = 6,  // dark tiles (baseColor), rainbow column colours, labels A0..J9, lines (patternColor)
    };

    struct MaterialDesc {
        glm::vec3 baseColor{ 0.8f };       // linear albedo
        float roughness = 0.6f;
        float metallic = 0.0f;
        glm::vec3 emissive{ 0.0f };        // linear radiance added after lighting
        Pattern pattern = Pattern::None;
        glm::vec3 patternColor{ 0.2f };
        float patternScale = 1.0f;         // pattern repeats per metre
        float lineWidth = 0.04f;           // Grid/Tiles/CalibrationGrid
        float patternRoughness = -1.0f;    // roughness inside the pattern (-1 = same)
        float patternGlow = 0.0f;          // self-illumination of pattern labels/lines (readable in the dark)
        std::string albedoTexture;         // asset path (e.g. "textures/bricks.png"); multiplies the albedo
        float textureScale = 1.0f;         // texture repeats per metre
    };
    bool RegisterMaterial(const std::string& name, const MaterialDesc& desc);   // replaces an existing one
    bool HasMaterial(const std::string& name);
    const MaterialDesc* FindMaterial(const std::string& name);

    // ---------------------------------------------------------------- meshes
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
    };
    struct MeshData {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;     // triangle list, counter-clockwise front faces
    };
    // Built-in unit meshes: "Cube" (1 m), "Sphere" (1 m diameter), "Plane" (1x1 m XZ, +Y up).
    bool RegisterMesh(const std::string& name, const MeshData& data);
    bool HasMesh(const std::string& name);

    // ---------------------------------------------------------------- debug drawing (this frame only)
    void DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);
    void DrawAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color);
    void DrawCross(const glm::vec3& p, float size, const glm::vec4& color);

    // Overlay text in framebuffer pixels from the top-left corner (built-in 5x7 font, ASCII).
    void DrawScreenText(const std::string& text, glm::vec2 position, const glm::vec4& color = glm::vec4(1.0f), float scale = 2.0f);
    glm::vec2 MeasureText(const std::string& text, float scale = 2.0f);
    glm::ivec2 GetFramebufferSize();

    // ---------------------------------------------------------------- verification / tooling
    // Reads back the most recently rendered frame (call after the frame was rendered, before the
    // next one). `rgba` is the final tonemapped image, `entityIds` holds Entity::Id() + 1 per pixel
    // (0 = background). Rows are bottom-up (OpenGL order). Stalls the GPU: tests and tools only.
    struct FrameCapture {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
        std::vector<uint32_t> entityIds;
    };
    bool CaptureFrame(FrameCapture& out);

    struct ShaderCacheStats {
        uint32_t programs = 0;       // programs the renderer uses
        uint32_t loadedFromCache = 0;
        uint32_t compiled = 0;       // cache misses compiled from source
        uint32_t written = 0;        // binaries stored after compiling
        uint32_t rejected = 0;       // cache files that were invalid/stale (recompiled)
        uint32_t failures = 0;       // programs that failed to build
        bool recoveredFromCrash = false;   // a previous run died while compiling (journal found)
        std::string directory;
    };
    ShaderCacheStats GetShaderCacheStats();

    // GL texture (RGBA8, tonemapped) of an offscreen camera target (Camera::target), 0 if none yet.
    uint32_t GetTargetTexture(const std::string& name);
    bool CaptureTarget(const std::string& name, FrameCapture& out);   // like CaptureFrame, for an offscreen target
    uint32_t GetGLErrorCount();          // GL_DEBUG_TYPE_ERROR messages since the renderer started
}
