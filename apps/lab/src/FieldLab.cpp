#include "FieldLab.h"
#include "SandboxScenes.h"
#include "SceneBuilder.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>

using namespace Phisiks;
using namespace Phisiks::Physics;
using namespace Phisiks::Physics::Advanced;

namespace Lab {
    using namespace FieldLayout;

    namespace {
        constexpr const char* MAT_PLANET = "FieldLab/Planet";
        constexpr const char* MAT_MOON = "FieldLab/Moon";
        constexpr const char* MAT_CLOTH = "FieldLab/Cloth";
        constexpr const char* MAT_DRAPE = "FieldLab/Drape";
        constexpr const char* MAT_SOFT = "FieldLab/Jelly";
        constexpr const char* MAT_STONE = "FieldLab/Stone";

        void RegisterFieldLabMaterials() {
            auto make = [](glm::vec3 base, float roughness, Render::Pattern pattern = Render::Pattern::None, glm::vec3 accent = glm::vec3(0.0f), float scale = 1.0f) {
                Render::MaterialDesc m;
                m.baseColor = base;
                m.roughness = roughness;
                m.pattern = pattern;
                m.patternColor = accent;
                m.patternScale = scale;
                return m;
            };
            Render::MaterialDesc planet = make({ 0.12f, 0.35f, 0.75f }, 0.6f, Render::Pattern::Noise, { 0.15f, 0.55f, 0.25f }, 3.0f);
            planet.emissive = glm::vec3(0.0f, 0.02f, 0.06f);
            Render::RegisterMaterial(MAT_PLANET, planet);
            Render::RegisterMaterial(MAT_MOON, make({ 0.75f, 0.74f, 0.72f }, 0.8f, Render::Pattern::Noise, { 0.4f, 0.4f, 0.42f }, 8.0f));
            Render::RegisterMaterial(MAT_CLOTH, make({ 0.70f, 0.08f, 0.10f }, 0.85f, Render::Pattern::Checker, { 0.85f, 0.75f, 0.2f }, 4.0f));
            Render::RegisterMaterial(MAT_DRAPE, make({ 0.10f, 0.45f, 0.40f }, 0.9f, Render::Pattern::Grid, { 0.8f, 0.8f, 0.7f }, 4.0f));
            Render::RegisterMaterial(MAT_SOFT, make({ 0.55f, 0.85f, 0.15f }, 0.25f));
            Render::RegisterMaterial(MAT_STONE, make({ 0.35f, 0.33f, 0.32f }, 0.7f, Render::Pattern::Noise, { 0.2f, 0.2f, 0.2f }, 5.0f));
        }

        Entity AddField(Scene& scene, const char* name, const glm::vec3& position, const ForceField& field) {
            Entity e = scene.CreateEntity(name);
            e.Get<Transform>().position = position;
            e.Add<ForceField>(field);
            return e;
        }
    }

