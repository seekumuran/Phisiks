#include "OldMcDonaldScene.h"
#include "OldMcDonaldSpec.h"
#include "ProjectKit.h"
#include "DroneAssembly.h"
#include "RoverAssembly.h"
#include "Showcase.h"
#include "Editor.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace S = OldMcDonald::Spec;
namespace Kit = Projects::ProjectKit;
namespace RA = Projects::RoverAssembly;
namespace DA = Projects::DroneAssembly;

namespace Projects {
    namespace {
        constexpr float PI = Kit::PI;
        const std::string MESH = "meshes/oldmcdonald/";
        const char* const ROOT = "Old-McDonald";
        const char* const GREENHOUSE = "1 Greenhouse";
        const char* const BEDS = "2 Growing Beds";
        const char* const DRONE = "3 Mapping Drone";
        const char* const ROVER = "4 Soil Rover";
        const char* const GRID = "5 Sampling Grid";
        const char* const ROUTE = "6 Mission Route";
        const char* const ROVER_ROUTE = "Rover route (A* plan)";
        const char* const DRONE_PATH = "Drone survey path";
        const char* const ARM = RA::ARM;
        const char* const LIDAR = RA::LIDAR;
        const char* const SENSORS = RA::SENSORS;
        const char* const NAV = "Navigation surface";
        const auto& JOINTS = RA::JOINTS;
        const auto& WHEELS = RA::WHEELS;

        const char* const STAGES[9] = {
            "Drone surveys the greenhouse", "Drone maps crop locations", "Mapping data transferred and processed",
            "Fixed soil-sampling grid generated", "Rover begins its autonomous mission", "Rover visits the sampling locations",
            "Arm positions the soil sensors", "Measurements collected", "Rover returns to the dock" };
        // What drives each stage: engine computation (A* route, inverse kinematics) or a timed script.
        const char* const STAGE_SOURCE[9] = { "scripted", "scripted", "scripted", "scripted", "A*", "A*", "IK", "scripted", "A*" };

        const char* const PRESET_NAMES[OldMcDonaldScene::PresetCount] = {
            "Perspective", "Drone close", "Rover close", "Overview", "Top plan", "Drone explode", "Rover explode", "Mission" };

        float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
        uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
        float Rand01(uint32_t seed) { return float(Hash(seed) & 0xFFFFFF) / float(0xFFFFFF); }

        void Mat(const std::string& name, glm::vec3 base, float roughness, float metallic = 0.0f, glm::vec3 emissive = glm::vec3(0.0f),
                 float opacity = 1.0f, int pattern = 0, glm::vec3 patternColor = glm::vec3(0.0f), float patternScale = 1.0f, float lineWidth = 0.04f) {
            Kit::Material(name, base, roughness, metallic, emissive, opacity, pattern, patternColor, patternScale, lineWidth);
        }

        void RegisterMaterials() {
            RA::RegisterMaterials();
            DA::RegisterMaterials();
            const int NOISE = 3, WOOD = 4, GRIDP = 2;
            Mat("OMD/Studio", { 0.74f, 0.75f, 0.77f }, 0.9f, 0.0f, glm::vec3(0.0f), 1.0f, GRIDP, { 0.62f, 0.64f, 0.68f }, 1.0f, 0.012f);
            Mat("OMD/Concrete", { 0.56f, 0.56f, 0.55f }, 0.85f, 0.0f, glm::vec3(0.0f), 1.0f, NOISE, { 0.47f, 0.47f, 0.46f }, 6.0f);
            Mat("OMD/Glass", { 0.80f, 0.90f, 0.92f }, 0.05f, 0.0f, glm::vec3(0.0f), 0.07f);
            Mat("OMD/Timber", { 0.40f, 0.26f, 0.15f }, 0.75f, 0.0f, glm::vec3(0.0f), 1.0f, WOOD, { 0.36f, 0.23f, 0.12f }, 3.0f);
            Mat("OMD/Soil", { 0.095f, 0.062f, 0.040f }, 0.95f, 0.0f, glm::vec3(0.0f), 1.0f, NOISE, { 0.055f, 0.036f, 0.024f }, 14.0f);
            Mat("OMD/LeafA", { 0.16f, 0.42f, 0.10f }, 0.55f);
            Mat("OMD/LeafB", { 0.24f, 0.50f, 0.13f }, 0.55f);
            Mat("OMD/CropRing", { 0.45f, 0.46f, 0.48f }, 0.6f);
            Mat("OMD/CropRingMapped", { 0.05f, 0.75f, 0.62f }, 0.4f, 0.0f, { 0.02f, 0.30f, 0.24f });
            Mat("OMD/SamplePending", { 0.90f, 0.90f, 0.88f }, 0.5f);
            Mat("OMD/SampleActive", { 1.00f, 0.62f, 0.05f }, 0.4f, 0.0f, { 0.9f, 0.45f, 0.02f });
            Mat("OMD/SampleDone", { 0.15f, 0.70f, 0.25f }, 0.4f, 0.0f, { 0.05f, 0.35f, 0.10f });
            Mat("OMD/Stake", { 0.92f, 0.92f, 0.90f }, 0.55f);
            Mat("OMD/GridLine", { 0.95f, 0.45f, 0.08f }, 0.6f, 0.0f, { 0.25f, 0.10f, 0.01f });
            Mat("OMD/DockPaint", { 0.95f, 0.75f, 0.05f }, 0.6f, 0.0f, { 0.10f, 0.08f, 0.0f });
            Mat("OMD/PadMat", { 0.10f, 0.11f, 0.12f }, 0.8f);
            Mat("OMD/PadPaint", { 0.92f, 0.92f, 0.92f }, 0.6f);
            Mat("OMD/RouteRover", { 1.00f, 0.65f, 0.05f }, 0.5f, 0.0f, { 0.8f, 0.45f, 0.02f });
            Mat("OMD/RouteDrone", { 0.20f, 0.75f, 1.00f }, 0.5f, 0.0f, { 0.12f, 0.45f, 0.70f });
            // drone, rover and shared hardware materials: DroneAssembly / RoverAssembly::RegisterMaterials()
        }

