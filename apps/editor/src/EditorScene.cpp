// The editor's empty document: a ground grid with a collider, a sun and an orbit camera.
#include "Editor.h"

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <memory>

using namespace Phisiks;

namespace Editor {
    namespace {
        class EmptyScene : public Scene {
        public:
            explicit EmptyScene(const std::string& name) : Scene(name) {}

            void OnEnter(GameContext& ctx) override {
                Render::MaterialDesc grid;
                grid.baseColor = glm::vec3(0.11f, 0.12f, 0.14f);
                grid.roughness = 0.95f;
                grid.pattern = Render::Pattern::Grid;
                grid.patternColor = glm::vec3(0.24f, 0.27f, 0.33f);
                grid.patternScale = 1.0f;
                grid.lineWidth = 0.03f;
                Render::RegisterMaterial("Editor/Grid", grid);

                Entity ground = CreateEntity("Ground");
                ground.Get<Transform>().scale = glm::vec3(40.0f, 1.0f, 40.0f);
                MeshRenderer plane{ "Plane", "Editor/Grid" };
                plane.uvScale = glm::vec2(20.0f);   // 2 m grid cells
                plane.castShadows = false;
                ground.Add<MeshRenderer>(plane);
                Entity slab = CreateEntity("Ground collider");   // 1 m slab whose top face is the plane
                slab.SetParent(ground, false);
                slab.Get<Transform>().position = glm::vec3(0.0f, -0.5f, 0.0f);
                Collider collider;
                collider.shape.shape = Physics::ShapeType::Box;
                collider.shape.halfExtents = glm::vec3(0.5f);
                slab.Add<Collider>(collider);

                Entity sun = CreateEntity("Sun");
                sun.Get<Transform>().rotation = glm::quatLookAt(glm::normalize(glm::vec3(-0.45f, -1.0f, -0.35f)), glm::vec3(0, 0, -1));
                Light light;
                light.type = LightType::Directional;
                light.intensity = 2.2f;
                light.color = glm::vec3(0.95f, 0.96f, 1.0f);
                light.castShadows = true;
                sun.Add<Light>(light);

                ctx.camera.SetOrbit(glm::vec3(0.0f, 0.5f, 0.0f), 9.0f, 0.75f, -0.45f);
            }
        };
    }

    void RegisterEmptyScene(SceneManager& scenes, const std::string& name) {
        scenes.Register(name, [name] { return std::make_unique<EmptyScene>(name); });
    }
}
