// Showcase 01 (game development): stunt car & tower demolition.
//
//   * Driveable 4-wheel car: raycast spring-damper suspension (Vehicle.h), friction-circle tyres.
//   * A Lua-scripted boost pad (on_trigger -> impulse) launches it up a kicker ramp into three
//     multi-storey towers of crates and glass panes; pieces hit hard enough shatter into planks /
//     shards (collision events), with HDR sparks (bloom), neon tyre trails and skid-mark decals.
//   * Guard bots patrol on the navigation grid (A* + crowd steering) with spatial sirens; the car
//     engine is a spatial source too: Doppler shift as they pass the camera.
#include "Showcase.h"
#include "ShowcaseTest.h"
#include "Vehicle.h"

#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/Core/InputState.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/VFX/VFX.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <unordered_set>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr float CAR_MASS = 1200.0f;
        constexpr float START_Z = 52.0f;
        constexpr Physics::Layer BOT_LAYER = 3;
        enum class Piece { Crate, Glass, Slab };
        struct Breakable { Piece piece = Piece::Crate; float threshold = 4.0f; };
        struct Fragment { float age = 0.0f; };
        struct Spark { glm::vec3 p, v; float life; };
        struct SkidMark { glm::vec3 p; float yaw; float age; };
        struct PendingVelocity { uint32_t id; glm::vec3 v; };

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }
    }

    class StuntCarScene : public ShowcaseScene {
    public:
        StuntCarScene() : ShowcaseScene("StuntCar") {}

        Vehicle car;
        bool chaseCam = true, nitro = false;
        int cratesBroken = 0, glassShattered = 0, sparksSpawned = 0, skidMarks = 0;
        float airTime = 0.0f, maxAirTime = 0.0f, maxSpeed = 0.0f;
        float sirenPitchMin = 1.0f, sirenPitchMax = 1.0f;
        float throttleIn = 0.0f, steerIn = 0.0f, brakeIn = 0.0f;   // set by input or tests
        bool autoDrive = false;
        std::vector<Spark> sparks;
        std::deque<SkidMark> skids;
        std::vector<PendingVelocity> pending;
        std::vector<int> botWaypoint;
        glm::vec3 camEye{ 0.0f, 4.0f, START_Z + 10.0f };
        float lastSkidDistance = 0.0f, odometer = 0.0f;
        float flippedTime = 0.0f;

        void Build(GameContext& ctx) override {
            Material("Car/Asphalt", { 0.05f, 0.055f, 0.06f }, 0.85f, 0.0f, {}, Render::Pattern::Grid, { 0.25f, 0.22f, 0.05f }, 0.1f, 0.015f);
            Material("Car/Paint", { 0.75f, 0.06f, 0.04f }, 0.25f, 0.6f);
            Material("Car/Cabin", { 0.03f, 0.04f, 0.05f }, 0.08f, 0.9f);
            Material("Car/Tyre", { 0.03f, 0.03f, 0.03f }, 0.9f);
            Material("Car/Ramp", { 0.55f, 0.5f, 0.42f }, 0.7f, 0.0f, {}, Render::Pattern::Tiles, { 0.15f, 0.12f, 0.1f }, 1.0f, 0.03f);
            Material("Car/Crate", { 0.5f, 0.32f, 0.16f }, 0.75f, 0.0f, {}, Render::Pattern::Wood, { 0.32f, 0.2f, 0.09f }, 3.0f);
            Material("Car/Glass", { 0.45f, 0.65f, 0.75f }, 0.04f, 0.3f, { 0.01f, 0.03f, 0.05f });
            Material("Car/Slab", { 0.42f, 0.42f, 0.44f }, 0.8f, 0.0f, {}, Render::Pattern::Noise, { 0.3f, 0.3f, 0.32f }, 2.0f);
            Material("Car/Boost", { 0.05f, 0.3f, 0.4f }, 0.3f, 0.0f, { 0.4f, 2.5f, 4.0f }, Render::Pattern::Grid, { 0.6f, 3.0f, 5.0f }, 1.0f, 0.08f);
            Material("Car/Bot", { 0.15f, 0.16f, 0.2f }, 0.4f, 0.8f, {}, Render::Pattern::Grid, { 0.9f, 0.7f, 0.1f }, 2.0f);
            Material("Car/Siren", { 0.9f, 0.1f, 0.1f }, 0.3f, 0.0f, { 6.0f, 0.4f, 0.3f });
            Material("Car/Wall", { 0.18f, 0.2f, 0.24f }, 0.6f, 0.0f, {}, Render::Pattern::Grid, { 1.2f, 0.9f, 0.15f }, 0.5f, 0.05f);

            Render::RenderSettings& r = Render::Settings();
            r.skyColorZenith = { 0.06f, 0.08f, 0.16f };
            r.skyColorHorizon = { 0.35f, 0.22f, 0.18f };
            r.fog.color = { 0.12f, 0.09f, 0.1f };
            r.fog.distanceDensity = 0.006f;
            r.bloom.intensity = 0.08f;
            r.exposure = 1.1f;

            Ground(200.0f, "Car/Asphalt", 1.0f);
            Sun(glm::vec3(-0.45f, -0.55f, -0.7f), glm::vec3(1.0f, 0.82f, 0.62f), 2.6f);
            // arena walls (static) left / right / back
            Box("WallLeft", { -50.0f, 1.0f, 10.0f }, { 1.0f, 2.0f, 110.0f }, "Car/Wall");
            Box("WallRight", { 50.0f, 1.0f, 10.0f }, { 1.0f, 2.0f, 110.0f }, "Car/Wall");
            Box("WallBack", { 0.0f, 1.0f, -45.0f }, { 100.0f, 2.0f, 1.0f }, "Car/Wall");
            // kicker ramp (14.6 deg) and a landing ramp
            const float angle = std::atan(2.6f / 10.0f);
            Box("KickerRamp", { 0.0f, 1.05f, 19.0f }, { 7.0f, 0.5f, 10.4f }, "Car/Ramp", 0.0f, glm::angleAxis(angle, glm::vec3(1, 0, 0)));
            Box("SideRamp", { -24.0f, 0.6f, 10.0f }, { 5.0f, 0.4f, 8.0f }, "Car/Ramp", 0.0f, glm::angleAxis(0.16f, glm::vec3(1, 0, 0)));
            Box("SideRampB", { 24.0f, 0.6f, 25.0f }, { 5.0f, 0.4f, 8.0f }, "Car/Ramp", 0.0f, glm::angleAxis(-0.16f, glm::vec3(0, 0, 1)) * glm::angleAxis(0.16f, glm::vec3(1, 0, 0)));
            // boost pads: Lua on_trigger scripts
            AddBoostPad("BoostPad1", { 0.0f, 0.0f, 36.0f }, { 0.0f, -1.0f });
            AddBoostPad("BoostPad2", { -24.0f, 0.0f, 24.0f }, { 0.0f, -1.0f });
            AddBoostPad("BoostPad3", { 24.0f, 0.0f, 40.0f }, { 0.0f, -1.0f });
            // towers
            BuildTower("TowerA", { 0.0f, 0.0f, 1.0f }, 0);
            BuildTower("TowerB", { 0.4f, 0.0f, -8.0f }, 1);
            BuildTower("TowerC", { -0.4f, 0.0f, -17.0f }, 2);
            BuildTower("TowerD", { 12.0f, 0.0f, -6.0f }, 1);
            BuildTower("TowerE", { -12.0f, 0.0f, -10.0f }, 0);
            // car
            Entity chassis = Box("Car", { 0.0f, 1.0f, START_Z }, { 1.9f, 0.5f, 4.2f }, "Car/Paint", CAR_MASS);
            chassis.Get<Rigidbody>().angularDamping = 0.6f;
            chassis.Get<Rigidbody>().continuousCollision = true;
            Entity cabin = Visual("CarCabin", "Cube", "Car/Cabin", { 0.0f, 0.42f, 0.25f }, { 0.82f, 0.8f, 0.48f });
            cabin.SetParent(chassis, false);
            Entity spoiler = Visual("CarSpoiler", "Cube", "Car/Cabin", { 0.0f, 0.8f, 0.47f }, { 0.95f, 0.08f, 0.08f });
            spoiler.SetParent(chassis, false);
            AudioSource& engine = chassis.Add<AudioSource>();
            engine.clip = "audio/engine_loop.wav";
            engine.loop = true;
            engine.playOnStart = true;
            engine.spatial = true;
            engine.volume = 0.5f;
            engine.minDistance = 4.0f;
            engine.maxDistance = 90.0f;
            std::vector<WheelDesc> wheels;
            const char* names[4] = { "WheelFL", "WheelFR", "WheelRL", "WheelRR" };
            for (int i = 0; i < 4; ++i) {
                WheelDesc w;
                w.mount = glm::vec3(i % 2 == 0 ? -0.85f : 0.85f, -0.05f, i < 2 ? -1.35f : 1.35f);
                w.radius = 0.36f;
                w.steer = i < 2;
                w.drive = true;
                w.visual = names[i];
                wheels.push_back(w);
                Visual(names[i], "Showcase/Cylinder", "Car/Tyre", chassis.WorldPosition() + w.mount, { 0.72f, 0.3f, 0.72f });
                if (i >= 2) {   // tyre trail emitters at the rear contact patches
                    Entity trail = CreateEntity(std::string("Trail") + names[i]);
                    Trail& t = trail.Add<Trail>();
                    t.lifetime = 1.1f;
                    t.width = 0.22f;
                    t.headColor = glm::vec4(0.2f, 1.6f, 3.2f, 1.0f);
                    t.tailColor = glm::vec4(0.05f, 0.2f, 0.6f, 0.0f);
                    t.minDistance = 0.08f;
                    t.emitting = false;
                }
            }
            VehicleDesc desc;
            desc.stiffness = 30000.0f;
            desc.damping = 2400.0f;
            car.Setup(chassis, desc, wheels);
            // patrol area (navigation grid) with guard bots
            Entity nav = CreateEntity("PatrolArea");
            NavigationSurface& surface = nav.Add<NavigationSurface>();
            surface.desc.boundsMin = glm::vec3(-48.0f, -1.0f, -40.0f);
            surface.desc.boundsMax = glm::vec3(-16.0f, 4.0f, 48.0f);
            surface.desc.cellSize = 0.5f;
            surface.desc.agentRadius = 0.6f;
            surface.desc.layers = Physics::ALL_LAYERS & ~Physics::LayerBit(BOT_LAYER);   // the bots' own colliders are not obstacles
            for (int i = 0; i < 4; ++i) AddBot(i);
            ctx.camera.SetOrbit(glm::vec3(0.0f, 1.0f, START_Z), 10.0f, 0.0f, -0.25f);
            camEye = glm::vec3(0.0f, 4.0f, START_Z + 10.0f);
        }

        void AddBoostPad(const std::string& name, const glm::vec3& at, const glm::vec2& dir) {
            Visual(name + "Glow", "Cube", "Car/Boost", at + glm::vec3(0.0f, 0.03f, 0.0f), { 4.0f, 0.06f, 3.0f });
            Entity pad = CreateEntity(name);
            pad.Get<Transform>().position = at + glm::vec3(0.0f, 0.8f, 0.0f);
            pad.Get<Transform>().scale = glm::vec3(4.0f, 1.6f, 3.0f);
            Collider c;
            c.shape.shape = Physics::ShapeType::Box;
            c.shape.halfExtents = glm::vec3(0.5f);
            c.shape.isTrigger = true;
            pad.Add<Collider>(c);
            char source[1024];
            std::snprintf(source, sizeof(source),
                "-- Boost pad (Lua): a forward + upward kick for the car when it enters the trigger.\n"
                "boosts = 0\n"
                "function on_trigger(self, other, entered)\n"
                "  if not entered or other:name() ~= 'Car' then return end\n"
                "  local m = other:mass()\n"
                "  other:impulse(%.3f * m * 9.0, m * 0.8, %.3f * m * 9.0)\n"
                "  boosts = boosts + 1\n"
                "  scene.event('BOOST x' .. boosts .. ' on ' .. self.entity:name())\n"
                "  self.entity:play_sound('audio/spawn_chime.wav')\n"
                "end\n", double(dir.x), double(dir.y));
            pad.Add<Script>().source = source;
        }

        void BuildTower(const std::string& name, const glm::vec3& base, int style) {
            const int floors = 6;
            float y = 0.0f;
            int n = 0;
            for (int f = 0; f < floors; ++f) {
                const bool glassFloor = style == 1 ? true : (style == 2 ? f % 2 == 1 : false);
                if (!glassFloor) {
                    for (int i = 0; i < 4; ++i) {
                        const glm::vec3 p = base + glm::vec3(i % 2 == 0 ? -0.5f : 0.5f, y + 0.45f, i < 2 ? -0.5f : 0.5f);
                        Entity e = Box(name + "_crate" + std::to_string(n++), p, glm::vec3(0.9f), "Car/Crate", 25.0f);
                        e.Add<Breakable>(Breakable{ Piece::Crate, 4.5f });
                    }
                    y += 0.9f;
                }
                else {
                    const float h = 1.0f;
                    Breakable glass{ Piece::Glass, 2.8f };
                    Box(name + "_glass" + std::to_string(n++), base + glm::vec3(0.0f, y + h * 0.5f, -0.9f), { 2.0f, h, 0.12f }, "Car/Glass", 10.0f).Add<Breakable>(glass);
                    Box(name + "_glass" + std::to_string(n++), base + glm::vec3(0.0f, y + h * 0.5f, 0.9f), { 2.0f, h, 0.12f }, "Car/Glass", 10.0f).Add<Breakable>(glass);
                    Box(name + "_glass" + std::to_string(n++), base + glm::vec3(-0.9f, y + h * 0.5f, 0.0f), { 0.12f, h, 1.64f }, "Car/Glass", 9.0f).Add<Breakable>(glass);
                    Box(name + "_glass" + std::to_string(n++), base + glm::vec3(0.9f, y + h * 0.5f, 0.0f), { 0.12f, h, 1.64f }, "Car/Glass", 9.0f).Add<Breakable>(glass);
                    y += h;
                }
                Entity slab = Box(name + "_slab" + std::to_string(n++), base + glm::vec3(0.0f, y + 0.06f, 0.0f), { 2.1f, 0.12f, 2.1f }, "Car/Slab", 45.0f);
                slab.Add<Breakable>(Breakable{ Piece::Slab, 9.0f });
                y += 0.12f;
            }
        }

        void AddBot(int i) {
            const glm::vec3 start(-40.0f + 6.0f * float(i % 2), 0.0f, -25.0f + 18.0f * float(i));
            Entity bot = Visual("GuardBot" + std::to_string(i), "Showcase/Cylinder", "Car/Bot", start + glm::vec3(0, 0.8f, 0), { 0.9f, 1.6f, 0.9f });
            Collider c;
            c.shape.shape = Physics::ShapeType::Box;
            c.shape.halfExtents = glm::vec3(0.5f);
            c.layer = BOT_LAYER;
            bot.Add<Collider>(c);
            Rigidbody rb;
            rb.type = Physics::BodyType::Kinematic;
            bot.Add<Rigidbody>(rb);
            NavAgent& agent = bot.Add<NavAgent>();
            agent.maxSpeed = 3.0f + 0.4f * float(i);
            agent.radius = 0.55f;
            agent.heightOffset = 0.8f;
            AI::SetTarget(agent, glm::vec3(-22.0f, 0.0f, start.z + (i % 2 ? -14.0f : 14.0f)));
            Entity head = Visual("GuardSiren" + std::to_string(i), "Sphere", "Car/Siren", { 0.0f, 0.62f, 0.0f }, { 0.45f, 0.18f, 0.45f });
            head.SetParent(bot, false);
            Entity light = PointLight("GuardLight" + std::to_string(i), { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.15f, 0.1f }, 9.0f, 25.0f);
            light.SetParent(head, false);
            AudioSource& siren = bot.Add<AudioSource>();
            siren.clip = "audio/siren.wav";
            siren.loop = true;
            siren.playOnStart = true;
            siren.spatial = true;
            siren.volume = 0.22f;
            siren.minDistance = 3.0f;
            siren.maxDistance = 70.0f;
        }

        // ---------------------------------------------------------------- gameplay
        void Tick(GameContext& ctx, float dt) override {
            // input (tests set the inputs directly)
            if (!autoDrive) {
                throttleIn = (ctx.input.Down(Action::MoveForward) ? 1.0f : 0.0f) - (ctx.input.Down(Action::MoveBack) ? 1.0f : 0.0f);
                steerIn = (ctx.input.Down(Action::MoveRight) ? 1.0f : 0.0f) - (ctx.input.Down(Action::MoveLeft) ? 1.0f : 0.0f);
                brakeIn = ctx.input.Down(Action::Jump) ? 1.0f : 0.0f;
                nitro = ctx.input.Down(Action::Sprint);
                if (ctx.input.Pressed(Action::Interact)) ResetCar();
                if (!UI::WantsKeyboard() && GetInputState().WasPressed(Key::C)) chaseCam = !chaseCam;
            }
            ProcessImpacts();
            UpdateSparks(dt);
            UpdateBots(dt);
            // airtime, flip recovery
            const int grounded = car.GroundedWheels();
            const float speed = std::abs(car.ForwardSpeed());
            maxSpeed = std::max(maxSpeed, speed);
            if (grounded == 0) { airTime += dt; maxAirTime = std::max(maxAirTime, airTime); }
            else airTime = 0.0f;
            Entity chassis = car.Chassis();
            const glm::vec3 up = chassis.WorldRotation() * glm::vec3(0, 1, 0);
            flippedTime = (up.y < 0.2f && speed < 2.0f) ? flippedTime + dt : 0.0f;
            if (flippedTime > 2.0f) ResetCar(chassis.WorldPosition() + glm::vec3(0, 1.5f, 0));
            if (chassis.WorldPosition().y < -5.0f) ResetCar();
            // trails + skid marks at the rear contact patches
            const auto& states = car.Wheels();
            odometer += speed * dt;
            for (int i = 2; i < 4; ++i) {
                Entity trail = FindByName(std::string("Trail") + car.WheelDescs()[size_t(i)].visual);
                if (!trail) continue;
                const WheelState& w = states[size_t(i)];
                Trail& t = trail.Get<Trail>();
                t.emitting = w.grounded && speed > 6.0f;
                const float drift = std::clamp(std::abs(w.lateralSlip) / 6.0f, 0.0f, 1.0f);
                t.headColor = glm::mix(glm::vec4(0.2f, 1.6f, 3.2f, 1.0f), glm::vec4(4.0f, 0.4f, 2.6f, 1.0f), drift);
                if (w.grounded) trail.SetWorldPosition(w.contact + glm::vec3(0, 0.04f, 0));
                if (w.grounded && (std::abs(w.lateralSlip) > 2.5f || (brakeIn > 0.5f && speed > 5.0f)) && odometer - lastSkidDistance > 0.35f) {
                    const glm::vec3 fwd = chassis.WorldRotation() * glm::vec3(0, 0, -1);
                    skids.push_back({ w.contact, std::atan2(fwd.x, fwd.z), 0.0f });
                    if (skids.size() > 220) skids.pop_front();
                    ++skidMarks;
                    if (i == 3) lastSkidDistance = odometer;
                }
            }
            for (SkidMark& s : skids) {
                s.age += dt;
                Render::DecalDesc d;
                d.world = glm::translate(glm::mat4(1.0f), s.p) * glm::mat4_cast(glm::angleAxis(s.yaw, glm::vec3(0, 1, 0))) * glm::scale(glm::mat4(1.0f), glm::vec3(0.34f, 0.5f, 0.75f));
                d.pattern = Render::DecalPattern::Scorch;
                d.blend = Render::DecalBlend::Multiply;
                d.color = glm::vec4(0.06f, 0.06f, 0.07f, std::clamp(0.85f - s.age * 0.02f, 0.2f, 0.85f));
                d.seed = s.p.x;
                Render::DrawDecal(d);
            }
            // fragments age out
            std::vector<Entity> expired;
            Each<Fragment>([&](Entity e, Fragment& f) { f.age += dt; if (f.age > 14.0f) expired.push_back(e); });
            for (Entity e : expired) e.Destroy();
            car.UpdateVisuals(dt);
            // siren Doppler (nearest bot)
            Each<AudioSource>([&](Entity e, AudioSource& a) {
                if (a.clip != "audio/siren.wav" || !a.voice) return;
                Audio::SpatialInfo info;
                if (Audio::GetSpatialInfo(a.voice, info) && info.distanceGain > 0.0f) {
                    sirenPitchMin = std::min(sirenPitchMin, info.dopplerPitch);
                    sirenPitchMax = std::max(sirenPitchMax, info.dopplerPitch);
                }
                (void)e;
            });
            // chase camera
            if (chaseCam && ActiveShot() < 0) {
                const glm::vec3 p = chassis.WorldPosition();
                glm::vec3 fwd = chassis.WorldRotation() * glm::vec3(0, 0, -1);
                fwd.y = 0.0f;
                fwd = glm::length(fwd) > 0.1f ? glm::normalize(fwd) : glm::vec3(0, 0, -1);
                const glm::vec3 desired = p - fwd * 9.5f + glm::vec3(0.0f, 3.6f, 0.0f);
                camEye = glm::mix(camEye, desired, 1.0f - std::exp(-dt * 4.0f));
                const glm::vec3 target = p + glm::vec3(0.0f, 1.0f, 0.0f) + fwd * 3.0f;
                ctx.camera.LookAt(camEye, target);
                ctx.camera.SetOrbit(target, glm::length(target - camEye), ctx.camera.GetYaw(), ctx.camera.GetPitch());
            }
        }

        void FixedTick(GameContext&, float dt) override {
            for (const PendingVelocity& pv : pending) {
                Entity e = FromId(pv.id);
                if (e.Valid() && e.GetBody().Valid()) Physics::SetLinearVelocity(e.GetBody(), pv.v);
            }
            pending.clear();
            VehicleDesc& d = car.Desc();
            d.engineForce = nitro ? 15000.0f : 9000.0f;
            car.FixedUpdate(dt, throttleIn, steerIn, brakeIn);
        }

        void ResetCar(glm::vec3 at = glm::vec3(0.0f, 1.0f, START_Z)) {
            car.Reset(at, 0.0f);
            flippedTime = 0.0f;
        }

        void SpawnSparks(const glm::vec3& at, const glm::vec3& normal, int count, float speed) {
            uint32_t seed = uint32_t(sparksSpawned * 2654435761u + 12345u);
            auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
            for (int i = 0; i < count && sparks.size() < 1500; ++i) {
                glm::vec3 dir = glm::normalize(normal * 0.8f + glm::vec3(rnd() - 0.5f, rnd(), rnd() - 0.5f) * 1.6f);
                sparks.push_back({ at, dir * speed * (0.4f + rnd()), 0.5f + 0.6f * rnd() });
            }
            sparksSpawned += count;
        }

        void UpdateSparks(float dt) {
            for (Spark& s : sparks) {
                s.v += glm::vec3(0.0f, -9.81f, 0.0f) * dt;
                s.p += s.v * dt;
                if (s.p.y < 0.02f) { s.p.y = 0.02f; s.v.y = -s.v.y * 0.35f; s.v *= 0.6f; }
                s.life -= dt;
            }
            sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& s) { return s.life <= 0.0f; }), sparks.end());
            for (const Spark& s : sparks) {
                const float heat = std::clamp(s.life, 0.0f, 1.0f);
                std::vector<Render::RibbonPoint> pts(2);
                pts[0] = { s.p, 0.035f, glm::vec4(6.0f * heat + 1.0f, 2.2f * heat + 0.3f, 0.4f * heat, 1.0f) };
                pts[1] = { s.p - s.v * 0.035f, 0.0f, glm::vec4(2.0f, 0.4f, 0.05f, 0.0f) };
                Render::DrawRibbon(pts);
            }
        }

        void Shatter(Entity e, const glm::vec3& contact) {
            const Breakable b = e.Get<Breakable>();
            const glm::vec3 p = e.WorldPosition();
            const glm::quat q = e.WorldRotation();
            const glm::vec3 size = e.Get<Transform>().scale;
            const glm::vec3 v = e.GetLinearVelocity();
            const std::string material = b.piece == Piece::Glass ? "Car/Glass" : (b.piece == Piece::Crate ? "Car/Crate" : "Car/Slab");
            // glass splits along its long side; crates into 8 blocks, slabs into 4 plates
            int nx = 2, ny = 2, nz = 2;
            if (b.piece == Piece::Glass) { const bool alongX = size.x > size.z; nx = alongX ? 3 : 1; nz = alongX ? 1 : 3; }
            if (b.piece == Piece::Slab) ny = 1;
            const glm::vec3 cell = size / glm::vec3(float(nx), float(ny), float(nz));
            int k = 0;
            for (int ix = 0; ix < nx; ++ix) for (int iy = 0; iy < ny; ++iy) for (int iz = 0; iz < nz; ++iz) {
                const glm::vec3 local = (glm::vec3(float(ix), float(iy), float(iz)) + 0.5f) * cell - size * 0.5f;
                const glm::vec3 fragSize = cell * 0.92f;
                const glm::vec3 wp = p + q * local;
                Entity f = Box(e.GetName() + "_f" + std::to_string(k++), wp, fragSize, material, b.piece == Piece::Glass ? 1.5f : 4.0f, q);
                f.Add<Fragment>();
                const glm::vec3 outward = glm::length(wp - contact) > 1e-3f ? glm::normalize(wp - contact) : glm::vec3(0, 1, 0);
                pending.push_back({ f.Id(), v + outward * 2.5f });
            }
            SpawnSparks(contact, glm::vec3(0, 1, 0), b.piece == Piece::Glass ? 18 : 10, b.piece == Piece::Glass ? 6.0f : 4.0f);
            if (b.piece == Piece::Glass) ++glassShattered;
            else if (b.piece == Piece::Crate) ++cratesBroken;
            e.Destroy();
        }

        void ProcessImpacts() {
            std::unordered_set<uint32_t> broken;
            int fragments = 0;
            Each<Fragment>([&](Entity, Fragment&) { ++fragments; });
            const Entity carEntity = car.Chassis();
            for (const Physics::CollisionEvent& ev : Physics::GetCollisionEvents()) {
                if (ev.kind != Physics::CollisionEvent::Kind::Enter) continue;
                Entity a = FromBody(ev.a), b = FromBody(ev.b);
                if ((a == carEntity || b == carEntity) && ev.impactSpeed > 3.0f) SpawnSparks(ev.point, ev.normal * (a == carEntity ? -1.0f : 1.0f), 8, 3.0f + ev.impactSpeed * 0.3f);
                for (Entity e : { a, b }) {
                    if (!e.Valid() || !e.Has<Breakable>() || broken.count(e.Id()) || IsPendingDestroy(e)) continue;
                    const float threshold = e.Get<Breakable>().threshold;
                    const bool byCar = a == carEntity || b == carEntity;
                    if (ev.impactSpeed < threshold * (byCar ? 0.6f : 1.0f) || fragments > 520) continue;
                    broken.insert(e.Id());
                    Shatter(e, ev.point);
                    fragments += 8;
                }
            }
        }

        void UpdateBots(float) {
            static const glm::vec3 waypoints[4] = { { -44.0f, 0, -30.0f }, { -22.0f, 0, -30.0f }, { -22.0f, 0, 40.0f }, { -44.0f, 0, 40.0f } };
            int i = 0;
            if (botWaypoint.size() < 4) botWaypoint.assign(4, 0);
            Each<NavAgent>([&](Entity e, NavAgent& a) {
                if (a.arrived || a.pathFailed) {
                    botWaypoint[size_t(i)] = (botWaypoint[size_t(i)] + (i % 2 ? 3 : 1)) % 4;   // clockwise / anticlockwise loops
                    AI::SetTarget(a, waypoints[botWaypoint[size_t(i)]] + glm::vec3(float(i) * 2.0f, 0.0f, 0.0f));
                }
                if (Entity light = FindByName("GuardLight" + std::to_string(i)); light.Valid()) {
                    const bool red = std::fmod(ElapsedTime() * 2.0f + float(i) * 0.25f, 1.0f) < 0.5f;
                    light.Get<Light>().color = red ? glm::vec3(1.0f, 0.12f, 0.08f) : glm::vec3(0.15f, 0.3f, 1.0f);
                }
                (void)e;
                ++i;
            });
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            return {
                DollyShot("Ramp launch", { -9.0f, 1.2f, 30.0f }, { -7.0f, 3.5f, 10.0f }, { 0.0f, 2.0f, 14.0f }, 9.0f),
                OrbitShot("Tower orbit", { 0.0f, 3.0f, -8.0f }, 16.0f, 5.0f, 14.0f),
                DollyShot("Guard patrol", { -15.0f, 2.0f, 30.0f }, { -15.0f, 2.5f, -20.0f }, { -32.0f, 1.0f, 5.0f }, 12.0f),
                DollyShot("Low tracking", { 3.0f, 0.6f, 48.0f }, { 3.0f, 0.9f, 22.0f }, { 0.0f, 2.0f, -6.0f }, 8.0f),
                OrbitShot("Overhead crane", { 0.0f, 0.0f, 8.0f }, 40.0f, 38.0f, 24.0f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            auto box = [&](const glm::vec3& p, const glm::vec3& half, const glm::quat& q, Render::SdfOperation op = Render::SdfOperation::Union, float k = 0.0f) {
                Render::SdfPrimitive prim;
                prim.shape = Render::SdfShape::RoundBox;
                prim.position = p;
                prim.rotation = q;
                prim.params = glm::vec4(half, 0.08f);
                prim.operation = op;
                prim.smoothness = k;
                sdf.primitives.push_back(prim);
            };
            box({ 0.0f, 1.05f, 19.0f }, { 3.5f, 0.25f, 5.2f }, glm::angleAxis(std::atan(0.26f), glm::vec3(1, 0, 0)));
            for (const glm::vec3& t : { glm::vec3(0, 3.3f, 1), glm::vec3(0.4f, 3.3f, -8), glm::vec3(-0.4f, 3.3f, -17), glm::vec3(12, 3.3f, -6), glm::vec3(-12, 3.3f, -10) })
                box(t, { 1.05f, 3.3f, 1.05f }, glm::quat(1, 0, 0, 0), Render::SdfOperation::Union, 0.6f);
            Render::SdfPrimitive car;
            car.shape = Render::SdfShape::Capsule;
            car.position = this->car.Chassis().WorldPosition();
            car.rotation = this->car.Chassis().WorldRotation() * glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0));
            car.params = glm::vec4(1.6f, 0.7f, 0, 0);
            sdf.primitives.push_back(car);
            sdf.boundsMin = glm::vec3(-20.0f, 0.0f, -25.0f);
            sdf.boundsMax = glm::vec3(20.0f, 9.0f, 60.0f);
            sdf.color = glm::vec3(0.3f, 0.75f, 1.0f);
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.0f, 8.0f, 46.0f); }
        uint32_t CpuEffectParticles() const override { return uint32_t(sparks.size()); }
        const char* Controls() const override { return "W/S throttle  A/D steer  SPACE brake  SHIFT nitro\nE reset car  C chase camera  ESC pause"; }

        void DrawPanel(GameContext& ctx) override {
            char big[32], small[96];
            std::snprintf(big, sizeof(big), "%5.0f KM/H", double(std::abs(car.ForwardSpeed()) * 3.6f));
            std::snprintf(small, sizeof(small), "AIR %.2f s  BEST %.2f s  TOP %.0f km/h", double(airTime), double(maxAirTime), double(maxSpeed * 3.6f));
            Retro::Lcd(big, small);
            const char* labels[4] = { "FL", "FR", "RL", "RR" };
            for (int i = 0; i < 4; ++i) {
                const WheelState& w = car.Wheels()[size_t(i)];
                char v[48];
                std::snprintf(v, sizeof(v), "%.0f mm %.1f kN", double(w.compression * 1000.0f), double(w.suspensionForce / 1000.0f));
                Retro::LedMeter(labels[i], w.compression / car.Desc().restLength, v);
            }
            const Scripting::Stats ls = Scripting::GetStats();
            const AI::Stats ai = AI::GetStats();
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "crates %d  glass %d  sparks %d  skids %d", cratesBroken, glassShattered, sparksSpawned, skidMarks);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "bots %u  paths %u  Lua scripts %u (%u errors)", ai.agents, ai.pathsPlanned, ls.scripts, ls.errors);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "siren Doppler pitch %.3f .. %.3f", double(sirenPitchMin), double(sirenPitchMax));
            Retro::LedToggle("CHASE CAM", &chaseCam);
            ImGui::SameLine();
            if (Retro::Button("RESET CAR")) ResetCar();
            ImGui::SameLine();
            if (Retro::Button("REBUILD")) ctx.scenes->RequestReload();
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("suspension at rest", [this](GameContext& c, ShowcaseTest& t) {
                // forces are applied as impulses each fixed step, so the instantaneous velocity carries one
                // step of gravity: measure the settled height drift over 0.5 s instead
                if (t.StepSeconds(c) < 2.0) { t.Value("y0") = car.Chassis().WorldPosition().y; return false; }
                if (t.StepSeconds(c) < 2.5) return false;
                float mean = 0.0f;
                for (const WheelState& w : car.Wheels()) mean += w.compression * 0.25f;
                const float expected = CAR_MASS * 9.81f / (4.0f * car.Desc().stiffness);
                const double drift = car.Chassis().WorldPosition().y - t.Value("y0");
                t.Expect("4 wheels grounded, static sag = m g / 4k", car.GroundedWheels() == 4 && std::abs(mean - expected) < expected * 0.15f && std::abs(drift) < 0.002,
                         Fmt("compression %.1f mm vs %.1f mm theory, ", mean * 1000.0, expected * 1000.0) + Fmt("chassis height drift %.2f mm over 0.5 s", drift * 1000.0));
                return true;
            });
            test.AddStep("drive, boost pad, ramp jump, demolition", [this](GameContext& c, ShowcaseTest& t) {
                autoDrive = true;
                throttleIn = 1.0f;
                steerIn = 0.0f;
                brakeIn = 0.0f;
                double& speedAtPad = t.Value("padSpeed");
                double& boost = t.Value("boost");
                const glm::vec3 p = car.Chassis().WorldPosition();
                if (p.z > 42.0f) speedAtPad = std::abs(car.ForwardSpeed());
                if (p.z < 33.0f && p.z > 30.0f) boost = std::max(boost, double(std::abs(car.ForwardSpeed())) - speedAtPad);
                const bool done = (cratesBroken + glassShattered >= 6 && t.StepSeconds(c) > 6.0) || t.StepSeconds(c) > 14.0;
                if (t.Frame() % 30 == 0) t.Screenshot("drive_" + std::to_string(t.Frame() / 30));
                if (!done) return false;
                bool boostEvent = false;
                for (const std::string& line : c.eventLog) boostEvent = boostEvent || line.find("BOOST") != std::string::npos;
                const Scripting::Stats ls = Scripting::GetStats();
                t.Expect("Lua boost pad fires on_trigger -> impulse", boostEvent && boost > 4.0 && ls.errors == 0,
                         Fmt("speed +%.1f m/s across the pad, %.0f Lua scripts, ", boost, ls.scripts) + Fmt("%.0f errors", ls.errors));
                t.Expect("ramp jump (all wheels off the ground)", maxAirTime > 0.4f, Fmt("longest airtime %.2f s, top speed %.1f km/h", maxAirTime, maxSpeed * 3.6));
                t.Expect("towers demolished (collision events -> fragments)", cratesBroken + glassShattered >= 4,
                         Fmt("%.0f crates broken, %.0f glass panes shattered", cratesBroken, glassShattered));
                const RenderStats r = GetRenderStats();
                t.Expect("HDR sparks + bloom + tyre trails", sparksSpawned > 40 && r.bloomMips > 0 && r.ribbonsDrawn > 0,
                         Fmt("%.0f sparks spawned, bloom %.0f mips, %.0f ribbons this frame", sparksSpawned, r.bloomMips, r.ribbonsDrawn));
                autoDrive = false;
                throttleIn = 0.0f;
                return true;
            }, 40.0);
            test.AddStep("drift: skid decals + trails", [this](GameContext& c, ShowcaseTest& t) {
                autoDrive = true;
                if (t.Frame() == 0) ResetCar(glm::vec3(12.0f, 1.0f, 48.0f));   // open tarmac, clear of the ramps
                throttleIn = 1.0f;
                steerIn = t.StepSeconds(c) > 2.0 ? 1.0f : 0.0f;
                brakeIn = (t.StepSeconds(c) > 2.0 && t.StepSeconds(c) < 2.6) ? 1.0f : 0.0f;
                if (t.StepSeconds(c) < 4.5) return false;
                const VFX::Stats v = VFX::GetStats();
                const RenderStats r = GetRenderStats();
                t.Expect("hard cornering leaves skid decals and neon tyre trails", skidMarks > 5 && v.trails >= 2 && r.decalsDrawn > 0,
                         Fmt("%.0f skid marks, %.0f decals drawn, %.0f trail points, ", skidMarks, r.decalsDrawn, v.trailPoints) + Fmt("speed %.1f m/s, %.0f wheels grounded, ", car.ForwardSpeed(), car.GroundedWheels()) + Fmt("car at (%.1f, %.1f)", car.Chassis().WorldPosition().x, car.Chassis().WorldPosition().z));
                autoDrive = false;
                throttleIn = steerIn = brakeIn = 0.0f;
                ResetCar();
                return true;
            }, 20.0);
            test.AddStep("guard bots patrol (A* + crowd) with Doppler sirens", [this](GameContext& c, ShowcaseTest& t) {
                if (t.StepSeconds(c) < 3.0) return false;
                const AI::Stats ai = AI::GetStats();
                float minTravel = 1e9f;
                Each<NavAgent>([&](Entity, NavAgent& a) { minTravel = std::min(minTravel, a.travelled); });
                t.Expect("4 bots on the nav grid, paths planned, none failed", ai.agents == 4 && ai.pathsPlanned >= 4 && ai.pathFailures == 0 && minTravel > 3.0f,
                         Fmt("%.0f walkable cells, %.0f paths, ", ai.walkableCells, ai.pathsPlanned) + Fmt("least travelled %.1f m", minTravel));
                t.Expect("spatial siren pitch shifts (Doppler)", sirenPitchMax - sirenPitchMin > 0.004f,
                         Fmt("siren pitch %.4f .. %.4f", sirenPitchMin, sirenPitchMax));
                return true;
            }, 20.0);
        }
    };

    void RegisterStuntCar(SceneManager& scenes) { scenes.Register("StuntCar", [] { return std::make_unique<StuntCarScene>(); }); }
}