        Entity Part(Scene& s, const std::string& name, Entity parent, const std::string& file, const std::string& material,
                    const glm::vec3& position = glm::vec3(0.0f), bool castShadows = true) {
            return Kit::Part(s, name, parent, MESH + file, material, position, castShadows);
        }
        Entity AddCollider(Scene& s, const std::string& name, Entity parent, const glm::vec3& position, const glm::vec3& half, const glm::vec3& localOffset = glm::vec3(0.0f)) {
            Entity e = Kit::Group(s, name, parent, position);
            Phisiks::Collider c;
            c.shape.shape = Physics::ShapeType::Box;
            c.shape.halfExtents = half;
            c.shape.localPosition = localOffset;
            e.Add<Phisiks::Collider>(c);
            return e;
        }
        glm::quat Yaw(float a) { return glm::angleAxis(a, glm::vec3(0, 1, 0)); }   // +X -> (cos a, 0, -sin a)
        // Painted / rod segment from a to b with a built-in cube (route lines).
        Entity Segment(Scene& s, const std::string& name, Entity parent, const glm::vec3& a, const glm::vec3& b, float width, float height, const char* material) {
            const glm::vec3 d = b - a;
            const float len = glm::length(d);
            Entity e = Kit::Part(s, name, parent, "Cube", material, (a + b) * 0.5f, false);
            Transform& t = e.Get<Transform>();
            if (std::abs(d.y) > 0.5f * len) { t.rotation = Kit::FromY(d); t.scale = glm::vec3(width, len, width); }
            else { t.rotation = Yaw(std::atan2(-d.z, d.x)); t.scale = glm::vec3(len, height, width); }
            return e;
        }
        std::string SampleName(int index) {
            char text[16];
            std::snprintf(text, sizeof(text), "SP-%c%d", S::ROW_NAME[index / S::GRID_COLS], index % S::GRID_COLS + 1);
            return text;
        }

        struct PresetView { glm::vec3 target; float distance, yaw, pitch; };
    }

    // ==================================================================== lifecycle
    void OldMcDonaldScene::OnEnter(GameContext& ctx) {
        m_savedSettings = Render::Settings();
        m_savedFov = ctx.camera.Settings().verticalFovRadians;
        m_savedMinDistance = ctx.camera.Settings().orbitMinDistance;
        m_savedOverlay = ctx.overlayVisible;
        ctx.camera.Settings().verticalFovRadians = 0.72f;
        ctx.camera.Settings().orbitMinDistance = 0.15f;
        ctx.overlayVisible = false;
        ctx.crosshairVisible = false;   // inspection scene: no first-person crosshair / aim label
        RegisterMaterials();
        ApplyEnvironment();
        Build();
        SetArm(StowPose());
        Showcase::SetCadInk(cadEdges, 0.45f, 1.0f);
        m_cameraNearSet = false;
        ApplyCamera(ctx, Presentation);
    }

    void OldMcDonaldScene::OnExit(GameContext& ctx) {
        Showcase::SetCadInk(false);
        Render::Settings() = m_savedSettings;
        ctx.camera.Settings().verticalFovRadians = m_savedFov;
        ctx.camera.Settings().orbitMinDistance = m_savedMinDistance;
        ctx.overlayVisible = m_savedOverlay;
        ctx.crosshairVisible = true;
    }

    void OldMcDonaldScene::ApplyEnvironment() {
        Render::RenderSettings& r = Render::Settings();
        r.skyColorZenith = { 0.52f, 0.60f, 0.70f };
        r.skyColorHorizon = { 0.70f, 0.72f, 0.74f };
        r.groundColor = { 0.50f, 0.51f, 0.53f };
        r.ambientUp = { 0.40f, 0.43f, 0.48f };
        r.ambientDown = { 0.22f, 0.22f, 0.21f };
        r.ambientIntensity = 1.0f;
        r.rimColor = { 0.95f, 0.95f, 1.0f };
        r.rimIntensity = 0.05f;
        r.fog.enabled = true;
        r.fog.color = r.skyColorHorizon;
        r.fog.distanceDensity = 0.012f;
        r.fog.heightDensity = 0.0f;
        r.fog.maxOpacity = 0.7f;
        r.exposure = 1.0f;
        r.bloom.threshold = 2.0f;
        r.bloom.intensity = 0.03f;
        r.ssao.radius = 0.12f;
        r.ssao.bias = 0.01f;
        r.shadowsEnabled = true;
        r.shadowDistance = 16.0f;
        r.clearColor = glm::vec4(r.skyColorHorizon, 1.0f);
    }

