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
    };

    RenderStats GetRenderStats();    // zeros when no renderer is running (e.g. headless)

    // Writes the most recently rendered frame (before present) as PNG.
    bool CaptureScreenshot(const std::string& path);
}
