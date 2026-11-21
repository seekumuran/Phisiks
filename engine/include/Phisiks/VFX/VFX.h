#pragma once
// Phisiks::VFX: ribbon trails, beams, orbit paths and projected decals as scene components. The
// scene's VFX system samples them every frame and submits Render::DrawRibbon / DrawDecal. The
// post-processing stack (HDR bloom, SSAO) lives in Render::RenderSettings (bloom, ssao) and the
// quality presets.

#include "Phisiks/Render/Renderer.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <deque>
#include <string>

namespace Phisiks {
    class Scene;

    // Motion trail behind the entity (world-space history, fades and narrows with age).
    struct Trail {
        float lifetime = 1.2f;                     // seconds a point lives
        float width = 0.12f;                       // metres at the head
        float tailWidth = 0.0f;                    // metres at the end
        glm::vec4 headColor{ 0.4f, 0.9f, 4.0f, 1.0f };   // linear HDR (values > 1 bloom)
        glm::vec4 tailColor{ 0.1f, 0.2f, 1.0f, 0.0f };
        float minDistance = 0.04f;                 // metres between recorded points
        uint32_t maxPoints = 256;
        bool emitting = true;
        glm::vec3 offset{ 0.0f };                  // local point that leaves the trail
        // runtime
        struct Point { glm::vec3 position; float age; };
        std::deque<Point> points;
    };

    // Plasma / laser beam from the entity to `target` (entity name) or `endPoint` (world), with an
    // animated jitter; `raycast` stops it at the first physics hit.
    struct Beam {
        std::string target;
        glm::vec3 endPoint{ 0.0f, 0.0f, -10.0f };
        glm::vec3 offset{ 0.0f };                  // local start point
        float width = 0.08f;
        glm::vec4 color{ 3.0f, 0.6f, 4.0f, 1.0f };
        float jitter = 0.06f;                      // metres of noise
        float frequency = 9.0f;                    // noise speed
        int segments = 32;
        bool raycast = false;
        bool enabled = true;
        // runtime
        glm::vec3 lastEnd{ 0.0f };                 // where it ended last frame
    };

    // Circle (or ellipse) around the entity's position in its local XZ plane: orbit guides.
    struct OrbitPath {
        float radiusX = 2.0f, radiusZ = 2.0f;
        float width = 0.025f;
        glm::vec4 color{ 0.6f, 0.7f, 1.0f, 0.5f };
        int segments = 96;
        float dash = 0.0f;                         // 0 = solid; else dash length (fraction of the circle)
    };

    // Projected decal: the entity's unit box (scaled by `size`) projects along its local -Y.
    struct Decal {
        glm::vec3 size{ 1.0f, 0.6f, 1.0f };
        Render::DecalPattern pattern = Render::DecalPattern::Scorch;
        Render::DecalBlend blend = Render::DecalBlend::Multiply;
        glm::vec4 color{ 0.1f, 0.08f, 0.06f, 0.9f };
        float angleFade = 0.35f;
        float seed = 0.0f;
        bool visible = true;
    };

    namespace VFX {
        void UpdateScene(Scene& scene, float deltaTime);   // trails age / record, then everything is submitted
        struct Stats { uint32_t trails = 0, trailPoints = 0, beams = 0, orbits = 0, decals = 0; };
        Stats GetStats();
    }
}