    // ==================================================================== build
    void OldMcDonaldScene::Build() {
        Scene& s = *this;
        m_crops.clear();
        m_explodeParts[0].clear();
        m_explodeParts[1].clear();
        m_explode[0] = m_explode[1] = m_explodeTarget[0] = m_explodeTarget[1] = 0.0f;

        // ---------------------------------------------------------------- studio
        Entity studio = Kit::Group(s, "Studio", Entity());
        {
            Entity cyc = Kit::Part(s, "Cyclorama", studio, "meshes/hero/cyclorama.obj", "OMD/Studio", glm::vec3(0.0f, -0.12f, -0.6f), false);
            cyc.Get<Transform>().scale = glm::vec3(1.2f);
            auto light = [&](const char* name, LightType type, glm::vec3 position, glm::vec3 color, float intensity, bool shadows) {
                Entity e = Kit::Group(s, name, studio, position);
                const glm::vec3 d = glm::normalize(-position);
                e.Get<Transform>().rotation = glm::quatLookAt(d, glm::vec3(0, 1, 0));
                Light l;
                l.type = type;
                l.color = color;
                l.intensity = intensity;
                l.castShadows = shadows;
                e.Add<Light>(l);
            };
            light("Sun", LightType::Directional, { 3.0f, 6.0f, 4.0f }, { 1.0f, 0.96f, 0.88f }, 2.6f, true);
            light("Sky fill", LightType::Directional, { -4.0f, 5.0f, -3.0f }, { 0.75f, 0.82f, 1.0f }, 0.55f, false);
        }

        Entity root = Kit::Group(s, ROOT, Entity());

        // ---------------------------------------------------------------- 1 greenhouse
        {
            Entity gh = Kit::Group(s, GREENHOUSE, root);
            Entity slab = Part(s, "Floor slab", gh, "gh_slab.obj", "OMD/Concrete");
            {
                Phisiks::Collider c;
                c.shape.halfExtents = glm::vec3(S::GH_HALF_X + S::SLAB_MARGIN, 0.06f, S::GH_HALF_Z + S::SLAB_MARGIN);
                c.shape.localPosition = glm::vec3(0.0f, -0.06f, 0.0f);
                slab.Add<Phisiks::Collider>(c);
            }
            Part(s, "Frame", gh, "gh_frame.obj", "OMD/Aluminium");
            Part(s, "Wall glazing", gh, "gh_glass_walls.obj", "OMD/Glass", glm::vec3(0.0f), false);
            Part(s, "Roof glazing", gh, "gh_glass_roof.obj", "OMD/Glass", glm::vec3(0.0f), false);
            Entity walls = Kit::Group(s, "Wall colliders", gh);
            AddCollider(s, "North wall collider", walls, { 0.0f, 0.5f, -S::GH_HALF_Z }, { S::GH_HALF_X, 0.5f, 0.05f });
            AddCollider(s, "South wall collider", walls, { 0.0f, 0.5f, S::GH_HALF_Z }, { S::GH_HALF_X, 0.5f, 0.05f });
            AddCollider(s, "West wall collider", walls, { -S::GH_HALF_X, 0.5f, 0.0f }, { 0.05f, 0.5f, S::GH_HALF_Z });
            AddCollider(s, "East wall collider", walls, { S::GH_HALF_X, 0.5f, 0.0f }, { 0.05f, 0.5f, S::GH_HALF_Z });
            Entity dock = Kit::Group(s, "Rover dock", gh, S::DOCK);
            Part(s, "Dock bay marking", dock, "dock_marking.obj", "OMD/DockPaint", glm::vec3(0.0f), false);
            Part(s, "Dock marker post", dock, "dock_post.obj", "OMD/DockPaint", { -S::DOCK_HALF.x - 0.06f, 0.0f, -S::DOCK_HALF.z + 0.02f });
            Entity pad = Kit::Group(s, "Drone landing pad", gh, S::PAD);
            Part(s, "Pad mat", pad, "pad_mat.obj", "OMD/PadMat", glm::vec3(0.0f), false);
            Part(s, "Pad marking", pad, "pad_marking.obj", "OMD/PadPaint", glm::vec3(0.0f), false);
        }

        // ---------------------------------------------------------------- 2 growing beds + crops
        {
            Entity beds = Kit::Group(s, BEDS, root);
            for (int b = 0; b < S::BEDS; ++b) {
                const std::string bedName = "Bed " + std::to_string(b + 1);
                Entity bed = Kit::Group(s, bedName, beds, { S::BED_CX, 0.0f, S::BED_Z[b] });
                Phisiks::Collider c;
                c.shape.halfExtents = glm::vec3(S::BED_HALF_X, 0.5f * S::BED_HEIGHT, S::BED_HALF_Z);
                c.shape.localPosition = glm::vec3(0.0f, 0.5f * S::BED_HEIGHT, 0.0f);
                bed.Add<Phisiks::Collider>(c);
                Part(s, "Timber frame", bed, "bed_frame.obj", "OMD/Timber");
                Part(s, "Soil", bed, "bed_soil.obj", "OMD/Soil");
                Entity crops = Kit::Group(s, "Crops " + std::to_string(b + 1), bed);
                for (int row = 0; row < 2; ++row) {
                    for (int k = 0; k < S::PLANTS_PER_ROW; ++k) {
                        char name[32];
                        std::snprintf(name, sizeof(name), "Crop %d-%d-%02d", b + 1, row + 1, k + 1);
                        const glm::vec3 local(S::PLANT_X0 + S::PLANT_DX * float(k) - S::BED_CX, S::SOIL_TOP, (row ? 1.0f : -1.0f) * S::PLANT_ROW_DZ);
                        Entity crop = Kit::Group(s, name, crops, local);
                        const uint32_t seed = uint32_t(b * 100 + row * 20 + k);
                        Entity plant = Part(s, "Plant", crop, Rand01(seed) < 0.5f ? "plant_a.obj" : "plant_b.obj", Rand01(seed + 7) < 0.5f ? "OMD/LeafA" : "OMD/LeafB");
                        plant.Get<Transform>().rotation = Yaw(Rand01(seed + 3) * 2.0f * PI);
                        plant.Get<Transform>().scale = glm::vec3(0.85f + 0.3f * Rand01(seed + 5));
                        const std::string ring = std::string(name) + " location";
                        Part(s, ring, crop, "crop_ring.obj", "OMD/CropRing", glm::vec3(0.0f), false);
                        m_crops.push_back(ring);
                    }
                }
            }
        }

        // ---------------------------------------------------------------- 3 mapping drone (on its pad): the shared assembly
        {
            DA::Result d = DA::Build(s, root, DRONE, S::PAD + glm::vec3(0.0f, 0.012f, 0.0f), glm::quat(1, 0, 0, 0));
            for (const DA::ExplodePart& p : d.explode) m_explodeParts[DroneAssembly].push_back({ p.entity, p.offset });
        }

        // ---------------------------------------------------------------- 4 soil rover (in its dock): the shared assembly
        {
            // NavAgent frame: -Z = direction of travel; the rover body inside it is +X forward
            RA::Result r = RA::Build(s, root, ROVER, S::DOCK, Yaw(-0.5f * PI), Yaw(0.5f * PI));
            for (const RA::ExplodePart& p : r.explode) m_explodeParts[RoverAssembly].push_back({ p.entity, p.offset });
        }

        // ---------------------------------------------------------------- 5 fixed sampling grid
        {
            Entity grid = Kit::Group(s, GRID, root);
            for (int i = 0; i < S::GRID_ROWS * S::GRID_COLS; ++i) {
                const std::string name = SampleName(i);
                Entity sp = Kit::Group(s, name, grid, SamplePoint(i));
                Part(s, name + " disc", sp, "sample_disc.obj", "OMD/SamplePending", glm::vec3(0.0f), false);
                Part(s, name + " stake", sp, "sample_stake.obj", "OMD/Stake", { 0.05f, 0.0f, 0.0f });
            }
            Entity lines = Kit::Group(s, "Grid lines", grid);
            for (int r = 0; r < S::GRID_ROWS; ++r) {
                const float y = S::SOIL_TOP + 0.003f, z = S::SAMPLE_Z[r];
                Segment(s, std::string("Row ") + S::ROW_NAME[r], lines, { S::SAMPLE_X[0] - 0.12f, y, z }, { S::SAMPLE_X[S::GRID_COLS - 1] + 0.12f, y, z }, 0.006f, 0.002f, "OMD/GridLine");
            }
        }

        // ---------------------------------------------------------------- 6 mission route (drone path now, rover route once the nav grid exists)
        {
            Entity route = Kit::Group(s, ROUTE, root);
            Kit::Group(s, ROVER_ROUTE, route);
            Entity dp = Kit::Group(s, DRONE_PATH, route);
            const float H = S::SURVEY_HEIGHT;
            // lawnmower passes along the beds; lead-in / lead-out points keep every pass straight under the spline
            m_surveyPoints = { { S::PAD.x, H, S::PAD.z }, { -2.45f, H, -1.4f }, { -2.15f, H, -1.4f }, { 2.25f, H, -1.4f }, { 2.55f, H, -1.4f },
                               { 2.55f, H, 0.0f }, { 2.25f, H, 0.0f }, { -2.15f, H, 0.0f }, { -2.45f, H, 0.0f },
                               { -2.45f, H, 1.4f }, { -2.15f, H, 1.4f }, { 2.25f, H, 1.4f }, { 2.55f, H, 1.4f },
                               { 2.55f, H, 1.82f }, { S::PAD.x, H, 1.82f }, { S::PAD.x, H, S::PAD.z } };
            Animation::Spline spline;
            spline.Build(Animation::SplineType::CatmullRom, m_surveyPoints, false, 24);
            float total = 0.0f, toFirstPass = 0.0f;
            for (size_t i = 1; i < m_surveyPoints.size(); ++i) {
                total += glm::length(m_surveyPoints[i] - m_surveyPoints[i - 1]);
                if (i == 1) toFirstPass = total;
            }
            m_surveyMapStart = toFirstPass / total;
            const int n = 72;
            for (int i = 0; i < n; i += 2) {   // dashed
                char name[32];
                std::snprintf(name, sizeof(name), "Survey dash %02d", i / 2 + 1);
                Segment(s, name, dp, spline.Evaluate(float(i) / float(n)), spline.Evaluate(float(i + 1) / float(n)), 0.012f, 0.012f, "OMD/RouteDrone");
            }
            Segment(s, "Take-off / landing", dp, S::PAD + glm::vec3(0.0f, 0.35f, 0.0f), S::PAD + glm::vec3(0.0f, H, 0.0f), 0.012f, 0.012f, "OMD/RouteDrone");
        }

        Kit::Group(s, NAV, root);
        EnsureRuntime();

        // mission stops: aisle A eastbound (rows A + B), aisle B westbound (rows C + D)
        m_stops.clear();
        for (int aisle = 0; aisle < 2; ++aisle) {
            const float heading = aisle == 0 ? 1.0f : -1.0f;
            for (int k = 0; k < S::GRID_COLS; ++k) {
                const int c = aisle == 0 ? k : S::GRID_COLS - 1 - k;
                Stop stop;
                stop.position = glm::vec3(S::SAMPLE_X[c] - heading * S::RV_ARM_BASE.x, 0.0f, S::AISLE_Z[aisle]);
                for (int r = 0; r < S::GRID_ROWS; ++r) if (S::ROW_AISLE[r] == aisle) stop.samples.push_back(r * S::GRID_COLS + c);
                m_stops.push_back(stop);
            }
        }
        m_phase = Phase::Idle;
        m_playing = false;
    }

