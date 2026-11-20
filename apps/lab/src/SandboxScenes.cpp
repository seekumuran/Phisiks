#include "SandboxScenes.h"
#include "SceneBuilder.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/Renderer.h"

#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>

using namespace Phisiks;

namespace Lab {
    namespace {
        // Procedural materials (registered in RegisterMaterials).
        constexpr const char* MAT_GROUND = "Sandbox/Ground";
        constexpr const char* MAT_WALL = "Sandbox/Bricks";
        constexpr const char* MAT_SPAWNER = "Sandbox/MetalPlate";
        constexpr const char* MAT_NEON = "Sandbox/Neon";
        constexpr const char* MAT_STATIC = "Sandbox/Concrete";
        constexpr const char* MAT_DYNAMIC = "Sandbox/Wood";
        constexpr const char* MAT_SPHERE = "Sandbox/Rock";
        constexpr const char* MAT_METAL = "Sandbox/Copper";

        void AddGroundAndLights(Scene& scene) {
            SceneBuilder::AddGround(scene, Layout::GROUND_SIZE, MAT_GROUND, 0.5f);
            SceneBuilder::AddSun(scene, Layout::SUN_DIRECTION, glm::vec3(0.92f, 0.95f, 1.0f), 2.2f);
            SceneBuilder::AddPointLight(scene, "KeyLight", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f, 0.85f, 0.7f), 14.0f, 12.0f, true);
            SceneBuilder::AddSpotLight(scene, "FillLight", glm::vec3(-4.0f, 4.0f, 3.0f), glm::vec3(-1.0f, 0.0f, -2.0f),
                                       glm::vec3(0.75f, 0.85f, 1.0f), 14.0f, 40.0f, 0.55f, true);
        }
    }

    void SandboxScene::OnEnter(GameContext& ctx) {
        using namespace Layout;
        AddGroundAndLights(*this);

        // Collision geometry for the character controller: a wall, a step and a ramp.
        SceneBuilder::AddStaticBox(*this, "Wall", WALL_CENTER, WALL_SIZE, MAT_WALL);
        SceneBuilder::AddStaticBox(*this, "Step", glm::vec3(5.0f, 0.15f, 3.0f), glm::vec3(2.0f, 0.3f, 2.0f), MAT_STATIC);
        SceneBuilder::AddStaticBox(*this, "Ramp", glm::vec3(6.0f, 0.6f, -3.0f), glm::vec3(2.5f, 0.2f, 5.0f), MAT_STATIC,
                                   glm::angleAxis(0.25f, glm::vec3(1.0f, 0.0f, 0.0f)));

        // Physics test objects.
        for (int i = 0; i < STACK_COUNT; ++i) {
            SceneBuilder::AddDynamicBox(*this, "Stack_" + std::to_string(i), glm::vec3(4.0f, 0.3f + i * 0.61f, 0.0f), glm::vec3(0.6f), 8.0f, MAT_DYNAMIC);
        }
        SceneBuilder::AddDynamicBox(*this, "FallingBox", FALLING_BOX_START, glm::vec3(FALLING_BOX_SIZE), 10.0f, MAT_DYNAMIC);
        SceneBuilder::AddDynamicSphere(*this, "Ball_0", glm::vec3(-1.0f, 2.0f, -3.0f), 0.3f, 3.0f, MAT_SPHERE);
        SceneBuilder::AddDynamicSphere(*this, "Ball_1", glm::vec3(1.0f, 3.0f, -3.0f), 0.4f, 5.0f, MAT_METAL);

        // Damage test: destructible target (4 hits of 25).
        SceneBuilder::AddDynamicBox(*this, "TargetBox", TARGET_CENTER, glm::vec3(TARGET_SIZE), 12.0f, Layout::MAT_TARGET, 100.0f);

        // Interaction test: pressing E on the spawner drops a new box on top of it and plays a sound.
        Entity spawner = SceneBuilder::AddStaticBox(*this, "Spawner", SPAWNER_CENTER, glm::vec3(1.0f), MAT_SPAWNER);
        AudioSource& sound = spawner.Add<AudioSource>();
        sound.clip = SPAWNER_SOUND;
        sound.volume = 0.6f;
        Interactable& interactable = spawner.Add<Interactable>();
        interactable.prompt = "spawn a box";
        interactable.onInteract = [this](GameContext& c, Entity self, Entity /*instigator*/) {
            const std::string name = "Spawned_" + std::to_string(++m_spawned);
            SceneBuilder::AddDynamicBox(*this, name, self.WorldPosition() + glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), 5.0f, MAT_DYNAMIC, 50.0f);
            self.Get<AudioSource>().Play();
            c.PushEvent("Spawner created " + name);
        };

        // On-demand asset: OBJ mesh loaded the first time it is rendered (no collider: decoration).
        Entity torus = CreateEntity("Torus");
        torus.Get<Transform>().position = TORUS_CENTER;
        torus.Get<Transform>().rotation = glm::angleAxis(1.2f, glm::vec3(1.0f, 0.0f, 0.0f));
        torus.Add<MeshRenderer>(MeshRenderer{ TORUS_MESH, MAT_NEON });

        Gameplay::SpawnPlayer(ctx, PLAYER_SPAWN);
        ctx.camera.SetYawPitch(0.0f, -0.15f);   // facing -Z toward the wall
    }

    void SandboxScene::Update(GameContext&, float deltaTime) {
        if (Entity torus = FindByName("Torus"); torus.Valid()) {
            Transform& t = torus.Get<Transform>();
            t.rotation = glm::normalize(glm::angleAxis(0.6f * deltaTime, glm::vec3(0.0f, 1.0f, 0.0f)) * t.rotation);
        }
    }

    void SandboxScene::DrawUI(GameContext&) { DrawViewLabels(*this); }
    void PhysicsTowerScene::DrawUI(GameContext&) { DrawViewLabels(*this); }

    void SetQuadView(Scene& scene, bool enabled) {
        Entity main = scene.FindByName("MainCamera");
        for (const char* name : { "TopView", "FrontView", "SideView" }) {
            if (Entity e = scene.FindByName(name); e.Valid()) scene.DestroyImmediate(e);
        }
        if (main.Valid()) main.Get<Camera>().viewport = enabled ? glm::vec4(0.0f, 0.5f, 0.5f, 0.5f) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        if (!enabled) return;
        struct Def { const char* name; glm::vec3 position, direction, up; glm::vec4 viewport; float height; };
        const Def defs[] = {
            { "TopView",   { 0.0f, 40.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, Layout::QUAD_TOP_ORTHO_HEIGHT },
            { "FrontView", { 0.0f, 1.5f, 40.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.5f, 0.5f }, Layout::QUAD_SIDE_ORTHO_HEIGHT },
            { "SideView",  { 40.0f, 1.5f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.0f, 0.5f, 0.5f }, Layout::QUAD_SIDE_ORTHO_HEIGHT },
        };
        int order = 1;
        for (const Def& d : defs) {
            Entity e = scene.CreateEntity(d.name);
            Transform& t = e.Get<Transform>();
            t.position = d.position;
            t.rotation = glm::quatLookAt(d.direction, d.up);
            Camera& c = e.Add<Camera>();
            c.projection = Projection::Orthographic;
            c.orthoHeight = d.height;
            c.nearPlane = 1.0f;
            c.farPlane = 100.0f;
            c.viewport = d.viewport;
            c.order = order++;
            c.primary = false;
        }
    }

    bool IsQuadView(Scene& scene) { return scene.FindByName("TopView").Valid(); }

    void DrawViewLabels(Scene& scene) {
        if (!IsQuadView(scene)) return;
        const glm::vec2 size = glm::vec2(Render::GetFramebufferSize());
        const glm::vec4 color(0.85f, 0.9f, 1.0f, 1.0f);
        const glm::vec2 half = size * 0.5f;
        Render::DrawScreenText("PERSPECTIVE", glm::vec2(half.x - 160.0f, 12.0f), color);
        Render::DrawScreenText("TOP (ORTHO)", glm::vec2(half.x + 12.0f, 12.0f), color);
        Render::DrawScreenText("FRONT (ORTHO)", glm::vec2(12.0f, half.y + 12.0f), color);
        Render::DrawScreenText("SIDE (ORTHO)", glm::vec2(half.x + 12.0f, half.y + 12.0f), color);
        for (float t = 0.0f; t < 1.0f; t += 1.0f / 64.0f) {   // divider lines (thin text dashes)
            Render::DrawScreenText("-", glm::vec2(size.x * t, half.y - 8.0f), glm::vec4(0.4f, 0.45f, 0.55f, 1.0f), 2.0f);
        }
    }

    void PhysicsTowerScene::OnEnter(GameContext& ctx) {
        AddGroundAndLights(*this);
        for (int i = 0; i < TOWER_BOXES; ++i) {
            const float x = (i % 2 == 0) ? 0.0f : 0.05f;   // slight offset so the tower is not perfectly stable
            SceneBuilder::AddDynamicBox(*this, "Tower_" + std::to_string(i), glm::vec3(x, 0.25f + i * 0.51f, -3.0f), glm::vec3(1.0f, 0.5f, 1.0f), 6.0f, MAT_DYNAMIC);
        }
        for (int i = 0; i < SPHERES; ++i) {
            SceneBuilder::AddDynamicSphere(*this, "TowerBall_" + std::to_string(i), glm::vec3(-3.0f + i * 1.5f, 4.0f + i, -1.0f), 0.35f, 4.0f, MAT_SPHERE);
        }
        Gameplay::SpawnPlayer(ctx, glm::vec3(0.0f, 0.05f, 4.0f));
        ctx.camera.SetYawPitch(0.0f, 0.05f);
    }

    void RegisterMaterials() {
        using Render::MaterialDesc;
        using Render::Pattern;
        auto make = [](glm::vec3 base, float roughness, Pattern pattern, glm::vec3 patternColor, float scale, float lineWidth = 0.04f) {
            MaterialDesc m;
            m.baseColor = base;
            m.roughness = roughness;
            m.pattern = pattern;
            m.patternColor = patternColor;
            m.patternScale = scale;
            m.lineWidth = lineWidth;
            return m;
        };
        // Ground: dark rainbow calibration grid, 1 m labelled tiles (A0..J9), glowing labels/lines.
        MaterialDesc ground = make({ 0.018f, 0.019f, 0.024f }, 0.85f, Pattern::CalibrationGrid, { 0.32f, 0.33f, 0.37f }, 1.0f, 0.015f);
        ground.patternRoughness = 0.55f;
        ground.patternGlow = 0.6f;
        Render::RegisterMaterial(MAT_GROUND, ground);
        // Textured materials (CC0 textures from tools/assetgen, streamed in on demand).
        MaterialDesc bricks = make({ 1.0f, 1.0f, 1.0f }, 0.85f, Pattern::None, {}, 1.0f);
        bricks.albedoTexture = "textures/bricks.png";
        Render::RegisterMaterial(MAT_WALL, bricks);
        MaterialDesc plate = make({ 1.0f, 1.0f, 1.0f }, 0.4f, Pattern::None, {}, 1.0f);
        plate.albedoTexture = "textures/metal_plate.png";
        plate.metallic = 0.85f;
        Render::RegisterMaterial(MAT_SPAWNER, plate);
        MaterialDesc neon = make({ 0.05f, 0.55f, 0.85f }, 0.3f, Pattern::None, {}, 1.0f);
        neon.emissive = glm::vec3(0.0f, 0.22f, 0.38f);
        Render::RegisterMaterial(MAT_NEON, neon);
        Render::RegisterMaterial(MAT_STATIC, make({ 0.42f, 0.42f, 0.44f }, 0.9f, Pattern::Noise, { 0.30f, 0.30f, 0.31f }, 4.0f));
        Render::RegisterMaterial(MAT_DYNAMIC, make({ 0.55f, 0.33f, 0.15f }, 0.6f, Pattern::Wood, { 0.33f, 0.18f, 0.07f }, 2.0f));
        Render::RegisterMaterial(MAT_SPHERE, make({ 0.32f, 0.32f, 0.34f }, 0.8f, Pattern::Noise, { 0.12f, 0.12f, 0.13f }, 6.0f));
        Render::RegisterMaterial(Layout::MAT_TARGET, make({ 0.72f, 0.07f, 0.05f }, 0.8f, Pattern::Noise, { 0.52f, 0.04f, 0.03f }, 8.0f));
        Render::RegisterMaterial(Layout::MAT_PLAYER, make({ 0.08f, 0.25f, 0.70f }, 0.35f, Pattern::None, {}, 1.0f));
        MaterialDesc copper = make({ 0.95f, 0.64f, 0.54f }, 0.3f, Pattern::None, {}, 1.0f);
        copper.metallic = 1.0f;
        Render::RegisterMaterial(MAT_METAL, copper);

        // Environment: the engine's dark studio defaults (Render::RenderSettings).
    }

    void RegisterScenes(SceneManager& scenes) {
        scenes.Register("Sandbox", [] { return std::make_unique<SandboxScene>(); });
        scenes.Register("PhysicsTower", [] { return std::make_unique<PhysicsTowerScene>(); });
    }
}
