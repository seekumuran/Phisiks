#include "P9Scenes.h"
#include "SceneBuilder.h"
#include "Studio.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace Phisiks;

namespace Lab {
    namespace {
        constexpr const char* MAT_STAGE = "P9/Stage";
        constexpr const char* MAT_DARK = "P9/DarkFloor";
        constexpr const char* MAT_STEEL = "P9/Steel";
        constexpr const char* MAT_WOOD = "P9/Wood";
        constexpr const char* MAT_ORB_A = "P9/OrbCyan";
        constexpr const char* MAT_ORB_B = "P9/OrbMagenta";
        constexpr const char* MAT_ORB_C = "P9/OrbAmber";
        constexpr const char* MAT_NEON = "P9/Neon";
        constexpr const char* MAT_AGENT = "P9/Agent";
        constexpr const char* MAT_CONCRETE = "P9/Concrete";

        std::string F(const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
            char text[256];
            std::snprintf(text, sizeof(text), format, a, b, c, d);
            return text;
        }

        Entity Mesh(Scene& scene, const std::string& name, const char* mesh, const char* material, const glm::vec3& position, const glm::vec3& scale) {
            Entity e = scene.CreateEntity(name);
            e.Get<Transform>().position = position;
            e.Get<Transform>().scale = scale;
            e.Add<MeshRenderer>(MeshRenderer{ mesh, material });
            return e;
        }

        // Dynamic box whose collider is the unit cube scaled by the transform.
        Entity Box(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& size, float mass, const char* material) {
            return SceneBuilder::AddDynamicBox(scene, name, position, size, mass, material);
        }
    }

    void ShowCaption(const std::string& line1, const std::string& line2) {
        if (UI::PanelsVisible()) { Studio::SetCaption(line1, line2); return; }
        const glm::ivec2 size = Render::GetFramebufferSize();
        Render::DrawScreenText(line1, glm::vec2(16.0f, float(size.y) - 58.0f), glm::vec4(0.6f, 0.9f, 1.0f, 1.0f), 2.0f);
        Render::DrawScreenText(line2, glm::vec2(16.0f, float(size.y) - 34.0f), glm::vec4(0.8f, 0.85f, 0.9f, 1.0f), 2.0f);
    }

    // ==================================================================== Animation
    void AnimationScene::OnEnter(GameContext& ctx) {
        SceneBuilder::AddGround(*this, 30.0f, MAT_STAGE, 1.0f);
        SceneBuilder::AddSun(*this, glm::vec3(-0.4f, -1.0f, -0.5f), glm::vec3(1.0f, 0.97f, 0.92f), 2.4f);
        SceneBuilder::AddPointLight(*this, "Fill", glm::vec3(0, 3.0f, 3.0f), glm::vec3(0.6f, 0.7f, 1.0f), 10.0f, 12.0f);
        struct Spec { const char* name; glm::vec3 position; const char* clip; Animation::SkinningMethod method; };
        const Spec specs[] = {
            { "Walker DQ", glm::vec3(-1.4f, 0, 0), "Twist", Animation::SkinningMethod::DualQuaternion },
            { "Walker LBS", glm::vec3(1.4f, 0, 0), "Twist", Animation::SkinningMethod::Linear },
            { "Walker Sway", glm::vec3(0, 0, -2.4f), "Sway", Animation::SkinningMethod::DualQuaternion },
        };
        for (const Spec& s : specs) {
            Entity e = CreateEntity(s.name);
            e.Get<Transform>().position = s.position;
            SkinnedModel& m = e.Add<SkinnedModel>();
            m.path = "models/walker.glb";
            m.clip = s.clip;
            m.method = s.method;
        }
        // pedestals
        Mesh(*this, "Pedestal DQ", "Cube", MAT_STEEL, glm::vec3(-1.4f, -0.04f, 0), glm::vec3(0.9f, 0.1f, 0.9f));
        Mesh(*this, "Pedestal LBS", "Cube", MAT_STEEL, glm::vec3(1.4f, -0.04f, 0), glm::vec3(0.9f, 0.1f, 0.9f));
        ctx.camera.SetOrbit(glm::vec3(0, 1.1f, -0.6f), 6.5f, 0.0f, -0.18f);
        cameraPathAttached = false;
    }

