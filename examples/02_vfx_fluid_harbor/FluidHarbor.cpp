// Showcase 02 (real-time VFX): screen-space fluid harbor with buoyancy.
//
//   * ~17k GPU (or CPU-fallback) particles: a harbor basin filled with an even lattice, a waterfall
//     emitter, a wave paddle and a whirlpool force field.
//   * Rendered by the engine's 4-pass screen-space fluid pipeline (Render::FluidSettings): R32F
//     sphere depth -> narrow-range bilateral depth blur -> normals -> Beer-Lambert + Fresnel + foam
//     (HDR, so foam blooms). The breakdown wipe walks through raw particles -> depth -> normals -> water.
//   * Two-way rigid-fluid coupling: a water-surface height field is rebuilt from the particles
//     (column volume per cell), Archimedes buoyancy + drag act on the boat and crates at sample
//     points; the bodies' colliders displace the particles (and receive their contact impulses).
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace Phisiks;
using namespace Phisiks::Physics::Advanced;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        constexpr float RHO = 1000.0f;                    // kg / m^3
        constexpr float BASIN_X = 4.0f, BASIN_Z = 3.0f;   // half extents of the water basin
        constexpr float PARTICLE_R = 0.07f;
        constexpr uint32_t POOL_COUNT = 9000;
        constexpr float CELL = 0.5f;
        constexpr int GX = int(2 * BASIN_X / CELL), GZ = int(2 * BASIN_Z / CELL);

        struct Floater { glm::vec3 size; };   // buoyant box (size = collider extents)

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c);
            return t;
        }
    }

    class FluidHarborScene : public ShowcaseScene {
    public:
        FluidHarborScene() : ShowcaseScene("FluidHarbor") {}

        float heights[GX * GZ] = {};
        bool heightsValid = false;
        int readCounter = 0;
        float meanLevel = 0.0f;
        bool paddleOn = true, waterfallOn = true, autoWipe = false;
        float wipeTime = 0.0f;
        float boatDraft = 0.0f, boatDraftTheory = 0.0f;
        float buoyancyForce = 0.0f;

        void Build(GameContext& ctx) override {
            Material("Harbor/Concrete", { 0.42f, 0.41f, 0.4f }, 0.85f, 0.0f, {}, Render::Pattern::Tiles, { 0.33f, 0.33f, 0.33f }, 0.5f, 0.01f);
            Material("Harbor/Floor", { 0.18f, 0.2f, 0.17f }, 0.9f, 0.0f, {}, Render::Pattern::Tiles, { 0.1f, 0.12f, 0.1f }, 1.0f, 0.04f);
            Material("Harbor/Hull", { 0.72f, 0.18f, 0.1f }, 0.45f, 0.1f);
            Material("Harbor/Deck", { 0.55f, 0.4f, 0.24f }, 0.7f, 0.0f, {}, Render::Pattern::Wood, { 0.35f, 0.24f, 0.12f }, 4.0f);
            Material("Harbor/Cabin", { 0.9f, 0.9f, 0.86f }, 0.4f);
            Material("Harbor/Crate", { 0.2f, 0.45f, 0.65f }, 0.6f, 0.2f, {}, Render::Pattern::Grid, { 0.9f, 0.9f, 0.9f }, 2.0f, 0.05f);
            Material("Harbor/Steel", { 0.5f, 0.52f, 0.55f }, 0.35f, 0.9f);
            Material("Harbor/Lamp", { 1.0f, 0.85f, 0.6f }, 0.3f, 0.0f, { 6.0f, 4.5f, 2.6f });
            Material("Harbor/Warehouse", { 0.32f, 0.22f, 0.18f }, 0.8f, 0.0f, {}, Render::Pattern::Grid, { 0.2f, 0.15f, 0.12f }, 0.5f);

            Render::RenderSettings& r = Render::Settings();
            r.skyColorZenith = { 0.10f, 0.16f, 0.32f };
            r.skyColorHorizon = { 0.85f, 0.48f, 0.30f };
            r.groundColor = { 0.05f, 0.05f, 0.06f };
            r.ambientUp = { 0.30f, 0.30f, 0.38f };
            r.fog.color = { 0.45f, 0.32f, 0.28f };
            r.fog.distanceDensity = 0.01f;
            r.bloom.intensity = 0.07f;
            Render::FluidSettings& f = r.fluid;
            f.enabled = true;
            f.particleScale = 1.6f;
            f.blurRadius = 0.13f;
            f.foamSpeed = 3.2f;
            f.foamIntensity = 1.1f;
            f.blurIterations = 1;

            Ground(120.0f, "Harbor/Floor", 1.0f);
            Sun(glm::vec3(-0.6f, -0.35f, -0.7f), glm::vec3(1.0f, 0.78f, 0.55f), 2.4f);
            // basin walls (the quay) and surroundings
            const float h = 1.3f, t = 0.5f;
            Box("QuayNorth", { 0.0f, h * 0.5f, -BASIN_Z - t * 0.5f }, { 2 * BASIN_X + 2 * t, h, t }, "Harbor/Concrete");
            Box("QuaySouth", { 0.0f, h * 0.5f, BASIN_Z + t * 0.5f }, { 2 * BASIN_X + 2 * t, h, t }, "Harbor/Concrete");
            Box("QuayWest", { -BASIN_X - t * 0.5f, h * 0.5f, 0.0f }, { t, h, 2 * BASIN_Z }, "Harbor/Concrete");
            Box("QuayEast", { BASIN_X + t * 0.5f, h * 0.5f, 0.0f }, { t, h, 2 * BASIN_Z }, "Harbor/Concrete");
            Visual("PierNorth", "Cube", "Harbor/Concrete", { 0.0f, h * 0.5f, -BASIN_Z - 4.5f }, { 22.0f, h, 7.5f });
            Visual("PierSouth", "Cube", "Harbor/Concrete", { 0.0f, h * 0.5f, BASIN_Z + 4.5f }, { 22.0f, h, 7.5f });
            Visual("PierEast", "Cube", "Harbor/Concrete", { BASIN_X + 3.5f, h * 0.5f, 0.0f }, { 6.0f, h, 7.0f });
            Visual("PierWest", "Cube", "Harbor/Concrete", { -BASIN_X - 3.5f, h * 0.5f, 0.0f }, { 6.0f, h, 7.0f });
            for (int i = 0; i < 5; ++i) Visual("Bollard" + std::to_string(i), "Showcase/Cylinder", "Harbor/Steel", { -3.2f + 1.6f * float(i), h + 0.2f, BASIN_Z + 0.6f }, { 0.25f, 0.4f, 0.25f });
            for (int i = 0; i < 3; ++i) {
                const glm::vec3 base(-4.0f + 4.0f * float(i), h, -BASIN_Z - 1.2f);
                Visual("LampPost" + std::to_string(i), "Showcase/Cylinder", "Harbor/Steel", base + glm::vec3(0, 1.5f, 0), { 0.12f, 3.0f, 0.12f });
                Visual("LampHead" + std::to_string(i), "Sphere", "Harbor/Lamp", base + glm::vec3(0, 3.05f, 0), glm::vec3(0.35f));

            }
            for (int i = 0; i < 4; ++i) Visual("Warehouse" + std::to_string(i), "Cube", "Harbor/Warehouse", { -12.0f + 8.0f * float(i), 3.0f, -15.0f - 2.0f * float(i % 2) }, { 7.0f, 6.0f + float(i % 3), 6.0f });
            // the water: a filled basin + a waterfall emitter
            Entity pool = CreateEntity("Water");
            pool.Get<Transform>().position = glm::vec3(0.0f, 0.45f, 0.0f);
            ParticleSystem& w = pool.Add<ParticleSystem>();
            w.count = POOL_COUNT;
            w.spawnShape = ParticleSpawn::Fill;
            w.spawnSize = glm::vec3(2 * BASIN_X - 0.2f, 0.9f, 2 * BASIN_Z - 0.2f);
            w.radius = PARTICLE_R;
            w.mass = 0.02f;
            w.friction = 0.0f;
            w.damping = 0.15f;
            w.render = ParticleRender::Fluid;
            w.color = { 0.1f, 0.35f, 0.6f };
            w.fastColor = { 0.9f, 0.95f, 1.0f };
            w.seed = 3;
            Entity fall = CreateEntity("Waterfall");
            fall.Get<Transform>().position = glm::vec3(3.3f, 2.4f, -2.4f);
            ParticleSystem& e = fall.Add<ParticleSystem>();
            e.count = 2000;
            e.emissionRate = 220.0f;
            e.lifetime = 9.0f;
            e.emitDirection = glm::normalize(glm::vec3(-0.5f, -0.2f, 0.4f));
            e.emitSpeed = 2.2f;
            e.emitSpread = 0.12f;
            e.spawnSize = glm::vec3(0.3f, 0.1f, 0.3f);
            e.radius = PARTICLE_R;
            e.mass = 0.02f;
            e.friction = 0.0f;
            e.damping = 0.1f;
            e.render = ParticleRender::Fluid;
            e.seed = 9;
            Visual("WaterfallPipe", "Showcase/Cylinder", "Harbor/Steel", { 3.55f, 2.45f, -2.6f }, { 0.4f, 0.6f, 0.4f }, glm::angleAxis(0.9f, glm::vec3(0, 0, 1)));
            // whirlpool
            Entity vortex = CreateEntity("Whirlpool");
            vortex.Get<Transform>().position = glm::vec3(-2.2f, 0.4f, 1.4f);
            Physics::ForceField& v = vortex.Add<Physics::ForceField>();
            v.type = Physics::FieldType::Vortex;
            v.strength = 2.0f;
            v.radius = 0.7f;
            v.falloff = 1.0f;
            v.influenceRadius = 2.2f;
            // wave maker: an oscillating uniform force field over the west end of the basin
            Entity waves = CreateEntity("WaveMaker");
            waves.Get<Transform>().position = glm::vec3(-BASIN_X + 0.8f, 0.4f, 0.0f);
            Physics::ForceField& wm = waves.Add<Physics::ForceField>();
            wm.type = Physics::FieldType::Uniform;
            wm.vector = glm::vec3(0.0f);
            wm.influenceRadius = 3.2f;
            wm.affectsRigidBodies = false;
            // the boat and crates (buoyant)
            Entity boat = Box("Boat", { 0.6f, 1.25f, -0.4f }, { 2.4f, 0.6f, 1.0f }, "Harbor/Hull", 430.0f);
            boat.Add<Floater>(Floater{ { 2.4f, 0.6f, 1.0f } });
            boat.Get<Rigidbody>().angularDamping = 0.4f;
            Entity deck = Visual("BoatDeck", "Cube", "Harbor/Deck", { 0.0f, 0.52f, 0.0f }, { 0.96f, 0.06f, 0.92f });
            deck.SetParent(boat, false);
            Entity cabin = Visual("BoatCabin", "Cube", "Harbor/Cabin", { -0.15f, 1.0f, 0.0f }, { 0.35f, 0.9f, 0.7f });
            cabin.SetParent(boat, false);
            for (int i = 0; i < 4; ++i) {
                Entity c = Box("Crate" + std::to_string(i), { -2.6f + 1.1f * float(i % 2), 1.3f + 0.6f * float(i / 2), 1.6f - 0.9f * float(i) }, glm::vec3(0.5f), "Harbor/Crate", 45.0f);
                c.Add<Floater>(Floater{ glm::vec3(0.5f) });
            }
            boatDraftTheory = 430.0f / (RHO * 2.4f * 1.0f);
            ctx.camera.SetOrbit(glm::vec3(0.0f, 0.6f, 0.0f), 10.5f, 0.55f, -0.42f);
        }

        // ---------------------------------------------------------------- water surface from the particles
        void RebuildHeights() {
            std::vector<glm::vec4> positions;
            int counts[GX * GZ] = {};
            for (const char* name : { "Water", "Waterfall" }) {
                Entity e = FindByName(name);
                if (!e.Valid() || !ReadParticlesAsync(e, positions)) continue;
                for (const glm::vec4& p : positions) {
                    if (p.w <= 0.0f) continue;   // dead emitter slot
                    const int x = int((p.x + BASIN_X) / CELL), z = int((p.z + BASIN_Z) / CELL);
                    if (x < 0 || z < 0 || x >= GX || z >= GZ || p.y > 2.0f) continue;
                    ++counts[z * GX + x];
                }
            }
            // column volume: every particle stands for a (2r)^3 cube of water
            // Cells under a floating body hold less water (the body displaced it), so the free surface
            // at every cell is the mean column height of the uncovered cells within +-3 cells (1.5 m).
            const float perParticle = 8.0f * PARTICLE_R * PARTICLE_R * PARTICLE_R / (CELL * CELL);
            bool covered[GX * GZ] = {};
            Each<Floater>([&](Entity e, Floater& f) {
                const glm::vec3 c = e.WorldPosition();
                const float r = 0.5f * std::max(f.size.x, f.size.z) + 0.1f;
                for (int z = 0; z < GZ; ++z) for (int x = 0; x < GX; ++x) {
                    const float cx = -BASIN_X + (float(x) + 0.5f) * CELL, cz = -BASIN_Z + (float(z) + 0.5f) * CELL;
                    if (std::abs(cx - c.x) < r && std::abs(cz - c.z) < r) covered[z * GX + x] = true;
                }
            });
            float column[GX * GZ];
            float sum = 0.0f;
            int open = 0;
            for (int i = 0; i < GX * GZ; ++i) {
                column[i] = float(counts[i]) * perParticle;
                if (!covered[i]) { sum += column[i]; ++open; }
            }
            meanLevel = open ? sum / float(open) : 0.0f;
            for (int z = 0; z < GZ; ++z) for (int x = 0; x < GX; ++x) {
                float acc = 0.0f;
                int n = 0;
                for (int dz = -3; dz <= 3; ++dz) for (int dx = -3; dx <= 3; ++dx) {
                    const int xx = x + dx, zz = z + dz;
                    if (xx < 0 || zz < 0 || xx >= GX || zz >= GZ || covered[zz * GX + xx]) continue;
                    acc += column[zz * GX + xx];
                    ++n;
                }
                heights[z * GX + x] = n ? acc / float(n) : meanLevel;
            }
            heightsValid = true;
        }

        float WaterHeight(float x, float z) const {
            if (!heightsValid) return 0.0f;
            const float fx = std::clamp((x + BASIN_X) / CELL - 0.5f, 0.0f, float(GX - 1)), fz = std::clamp((z + BASIN_Z) / CELL - 0.5f, 0.0f, float(GZ - 1));
            const int x0 = int(fx), z0 = int(fz), x1 = std::min(x0 + 1, GX - 1), z1 = std::min(z0 + 1, GZ - 1);
            const float tx = fx - float(x0), tz = fz - float(z0);
            const float a = glm::mix(heights[z0 * GX + x0], heights[z0 * GX + x1], tx), b = glm::mix(heights[z1 * GX + x0], heights[z1 * GX + x1], tx);
            return glm::mix(a, b, tz);
        }

        void Tick(GameContext&, float dt) override {
            if (++readCounter % 3 == 1) RebuildHeights();   // non-blocking readback (GPU copy + fence)
            if (Entity waves = FindByName("WaveMaker"); waves.Valid())
                waves.Get<Physics::ForceField>().vector = glm::vec3(paddleOn ? 7.0f * std::sin(ElapsedTime() * 2.4f) : 0.0f, 0.0f, 0.0f);
            if (Entity fall = FindByName("Waterfall"); fall.Valid()) fall.Get<ParticleSystem>().emissionRate = waterfallOn ? 220.0f : 0.0f;
            if (autoWipe) {
                wipeTime += dt;
                Render::Settings().fluid.breakdown = std::fmod(wipeTime * 0.4f, 3.6f) > 3.0f ? 3.0f : std::fmod(wipeTime * 0.4f, 3.6f);
            }
            if (Entity boat = FindByName("Boat"); boat.Valid()) {
                const glm::vec3 p = boat.WorldPosition();
                const glm::vec3 down = boat.WorldRotation() * glm::vec3(0, -0.3f, 0);
                boatDraft = WaterHeight(p.x, p.z) - (p + down).y;
            }
        }

        void FixedTick(GameContext&, float dt) override {
            if (!heightsValid) return;
            buoyancyForce = 0.0f;
            Each<Floater>([&](Entity e, Floater& f) {
                const Physics::BodyId body = e.GetBody();
                if (!body.Valid()) return;
                const glm::vec3 c = Physics::GetPosition(body);
                const glm::quat q = Physics::GetRotation(body);
                const glm::vec3 v = Physics::GetLinearVelocity(body), w = Physics::GetAngularVelocity(body);
                const int n = 3;
                const float pointVolume = f.size.x * f.size.y * f.size.z / float(n * 2 * n);
                const float slab = f.size.y * 0.5f;
                for (int i = 0; i < n; ++i) for (int j = 0; j < 2; ++j) for (int k = 0; k < n; ++k) {
                    const glm::vec3 local = (glm::vec3((float(i) + 0.5f) / float(n), (float(j) + 0.5f) / 2.0f, (float(k) + 0.5f) / float(n)) - 0.5f) * f.size;
                    const glm::vec3 p = c + q * local;
                    if (std::abs(p.x) > BASIN_X || std::abs(p.z) > BASIN_Z) continue;
                    const float submerged = std::clamp((WaterHeight(p.x, p.z) - p.y) / slab + 0.5f, 0.0f, 1.0f);
                    if (submerged <= 0.0f) continue;
                    const float buoyancy = RHO * 9.81f * pointVolume * submerged;
                    const glm::vec3 pv = v + glm::cross(w, p - c);
                    const glm::vec3 drag = -pv * (RHO * pointVolume * submerged * 1.6f);
                    Physics::AddImpulseAtPoint(body, (glm::vec3(0.0f, buoyancy, 0.0f) + drag) * dt, p);
                    buoyancyForce += buoyancy;
                }
            });
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            CameraShot fall = DollyShot("Waterfall close-up", { 1.2f, 1.6f, -0.2f }, { 2.0f, 1.2f, -1.0f }, { 3.0f, 0.6f, -2.4f }, 7.0f);
            return {
                DollyShot("Low over the water", { -5.5f, 2.0f, 3.6f }, { 4.5f, 2.0f, 3.6f }, { 0.5f, 0.5f, -0.5f }, 10.0f),
                OrbitShot("Harbor orbit", { 0.0f, 0.5f, 0.0f }, 9.0f, 4.0f, 18.0f),
                fall,
                DollyShot("Boat tracking", { -3.0f, 2.2f, 4.5f }, { 3.0f, 2.2f, 4.5f }, { 0.6f, 0.7f, -0.4f }, 9.0f),
                DollyShot("Top-down", { 0.0f, 11.0f, 0.5f }, { 0.0f, 9.0f, 0.4f }, { 0.0f, 0.0f, 0.0f }, 8.0f),
            };
        }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            for (const char* name : { "Boat", "Crate0", "Crate1", "Crate2", "Crate3" }) {
                Entity e = FindByName(name);
                if (!e.Valid()) continue;
                Render::SdfPrimitive p;
                p.shape = Render::SdfShape::RoundBox;
                p.position = e.WorldPosition();
                p.rotation = e.WorldRotation();
                p.params = glm::vec4(e.Get<Floater>().size * 0.5f, 0.08f);
                p.smoothness = 0.3f;
                sdf.primitives.push_back(p);
            }
            Render::SdfPrimitive torus;
            torus.shape = Render::SdfShape::Torus;
            torus.position = glm::vec3(-2.2f, 0.5f, 1.4f);
            torus.params = glm::vec4(0.7f, 0.06f, 0, 0);
            sdf.primitives.push_back(torus);
            sdf.boundsMin = glm::vec3(-BASIN_X, 0.0f, -BASIN_Z);
            sdf.boundsMax = glm::vec3(BASIN_X, 2.5f, BASIN_Z);
            sdf.color = glm::vec3(1.0f, 0.55f, 0.25f);
        }

        bool BuildFieldView(Render::FieldVisualization& field) override {
            field.boundsMin = glm::vec3(-4.4f, 0.5f, -1.0f);
            field.boundsMax = glm::vec3(0.0f, 0.9f, 3.8f);
            field.resolution = glm::ivec3(12, 2, 12);
            field.arrowScale = 0.12f;
            field.maxLength = 0.45f;
            return true;
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.0f, 0.0f, 0.0f, 7.0f); }
        const char* Controls() const override { return "RMB orbit, MMB pan, wheel zoom\nBreakdown slider: raw -> depth -> normals -> water"; }

        void DrawPanel(GameContext&) override {
            Render::FluidSettings& f = Render::Settings().fluid;
            const Stats s = GetStats();
            char big[32], small[96];
            static const char* stages[4] = { "RAW", "DEPTH", "NORMALS", "WATER" };
            std::snprintf(big, sizeof(big), "%s", stages[std::min(int(f.breakdown + 1e-3f), 3)]);
            std::snprintf(small, sizeof(small), "%u particles  level %.2f m", s.particlesAlive ? s.particles : s.particles, double(meanLevel));
            Retro::Lcd(big, small);
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.3f, 1), "BREAKDOWN WIPE");
            Retro::EqSlider("stage", &f.breakdown, 0.0f, 3.0f, "%.2f");
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (Retro::Button(stages[i], 78.0f, int(f.breakdown + 1e-3f) == i ? 1 : 0)) { f.breakdown = float(i); autoWipe = false; }
            }
            Retro::LedToggle("AUTO WIPE", &autoWipe); ImGui::SameLine();
            Retro::LedToggle("WAVES", &paddleOn); ImGui::SameLine();
            Retro::LedToggle("FALL", &waterfallOn);
            Retro::EqSlider("blur radius (m)", &f.blurRadius, 0.02f, 0.3f);
            Retro::EqSlider("absorption R", &f.absorption.x, 0.0f, 2.0f);
            Retro::EqSlider("foam speed", &f.foamSpeed, 0.5f, 5.0f);
            char v[64];
            std::snprintf(v, sizeof(v), "%.3f m (Archimedes %.3f)", double(boatDraft), double(boatDraftTheory));
            Retro::LedMeter("DRAFT", boatDraft / 0.4f, v);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "buoyancy %.1f kN  coupled bodies %u", double(buoyancyForce / 1000.0f), s.coupledBodies);
            ImGui::TextColored(ImVec4(0, 1, 0.25f, 1), "%s backend  %u SSF passes", s.gpuAvailable ? "GPU" : "CPU", GetRenderStats().fluidPasses);
        }

        void AddTests(ShowcaseTest& test) override {
            test.AddStep("fluid settles, SSF passes run", [this](GameContext& c, ShowcaseTest& t) {
                paddleOn = false;
                if (t.StepSeconds(c) < 3.0) return false;
                const Stats s = GetStats();
                const RenderStats r = GetRenderStats();
                t.Expect("particle pool + emitter simulated", s.particleSystems == 2 && s.particles >= POOL_COUNT,
                         Fmt("%.0f systems, %.0f particles (%.0f emitted)", s.particleSystems, s.particles, double(s.particlesEmitted)) + (s.gpuAvailable ? ", GPU" : ", CPU backend"));
                t.Expect("screen-space fluid: depth, blur (H + V x iterations), normals, shading passes", r.fluidActive && r.fluidPasses == uint32_t(3 + 2 * Render::Settings().fluid.blurIterations) && r.fluidParticles > 0,
                         Fmt("%.0f passes, %.0f fluid particles rendered", r.fluidPasses, r.fluidParticles));
                t.Expect("water level from particle columns", meanLevel > 0.3f && meanLevel < 1.2f, Fmt("mean level %.3f m", meanLevel));
                return true;
            }, 30.0);
            test.AddStep("buoyancy: boat draft = m / (rho A)", [this](GameContext& c, ShowcaseTest& t) {
                paddleOn = false;
                waterfallOn = false;
                if (t.StepSeconds(c) < 7.0) { t.Value("y0") = FindByName("Boat").WorldPosition().y; return false; }
                if (t.StepSeconds(c) < 8.0) return false;
                const double drift = FindByName("Boat").WorldPosition().y - t.Value("y0");
                t.Expect("boat floats at the Archimedes draft", std::abs(boatDraft - boatDraftTheory) < boatDraftTheory * 0.35f && std::abs(drift) < 0.03,
                         Fmt("draft %.3f m vs %.3f m theory, ", boatDraft, boatDraftTheory) + Fmt("height drift %.3f m over 1 s", drift));
                int floating = 0;
                for (int i = 0; i < 4; ++i) {
                    Entity e = FindByName("Crate" + std::to_string(i));
                    if (e.Valid() && e.WorldPosition().y > WaterHeight(e.WorldPosition().x, e.WorldPosition().z) - 0.25f) ++floating;
                }
                const Stats s = GetStats();
                t.Expect("crates float; particles push the bodies back (two-way)", floating == 4 && s.coupledImpulsesApplied > 0,
                         Fmt("%.0f / 4 crates at the surface, %.0f coupling impulses applied", floating, double(s.coupledImpulsesApplied)));
                return true;
            }, 30.0);
            test.AddStep("breakdown wipe: raw -> depth -> normals -> water", [this](GameContext& c, ShowcaseTest& t) {
                Render::FluidSettings& f = Render::Settings().fluid;
                const int stage = t.Phase() / 3;
                if (stage == 0 && t.Phase() == 0) { PlayShot(c, 1); }
                if (stage >= 5) { f.breakdown = 3.0f; waterfallOn = true; paddleOn = true; StopShot(c); return true; }
                static const float values[5] = { 0.0f, 1.0f, 2.0f, 3.0f, 1.5f };
                switch (t.Phase() % 3) {
                    case 0: f.breakdown = values[stage]; ++t.Phase(); t.Value("w") = 0; return false;
                    case 1: if (++t.Value("w") < 4) return false; t.RequestCapture(); ++t.Phase(); return false;
                    default: {
                        if (!t.CaptureReady()) return false;
                        const Render::FrameCapture& cap = t.Capture();
                        double r = 0, g = 0, b = 0;
                        for (size_t i = 0; i + 3 < cap.rgba.size(); i += 4) { r += cap.rgba[i]; g += cap.rgba[i + 1]; b += cap.rgba[i + 2]; }
                        const double n = double(cap.rgba.size() / 4);
                        t.Value("r" + std::to_string(stage)) = r / n; t.Value("g" + std::to_string(stage)) = g / n; t.Value("b" + std::to_string(stage)) = b / n;
                        if (stage == 0) t.stored = cap;
                        else {
                            const double diff = ShowcaseTest::MeanAbsDiff(cap, t.stored);
                            static const char* names[5] = { "raw", "depth", "normals", "final water", "wipe 1.5 (half depth / half normals)" };
                            t.Expect(std::string("breakdown stage: ") + names[stage] + " differs from raw particles", diff > 1.0, Fmt("mean abs diff %.2f, mean rgb (%.0f, ", diff, r / n) + Fmt("%.0f, %.0f)", g / n, b / n));
                        }
                        t.Screenshot("breakdown_" + std::to_string(stage));
                        ++t.Phase();
                        return false;
                    }
                }
            }, 40.0);
        }
    };

    void RegisterFluidHarbor(SceneManager& scenes) { scenes.Register("FluidHarbor", [] { return std::make_unique<FluidHarborScene>(); }); }
}
