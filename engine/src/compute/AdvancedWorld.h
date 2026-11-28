#pragma once
// Engine-private: GPU particle systems and XPBD soft bodies of the current scene, plus the shared
// force-field and collider buffers. Driven by the Application (Step each fixed step) and drawn by
// the renderer (CollectDraws).

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks { class Scene; }

namespace Phisiks::Advanced {
    bool Init();                 // after the renderer (needs the GL context and programs)
    void Shutdown();
    bool Available();

    void Step(Scene& scene, float dt);       // create/destroy systems for components, upload fields/colliders, simulate
    void Clear();                            // destroys every GPU system (scene unload)

    struct SoftBodyDraw {
        uint32_t positions = 0, normals = 0, vao = 0;
        uint32_t indexCount = 0;
        uint32_t entityId = 0;
        std::string material;
    };
    struct SpriteDraw {
        uint32_t buffer = 0;
        uint32_t count = 0;
        uint32_t stride = 2;                 // vec4s per element
        float radius = 0.0f;                 // > 0 overrides the per-element radius
        glm::vec3 color{ 1.0f }, fastColor{ 1.0f };
        float fastSpeed = 1.0f;
        uint32_t entityId = 0;
        bool fluid = false;                  // ParticleRender::Fluid (screen-space fluid on window views)
        uint32_t colorMode = 0;              // ParticleColor
        float glow = 0.0f, dopplerSpeed = 10.0f;
    };
    void CollectDraws(std::vector<SoftBodyDraw>& softBodies, std::vector<SpriteDraw>& sprites);

    // Field sampling for the vector-field visualiser: fills (and returns) a GL buffer of vec4 samples.
    uint32_t SampleFieldGrid(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::ivec3& resolution);
    uint32_t FieldCount();
}