    OldMcDonaldScene::ArmPose OldMcDonaldScene::StowPose() {
        const RA::ArmPose p = RA::StowPose();   // shared with the rover CAD workspace
        return ArmPose{ { p.q[0], p.q[1], p.q[2], p.q[3] } };
    }

    glm::vec3 OldMcDonaldScene::SamplePoint(int index) const {
        return glm::vec3(S::SAMPLE_X[index % S::GRID_COLS], S::SOIL_TOP, S::SAMPLE_Z[index / S::GRID_COLS]);
    }
    Entity OldMcDonaldScene::Rover() { return FindByName(ROVER); }
    Entity OldMcDonaldScene::Drone() { return FindByName(DRONE); }

    void OldMcDonaldScene::EnsureRuntime() {
        if (Entity nav = FindByName(NAV); nav && !nav.Has<NavigationSurface>()) {
            NavigationSurface& surface = nav.Add<NavigationSurface>();
            surface.desc.boundsMin = glm::vec3(-S::GH_HALF_X, -0.3f, -S::GH_HALF_Z);
            surface.desc.boundsMax = glm::vec3(S::GH_HALF_X, 1.0f, S::GH_HALF_Z);
            surface.desc.cellSize = 0.05f;
            surface.desc.agentRadius = 0.27f;
            surface.desc.agentHeight = 0.45f;
            surface.desc.maxStepHeight = 0.05f;
            surface.desc.maxSlopeDegrees = 20.0f;
        }
        if (Entity rover = Rover(); rover && !rover.Has<NavAgent>()) {
            NavAgent& a = rover.Add<NavAgent>();
            a.radius = 0.27f;
            a.arriveRadius = 0.03f;
            a.maxSpeed = 0.25f;
            a.maxAcceleration = 0.8f;
            a.separation = 0.0f;
            a.avoidance = 0.0f;
            a.repathInterval = 2.0f;
            m_lastTravelled = 0.0f;
        }
    }

    bool OldMcDonaldScene::RouteBuilt() const {
        Entity route = const_cast<OldMcDonaldScene*>(this)->FindByName(ROVER_ROUTE);
        return route && !route.GetChildren().empty();
    }