    void AnimationScene::Update(GameContext&, float) {
        if (cameraPathAttached) return;
        Entity camera = FindByName("MainCamera");
        if (!camera) return;
        CameraPath& path = camera.Add<CameraPath>();
        path.type = Animation::SplineType::CatmullRom;
        path.closed = true;
        path.duration = 16.0f;
        path.playing = false;   // studio: "Fly-through" toggle
        for (int i = 0; i < 6; ++i) {
            const float a = float(i) / 6.0f * 6.2831853f;
            path.points.push_back(glm::vec3(std::sin(a) * 6.0f, 1.6f + 0.8f * std::sin(a * 2.0f), std::cos(a) * 6.0f - 0.6f));
            path.lookAt.push_back(glm::vec3(0.0f, 1.0f, -0.6f));
        }
        cameraPathAttached = true;
    }

    void AnimationScene::DrawUI(GameContext&) {
        const Animation::Stats s = Animation::GetStats();
        ShowCaption("glTF skinning: dual quaternions (left) keep the twist volume; linear blending (right) collapses",
                    F("%.0f models, %.0f vertices skinned on the CPU in %.2f ms, %.0f camera path(s)", s.models, s.skinnedVertices, s.skinningMs, s.cameraPaths));
    }

    // ==================================================================== VFX
    void VfxScene::OnEnter(GameContext& ctx) {
        SceneBuilder::AddGround(*this, 30.0f, MAT_DARK, 1.0f);
        SceneBuilder::AddSun(*this, glm::vec3(-0.3f, -1.0f, -0.6f), glm::vec3(0.7f, 0.75f, 0.9f), 0.9f);
        // orbiting emissive orbs with trails and orbit guides
        const char* materials[3] = { MAT_ORB_A, MAT_ORB_B, MAT_ORB_C };
        const glm::vec4 heads[3] = { { 0.4f, 2.4f, 6.0f, 1.0f }, { 5.0f, 0.6f, 4.0f, 1.0f }, { 6.0f, 3.0f, 0.4f, 1.0f } };
        for (int i = 0; i < 3; ++i) {
            Entity centre = CreateEntity("Orbit" + std::to_string(i));
            centre.Get<Transform>().position = glm::vec3(0, 1.4f + 0.4f * float(i), 0);
            centre.Get<Transform>().rotation = glm::angleAxis(0.25f * float(i), glm::vec3(1, 0, 0));
            OrbitPath& o = centre.Add<OrbitPath>();
            o.radiusX = o.radiusZ = 1.6f + 0.9f * float(i);
            o.color = glm::vec4(glm::vec3(heads[i]) * 0.15f, 0.8f);
            o.dash = 0.02f;
            Entity orb = Mesh(*this, "Orb" + std::to_string(i), "Sphere", materials[i], glm::vec3(0.0f), glm::vec3(0.22f));
            Trail& t = orb.Add<Trail>();
            t.headColor = heads[i];
            t.tailColor = glm::vec4(glm::vec3(heads[i]) * 0.2f, 0.0f);
            t.width = 0.14f;
            t.lifetime = 1.4f;
        }
        // pylons and a raycast beam that stops on the crate between them
        Entity a = SceneBuilder::AddStaticBox(*this, "PylonA", glm::vec3(-5.0f, 1.0f, -3.0f), glm::vec3(0.4f, 2.0f, 0.4f), MAT_STEEL);
        SceneBuilder::AddStaticBox(*this, "PylonB", glm::vec3(5.0f, 1.0f, -3.0f), glm::vec3(0.4f, 2.0f, 0.4f), MAT_STEEL);
        Beam& beam = a.Add<Beam>();
        beam.target = "PylonB";
        beam.offset = glm::vec3(0.0f, -0.3f, 0.0f);   // 0.4 m above the floor: the crate blocks it
        beam.raycast = true;
        Box(*this, "BeamCrate", glm::vec3(1.0f, 0.4f, -3.0f), glm::vec3(0.8f), 4.0f, MAT_WOOD);
        // decals
        struct D { const char* name; glm::vec3 p; float size; Render::DecalPattern pattern; Render::DecalBlend blend; glm::vec4 color; };
        const D decals[] = {
            { "Scorch", { -3.0f, 0, 2.5f }, 2.2f, Render::DecalPattern::Scorch, Render::DecalBlend::Multiply, { 0.08f, 0.06f, 0.05f, 0.95f } },
            { "RingGlow", { 0.0f, 0, 2.5f }, 2.4f, Render::DecalPattern::Ring, Render::DecalBlend::Additive, { 0.4f, 1.6f, 4.0f, 1.0f } },
            { "Paint", { 3.0f, 0, 2.5f }, 2.0f, Render::DecalPattern::Paint, Render::DecalBlend::Alpha, { 0.85f, 0.05f, 0.08f, 0.9f } },
            { "Hazard", { 0.0f, 0, 5.5f }, 2.6f, Render::DecalPattern::Hazard, Render::DecalBlend::Alpha, { 0.95f, 0.75f, 0.05f, 0.9f } },
        };
        int seed = 0;
        for (const D& d : decals) {
            Entity e = CreateEntity(std::string("Decal ") + d.name);
            e.Get<Transform>().position = d.p + glm::vec3(0, 0.1f, 0);
            Decal& c = e.Add<Decal>();
            c.size = glm::vec3(d.size, 0.6f, d.size);
            c.pattern = d.pattern;
            c.blend = d.blend;
            c.color = d.color;
            c.seed = float(seed++);
        }
        // SSAO corner: two walls and a stack of boxes in the crease
        SceneBuilder::AddStaticBox(*this, "WallBack", glm::vec3(5.0f, 1.0f, -7.0f), glm::vec3(4.0f, 2.0f, 0.2f), MAT_CONCRETE);
        SceneBuilder::AddStaticBox(*this, "WallSide", glm::vec3(7.1f, 1.0f, -5.0f), glm::vec3(0.2f, 2.0f, 4.0f), MAT_CONCRETE);
        for (int i = 0; i < 4; ++i) SceneBuilder::AddStaticBox(*this, "AOBox" + std::to_string(i), glm::vec3(6.6f - 0.5f * float(i % 2), 0.25f + 0.5f * float(i / 2), -6.6f + 0.55f * float(i % 2)), glm::vec3(0.5f), MAT_CONCRETE);
        // neon sign for bloom
        Mesh(*this, "Neon", "Cube", MAT_NEON, glm::vec3(-4.0f, 2.6f, -6.5f), glm::vec3(3.0f, 0.5f, 0.12f));
        ctx.camera.SetOrbit(glm::vec3(0, 1.0f, 1.0f), 11.0f, 0.15f, -0.32f);
        m_time = 0.0f;
    }

