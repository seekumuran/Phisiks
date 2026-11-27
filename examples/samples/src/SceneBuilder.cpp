#include "SceneBuilder.h"
#include "Phisiks/Framework/GameplayComponents.h"

using namespace Phisiks;

namespace Samples::SceneBuilder {
    namespace {
        Collider BoxCollider() {
            Collider collider;
            collider.shape.shape = Physics::ShapeType::Box;
            collider.shape.halfExtents = glm::vec3(0.5f);   // unit cube, scaled by Transform
            return collider;
        }
    }

    Entity AddStaticBox(Scene& scene, const std::string& name, const glm::vec3& center, const glm::vec3& size, const std::string& material, const glm::quat& rotation) {
        Entity e = scene.CreateEntity(name);
        Transform& t = e.Get<Transform>();
        t.position = center;
        t.rotation = rotation;
        t.scale = size;
        e.Add<MeshRenderer>(MeshRenderer{ "Cube", material });
        e.Add<Collider>(BoxCollider());
        return e;
    }

    Entity AddDynamicBox(Scene& scene, const std::string& name, const glm::vec3& center, const glm::vec3& size, float mass, const std::string& material, float maxHealth) {
        Entity e = AddStaticBox(scene, name, center, size, material);
        Rigidbody body;
        body.mass = mass;
        e.Add<Rigidbody>(body);
        if (maxHealth > 0.0f) e.Add<Health>(Health{ maxHealth, maxHealth });
        return e;
    }

    Entity AddDynamicSphere(Scene& scene, const std::string& name, const glm::vec3& center, float radius, float mass, const std::string& material) {
        Entity e = scene.CreateEntity(name);
        Transform& t = e.Get<Transform>();
        t.position = center;
        t.scale = glm::vec3(radius * 2.0f);
        e.Add<MeshRenderer>(MeshRenderer{ "Sphere", material });
        Collider collider;
        collider.shape.shape = Physics::ShapeType::Sphere;
        collider.shape.radius = 0.5f;                       // unit sphere, scaled by Transform
        e.Add<Collider>(collider);
        Rigidbody body;
        body.mass = mass;
        e.Add<Rigidbody>(body);
        return e;
    }

    Entity AddPointLight(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& color, float range, float intensity, bool castShadows) {
        Entity e = scene.CreateEntity(name);
        e.Get<Transform>().position = position;
        Light light;
        light.type = LightType::Point;
        light.color = color;
        light.range = range;
        light.intensity = intensity;
        light.castShadows = castShadows;
        e.Add<Light>(light);
        return e;
    }

    Entity AddSpotLight(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& target, const glm::vec3& color,
                        float range, float intensity, float outerAngle, bool castShadows) {
        Entity e = scene.CreateEntity(name);
        Transform& t = e.Get<Transform>();
        t.position = position;
        t.rotation = glm::quatLookAt(glm::normalize(target - position), glm::vec3(0, 1, 0));   // -Z towards the target
        Light light;
        light.type = LightType::Spot;
        light.color = color;
        light.range = range;
        light.intensity = intensity;
        light.outerConeAngle = outerAngle;
        light.innerConeAngle = outerAngle * 0.6f;
        light.castShadows = castShadows;
        e.Add<Light>(light);
        return e;
    }

    Entity AddSun(Scene& scene, const glm::vec3& direction, const glm::vec3& color, float illuminance) {
        Entity e = scene.CreateEntity("Sun");
        e.Get<Transform>().rotation = glm::quatLookAt(glm::normalize(direction), glm::vec3(0, 0, -1));
        Light light;
        light.type = LightType::Directional;
        light.color = color;
        light.intensity = illuminance;
        light.castShadows = true;
        e.Add<Light>(light);
        return e;
    }

    Entity AddGround(Scene& scene, float size, const std::string& material, float patternPerMetre) {
        Entity ground = scene.CreateEntity("Ground");
        ground.Get<Transform>().scale = glm::vec3(size, 1.0f, size);
        MeshRenderer renderer{ "Plane", material };
        renderer.uvScale = glm::vec2(patternPerMetre);
        ground.Add<MeshRenderer>(renderer);

        // Collider as a child: a 1 m thick slab whose top face is the plane.
        Entity slab = scene.CreateEntity("GroundCollider");
        slab.SetParent(ground, false);
        slab.Get<Transform>().position = glm::vec3(0.0f, -0.5f, 0.0f);
        slab.Add<Collider>(BoxCollider());
        return ground;
    }
}
