#pragma once
// FieldLab: GPU compute showcase scene.
//   - planet (spherical gravity) with rigid moons on circular orbits
//   - vortex field swirling GPU particles (spatial-hash collisions), shown by field arrows
//   - n-body cluster of rigid spheres attracting each other
//   - XPBD cloth curtain (pinned), cloth draped over a sphere, rope, soft box
//   - raymarched smooth-CSG SDF sculpture
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

namespace Samples {
    namespace FieldLayout {
        inline const glm::vec3 PLANET_CENTER{ 0.0f, 3.0f, -6.0f };
        inline constexpr float PLANET_RADIUS = 0.8f;
        inline constexpr float PLANET_GM = 6.0f;           // m^3/s^2
        inline constexpr float MOON_ORBIT = 2.2f;          // m
        inline const glm::vec3 VORTEX_CENTER{ 6.0f, 0.0f, -2.0f };
        inline const glm::vec3 CLUSTER_CENTER{ -7.0f, 2.5f, -3.0f };
        inline const glm::vec3 DRAPE_SPHERE_CENTER{ -3.5f, 0.9f, 1.0f };
        inline constexpr float DRAPE_SPHERE_RADIUS = 0.9f;
        inline const glm::vec3 SDF_CENTER{ 2.5f, 1.4f, 2.0f };
        inline const glm::vec3 FIELD_BOUNDS_MIN{ 2.5f, 0.6f, -5.5f };
        inline const glm::vec3 FIELD_BOUNDS_MAX{ 9.5f, 2.6f, 1.5f };
        inline const glm::ivec3 FIELD_RESOLUTION{ 9, 3, 9 };
        inline constexpr uint32_t PARTICLES = 4096;
        inline constexpr float PARTICLE_RADIUS = 0.045f;
    }

    class FieldLabScene : public Phisiks::Scene {
    public:
        FieldLabScene() : Scene("FieldLab") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void OnExit(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float deltaTime) override;
        void DrawUI(Phisiks::GameContext& ctx) override;
        // Restores the scene's SDF sculpture (tests temporarily replace it).
        static void ApplySdfSculpture(Phisiks::Scene& scene);
    private:
        bool m_launched = false;
    };
}