    void VfxScene::Update(GameContext&, float dt) {
        m_time += dt;
        for (int i = 0; i < 3; ++i) {
            Entity centre = FindByName("Orbit" + std::to_string(i));
            Entity orb = FindByName("Orb" + std::to_string(i));
            if (!centre || !orb) continue;
            const float r = 1.6f + 0.9f * float(i);
            const float a = m_time * (1.6f - 0.35f * float(i)) + float(i) * 2.1f;
            const glm::vec3 local(std::cos(a) * r, 0.0f, std::sin(a) * r);
            orb.Get<Transform>().position = centre.WorldPosition() + centre.WorldRotation() * local;
        }
    }

    void VfxScene::DrawUI(GameContext&) {
        const RenderStats r = GetRenderStats();
        const VFX::Stats v = VFX::GetStats();
        ShowCaption("Post FX + VFX: HDR bloom, SSAO, trails / orbit paths, a raycast plasma beam, projected decals",
                    F("bloom %.0f mips, ", r.bloomMips) + (r.ssaoActive ? std::string("SSAO on") : std::string("SSAO off (Medium / High quality)"))
                        + F(", %.0f decals, %.0f ribbons, %.0f trail points", double(r.decalsDrawn), double(r.ribbonsDrawn), double(v.trailPoints)));
    }