    void OldMcDonaldScene::BuildRoverRoute() {
        Entity route = FindByName(ROVER_ROUTE);
        AI::NavGrid* grid = AI::SceneGrid(*this);
        if (!route || !grid || !grid->Valid() || !route.GetChildren().empty()) return;
        std::vector<glm::vec3> waypoints = { S::DOCK };
        for (const Stop& stop : m_stops) waypoints.push_back(stop.position);
        waypoints.push_back(S::DOCK);
        int segment = 1;
        const bool visible = !(m_playing && m_phase < Phase::Depart);   // revealed with the grid during playback
        for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
            const AI::PathResult path = grid->FindPath(waypoints[i], waypoints[i + 1]);
            if (!path.found) { Log::Warning() << "OldMcDonald: no route between waypoints " << i << " and " << i + 1; continue; }
            for (size_t k = 0; k + 1 < path.points.size(); ++k) {
                char name[32];
                std::snprintf(name, sizeof(name), "Leg %02zu-%02d", i + 1, segment++);
                Entity e = Segment(*this, name, route, path.points[k] + glm::vec3(0.0f, 0.004f, 0.0f), path.points[k + 1] + glm::vec3(0.0f, 0.004f, 0.0f), 0.025f, 0.003f, "OMD/RouteRover");
                e.Get<MeshRenderer>().visible = visible;
            }
        }
    }

    // ==================================================================== arm
    void OldMcDonaldScene::SetArm(const ArmPose& pose) { RA::SetArm(*this, RA::ArmPose{ { pose.q[0], pose.q[1], pose.q[2], pose.q[3] } }); }

    OldMcDonaldScene::ArmPose OldMcDonaldScene::CurrentArm() {
        const RA::ArmPose p = RA::GetArm(*this);
        return ArmPose{ { p.q[0], p.q[1], p.q[2], p.q[3] } };
    }

    // Analytic IK: J1 yaw towards the target, J2 / J3 planar two-link (elbow up) to the wrist point
    // RV_HEAD above the probe tips, J4 keeps the sensor head vertical.
    bool OldMcDonaldScene::SolveArm(const glm::vec3& tipWorld, ArmPose& out) {
        Entity arm = FindByName(ARM);
        if (!arm) return false;
        const glm::vec3 p = glm::vec3(glm::inverse(arm.WorldMatrix()) * glm::vec4(tipWorld, 1.0f));
        const float r = std::sqrt(p.x * p.x + p.z * p.z);
        const float dy = p.y + S::RV_HEAD - S::RV_SHOULDER_H;
        const float L2 = S::RV_L2, L3 = S::RV_L3;
        const float c3 = (r * r + dy * dy - L2 * L2 - L3 * L3) / (2.0f * L2 * L3);
        if (c3 < -1.0f || c3 > 1.0f) return false;
        out.q[0] = std::atan2(-p.z, p.x);
        out.q[2] = -std::acos(c3);
        out.q[1] = std::atan2(dy, r) - std::atan2(L3 * std::sin(out.q[2]), L2 + L3 * std::cos(out.q[2]));
        out.q[3] = -(out.q[1] + out.q[2]);
        return true;
    }

    // ==================================================================== explode
    void OldMcDonaldScene::ApplyExplode(int assembly, float value) {
        const float delta = value - m_explode[assembly];
        if (std::abs(delta) < 1e-6f) return;
        for (const ExplodePart& part : m_explodeParts[assembly]) if (Entity e = FindByName(part.entity)) e.LocalTransform().position += part.offset * delta;
        m_explode[assembly] = value;
        // wiring stays with the assembled layout: hidden while exploded
        const bool wiring = value < 0.01f;
        if (assembly == DroneAssembly) {
            if (Entity w = FindByName("Drone wiring")) Kit::SetVisible(w, wiring);
            for (int i = 1; i <= 4; ++i) if (Entity w = FindByName("ESC leads " + std::to_string(i))) Kit::SetVisible(w, wiring);
        } else if (Entity w = FindByName(RA::WIRING)) Kit::SetVisible(w, wiring);
    }
    void OldMcDonaldScene::SetExplodeNow(int assembly, float value) {
        m_explodeTarget[assembly] = value;
        ApplyExplode(assembly, value);
    }

    // ==================================================================== camera
    void OldMcDonaldScene::ApplyCamera(GameContext& ctx, int preset) {
        m_camera = std::clamp(preset, 0, PresetCount - 1);
        const glm::vec3 drone = Drone() ? Drone().WorldPosition() : S::PAD;
        const glm::vec3 rover = Rover() ? Rover().WorldPosition() : S::DOCK;
        PresetView v{ { -2.05f, 0.22f, 0.40f }, 4.6f, 0.62f, -0.60f };
        m_explodeTarget[0] = m_explodeTarget[1] = 0.0f;
        switch (m_camera) {
            case DroneClose: v = { drone + glm::vec3(0.0f, 0.15f, 0.0f), 1.15f, 2.20f, -0.38f }; break;
            case RoverClose: v = { rover + glm::vec3(0.0f, 0.22f, 0.0f), 1.25f, 0.65f, -0.42f }; break;
            case Overview: v = { { 0.0f, 1.2f, 0.0f }, 10.5f, -0.55f, -0.30f }; break;
            case TopPlan: v = { { 0.1f, 0.0f, 0.0f }, 9.0f, 0.0f, -1.5f }; break;
            case DroneExploded: v = { drone + glm::vec3(0.0f, 0.22f, 0.0f), 1.7f, 0.75f, -0.30f }; m_explodeTarget[DroneAssembly] = 1.0f; v.yaw = 2.20f; break;
            case RoverExploded: v = { rover + glm::vec3(0.0f, 0.32f, 0.0f), 1.9f, 0.65f, -0.35f }; m_explodeTarget[RoverAssembly] = 1.0f; break;
            case MissionView: v = { { 0.0f, 0.3f, 0.0f }, 8.2f, 0.40f, -0.75f }; break;
            default: break;
        }
        ctx.camera.SetOrbit(v.target, v.distance, v.yaw, v.pitch);
        if (m_camera == MissionView && !m_playing) MissionPlay();
    }

    // ==================================================================== mission
    int OldMcDonaldScene::Stage() const {
        switch (m_phase) {
            case Phase::Idle: return 0;
            case Phase::Takeoff: return 1;
            case Phase::Survey: return m_surveyU < m_surveyMapStart ? 1 : 2;
            case Phase::Land: return 2;
            case Phase::Transfer: return 3;
            case Phase::Grid: return 4;
            case Phase::Depart: return 5;
            case Phase::Drive: return 6;
            case Phase::Deploy: return 7;
            case Phase::Measure: return (m_phaseTime >= 0.6f && m_phaseTime < 1.8f) ? 8 : 7;
            case Phase::Stow: return 7;
            case Phase::Return: return 9;
            case Phase::Docked: return 10;
        }
        return 0;
    }

    void OldMcDonaldScene::SetSampleState(int index, int state) {
        if (Entity d = FindByName(SampleName(index) + " disc"))
            if (MeshRenderer* r = d.TryGet<MeshRenderer>()) r->material = state == 0 ? "OMD/SamplePending" : state == 1 ? "OMD/SampleActive" : "OMD/SampleDone";
    }
    void OldMcDonaldScene::SetCropMapped(size_t index, bool mapped) {
        if (Entity e = FindByName(m_crops[index])) if (MeshRenderer* r = e.TryGet<MeshRenderer>()) r->material = mapped ? "OMD/CropRingMapped" : "OMD/CropRing";
    }

    void OldMcDonaldScene::MissionReset() {
        m_playing = false;
        m_phase = Phase::Idle;
        m_phaseTime = m_surveyU = 0.0f;
        m_stop = m_stopSample = 0;
        m_samplesTaken = m_cropsMapped = m_pathFailures = m_gridRevealed = m_stagesSeen = 0;
        m_droneLandedBeforeRover = false;
        if (Entity d = Drone()) { d.LocalTransform().position = S::PAD + glm::vec3(0.0f, 0.012f, 0.0f); d.LocalTransform().rotation = glm::quat(1, 0, 0, 0); }
        if (Entity r = Rover()) {
            r.LocalTransform().position = S::DOCK;
            r.LocalTransform().rotation = Yaw(-0.5f * PI);
            if (NavAgent* a = r.TryGet<NavAgent>()) {
                a->hasTarget = false;
                a->arrived = false;
                a->path.clear();
                a->velocity = glm::vec3(0.0f);
                m_lastTravelled = a->travelled;
            }
        }
        SetArm(StowPose());
        for (size_t i = 0; i < m_crops.size(); ++i) SetCropMapped(i, false);
        for (int i = 0; i < S::GRID_ROWS * S::GRID_COLS; ++i) SetSampleState(i, 0);
        for (const char* name : { GRID, ROUTE, DRONE, ROVER }) if (Entity e = FindByName(name)) Kit::SetVisible(e, true);
        ApplyExplode(DroneAssembly, 0.0f);
        ApplyExplode(RoverAssembly, 0.0f);
        m_explodeTarget[0] = m_explodeTarget[1] = 0.0f;
    }

    void OldMcDonaldScene::MissionPlay() {
        if (m_phase == Phase::Idle || m_phase == Phase::Docked) {
            MissionReset();
            if (Entity g = FindByName(GRID)) Kit::SetVisible(g, false);           // generated in stage 4
            if (Entity r = FindByName(ROVER_ROUTE)) Kit::SetVisible(r, false);
            SetPhase(Phase::Takeoff);
        }
        m_playing = true;
    }

    void OldMcDonaldScene::SetPhase(Phase phase) {
        m_phase = phase;
        m_phaseTime = 0.0f;
    }

    void OldMcDonaldScene::MissionStep(GameContext&, float dt) {
        m_phaseTime += dt;
        Entity drone = Drone(), rover = Rover();
        if (!drone || !rover) return;
        NavAgent* agent = rover.TryGet<NavAgent>();
        const glm::vec3 padTop = S::PAD + glm::vec3(0.0f, 0.012f, 0.0f);
        const float H = S::SURVEY_HEIGHT;
        auto target = [&](const glm::vec3& p) {
            if (!agent) return;
            agent->maxSpeed = 0.25f * missionSpeed;
            agent->maxAcceleration = 0.8f * missionSpeed;
            AI::SetTarget(*agent, p);
        };
        auto arrivedAt = [&](const glm::vec3& p) {
            if (!agent) return false;
            if (agent->pathFailed) return true;
            const glm::vec3 q = rover.WorldPosition();
            return agent->arrived && glm::length(glm::vec2(q.x - p.x, q.z - p.z)) < 0.06f;
        };
        auto beginSample = [&] {
            const int sample = m_stops[m_stop].samples[m_stopSample];
            SetSampleState(sample, 1);
            m_armFrom = CurrentArm();
            if (!SolveArm(SamplePoint(sample) + glm::vec3(0.0f, 0.05f, 0.0f), m_armTo)) m_armTo = m_armFrom;
            SetPhase(Phase::Deploy);
        };
        auto lerpPose = [](const ArmPose& a, const ArmPose& b, float t) {
            ArmPose p{};
            for (int i = 0; i < 4; ++i) p.q[i] = a.q[i] + std::remainder(b.q[i] - a.q[i], 2.0f * PI) * t;
            return p;
        };

        switch (m_phase) {
            case Phase::Takeoff: {
                const float t = Smooth(m_phaseTime / 3.0f);
                drone.LocalTransform().position = glm::mix(padTop, m_surveyPoints.front(), t);
                if (m_phaseTime >= 3.0f) SetPhase(Phase::Survey);
                break;
            }
            case Phase::Survey: {
                Animation::Spline spline;
                spline.Build(Animation::SplineType::CatmullRom, m_surveyPoints, false, 24);
                m_surveyU = std::min(1.0f, m_surveyU + dt * 0.7f / std::max(spline.Length(), 1.0f));
                const glm::vec3 p = spline.Evaluate(m_surveyU), t = spline.Tangent(m_surveyU);
                drone.LocalTransform().position = p;
                if (glm::length(glm::vec2(t.x, t.z)) > 1e-3f) drone.LocalTransform().rotation = Yaw(std::atan2(-t.z, t.x));
                if (m_surveyU >= m_surveyMapStart) {   // geometric coverage under the assumed footprint
                    for (size_t i = 0; i < m_crops.size(); ++i) {
                        Entity ring = FindByName(m_crops[i]);
                        if (!ring || ring.Get<MeshRenderer>().material == "OMD/CropRingMapped") continue;
                        const glm::vec3 c = ring.WorldPosition();
                        if (std::abs(c.x - p.x) < S::FOOTPRINT_HALF_X && std::abs(c.z - p.z) < S::FOOTPRINT_HALF_Z) { SetCropMapped(i, true); ++m_cropsMapped; }
                    }
                }
                if (m_surveyU >= 1.0f) {
                    std::string missed;
                    for (const std::string& crop : m_crops) if (Entity r = FindByName(crop); r && r.Get<MeshRenderer>().material != "OMD/CropRingMapped") missed += " " + crop;
                    if (!missed.empty()) Log::Warning() << "OldMcDonald: survey footprint missed" << missed;
                    SetPhase(Phase::Land);
                }
                break;
            }
            case Phase::Land: {
                const float t = Smooth(m_phaseTime / 3.0f);
                drone.LocalTransform().position = glm::mix(glm::vec3(S::PAD.x, H, S::PAD.z), padTop, t);
                drone.LocalTransform().rotation = glm::slerp(drone.LocalTransform().rotation, glm::quat(1, 0, 0, 0), std::min(1.0f, dt * 2.0f));
                if (m_phaseTime >= 3.0f) {
                    drone.LocalTransform().rotation = glm::quat(1, 0, 0, 0);
                    if (Entity path = FindByName(DRONE_PATH)) Kit::SetVisible(path, false);   // survey done (Reset shows it again)
                    SetPhase(Phase::Transfer);
                }
                break;
            }
            case Phase::Transfer:
                if (m_phaseTime >= 3.0f) { SetPhase(Phase::Grid); m_gridRevealed = 0; }
                break;
            case Phase::Grid: {
                const int n = S::GRID_ROWS * S::GRID_COLS;
                while (m_gridRevealed < n && m_phaseTime >= 0.3f * float(m_gridRevealed + 1)) {
                    if (Entity sp = FindByName(SampleName(m_gridRevealed))) Kit::SetVisible(sp, true);
                    ++m_gridRevealed;
                }
                if (m_gridRevealed >= n && m_phaseTime >= 0.3f * float(n) + 0.6f) {
                    if (Entity g = FindByName(GRID)) Kit::SetVisible(g, true);
                    if (Entity r = FindByName(ROVER_ROUTE)) Kit::SetVisible(r, true);
                    m_droneLandedBeforeRover = drone.WorldPosition().y < 0.05f;
                    m_stop = 0;
                    m_stopSample = 0;
                    target(m_stops[0].position);
                    SetPhase(Phase::Depart);
                }
                break;
            }
            case Phase::Depart:
            case Phase::Drive: {
                if (agent) agent->maxSpeed = 0.25f * missionSpeed;
                if (m_phase == Phase::Depart && glm::length(rover.WorldPosition() - S::DOCK) > 0.35f) SetPhase(Phase::Drive);
                if (arrivedAt(m_stops[m_stop].position)) {
                    if (agent && agent->pathFailed) { ++m_pathFailures; agent->pathFailed = false; }
                    m_stopSample = 0;
                    beginSample();
                }
                break;
            }
            case Phase::Deploy: {
                SetArm(lerpPose(m_armFrom, m_armTo, Smooth(m_phaseTime / 1.4f)));
                if (m_phaseTime >= 1.4f) SetPhase(Phase::Measure);
                break;
            }
            case Phase::Measure: {
                // descend 0.6 s (probes into the soil), hold 1.2 s (measurement), lift 0.6 s
                const int sample = m_stops[m_stop].samples[m_stopSample];
                const float t = m_phaseTime;
                const float depth = t < 0.6f ? Smooth(t / 0.6f) : t < 1.8f ? 1.0f : 1.0f - Smooth((t - 1.8f) / 0.6f);
                ArmPose pose;
                if (SolveArm(SamplePoint(sample) + glm::vec3(0.0f, 0.05f - (0.05f + S::PROBE_DEPTH) * depth, 0.0f), pose)) SetArm(pose);
                if (t >= 1.8f && t - dt < 1.8f) { SetSampleState(sample, 2); ++m_samplesTaken; }
                if (t >= 2.4f) {
                    if (++m_stopSample < m_stops[m_stop].samples.size()) beginSample();
                    else { m_armFrom = CurrentArm(); m_armTo = StowPose(); SetPhase(Phase::Stow); }
                }
                break;
            }
            case Phase::Stow: {
                SetArm(lerpPose(m_armFrom, m_armTo, Smooth(m_phaseTime / 1.2f)));
                if (m_phaseTime >= 1.2f) {
                    if (++m_stop < m_stops.size()) { target(m_stops[m_stop].position); SetPhase(Phase::Drive); }
                    else { target(S::DOCK); SetPhase(Phase::Return); }
                }
                break;
            }
            case Phase::Return:
                if (agent) agent->maxSpeed = 0.25f * missionSpeed;
                if (arrivedAt(S::DOCK)) {
                    if (agent && agent->pathFailed) { ++m_pathFailures; agent->pathFailed = false; }
                    SetPhase(Phase::Docked);
                    m_playing = false;
                }
                break;
            default: break;
        }
    }

    void OldMcDonaldScene::Update(GameContext& ctx, float dt) {
        EnsureRuntime();
        if (!RouteBuilt()) BuildRoverRoute();
        if (Entity camera = FindByName("MainCamera")) {
            if (Camera* cam = camera.TryGet<Camera>()) {
                if (!m_cameraNearSet) { cam->nearPlane = 0.02f; cam->farPlane = 120.0f; m_cameraNearSet = true; }
                cam->projection = m_camera == TopPlan ? Projection::Orthographic : Projection::Perspective;
                cam->orthoHeight = 5.2f;
            }
        }
        // exploded views ease towards their targets
        for (int a = 0; a < 2; ++a) {
            const float d = m_explodeTarget[a] - m_explode[a];
            if (std::abs(d) > 1e-4f) ApplyExplode(a, m_explode[a] + std::clamp(d, -dt * 1.6f, dt * 1.6f));
        }
        const float mdt = dt * missionSpeed;
        if (m_playing && ctx.state == GameState::Running) MissionStep(ctx, mdt);
        m_stagesSeen |= 1 << Stage();

        // visual motion: propellers while flying, LiDAR head while the rover is on its mission, wheels by distance
        const bool flying = m_phase == Phase::Takeoff || m_phase == Phase::Survey || m_phase == Phase::Land;
        if (spin && flying) {
            for (int i = 1; i <= 4; ++i) if (Entity p = FindByName("Propeller " + std::to_string(i))) {
                Transform& t = p.LocalTransform();
                t.rotation = glm::normalize(t.rotation * glm::angleAxis((i % 2 ? 1.0f : -1.0f) * dt * 2.0f * PI * 3.0f, glm::vec3(0, 1, 0)));
            }
        }
        const bool roving = m_phase >= Phase::Depart && m_phase <= Phase::Return;
        if (spin && roving) if (Entity h = FindByName("LiDAR head")) {
            Transform& t = h.LocalTransform();
            t.rotation = glm::normalize(t.rotation * glm::angleAxis(dt * 2.0f * PI * 1.5f, glm::vec3(0, 1, 0)));
        }
        if (Entity r = Rover()) if (const NavAgent* a = r.TryGet<NavAgent>()) {
            const float d = a->travelled - m_lastTravelled;
            m_lastTravelled = a->travelled;
            if (d > 0.0f && d < 1.0f) for (int w = 0; w < 4; ++w) if (Entity wheel = FindByName(WHEELS[w])) {
                const float sign = (w % 2) ? 1.0f : -1.0f;   // right-side wheels are turned 180 degrees
                Transform& t = wheel.LocalTransform();
                t.rotation = glm::normalize(t.rotation * glm::angleAxis(sign * d / S::RV_WHEEL_R, glm::vec3(0, 0, 1)));
            }
        }
        if (follow && m_playing) {
            Entity v = (m_phase <= Phase::Land) ? Drone() : Rover();
            if (v) {
                const glm::vec3 to = v.WorldPosition() + glm::vec3(0.0f, 0.2f, 0.0f);
                const glm::vec3 from = ctx.camera.GetOrbitTarget();
                ctx.camera.SetOrbit(glm::mix(from, to, std::min(1.0f, dt * 3.0f)), ctx.camera.GetOrbitDistance(), ctx.camera.GetYaw(), ctx.camera.GetPitch());
            }
        }
    }

    void OldMcDonaldScene::DrawUI(GameContext&) {
        if (m_phase != Phase::Survey || m_surveyU < m_surveyMapStart) return;
        Entity drone = Drone();
        if (!drone) return;
        const glm::vec3 p = drone.WorldPosition();
        const float y = S::SOIL_TOP + 0.02f, x0 = p.x - S::FOOTPRINT_HALF_X, x1 = p.x + S::FOOTPRINT_HALF_X, z0 = p.z - S::FOOTPRINT_HALF_Z, z1 = p.z + S::FOOTPRINT_HALF_Z;
        const glm::vec4 c(0.2f, 0.85f, 1.0f, 1.0f);
        const glm::vec3 corners[4] = { { x0, y, z0 }, { x1, y, z0 }, { x1, y, z1 }, { x0, y, z1 } };
        for (int i = 0; i < 4; ++i) {
            Render::DrawLine(corners[i], corners[(i + 1) % 4], c);
            Render::DrawLine(p, corners[i], glm::vec4(0.2f, 0.85f, 1.0f, 0.35f));
        }
    }

    // ==================================================================== studio
    // ==================================================================== persistence hooks
    // Every save / load path runs these through the editor: the file always holds both assemblies
    // collapsed, and loading resets the mission and explode state to match the file.
    void OldMcDonaldScene::OnBeforeSave(GameContext&) {
        m_savedExplode[0] = m_explode[0];
        m_savedExplode[1] = m_explode[1];
        ApplyExplode(0, 0.0f);
        ApplyExplode(1, 0.0f);
    }

    void OldMcDonaldScene::OnAfterSave(GameContext&) {
        ApplyExplode(0, m_savedExplode[0]);
        ApplyExplode(1, m_savedExplode[1]);
    }

    void OldMcDonaldScene::OnAfterLoad(GameContext&) {
        m_playing = false;
        m_phase = Phase::Idle;
        m_explode[0] = m_explode[1] = m_explodeTarget[0] = m_explodeTarget[1] = 0.0f;
        m_cameraNearSet = false;
    }

    // ==================================================================== workspace panel
    void OldMcDonaldScene::DrawWorkspacePanel(GameContext& ctx) {
        const ImVec4 heading(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f);
        const float bw = Editor::RowButtonWidth(2);

        ImGui::TextColored(heading, "CAMERA");
        for (int i = 0; i < PresetCount; ++i) {
            if (i % 2) ImGui::SameLine();
            if (Retro::Button(PRESET_NAMES[i], bw, m_camera == i ? 1 : 0)) ApplyCamera(ctx, i);
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "MISSION PLAYBACK");
        if (Retro::TransportButton("##omd_play", Retro::Glyph::Play, m_playing, "Play mission")) MissionPlay();
        ImGui::SameLine();
        if (Retro::TransportButton("##omd_pause", Retro::Glyph::Pause, !m_playing && m_phase != Phase::Idle, "Pause")) MissionPause();
        ImGui::SameLine();
        if (Retro::TransportButton("##omd_stop", Retro::Glyph::Stop, false, "Reset (drone on pad, rover docked)")) MissionReset();
        ImGui::SameLine();
        Retro::LedToggle("Follow", &follow);
        Retro::EqSlider("speed", &missionSpeed, 0.5f, 6.0f, "%.1fx");
        const int stage = Stage();
        if (ImGui::BeginTable("##stages", 3, ImGuiTableFlags_SizingFixedFit)) {
            for (int i = 0; i < 9; ++i) {
                const bool active = stage == i + 1, done = stage > i + 1;
                const ImVec4 colour = active ? heading : done ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f) : ImVec4(0.55f, 0.58f, 0.62f, 1.0f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextColored(colour, "%s%d", active ? ">" : done ? "+" : " ", i + 1);
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", STAGE_SOURCE[i]);
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(colour, "%s", STAGES[i]);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", STAGES[i], STAGE_SOURCE[i]);
            }
            ImGui::EndTable();
        }
        ImGui::TextColored(ImVec4(Retro::READOUT[0], Retro::READOUT[1], Retro::READOUT[2], 1.0f), "crops %d/%zu   samples %d/%d   path failures %d", m_cropsMapped, m_crops.size(),
                           m_samplesTaken, S::GRID_ROWS * S::GRID_COLS, m_pathFailures);

        ImGui::Spacing();
        ImGui::TextColored(heading, "DISPLAY");
        if (Retro::LedToggle("CAD edges", &cadEdges)) Showcase::SetCadInk(cadEdges, 0.45f, 1.0f);
        ImGui::SameLine();
        Retro::LedToggle("Spin", &spin);
        ImGui::TextColored(heading, "EXPLODE");
        float ed = m_explodeTarget[DroneAssembly], er = m_explodeTarget[RoverAssembly];
        if (Retro::EqSlider("DRONE", &ed, 0.0f, 1.0f)) m_explodeTarget[DroneAssembly] = ed;
        if (Retro::EqSlider("ROVER", &er, 0.0f, 1.0f)) m_explodeTarget[RoverAssembly] = er;

        ImGui::Spacing();
        ImGui::TextColored(heading, "SUBSYSTEMS");
        Kit::SubsystemList(*this, "##omd_subsystems", { { GREENHOUSE, "Greenhouse" }, { BEDS, "Growing beds + crops" }, { DRONE, "Mapping drone" }, { ROVER, "Soil rover" },
                                                       { ARM, "Robotic arm" }, { LIDAR, "LiDAR" }, { SENSORS, "Soil sensors" }, { GRID, "Sampling grid" }, { ROUTE, "Mission route" } }, 9.5f);
        Kit::SelectedAssembly(*this, ROOT);

        ImGui::Spacing();
        ImGui::TextColored(heading, "SCENE FILE");
        Kit::ProjectFileButtons(ctx, *this, m_status);
        Kit::ScreenshotButton("old_mcdonald", m_status);
    }

    void RegisterOldMcDonaldScene(SceneManager& scenes) {
        scenes.Register("OldMcDonald", [] { return std::make_unique<OldMcDonaldScene>(); });
    }
}
