#pragma once
// Renderer-side counters (what is currently submitted for drawing).

#include <cstdint>
#include <string>

namespace Phisiks {
    struct RenderStats {
        uint32_t meshObjects = 0;    // MeshRenderer objects that exist on the renderer side
        uint32_t lights = 0;
        uint32_t planes = 0;         // objects using the "Plane" mesh
        bool cameraBound = false;    // a scene camera drives the screen
        // Last rendered frame
        uint32_t instancesDrawn = 0; // visible objects after frustum culling (all views)
        uint32_t instancesCulled = 0;
        uint32_t drawCommands = 0;   // indirect draw commands of the first window view (one per mesh)
        uint32_t shadowViews = 0;    // shadow map views rendered (directional + spot + 6 per point)
        uint32_t trianglesDrawn = 0; // all views
        uint32_t views = 0;          // cameras rendered (window + offscreen)
        uint32_t texturesResident = 0;
        uint32_t texturesPending = 0;   // being decoded on the asset worker
        uint32_t meshesLoaded = 0;      // meshes registered (built-in + loaded on demand)
        bool gpuCulling = false;        // the GPU culled this frame (instance counts are from 2 frames ago)
        bool occlusionActive = false;   // Hi-Z occlusion ran for at least one view
        uint32_t occlusionCulled = 0;
        uint64_t ringBlockedWaits = 0;  // persistent upload ring: times the CPU had to wait for the GPU
        uint32_t particlesDrawn = 0;    // GPU particles + rope nodes
        uint32_t softBodiesDrawn = 0;
        int internalWidth = 0, internalHeight = 0;   // scene resolution (window x renderScale)
        int outputWidth = 0, outputHeight = 0;
        bool upscaled = false;          // internal < output: EASU + RCAS (or bilinear) ran
        bool compatTier = false;        // OpenGL 4.1 feature tier (CPU culling, CPU compute)
        // Post-processing / VFX (last frame)
        int bloomMips = 0;              // bloom chain levels used (0 = off)
        bool ssaoActive = false;
        int ssaoWidth = 0, ssaoHeight = 0;
        uint32_t decalsDrawn = 0;
        uint32_t ribbonsDrawn = 0, ribbonVertices = 0;
        uint32_t overlayGlyphs = 0;     // bitmap overlay text glyphs in the presented frame
    };

    RenderStats GetRenderStats();    // zeros when no renderer is running (e.g. headless)

    // Writes the most recently rendered frame (before present) as PNG.
    bool CaptureScreenshot(const std::string& path);
}
