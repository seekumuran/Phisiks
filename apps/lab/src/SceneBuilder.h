#pragma once
// Helpers that assemble test scenes from components (unit primitives scaled by Transform).
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <string>

namespace Lab::SceneBuilder {
    Phisiks::Entity AddStaticBox(Phisiks::Scene& scene, const std::string& name, const glm::vec3& center, const glm::vec3& size,
                                const std::string& material, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    Phisiks::Entity AddDynamicBox(Phisiks::Scene& scene, const std::string& name, const glm::vec3& center, const glm::vec3& size,
                                 float mass, const std::string& material, float maxHealth = 0.0f);
    Phisiks::Entity AddDynamicSphere(Phisiks::Scene& scene, const std::string& name, const glm::vec3& center, float radius,
                                    float mass, const std::string& material);
    Phisiks::Entity AddPointLight(Phisiks::Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& color,
                                 float range, float intensity, bool castShadows = false);
    Phisiks::Entity AddSpotLight(Phisiks::Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& target,
                                const glm::vec3& color, float range, float intensity, float outerAngle, bool castShadows = true);
    Phisiks::Entity AddSun(Phisiks::Scene& scene, const glm::vec3& direction, const glm::vec3& color, float illuminance);
    // Square ground at y = 0: a "Plane" renderer (pattern repeats `patternPerMetre` times per metre)
    // plus a box collider below it.
    Phisiks::Entity AddGround(Phisiks::Scene& scene, float size, const std::string& material, float patternPerMetre);
}
