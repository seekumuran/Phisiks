// Showcase 05 (AI & computer vision): autonomous-driving LiDAR & segmentation proving ground.
//
//   * A signalised 4-way intersection: kinematic traffic obeying the lights (stop lines, gaps),
//     pedestrians on the navigation grid (A* + crowd avoidance), buildings, poles, trees.
//   * The ego car carries a 360-degree spinning LiDAR: 16 channels (-15 .. +15 deg) x 720 azimuth
//     steps at 5 Hz, cast as physics raycasts (either backend), drawn as a distance-coloured
//     point cloud. Every point keeps the entity it hit (ground-truth labels).
//   * Split-screen: the left half is the camera image, the right half the same view rendered as an
//     instance-segmentation mask (Camera::entityIdColors, live on the GPU).
//   * --headless --export-vision --export-dir <dir>: batch export of aligned RGB + linear depth +
//     world normals + entity-id mask + camera CSV (engine Recorder), plus LiDAR sweeps as CSV and a
//     class table (id, name, class) written by this demo.
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr int CHANNELS = 16, AZIMUTHS = 720;
        constexpr float LIDAR_RANGE = 60.0f, SWEEP_PERIOD = 0.2f;
        constexpr Physics::Layer MOVER_LAYER = 4;    // cars and pedestrians: not obstacles for the nav grid
        constexpr float STOP_LINE = 9.5f, LANE = 2.4f, LOOP = 62.0f;

        struct LidarPoint { glm::vec3 p{ 0.0f }; float distance = 0.0f; uint32_t entity = 0; bool hit = false; };
        struct TrafficCar { uint32_t id; int axis; float dir; float s; float speed; float cruise; float minSpeedAtRed = 99.0f; float distance = 0.0f; };

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }

        glm::vec3 Turbo(float t) {   // compact rainbow for distance colouring
            t = std::clamp(t, 0.0f, 1.0f);
            return glm::clamp(glm::vec3(1.5f - std::abs(4.0f * t - 3.0f), 1.5f - std::abs(4.0f * t - 2.0f), 1.5f - std::abs(4.0f * t - 1.0f)), 0.0f, 1.0f);
        }

        const char* ClassOf(const std::string& name) {
            if (name.rfind("Car", 0) == 0 || name.rfind("Ego", 0) == 0) return "vehicle";
            if (name.rfind("Ped", 0) == 0) return "pedestrian";
            if (name.rfind("Building", 0) == 0) return "building";
            if (name.rfind("Road", 0) == 0 || name.rfind("Crosswalk", 0) == 0 || name.rfind("Lane", 0) == 0) return "road";
            if (name.rfind("Sidewalk", 0) == 0) return "sidewalk";
            if (name.rfind("Tree", 0) == 0) return "vegetation";
            if (name.rfind("Pole", 0) == 0 || name.rfind("Signal", 0) == 0) return "pole";
            if (name == "Ground") return "terrain";
            return "other";
        }
    }

    class VisionScene : public ShowcaseScene {
    public:
        VisionScene() : ShowcaseScene("Vision") {}

        std::array<LidarPoint, CHANNELS * AZIMUTHS> cloud{};
        float sweepPhase = 0.0f;
        int nextColumn = 0;
        uint32_t sweeps = 0, raysCast = 0;
        bool showCloud = true, splitScreen = true;
        std::vector<TrafficCar> cars;
        float signalTime = 0.0f;
        int exportedSweeps = 0;
        bool classesWritten = false;

        bool GreenFor(int axis) const {
            const float t = std::fmod(signalTime, 16.0f);
            return axis == 0 ? t < 7.0f : (t >= 8.0f && t < 15.0f);
        }

        void Build(GameContext& ctx) override {
            Material("Vis/Asphalt", { 0.06f, 0.065f, 0.07f }, 0.8f);
            Material("Vis/Sidewalk", { 0.45f, 0.44f, 0.42f }, 0.85f, 0.0f, {}, Render::Pattern::Tiles, { 0.3f, 0.3f, 0.3f }, 1.0f, 0.03f);
            Material("Vis/Paint", { 0.85f, 0.85f, 0.8f }, 0.6f, 0.0f, { 0.15f, 0.15f, 0.14f });
            Material("Vis/Grass", { 0.12f, 0.25f, 0.08f }, 0.95f);
            const glm::vec3 carColors[4] = { { 0.7f, 0.08f, 0.06f }, { 0.08f, 0.25f, 0.7f }, { 0.8f, 0.8f, 0.82f }, { 0.12f, 0.12f, 0.13f } };
            for (int i = 0; i < 4; ++i) Material("Vis/Car" + std::to_string(i), carColors[i], 0.3f, 0.6f);
            Material("Vis/Ego", { 0.95f, 0.55f, 0.05f }, 0.35f, 0.4f);
            Material("Vis/Sensor", { 0.05f, 0.05f, 0.06f }, 0.2f, 0.8f, { 0.0f, 0.6f, 1.2f });
            Material("Vis/Ped", { 0.3f, 0.5f, 0.35f }, 0.7f);
            Material("Vis/Building", { 0.35f, 0.37f, 0.42f }, 0.6f, 0.3f, {}, Render::Pattern::Grid, { 0.7f, 0.8f, 0.9f }, 0.35f, 0.08f);
            Material("Vis/Pole", { 0.2f, 0.2f, 0.22f }, 0.4f, 0.9f);
            Material("Vis/Red", { 0.3f, 0.02f, 0.02f }, 0.3f, 0.0f, { 6.0f, 0.2f, 0.1f });
            Material("Vis/Green", { 0.02f, 0.3f, 0.05f }, 0.3f, 0.0f, { 0.2f, 5.0f, 0.6f });
            Material("Vis/Off", { 0.04f, 0.04f, 0.04f }, 0.3f);
            Material("Vis/Leaves", { 0.1f, 0.3f, 0.08f }, 0.8f);
            Render::RenderSettings& r = Render::Settings();
            r.skyColorZenith = { 0.15f, 0.3f, 0.6f };
            r.skyColorHorizon = { 0.6f, 0.68f, 0.78f };
            r.ambientUp = { 0.35f, 0.38f, 0.45f };
            r.fog.color = { 0.55f, 0.6f, 0.68f };
            r.fog.distanceDensity = 0.006f;

            Ground(200.0f, "Vis/Grass", 1.0f);
            Sun(glm::vec3(-0.35f, -0.85f, -0.4f), glm::vec3(1.0f, 0.97f, 0.92f), 3.0f);
            Box("RoadNS", { 0.0f, 0.01f, 0.0f }, { 2.0f * 7.0f, 0.02f, 2.0f * LOOP + 20.0f }, "Vis/Asphalt");
            Box("RoadEW", { 0.0f, 0.012f, 0.0f }, { 2.0f * LOOP + 20.0f, 0.02f, 2.0f * 7.0f }, "Vis/Asphalt");
            for (int i = -12; i <= 12; ++i) {   // centre lane dashes (skipping the box junction)
                if (std::abs(i) < 2) continue;
                Visual("LaneNS" + std::to_string(i), "Cube", "Vis/Paint", { 0.0f, 0.03f, float(i) * 5.0f }, { 0.15f, 0.01f, 2.5f });
                Visual("LaneEW" + std::to_string(i), "Cube", "Vis/Paint", { float(i) * 5.0f, 0.03f, 0.0f }, { 2.5f, 0.01f, 0.15f });
            }
            for (int side = 0; side < 4; ++side)
                for (int k = 0; k < 6; ++k) {
                    const float o = -5.0f + 2.0f * float(k);
                    const glm::vec3 p = side == 0 ? glm::vec3(o, 0.03f, -8.5f) : side == 1 ? glm::vec3(o, 0.03f, 8.5f) : side == 2 ? glm::vec3(-8.5f, 0.03f, o) : glm::vec3(8.5f, 0.03f, o);
                    Visual("Crosswalk" + std::to_string(side * 6 + k), "Cube", "Vis/Paint", p, side < 2 ? glm::vec3(1.0f, 0.01f, 3.0f) : glm::vec3(3.0f, 0.01f, 1.0f));
                }
            // sidewalk blocks, buildings, trees, poles at the four corners
            int b = 0;
            for (int qx = -1; qx <= 1; qx += 2) for (int qz = -1; qz <= 1; qz += 2) {
                const glm::vec3 corner(float(qx) * 38.5f, 0.0f, float(qz) * 38.5f);
                Box("Sidewalk" + std::to_string(b), corner + glm::vec3(0.0f, 0.075f, 0.0f), { 63.0f, 0.15f, 63.0f }, "Vis/Sidewalk");
                for (int i = 0; i < 3; ++i) {
                    const float height = 12.0f + float((b * 3 + i) % 4) * 6.0f;
                    const glm::vec3 p(float(qx) * (16.0f + 11.0f * float(i)), height * 0.5f, float(qz) * (18.0f + 3.0f * float(i % 2)));
                    Box("Building" + std::to_string(b * 3 + i), p, { 9.0f, height, 9.0f }, "Vis/Building");
                }
                for (int i = 0; i < 3; ++i) {
                    const glm::vec3 p(float(qx) * (10.0f + 5.0f * float(i)), 0.0f, float(qz) * 9.5f);
                    Box("TreeTrunk" + std::to_string(b * 3 + i), p + glm::vec3(0.0f, 1.2f, 0.0f), { 0.3f, 2.4f, 0.3f }, "Vis/Pole");
                    Ball("TreeCrown" + std::to_string(b * 3 + i), p + glm::vec3(0.0f, 3.2f, 0.0f), 1.3f, "Vis/Leaves");
                }
                // signal pole on the corner, facing the approaching lanes
                const glm::vec3 pole(float(qx) * 8.2f, 0.0f, float(qz) * 8.2f);
                Box("PoleSignal" + std::to_string(b), pole + glm::vec3(0.0f, 2.2f, 0.0f), { 0.2f, 4.4f, 0.2f }, "Vis/Pole");
                for (int axis = 0; axis < 2; ++axis) {
                    Visual("SignalRed" + std::to_string(b * 2 + axis), "Sphere", "Vis/Off", pole + glm::vec3(axis ? 0.0f : 0.0f, 4.2f, 0.0f) + (axis ? glm::vec3(0, 0, -0.25f * float(qz)) : glm::vec3(-0.25f * float(qx), 0, 0)), glm::vec3(0.32f));
                    Visual("SignalGreen" + std::to_string(b * 2 + axis), "Sphere", "Vis/Off", pole + glm::vec3(0.0f, 3.75f, 0.0f) + (axis ? glm::vec3(0, 0, -0.25f * float(qz)) : glm::vec3(-0.25f * float(qx), 0, 0)), glm::vec3(0.32f));
                }
                ++b;
            }
            // traffic: two cars per approach lane
            cars.clear();
            int n = 0;
            for (int axis = 0; axis < 2; ++axis) for (int d = 0; d < 2; ++d) for (int k = 0; k < 2; ++k) {
                TrafficCar car;
                car.axis = axis;
                car.dir = d == 0 ? 1.0f : -1.0f;
                car.s = -50.0f + 22.0f * float(k) + 7.0f * float(axis + d);
                car.cruise = 8.0f + 1.5f * float((n * 7) % 3);
                car.speed = car.cruise;
                Entity e = MakeCar("Car" + std::to_string(n), "Vis/Car" + std::to_string(n % 4));
                car.id = e.Id();
                cars.push_back(car);
                ++n;
            }
            // the ego vehicle with the spinning LiDAR (eastbound lane)
            {
                TrafficCar ego;
                ego.axis = 1;
                ego.dir = 1.0f;
                ego.s = -30.0f;
                ego.cruise = 7.0f;
                ego.speed = 7.0f;
                Entity e = MakeCar("EgoCar", "Vis/Ego");
                Entity sensor = Visual("EgoLidar", "Showcase/Cylinder", "Vis/Sensor", { 0.0f, 1.25f, 0.0f }, glm::vec3(0.35f / 1.9f, 0.25f / 1.0f, 0.35f / 4.3f));
                sensor.SetParent(e, false);
                ego.id = e.Id();
                cars.push_back(ego);
            }
            // pedestrians on the navigation grid
            Entity nav = CreateEntity("Walkways");
            NavigationSurface& surface = nav.Add<NavigationSurface>();
            surface.desc.boundsMin = glm::vec3(-16.0f, -1.0f, -16.0f);
            surface.desc.boundsMax = glm::vec3(16.0f, 4.0f, 16.0f);
            surface.desc.cellSize = 0.5f;
            surface.desc.agentRadius = 0.3f;
            surface.desc.layers = Physics::ALL_LAYERS & ~Physics::LayerBit(MOVER_LAYER);
            for (int i = 0; i < 10; ++i) {
                const glm::vec3 start(float((i % 2) * 2 - 1) * (11.0f + float(i % 3) * 0.9f), 0.15f, float(((i / 2) % 2) * 2 - 1) * (11.0f + float(i % 4) * 0.4f));   // clear of the trees and buildings
                Entity ped = Visual("Ped" + std::to_string(i), "Showcase/Cylinder", "Vis/Ped", start + glm::vec3(0, 0.85f, 0), { 0.5f, 1.7f, 0.5f });
                Collider c;
                c.shape.shape = Physics::ShapeType::Box;
                c.shape.halfExtents = glm::vec3(0.5f);
                c.layer = MOVER_LAYER;
                ped.Add<Collider>(c);
                Rigidbody rb;
                rb.type = Physics::BodyType::Kinematic;
                ped.Add<Rigidbody>(rb);
                NavAgent& a = ped.Add<NavAgent>();
                a.maxSpeed = 1.3f + 0.1f * float(i % 4);
                a.radius = 0.3f;
                a.heightOffset = 0.85f;
                AI::SetTarget(a, -start);
            }
            // segmentation camera (right half of the screen)
            Entity seg = CreateEntity("SegmentationCamera");
            Camera& cam = seg.Add<Camera>();
            cam.target = "Segmentation";
            cam.targetSize = glm::ivec2(640, 720);
            cam.entityIdColors = true;
            cam.primary = false;
            cam.order = -1;
            cam.farPlane = 300.0f;
            ctx.camera.SetOrbit(glm::vec3(0.0f, 1.0f, 0.0f), 34.0f, 0.75f, -0.5f);
            sweeps = 0;
            classesWritten = false;
        }

        Entity MakeCar(const std::string& name, const std::string& material) {
            Entity e = Box(name, { 0.0f, 0.75f, -100.0f }, { 1.9f, 1.0f, 4.3f }, material);
            e.Get<Collider>().layer = MOVER_LAYER;
            Rigidbody rb;
            rb.type = Physics::BodyType::Kinematic;
            e.Add<Rigidbody>(rb);
            Entity cabin = Visual(name + "Cabin", "Cube", material, glm::vec3(0.0f, 0.85f, 0.08f), glm::vec3(0.85f, 0.75f, 0.5f));
            cabin.SetParent(e, false);
            return e;
        }

        glm::vec3 LanePosition(const TrafficCar& c) const {
            // right-hand traffic: +Z movers on x = -LANE, -Z on +LANE; +X on z = +LANE, -X on -LANE
            if (c.axis == 0) return glm::vec3(c.dir > 0.0f ? -LANE : LANE, 0.75f, c.s * c.dir);
            return glm::vec3(c.s * c.dir, 0.75f, c.dir > 0.0f ? LANE : -LANE);
        }

        void UpdateTraffic(float dt) {
            signalTime += dt;
            for (TrafficCar& c : cars) {
                float target = c.cruise;
                const bool green = GreenFor(c.axis);
                if (!green && c.s < -STOP_LINE && c.s > -STOP_LINE - 18.0f) target = std::min(target, std::max(0.0f, (-STOP_LINE - c.s - 0.5f) * 0.9f));
                for (const TrafficCar& o : cars) {   // keep a gap to the car ahead in the same lane
                    if (&o == &c || o.axis != c.axis || o.dir != c.dir) continue;
                    float gap = o.s - c.s;
                    if (gap < 0.0f) gap += 2.0f * LOOP;
                    if (gap < 14.0f) target = std::min(target, std::max(0.0f, (gap - 7.0f) * 0.9f));
                }
                c.speed += std::clamp(target - c.speed, -7.0f * dt, 3.5f * dt);
                c.s += c.speed * dt;
                c.distance += c.speed * dt;
                if (c.s > LOOP) c.s -= 2.0f * LOOP;
                if (!green && c.s < -STOP_LINE && c.s > -STOP_LINE - 4.0f) c.minSpeedAtRed = std::min(c.minSpeedAtRed, c.speed);
                if (Entity e = FromId(c.id); e.Valid()) {
                    Transform& t = e.Get<Transform>();
                    t.position = LanePosition(c);
                    t.rotation = glm::quatLookAt(c.axis == 0 ? glm::vec3(0, 0, c.dir) : glm::vec3(c.dir, 0, 0), glm::vec3(0, 1, 0));
                }
            }
            // signal lamps
            for (int i = 0; i < 8; ++i) {
                const bool green = GreenFor(i % 2);
                if (Entity r = FindByName("SignalRed" + std::to_string(i)); r.Valid()) r.Get<MeshRenderer>().material = green ? "Vis/Off" : "Vis/Red";
                if (Entity g = FindByName("SignalGreen" + std::to_string(i)); g.Valid()) g.Get<MeshRenderer>().material = green ? "Vis/Green" : "Vis/Off";
            }
        }

        // 360-degree LiDAR: cast the azimuth columns this frame's share of the 5 Hz revolution.
        void UpdateLidar(float dt) {
            Entity ego = FindByName("EgoCar");
            if (!ego.Valid()) return;
            const glm::vec3 origin = ego.WorldPosition() + glm::vec3(0.0f, 1.35f, 0.0f);
            const float heading = std::atan2(-(ego.WorldRotation() * glm::vec3(0, 0, -1)).x, -(ego.WorldRotation() * glm::vec3(0, 0, -1)).z);
            Physics::QueryOptions options;
            options.ignoreBody = ego.GetBody();
            sweepPhase += dt / SWEEP_PERIOD * float(AZIMUTHS);
            int columns = int(sweepPhase);
            sweepPhase -= float(columns);
            columns = std::min(columns, AZIMUTHS);
            for (int k = 0; k < columns; ++k) {
                const int a = nextColumn;
                const float az = heading + float(a) / float(AZIMUTHS) * 6.2831853f;
                for (int ch = 0; ch < CHANNELS; ++ch) {
                    const float el = glm::radians(-15.0f + 2.0f * float(ch));
                    const glm::vec3 dir(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
                    LidarPoint& pt = cloud[size_t(ch * AZIMUTHS + a)];
                    const auto hit = Physics::Raycast(origin, dir, LIDAR_RANGE, options);
                    ++raysCast;
                    pt.hit = hit.has_value();
                    if (hit) { pt.p = hit->point; pt.distance = hit->distance; pt.entity = uint32_t(hit->userData); }
                }
                if (++nextColumn == AZIMUTHS) { nextColumn = 0; ++sweeps; OnSweepComplete(); }
            }
        }

        void OnSweepComplete() {
            if (!ExportVision() || ExportDirectory().empty()) return;
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(ExportDirectory() + "/vision", ec);
            if (!classesWritten) {
                classesWritten = true;
                if (std::FILE* f = std::fopen((ExportDirectory() + "/vision/classes.csv").c_str(), "wb")) {
                    std::fputs("id,name,class\n", f);
                    Each<MeshRenderer>([&](Entity e, MeshRenderer&) { std::fprintf(f, "%u,%s,%s\n", e.Id() + 1, e.GetName().c_str(), ClassOf(e.GetName())); });
                    std::fclose(f);
                }
            }
            char name[64];
            std::snprintf(name, sizeof(name), "/vision/lidar_sweep_%04d.csv", exportedSweeps++);
            WriteSweep(ExportDirectory() + name);
        }

        size_t WriteSweep(const std::string& path) const {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return 0;
            std::fputs("x,y,z,distance,channel,azimuth_step,entity_id,class\n", f);
            size_t rows = 0;
            for (int ch = 0; ch < CHANNELS; ++ch) for (int a = 0; a < AZIMUTHS; ++a) {
                const LidarPoint& p = cloud[size_t(ch * AZIMUTHS + a)];
                if (!p.hit) continue;
                const Entity e = p.entity ? FromId(p.entity - 1) : Entity();
                std::fprintf(f, "%.3f,%.3f,%.3f,%.3f,%d,%d,%u,%s\n", p.p.x, p.p.y, p.p.z, p.distance, ch, a, p.entity, e.Valid() ? ClassOf(e.GetName()) : "unknown");
                ++rows;
            }
            std::fclose(f);
            return rows;
        }

        void Tick(GameContext& ctx, float dt) override {
            UpdateTraffic(dt);
            UpdateLidar(dt);
            // pedestrians walk back and forth across the junction
            int i = 0;
            Each<NavAgent>([&](Entity e, NavAgent& a) {
                if (a.arrived || a.pathFailed) AI::SetTarget(a, glm::vec3(-a.target.x, 0.15f, (i % 2 ? 1.0f : -1.0f) * a.target.z));
                (void)e;
                ++i;
            });
            // split screen: camera image left, live instance mask right (same pose, one frame behind)
            const bool split = splitScreen && UI::IsActive() && GetViewMode() != ViewMode::Cad;
            if (Entity main = FindByName("MainCamera"); main.Valid() && main.Has<Camera>() && GetViewMode() != ViewMode::Cad)
                main.Get<Camera>().viewport = split ? glm::vec4(0.0f, 0.0f, 0.5f, 1.0f) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            if (Entity seg = FindByName("SegmentationCamera"); seg.Valid()) {
                seg.Get<Camera>().enabled = split || segmentationForTest;
                if (Entity main = FindByName("MainCamera"); main.Valid()) seg.SetWorldPose(main.WorldPosition(), main.WorldRotation());
            }
            if (showCloud) {
                for (const LidarPoint& p : cloud) {
                    if (!p.hit) continue;
                    const glm::vec3 c = Turbo(p.distance / 40.0f) * 2.2f;
                    Render::DrawLine(p.p, p.p + glm::vec3(0.0f, 0.06f, 0.0f), glm::vec4(c, 1.0f));
                }
            }
            (void)ctx;
        }

        bool segmentationForTest = false;
        uint32_t PointCount() const { uint32_t n = 0; for (const LidarPoint& p : cloud) n += p.hit ? 1u : 0u; return n; }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            Entity ego = FindByName("EgoCar");
            const glm::vec3 e = ego.Valid() ? ego.WorldPosition() : glm::vec3(0.0f);
            return {
                OrbitShot("Junction orbit", { 0.0f, 0.0f, 0.0f }, 32.0f, 16.0f, 24.0f),
                DollyShot("Ego follow", e + glm::vec3(-12.0f, 4.0f, 4.0f), e + glm::vec3(6.0f, 3.0f, 6.0f), e + glm::vec3(6.0f, 0.5f, 0.0f), 8.0f),
                DollyShot("Crosswalk", { -14.0f, 1.7f, 12.0f }, { 14.0f, 1.7f, 12.0f }, { 0.0f, 1.0f, 0.0f }, 10.0f),
                DollyShot("LiDAR top-down", { 0.0f, 55.0f, 1.0f }, { 0.0f, 40.0f, 0.5f }, { 0.0f, 0.0f, 0.0f }, 8.0f),
                DollyShot("Street level", { 3.5f, 1.2f, 45.0f }, { 3.5f, 1.4f, 15.0f }, { 0.0f, 2.0f, -20.0f }, 9.0f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            for (const TrafficCar& c : cars) {   // vehicle "safety capsules" (what a planner would keep clear)
                Entity e = FromId(c.id);
                if (!e.Valid() || sdf.primitives.size() >= 12) continue;
                Render::SdfPrimitive p;
                p.shape = Render::SdfShape::Capsule;
                p.position = e.WorldPosition() + glm::vec3(0.0f, 0.3f, 0.0f);
                p.rotation = e.WorldRotation() * glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0));
                p.params = glm::vec4(2.2f, 1.4f, 0, 0);
                sdf.primitives.push_back(p);
            }
            sdf.boundsMin = glm::vec3(-60.0f, 0.0f, -60.0f);
            sdf.boundsMax = glm::vec3(60.0f, 3.0f, 60.0f);
            sdf.color = glm::vec3(1.0f, 0.4f, 0.2f);
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(0.0f, 1.0f, 0.0f, 0.8f);
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.0f, 0.0f, 32.0f); }
        uint32_t CpuEffectParticles() const override { return PointCount(); }
        const char* Controls() const override { return "RMB orbit, MMB pan, wheel zoom\nLeft: camera  Right: live instance mask"; }

        void DrawPanel(GameContext&) override {
            char big[32], small[96];
            std::snprintf(big, sizeof(big), "%6u PTS", PointCount());
            std::snprintf(small, sizeof(small), "LIDAR 16 x 720 @ 5 Hz  sweep %u", sweeps);
            Retro::Lcd(big, small);
            Retro::LedToggle("POINT CLOUD", &showCloud);
            ImGui::SameLine();
            Retro::LedToggle("SPLIT MASK", &splitScreen);
            const AI::Stats ai = AI::GetStats();
            int moving = 0;
            for (const TrafficCar& c : cars) moving += c.speed > 0.5f ? 1 : 0;
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "signal: N-S %s  E-W %s", GreenFor(0) ? "GREEN" : "RED", GreenFor(1) ? "GREEN" : "RED");
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "traffic %d / %zu moving, pedestrians %u", moving, cars.size(), ai.agents);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "rays cast %u", raysCast);
            ImGui::TextDisabled("export: --headless --export-vision --export-dir <dir>");
            // the right half of the window shows the mask
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            const uint32_t texture = Render::GetTargetTexture("Segmentation");
            if (texture && splitScreen && GetViewMode() != ViewMode::Cad)
                ImGui::GetBackgroundDrawList()->AddImage(ImTextureID(uintptr_t(texture)), ImVec2(display.x * 0.5f, 0.0f), display, ImVec2(0, 1), ImVec2(1, 0));
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("360-degree LiDAR point cloud", [this](GameContext&, ShowcaseTest& t) {
                if (sweeps < 3) return false;
                uint32_t ground = 0, vehicles = 0, buildings = 0;
                double groundErr = 0.0;
                for (const LidarPoint& p : cloud) {
                    if (!p.hit || !p.entity) continue;
                    const Entity e = FromId(p.entity - 1);
                    if (!e.Valid()) continue;
                    const std::string cls = ClassOf(e.GetName());
                    if (cls == "road") { ++ground; groundErr = std::max(groundErr, double(std::abs(p.p.y - (e.WorldPosition().y + e.Get<Transform>().scale.y * 0.5f)))); }
                    if (cls == "vehicle" || cls == "pedestrian") ++vehicles;
                    if (cls == "building") ++buildings;
                }
                const uint32_t n = PointCount();
                t.Expect("full sweeps of 16 x 720 rays, labelled hits", sweeps >= 3 && n > 4000 && ground > 500 && buildings > 100,
                         Fmt("%.0f points this sweep (%.0f road, %.0f building, ", n, ground, buildings) + Fmt("%.0f vehicle / pedestrian), %.0f sweeps", vehicles, sweeps));
                t.Expect("road hits lie on the road surface", groundErr < 0.005, Fmt("worst road-point height error %.4f m", groundErr));
                const size_t rows = WriteSweep(t.RunDirectory() + "/lidar_sweep.csv");
                t.Expect("sweep exported as CSV (x, y, z, distance, channel, azimuth, id, class)", rows == n, Fmt("%.0f rows written", double(rows)));
                return true;
            }, 20.0);
            test.AddStep("live instance-segmentation mask", [this](GameContext&, ShowcaseTest& t) {
                segmentationForTest = true;
                if (t.Frame() < 4) return false;
                Render::FrameCapture cap;
                if (!Render::CaptureTarget("Segmentation", cap) || cap.width == 0) { t.Expect("segmentation target", false, "no capture"); segmentationForTest = false; return true; }
                size_t match = 0, foreground = 0;
                std::set<uint32_t> ids;
                for (size_t p = 0; p < cap.entityIds.size(); ++p) {
                    const glm::u8vec3 c = Render::EntityIdColor(cap.entityIds[p]);
                    if (std::abs(int(cap.rgba[p * 4]) - int(c.r)) <= 1 && std::abs(int(cap.rgba[p * 4 + 1]) - int(c.g)) <= 1 && std::abs(int(cap.rgba[p * 4 + 2]) - int(c.b)) <= 1) ++match;
                    if (cap.entityIds[p]) { ++foreground; ids.insert(cap.entityIds[p]); }
                }
                const double fraction = double(match) / double(std::max<size_t>(cap.entityIds.size(), 1));
                t.Expect("every pixel's false colour encodes its entity id (GPU = CPU hash)", fraction > 0.999 && ids.size() >= 10,
                         Fmt("%.4f of %.0f pixels match, %.0f distinct instances", fraction, double(cap.entityIds.size()), double(ids.size())));
                segmentationForTest = false;
                return true;
            });
            test.AddStep("traffic lights, traffic and pedestrians", [this](GameContext& c, ShowcaseTest& t) {
                if (t.StepSeconds(c) < 18.0) return false;
                int stopped = 0;
                float minDistance = 1e9f;
                for (const TrafficCar& car : cars) { stopped += car.minSpeedAtRed < 0.3f ? 1 : 0; minDistance = std::min(minDistance, car.distance); }
                const AI::Stats ai = AI::GetStats();
                t.Expect("cars stop at red lights and drive on green", stopped >= 2 && minDistance > 20.0f, Fmt("%.0f cars waited at a red light, every car drove > %.0f m", stopped, minDistance));
                float walked = 1e9f;
                Each<NavAgent>([&](Entity e, NavAgent& a) { walked = std::min(walked, a.travelled); (void)e; });
                t.Expect("pedestrians cross on the navigation grid", ai.agents == 10 && ai.pathFailures == 0 && walked > 5.0f,
                         Fmt("%.0f pedestrians, %.0f paths, least walked %.1f m", ai.agents, ai.pathsPlanned, walked));
                return true;
            }, 40.0);
            test.AddStep("synthetic vision G-buffer (export path)", [](GameContext&, ShowcaseTest& t) {
                if (t.Phase() == 0) { Render::RequestVisionCapture(); t.Phase() = 1; return false; }
                Render::VisionCapture v;
                if (!Render::GetVisionCapture(v)) return t.Frame() > 10;
                size_t agree = 0, labelled = 0;
                double normalErr = 0.0;
                for (size_t p = 0; p < v.entityIds.size(); ++p) {
                    const bool hasId = v.entityIds[p] != 0, hasDepth = v.linearDepth[p] > 0.0f;
                    agree += hasId == hasDepth ? 1 : 0;
                    if (hasId) { ++labelled; normalErr = std::max(normalErr, double(std::abs(glm::length(v.normals[p]) - 1.0f))); }
                }
                const double a = double(agree) / double(std::max<size_t>(v.entityIds.size(), 1));
                t.Expect("aligned RGB / depth / normals / id buffers", v.width > 0 && v.rgba.size() == v.entityIds.size() * 4 && a > 0.98 && normalErr < 0.01,
                         Fmt("%.0f x %.0f, depth/id agreement %.4f, ", v.width, v.height, a) + Fmt("worst normal length error %.4f, %.0f labelled pixels", normalErr, double(labelled)));
                return true;
            });
        }
    };

    void RegisterVision(SceneManager& scenes) { scenes.Register("Vision", [] { return std::make_unique<VisionScene>(); }); }
}