    // ==================================================================== Terrain
    void TerrainScene::OnEnter(GameContext& ctx) {
        SceneBuilder::AddSun(*this, glm::vec3(-0.5f, -0.8f, -0.35f), glm::vec3(1.0f, 0.95f, 0.85f), 2.6f);
        Entity ground = CreateEntity("Terrain");
        Terrain& t = ground.Add<Terrain>();
        t.generator.resolution = 129;
        t.generator.cellSize = 0.5f;
        t.generator.height = 7.0f;
        t.generator.seed = 4;
        t.generator.flatRadius = 9.0f;
        t.generator.flatBlend = 6.0f;
        struct S { const char* name; const char* mesh; uint32_t count; float maxSlope; float minH, maxH; float spacing; bool align; float minScale, maxScale; };
        const S scatters[] = {
            { "Pines", "Foliage/Pine", 260, 28.0f, 0.6f, 5.0f, 1.4f, false, 0.8f, 1.5f },
            { "Rocks", "Foliage/Rock", 120, 45.0f, -1.0f, 9.0f, 1.0f, true, 0.4f, 1.2f },
            { "Bushes", "Foliage/Bush", 180, 25.0f, 0.2f, 4.0f, 0.9f, false, 0.6f, 1.1f },
            { "Grass", "Foliage/Grass", 900, 22.0f, -1.0f, 3.0f, 0.35f, false, 0.7f, 1.3f },
        };
        uint32_t seed = 10;
        for (const S& s : scatters) {
            Entity e = CreateEntity(s.name);
            e.SetParent(ground, false);
            FoliageScatter& f = e.Add<FoliageScatter>();
            f.mesh = s.mesh;
            f.scatter.count = s.count;
            f.scatter.seed = seed++;
            f.scatter.maxSlopeDegrees = s.maxSlope;
            f.scatter.minHeight = s.minH;
            f.scatter.maxHeight = s.maxH;
            f.scatter.minSpacing = s.spacing;
            f.scatter.alignToNormal = s.align;
            f.scatter.minScale = s.minScale;
            f.scatter.maxScale = s.maxScale;
            f.scatter.keepOutRadius = 9.5f;
            f.castShadows = std::string(s.mesh) != "Foliage/Grass";
        }
        // obstacles on the flat pad (navigation must route around them)
        SceneBuilder::AddStaticBox(*this, "WallNorth", glm::vec3(0.0f, 0.75f, -2.0f), glm::vec3(5.0f, 1.5f, 0.4f), MAT_CONCRETE);
        SceneBuilder::AddStaticBox(*this, "WallSouth", glm::vec3(1.5f, 0.75f, 2.5f), glm::vec3(3.0f, 1.5f, 0.4f), MAT_CONCRETE);
        Entity surface = CreateEntity("NavSurface");
        NavigationSurface& nav = surface.Add<NavigationSurface>();
        nav.desc.boundsMin = glm::vec3(-12.0f, -1.0f, -12.0f);
        nav.desc.boundsMax = glm::vec3(12.0f, 10.0f, 12.0f);
        nav.desc.cellSize = 0.35f;
        nav.desc.agentRadius = 0.35f;
        nav.desc.maxStepHeight = 0.4f;
        nav.desc.maxSlopeDegrees = 35.0f;
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) / 8.0f * 6.2831853f;
            const glm::vec3 start(std::cos(a) * 7.0f, 0.4f, std::sin(a) * 7.0f);
            Entity agent = Mesh(*this, "Agent" + std::to_string(i), "Sphere", MAT_AGENT, start, glm::vec3(0.7f));
            NavAgent& n = agent.Add<NavAgent>();
            n.heightOffset = 0.35f;
            n.maxSpeed = 2.2f + 0.15f * float(i % 3);
            AI::SetTarget(n, -start);
        }
        // spatial-audio drone: fast circle overhead (Doppler) with a trail
        Entity drone = Mesh(*this, "Drone", "Sphere", MAT_ORB_C, glm::vec3(10.0f, 4.0f, 0.0f), glm::vec3(0.35f));
        AudioSource& audio = drone.Add<AudioSource>();
        audio.clip = "audio/drone_hum.wav";
        audio.loop = true;
        audio.playOnStart = true;
        audio.spatial = true;
        audio.minDistance = 2.0f;
        audio.maxDistance = 60.0f;
        audio.volume = 0.6f;
        Trail& trail = drone.Add<Trail>();
        trail.headColor = glm::vec4(6.0f, 3.0f, 0.4f, 1.0f);
        trail.lifetime = 0.8f;
        ctx.camera.SetOrbit(glm::vec3(0, 1.0f, 0), 22.0f, 0.5f, -0.5f);
        m_time = 0.0f;
        crossings = 0;
    }

    void TerrainScene::Update(GameContext&, float dt) {
        m_time += dt;
        if (Entity drone = FindByName("Drone")) {
            const float a = m_time * 1.1f;   // 11 m/s on a 10 m circle
            drone.Get<Transform>().position = glm::vec3(std::cos(a) * 10.0f, 4.0f + std::sin(a * 2.0f), std::sin(a) * 10.0f);
        }
        Each<NavAgent>([&](Entity, NavAgent& n) {
            if (!n.arrived) return;
            ++crossings;
            AI::SetTarget(n, glm::vec3(-n.target.x, n.target.y, -n.target.z));
        });
    }

    void TerrainScene::DrawUI(GameContext&) {
        const World::Stats w = World::GetStats();
        const AI::Stats a = AI::GetStats();
        ShowCaption("Terrain: splatmap + height-field collision, instanced foliage, nav grid + A* crowd, Doppler drone",
                    F("%.0f foliage instances in %.0f batches, %.0f walkable cells, %.0f agents", w.foliageInstances, w.foliageBatches, a.walkableCells, a.agents)
                        + F(", %.0f crossings, min gap %.2f m", crossings, a.minSeparation));
    }

    // ==================================================================== Joints
    void JointsScene::OnEnter(GameContext& ctx) {
        SceneBuilder::AddGround(*this, 30.0f, MAT_STAGE, 1.0f);
        SceneBuilder::AddSun(*this, glm::vec3(-0.4f, -1.0f, -0.45f), glm::vec3(1.0f, 0.97f, 0.92f), 2.4f);
        SceneBuilder::AddStaticBox(*this, "Beam", glm::vec3(0.0f, 4.2f, 0.0f), glm::vec3(9.0f, 0.3f, 0.3f), MAT_STEEL);
        SceneBuilder::AddStaticBox(*this, "PostL", glm::vec3(-4.4f, 2.1f, 0.0f), glm::vec3(0.3f, 4.2f, 0.3f), MAT_STEEL);
        SceneBuilder::AddStaticBox(*this, "PostR", glm::vec3(4.4f, 2.1f, 0.0f), glm::vec3(0.3f, 4.2f, 0.3f), MAT_STEEL);

        // ball-and-socket chain hanging from the beam, kicked sideways
        std::string previous = "Beam";
        for (int i = 0; i < 5; ++i) {
            Entity link = Box(*this, "Link" + std::to_string(i), glm::vec3(-2.5f, 3.7f - 0.6f * float(i), 0.0f), glm::vec3(0.16f, 0.5f, 0.16f), 0.4f, MAT_WOOD);
            Joint& j = link.Add<Joint>();
            j.type = Physics::JointType::BallSocket;
            j.connectedTo = previous;
            j.anchor = glm::vec3(0.0f, 0.55f, 0.0f);   // top end (local units: the unit cube scaled)
            previous = link.GetName();
        }
        // spring-damper bob
        Entity bob = Box(*this, "SpringBob", glm::vec3(-0.8f, 2.2f, 0.0f), glm::vec3(0.5f), 2.0f, MAT_ORB_B);
        Joint& spring = bob.Add<Joint>();
        spring.type = Physics::JointType::SpringDamper;
        spring.connectedTo = "Beam";
        spring.anchor = glm::vec3(0.0f, 0.5f, 0.0f);
        spring.connectedAnchor = glm::vec3(-0.8f / 9.0f, -0.5f, 0.0f);
        spring.stiffness = 90.0f;
        spring.damping = 1.5f;
        spring.restLength = 1.2f;
        // hinged door (limits) pushed by a script
        Entity door = Box(*this, "Door", glm::vec3(1.2f, 1.06f, 2.5f), glm::vec3(1.2f, 2.0f, 0.1f), 6.0f, MAT_WOOD);
        Joint& hinge = door.Add<Joint>();
        hinge.type = Physics::JointType::Hinge;
        hinge.anchor = glm::vec3(-0.5f, 0.0f, 0.0f);
        hinge.axis = glm::vec3(0.0f, 1.0f, 0.0f);
        hinge.enableLimits = true;
        hinge.lowerLimit = -1.4f;
        hinge.upperLimit = 1.4f;
        door.Add<Script>().source = R"lua(
            function on_fixed_update(self, dt)
                self.t = (self.t or 0) + dt
                if self.t > 2.0 then
                    self.t = 0
                    self.side = -(self.side or 1)
                    self.entity:impulse(0, 0, 14 * self.side)
                end
            end
        )lua";
        // motorised windmill on a world hinge
        Entity rotor = Box(*this, "Windmill", glm::vec3(2.6f, 2.4f, -1.2f), glm::vec3(2.4f, 0.25f, 0.08f), 3.0f, MAT_ORB_A);
        Joint& motor = rotor.Add<Joint>();
        motor.type = Physics::JointType::Hinge;
        motor.axis = glm::vec3(0.0f, 0.0f, 1.0f);
        motor.enableMotor = true;
        motor.motorSpeed = 2.5f;
        motor.maxMotorForce = 300.0f;
        rotor.Get<Rigidbody>().useGravity = false;
        // prismatic elevator: the script reverses the motor at the travel limits
        Entity lift = Box(*this, "Elevator", glm::vec3(-3.4f, 0.4f, 2.4f), glm::vec3(1.0f, 0.15f, 1.0f), 8.0f, MAT_STEEL);
        Joint& slider = lift.Add<Joint>();
        slider.type = Physics::JointType::Prismatic;
        slider.axis = glm::vec3(0.0f, 1.0f, 0.0f);
        slider.enableLimits = true;
        slider.lowerLimit = 0.0f;
        slider.upperLimit = 2.2f;
        slider.enableMotor = true;
        slider.motorSpeed = 0.8f;
        slider.maxMotorForce = 600.0f;
        lift.Add<Script>().source = R"lua(
            function on_fixed_update(self, dt)
                local position, speed = self.entity:joint_state()
                if position > 2.1 then self.entity:joint_motor(-0.8) end
                if position < 0.1 then self.entity:joint_motor(0.8) end
            end
        )lua";
        Box(*this, "Rider", glm::vec3(-3.4f, 0.8f, 2.4f), glm::vec3(0.4f), 1.0f, MAT_WOOD);
        // Lua bumper: flashes and launches whatever hits it; interact (E) drops a ball onto it
        Entity bumper = SceneBuilder::AddStaticBox(*this, "Bumper", glm::vec3(0.0f, 0.3f, -2.5f), glm::vec3(1.2f, 0.6f, 1.2f), MAT_NEON);
        bumper.Add<Script>().source = R"lua(
            hits = 0
            function on_collision(self, other, speed)
                hits = hits + 1
                if other then other:impulse(0, 4 * other:mass(), 0) end
                self.entity:set_color(1.0, 0.2 + 0.1 * (hits % 8), 0.1)
                scene.event("bumper hit " .. hits)
            end
            function on_update(self, dt)
                if input.pressed("interact") then
                    local x, y, z = self.entity:position()
                    scene.spawn("Ball", "Sphere", x, y + 3, z, "sphere", 0.5)
                end
            end
        )lua";
        Entity spinner = SceneBuilder::AddStaticBox(*this, "Spinner", glm::vec3(3.0f, 0.5f, 2.6f), glm::vec3(1.4f, 0.2f, 0.2f), MAT_ORB_C);
        spinner.Add<Rigidbody>().type = Physics::BodyType::Kinematic;
        spinner.Add<Script>().file = "scripts/spinner.lua";
        ctx.camera.SetOrbit(glm::vec3(0, 1.8f, 0.5f), 11.0f, 0.0f, -0.2f);
    }

    void JointsScene::DrawUI(GameContext&) {
        const Physics::Stats p = Physics::GetStats();
        const Scripting::Stats s = Scripting::GetStats();
        ShowCaption("Joints + Lua: ball-socket chain, spring-damper, hinge door (limits), motor windmill, prismatic lift",
                    F("%.0f joints on ", p.joints) + Physics::BackendName() + F(", %.0f Lua scripts running, %.0f errors, %.0f callbacks", s.running, s.errors, double(s.callbacks)));
    }

    // ==================================================================== registration
    void RegisterP9Scenes(SceneManager& scenes) {
        scenes.Register("Animation", [] { return std::make_unique<AnimationScene>(); });
        scenes.Register("VFX", [] { return std::make_unique<VfxScene>(); });
        scenes.Register("Terrain", [] { return std::make_unique<TerrainScene>(); });
        scenes.Register("Joints", [] { return std::make_unique<JointsScene>(); });
    }

    void RegisterP9Materials() {
        auto make = [](const char* name, glm::vec3 base, float roughness, float metallic = 0.0f, glm::vec3 emissive = glm::vec3(0.0f),
                       Render::Pattern pattern = Render::Pattern::None, glm::vec3 accent = glm::vec3(0.0f), float scale = 1.0f) {
            Render::MaterialDesc m;
            m.baseColor = base;
            m.roughness = roughness;
            m.metallic = metallic;
            m.emissive = emissive;
            m.pattern = pattern;
            m.patternColor = accent;
            m.patternScale = scale;
            Render::RegisterMaterial(name, m);
        };
        make(MAT_STAGE, glm::vec3(0.32f, 0.33f, 0.36f), 0.8f, 0.0f, glm::vec3(0.0f), Render::Pattern::Grid, glm::vec3(0.18f, 0.2f, 0.26f), 1.0f);
        make(MAT_DARK, glm::vec3(0.06f, 0.06f, 0.08f), 0.55f, 0.0f, glm::vec3(0.0f), Render::Pattern::Grid, glm::vec3(0.12f, 0.14f, 0.2f), 1.0f);
        make(MAT_STEEL, glm::vec3(0.55f, 0.56f, 0.6f), 0.35f, 1.0f);
        make(MAT_WOOD, glm::vec3(0.45f, 0.28f, 0.14f), 0.7f, 0.0f, glm::vec3(0.0f), Render::Pattern::Wood, glm::vec3(0.3f, 0.17f, 0.08f), 2.0f);
        make(MAT_ORB_A, glm::vec3(0.1f, 0.5f, 0.9f), 0.3f, 0.0f, glm::vec3(0.6f, 2.4f, 6.0f));
        make(MAT_ORB_B, glm::vec3(0.8f, 0.1f, 0.7f), 0.3f, 0.0f, glm::vec3(5.0f, 0.6f, 4.0f));
        make(MAT_ORB_C, glm::vec3(0.9f, 0.6f, 0.1f), 0.3f, 0.0f, glm::vec3(6.0f, 3.0f, 0.4f));
        make(MAT_NEON, glm::vec3(1.0f, 0.2f, 0.8f), 0.4f, 0.0f, glm::vec3(8.0f, 0.8f, 6.0f));
        make(MAT_AGENT, glm::vec3(0.9f, 0.85f, 0.2f), 0.45f, 0.0f, glm::vec3(0.4f, 0.35f, 0.0f));
        make(MAT_CONCRETE, glm::vec3(0.55f, 0.54f, 0.52f), 0.9f, 0.0f, glm::vec3(0.0f), Render::Pattern::Noise, glm::vec3(0.42f, 0.41f, 0.4f), 2.0f);
    }
}