    void FieldLabScene::ApplySdfSculpture(Scene& scene) {
        Render::SdfVisualization sdf;
        sdf.enabled = true;
        const glm::vec3 c = SDF_CENTER;
        auto prim = [&](Render::SdfShape shape, glm::vec3 offset, glm::vec4 params, Render::SdfOperation op, float k,
                        glm::quat rotation = glm::quat(1, 0, 0, 0)) {
            Render::SdfPrimitive p;
            p.shape = shape;
            p.position = c + offset;
            p.rotation = rotation;
            p.params = params;
            p.operation = op;
            p.smoothness = k;
            sdf.primitives.push_back(p);
        };
        // Rounded block, a sphere carved out of its top (smooth), a torus blended around it, a capsule handle.
        prim(Render::SdfShape::RoundBox, { 0, 0, 0 }, { 0.55f, 0.45f, 0.55f, 0.08f }, Render::SdfOperation::Union, 0.0f);
        prim(Render::SdfShape::Sphere, { 0, 0.45f, 0 }, { 0.45f, 0, 0, 0 }, Render::SdfOperation::Subtraction, 0.12f);
        prim(Render::SdfShape::Torus, { 0, -0.15f, 0 }, { 0.75f, 0.12f, 0, 0 }, Render::SdfOperation::Union, 0.18f);
        prim(Render::SdfShape::Capsule, { 0, 0.9f, 0 }, { 0.35f, 0.08f, 0, 0 }, Render::SdfOperation::Union, 0.1f,
             glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1)));
        sdf.boundsMin = c - glm::vec3(1.0f, 0.75f, 1.0f);
        sdf.boundsMax = c + glm::vec3(1.0f, 1.15f, 1.0f);
        sdf.color = glm::vec3(0.85f, 0.62f, 0.25f);
        sdf.roughness = 0.3f;
        sdf.metallic = 1.0f;
        const Entity marker = scene.FindByName("SdfSculpture");
        sdf.entityId = marker.Valid() ? marker.Id() : 0xFFFFFFFFu;
        Render::SetSdfVisualization(sdf);
    }

    void FieldLabScene::OnEnter(GameContext& ctx) {
        RegisterFieldLabMaterials();
        SceneBuilder::AddGround(*this, 40.0f, "Sandbox/Ground", 1.0f);
        SceneBuilder::AddSun(*this, glm::vec3(-0.45f, -1.0f, -0.35f), glm::vec3(0.92f, 0.95f, 1.0f), 2.2f);
        SceneBuilder::AddPointLight(*this, "KeyLight", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f, 0.85f, 0.7f), 14.0f, 12.0f, true);

        // Planet: a static sphere with spherical gravity (rigid bodies and particles), three moons.
        Entity planet = SceneBuilder::AddStaticBox(*this, "Planet", PLANET_CENTER, glm::vec3(PLANET_RADIUS * 2.0f), MAT_PLANET);
        planet.Get<MeshRenderer>().mesh = "Sphere";
        Collider sphere;
        sphere.shape.shape = ShapeType::Sphere;
        sphere.shape.radius = 0.5f;
        planet.Add<Collider>(sphere);
        ForceField gravity;
        gravity.type = FieldType::SphericalGravity;
        gravity.strength = PLANET_GM;
        gravity.radius = PLANET_RADIUS;
        gravity.influenceRadius = 4.0f;
        gravity.affectsSoftBodies = false;
        planet.Add<ForceField>(gravity);
        for (int i = 0; i < 3; ++i) {
            const float a = i * glm::two_pi<float>() / 3.0f;
            const glm::vec3 offset = MOON_ORBIT * glm::vec3(std::cos(a), 0.0f, std::sin(a));
            Entity moon = SceneBuilder::AddDynamicSphere(*this, "Moon_" + std::to_string(i), PLANET_CENTER + offset, 0.12f, 0.5f, MAT_MOON);
            Rigidbody& rb = moon.Get<Rigidbody>();
            rb.useGravity = false;
            rb.angularDamping = 0.0f;
        }

        // Vortex with GPU particles.
        ForceField vortex;
        vortex.type = FieldType::Vortex;
        vortex.strength = 7.0f;
        vortex.radius = 1.2f;
        vortex.falloff = 1.0f;
        vortex.influenceRadius = 3.5f;
        vortex.affectsRigidBodies = false;
        AddField(*this, "Vortex", VORTEX_CENTER, vortex);
        Entity particles = CreateEntity("Particles");
        particles.Get<Transform>().position = VORTEX_CENTER + glm::vec3(0.0f, 2.5f, 0.0f);
        ParticleSystem ps;
        ps.count = PARTICLES;
        ps.radius = PARTICLE_RADIUS;
        ps.spawnSize = glm::vec3(2.4f, 2.0f, 2.4f);
        particles.Add<ParticleSystem>(ps);

        // N-body cluster (rigid bodies only): six spheres attracting each other in mid-air.
        ForceField nbody;
        nbody.type = FieldType::NBody;
        nbody.strength = 0.6f;
        nbody.softening = 0.2f;
        nbody.affectsParticles = false;
        nbody.affectsSoftBodies = false;
        nbody.influenceRadius = 3.0f;   // only the cluster: the moons stay on clean Keplerian orbits
        AddField(*this, "NBody", CLUSTER_CENTER, nbody);
        for (int i = 0; i < 6; ++i) {
            const float a = i * glm::two_pi<float>() / 6.0f;
            Entity s = SceneBuilder::AddDynamicSphere(*this, "Cluster_" + std::to_string(i),
                                                       CLUSTER_CENTER + glm::vec3(1.6f * std::cos(a), 0.4f * std::sin(2.0f * a), 1.6f * std::sin(a)),
                                                       0.15f, 1.0f, MAT_STONE);
            s.Get<Rigidbody>().useGravity = false;
        }

        // XPBD: pinned curtain, cloth draped over a sphere, rope, soft box.
        Entity curtain = CreateEntity("Curtain");
        curtain.Get<Transform>().position = glm::vec3(-3.0f, 3.6f, -3.0f);
        SoftBody cloth;
        cloth.kind = SoftBodyKind::Cloth;
        cloth.size = glm::vec3(2.4f, 2.4f, 0.0f);
        cloth.resolution = glm::ivec3(28, 28, 1);
        cloth.mass = 1.0f;
        cloth.material = MAT_CLOTH;
        curtain.Add<SoftBody>(cloth);

        Entity drapeSphere = SceneBuilder::AddStaticBox(*this, "DrapeSphere", DRAPE_SPHERE_CENTER, glm::vec3(DRAPE_SPHERE_RADIUS * 2.0f), MAT_STONE);
        drapeSphere.Get<MeshRenderer>().mesh = "Sphere";
        drapeSphere.Add<Collider>(sphere);
        Entity drape = CreateEntity("Drape");
        drape.Get<Transform>().position = DRAPE_SPHERE_CENTER + glm::vec3(0.0f, 1.3f, -1.0f);
        drape.Get<Transform>().rotation = glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1, 0, 0));   // horizontal sheet
        SoftBody sheet = cloth;
        sheet.size = glm::vec3(2.0f, 2.0f, 0.0f);
        sheet.resolution = glm::ivec3(26, 26, 1);
        sheet.pinned = false;
        sheet.material = MAT_DRAPE;
        drape.Add<SoftBody>(sheet);

        Entity rope = CreateEntity("Rope");
        rope.Get<Transform>().position = glm::vec3(-0.5f, 4.2f, -1.0f);
        rope.Get<Transform>().rotation = glm::angleAxis(1.0f, glm::vec3(0, 0, 1));   // starts tilted: swings
        SoftBody ropeBody;
        ropeBody.kind = SoftBodyKind::Rope;
        ropeBody.size = glm::vec3(0.0f, 2.6f, 0.0f);
        ropeBody.resolution = glm::ivec3(26, 1, 1);
        ropeBody.mass = 0.5f;
        ropeBody.damping = 0.1f;
        rope.Add<SoftBody>(ropeBody);

        Entity jelly = CreateEntity("Jelly");
        jelly.Get<Transform>().position = glm::vec3(-6.0f, 2.0f, 2.0f);
        SoftBody box;
        box.kind = SoftBodyKind::Box;
        box.size = glm::vec3(0.8f);
        box.resolution = glm::ivec3(3, 3, 3);
        box.mass = 2.0f;
        box.compliance = 1e-4f;
        box.volumeCompliance = 0.0f;
        box.substeps = 16;
        box.material = MAT_SOFT;
        jelly.Add<SoftBody>(box);

        // SDF sculpture (raymarched) + field arrows over the vortex.
        CreateEntity("SdfSculpture").Get<Transform>().position = SDF_CENTER;
        ApplySdfSculpture(*this);
        Render::FieldVisualization arrows;
        arrows.enabled = true;
        arrows.boundsMin = FIELD_BOUNDS_MIN;
        arrows.boundsMax = FIELD_BOUNDS_MAX;
        arrows.resolution = FIELD_RESOLUTION;
        arrows.arrowScale = 0.06f;
        arrows.maxLength = 0.6f;
        Render::SetFieldVisualization(arrows);

        Gameplay::SpawnPlayer(ctx, glm::vec3(0.0f, 0.05f, 8.0f));
        ctx.camera.SetYawPitch(0.0f, -0.12f);
        m_launched = false;
    }

    void FieldLabScene::OnExit(GameContext&) {
        Render::SetSdfVisualization({});
        Render::SetFieldVisualization({});
    }

    void FieldLabScene::Update(GameContext&, float) {
        // Moons start on circular orbits once their bodies exist: v = sqrt(GM / r), tangential.
        if (m_launched) return;
        bool all = true;
        for (int i = 0; i < 3; ++i) {
            Entity moon = FindByName("Moon_" + std::to_string(i));
            if (!moon.Valid() || !moon.GetBody().Valid()) { all = false; continue; }
            const glm::vec3 r = moon.WorldPosition() - PLANET_CENTER;
            const glm::vec3 tangent = glm::normalize(glm::cross(glm::vec3(0, 1, 0), r));
            Physics::SetLinearVelocity(moon.GetBody(), tangent * std::sqrt(PLANET_GM / glm::length(r)));
        }
        m_launched = all;
    }

    void FieldLabScene::DrawUI(GameContext&) {
        DrawViewLabels(*this);
        const Advanced::Stats s = Advanced::GetStats();
        char line[200];
        std::snprintf(line, sizeof(line), "GPU: %u particles, %u soft bodies (%u nodes, %u constraints, %u colours), %u fields, %u colliders",
                      s.particles, s.softBodies, s.softBodyNodes, s.constraints, s.colorsMax, s.fields, s.colliders);
        const glm::vec2 size = glm::vec2(Render::GetFramebufferSize());
        Render::DrawScreenText(line, glm::vec2(16.0f, size.y - 34.0f), glm::vec4(0.6f, 0.9f, 1.0f, 1.0f), 2.0f);
    }
}
