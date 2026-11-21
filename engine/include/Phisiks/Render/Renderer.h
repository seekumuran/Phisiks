#pragma once
// Phisiks::Render: forward PBR renderer. OpenGL 4.6 tier (GPU-driven culling, compute) or the
// OpenGL 4.1 tier (macOS / older iGPUs: CPU culling, CPU compute fallback, same features otherwise).
//
// Scenes feed it automatically (MeshRenderer, Light, Camera components). Games use this header to
// register meshes and materials, tune the environment (exposure, ambient, fog, shadows) and draw
// debug lines / overlay text. Everything here must be called on the main (GL) thread.

#include "Phisiks/Render/RenderStats.h"

#include <glm/gtc/quaternion.hpp>
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

    // Spatial upscaling of the window image from renderScale x the window size.
    enum class Upscaler : uint32_t { Bilinear = 0, FSR1 = 1 };   // FSR1: edge-adaptive EASU + RCAS sharpening

    struct RenderSettings {
        float exposure = 1.0f;
        float renderScale = 1.0f;          // window views render at this fraction of the window (0.5 .. 1)
        Upscaler upscaler = Upscaler::FSR1;
        float sharpness = 0.25f;           // RCAS strength in stops (0 = strongest, 2 = mild)
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
        bool gpuCulling = true;            // frustum culling in a compute shader writing the indirect commands
        bool occlusionCulling = true;      // + Hi-Z occlusion against the previous frame's depth (window views)
        glm::vec4 clearColor{ 0.012f, 0.013f, 0.018f, 1.0f };
    };
    RenderSettings& Settings();

    // Scalability presets for integrated GPUs (tuned on a Core i3 / 8 GB / Intel UHD laptop):
    //   Low     renderScale 0.667 (720p -> 1080p) FSR1, no MSAA, 1024 shadows over 25 m, no occlusion pass
    //   Medium  renderScale 0.77 FSR1, 2x MSAA, 2048 shadows over 35 m
    //   High    native resolution, 4x MSAA, 2048 shadows over 40 m, Hi-Z occlusion
    enum class QualityPreset { Low, Medium, High };
    void ApplyQualityPreset(QualityPreset preset);
    const char* ToString(QualityPreset preset);

    struct RendererInfo {
        int contextVersion = 0;            // e.g. 46 (driver)
        bool compatTier = false;           // 4.1 feature tier in use
        bool gpuCompute = false;           // compute shaders available to the engine
        std::string renderer, vendor;
    };
    RendererInfo GetRendererInfo();

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

    // Multi-pass synthetic vision: aligned colour, linear depth, world normals and entity ids of the
    // first window view at the internal (scene) resolution. RequestVisionCapture() before a frame;
    // GetVisionCapture() after it (stalls the GPU: export / tests only). Covers meshes and soft
    // bodies; particles sprites and the SDF visualiser are colour-only.
    struct VisionCapture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;           // tonemapped, rows bottom-up
        std::vector<float> linearDepth;      // metres along the view axis, 0 = background
        std::vector<glm::vec3> normals;      // world space unit normals, 0 = background
        std::vector<uint32_t> entityIds;     // Entity::Id() + 1, 0 = background
        glm::mat4 view{ 1.0f }, projection{ 1.0f };
        float nearPlane = 0.0f, farPlane = 0.0f;
        bool orthographic = false;
    };
    void RequestVisionCapture();
    bool GetVisionCapture(VisionCapture& out);

    // Runs the upscaler (EASU + RCAS with `sharpness` stops, or bilinear) on an RGBA8 image on the
    // GPU (tests/tools; synchronous). Rows bottom-up.
    bool UpscaleImage(const std::vector<uint8_t>& rgba, int width, int height, int outWidth, int outHeight,
                      Upscaler upscaler, float sharpness, std::vector<uint8_t>& out);

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
    uint32_t GetGLErrorCount();

    // ---------------------------------------------------------------- analytical visualisers
    // Raymarched SDF / smooth-CSG scene (mirrors Phisiks::Math::SDF). Primitives are combined in order:
    // the first starts the shape, each next one is unioned / subtracted / intersected with smoothness k.
    enum class SdfShape : uint32_t { Sphere = 0, Box = 1, RoundBox = 2, Torus = 3, Capsule = 4, Cylinder = 5 };
    enum class SdfOperation : uint32_t { Union = 0, Subtraction = 1, Intersection = 2 };
    struct SdfPrimitive {
        SdfShape shape = SdfShape::Sphere;
        glm::vec3 position{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        // Sphere: x radius | Box: xyz half extents | RoundBox: xyz half extents, w rounding |
        // Torus: x ring radius, y tube radius | Capsule: x half length (local Y), y radius | Cylinder: x half height, y radius
        glm::vec4 params{ 0.5f, 0.5f, 0.5f, 0.0f };
        SdfOperation operation = SdfOperation::Union;
        float smoothness = 0.0f;           // k (m); 0 = sharp CSG
    };
    struct SdfVisualization {
        bool enabled = false;
        std::vector<SdfPrimitive> primitives;   // up to a few dozen
        glm::vec3 boundsMin{ -1.0f }, boundsMax{ 1.0f };   // raymarch region (world)
        glm::vec3 color{ 0.8f, 0.82f, 0.85f };
        float roughness = 0.35f;
        float metallic = 0.0f;
        uint32_t entityId = 0xFFFFFFFFu;    // Entity::Id() reported in the id buffer (0xFFFFFFFF = none)
        bool showSlice = false;             // slice plane coloured by signed distance (0.1 m isolines)
        glm::vec4 slicePlane{ 0.0f, 0.0f, 1.0f, 0.0f };   // xyz unit normal, w offset
    };
    void SetSdfVisualization(const SdfVisualization& sdf);
    const SdfVisualization& GetSdfVisualization();

    // Vector-field arrows: the scene's ForceField components sampled on the GPU on a grid.
    struct FieldVisualization {
        bool enabled = false;
        glm::vec3 boundsMin{ -5.0f, 0.5f, -5.0f }, boundsMax{ 5.0f, 3.0f, 5.0f };
        glm::ivec3 resolution{ 10, 3, 10 };
        float arrowScale = 0.05f;           // metres per (m/s^2)
        float maxLength = 0.6f;
    };
    void SetFieldVisualization(const FieldVisualization& field);          // GL_DEBUG_TYPE_ERROR messages since the renderer started
}
