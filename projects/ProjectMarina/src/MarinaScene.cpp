#include "MarinaScene.h"
#include "MarinaSpec.h"
#include "ProjectKit.h"
#include "Showcase.h"
#include "Editor.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace S = Marina::Spec;

namespace Projects {
    namespace {
        constexpr float PI = 3.14159265358979f;
        const std::string MESH = "meshes/marina/";
        const char* const ROOT = "Project Marina";

        // ---------------------------------------------------------------- materials
        void Mat(const std::string& name, glm::vec3 base, float roughness, float metallic = 0.0f, glm::vec3 emissive = glm::vec3(0.0f),
                 float opacity = 1.0f, Render::Pattern pattern = Render::Pattern::None, glm::vec3 patternColor = glm::vec3(0.0f),
                 float patternScale = 1.0f, float lineWidth = 0.04f) {
            ProjectKit::Material(name, base, roughness, metallic, emissive, opacity, int(pattern), patternColor, patternScale, lineWidth);
        }

        // Process circuits: one colour family per side (warm = heat / hot seawater, cool = cold side).
        struct Circuit { const char* material; const char* name; glm::vec3 color; const char* about; };
        const Circuit COOLANT = { "Marina/Fluid/Coolant", "Coolant loop (closed)", { 1.00f, 0.52f, 0.06f }, "SBC water block -> P1 -> HX1 -> SBC" };
        const Circuit FEED = { "Marina/Fluid/Seawater", "Seawater feed", { 0.08f, 0.62f, 0.56f }, "TK1 -> HX1 (ambient)" };
        const Circuit HOTFEED = { "Marina/Fluid/HotFeed", "Heated feed (hot side)", { 0.95f, 0.14f, 0.08f }, "HX1 -> MD1 hot chamber" };
        const Circuit BRINE = { "Marina/Fluid/Brine", "Brine return", { 0.62f, 0.10f, 0.36f }, "MD1 hot chamber -> TK1" };
        const Circuit VAPOUR = { "Marina/Fluid/Vapour", "Vapour (cold side)", { 0.62f, 0.84f, 1.00f }, "MD1 cold chamber -> CD1" };
        const Circuit FRESH = { "Marina/Fluid/Freshwater", "Freshwater condensate", { 0.12f, 0.42f, 1.00f }, "CD1 -> TK2" };
        const Circuit* const CIRCUITS[] = { &COOLANT, &FEED, &HOTFEED, &BRINE, &VAPOUR, &FRESH };

        void RegisterMarinaMaterials() {
            using P = Render::Pattern;
            Mat("Marina/Studio", { 0.74f, 0.75f, 0.77f }, 0.9f, 0.0f, glm::vec3(0.0f), 1.0f, P::Grid, { 0.62f, 0.64f, 0.68f }, 1.0f, 0.012f);
            Mat("Marina/Plate", { 0.80f, 0.81f, 0.82f }, 0.42f, 1.0f, glm::vec3(0.0f), 1.0f, P::Grid, { 0.55f, 0.56f, 0.58f }, 4.0f, 0.006f);
            Mat("Marina/Aluminium", { 0.88f, 0.89f, 0.90f }, 0.32f, 1.0f);
            Mat("Marina/AnodisedBlack", { 0.035f, 0.037f, 0.042f }, 0.42f, 0.6f);
            Mat("Marina/DarkPolymer", { 0.045f, 0.047f, 0.052f }, 0.55f);
            Mat("Marina/Rubber", { 0.02f, 0.02f, 0.02f }, 0.92f);
            Mat("Marina/DuctGrey", { 0.30f, 0.31f, 0.33f }, 0.65f);
            Mat("Marina/Stainless", { 0.66f, 0.67f, 0.68f }, 0.26f, 1.0f);
            Mat("Marina/Copper", { 0.93f, 0.56f, 0.40f }, 0.30f, 1.0f);
            Mat("Marina/Tin", { 0.80f, 0.80f, 0.82f }, 0.30f, 1.0f);
            Mat("Marina/Gold", { 0.95f, 0.76f, 0.38f }, 0.28f, 1.0f);
            Mat("Marina/PCB/Green", { 0.030f, 0.140f, 0.065f }, 0.38f);
            Mat("Marina/PCB/Teal", { 0.000f, 0.230f, 0.300f }, 0.38f);
            Mat("Marina/PCB/Black", { 0.020f, 0.022f, 0.025f }, 0.40f);
            Mat("Marina/PCB/Blue", { 0.030f, 0.060f, 0.170f }, 0.40f);
            Mat("Marina/ICBlack", { 0.018f, 0.018f, 0.020f }, 0.35f);
            Mat("Marina/TerminalBlue", { 0.06f, 0.20f, 0.55f }, 0.50f);
            Mat("Marina/Breadboard", { 0.86f, 0.86f, 0.84f }, 0.62f, 0.0f, glm::vec3(0.0f), 1.0f, P::Tiles, { 0.20f, 0.20f, 0.21f }, 393.7f, 0.30f);
            Mat("Marina/RailRed", { 0.70f, 0.05f, 0.04f }, 0.5f);
            Mat("Marina/RailBlue", { 0.04f, 0.12f, 0.60f }, 0.5f);
            Mat("Marina/PTFE", { 0.92f, 0.92f, 0.90f }, 0.70f);
            Mat("Marina/HotBand", { 0.70f, 0.06f, 0.04f }, 0.45f);
            Mat("Marina/ColdBand", { 0.05f, 0.20f, 0.72f }, 0.45f);
            Mat("Marina/ClearTube", { 0.86f, 0.90f, 0.94f }, 0.05f, 0.0f, glm::vec3(0.0f), 0.10f);
            Mat("Marina/ClearVessel", { 0.90f, 0.94f, 0.96f }, 0.05f, 0.0f, glm::vec3(0.0f), 0.16f);
            Mat("Marina/SeawaterBody", { 0.05f, 0.42f, 0.40f }, 0.10f, 0.0f, { 0.004f, 0.03f, 0.03f }, 0.55f);
            Mat("Marina/FreshwaterBody", { 0.20f, 0.45f, 0.85f }, 0.10f, 0.0f, { 0.01f, 0.03f, 0.07f }, 0.45f);
            for (const Circuit* c : CIRCUITS) Mat(c->material, c->color * 0.70f, 0.30f, 0.0f, c->color * 0.22f);
            Mat("Marina/Wire/Red", { 0.62f, 0.04f, 0.03f }, 0.45f);
            Mat("Marina/Wire/Black", { 0.025f, 0.025f, 0.028f }, 0.45f);
            Mat("Marina/Wire/Yellow", { 0.80f, 0.62f, 0.05f }, 0.45f);
            Mat("Marina/Wire/Green", { 0.05f, 0.45f, 0.12f }, 0.45f);
            Mat("Marina/Wire/White", { 0.82f, 0.82f, 0.80f }, 0.45f);
            Mat("Marina/Wire/Blue", { 0.05f, 0.18f, 0.70f }, 0.45f);
            Mat("Marina/Cable", { 0.06f, 0.062f, 0.068f }, 0.60f);
        }

        // ---------------------------------------------------------------- entity helpers (ProjectKit)
        using ProjectKit::Group;
        using ProjectKit::RunStyle;
        using ProjectKit::SortedChildren;
        using ProjectKit::CountVisible;
        Entity Part(Scene& scene, const std::string& name, Entity parent, const std::string& file, const std::string& material,
                    const glm::vec3& position = glm::vec3(0.0f), bool castShadows = true) {
            return ProjectKit::Part(scene, name, parent, MESH + file, material, position, castShadows);
        }
        Entity MarinaRun(Scene& scene, Entity parent, const std::string& name, const std::vector<glm::vec3>& pts, RunStyle style) {
            if (!style.core.empty()) style.sleeve = S::SLEEVE;   // tubing sleeves over the barbs
            return ProjectKit::BuildRun(scene, parent, name, pts, style);
        }

        glm::vec3 W(const glm::vec3& origin, const glm::vec3& local) { return origin + local; }

        const MarinaScene::CameraPreset PRESETS[] = {
            { "Perspective", { 0.0f, 0.06f, 0.0f }, 1.12f, 0.60f, -0.58f },
            { "Front", { 0.0f, 0.10f, 0.0f }, 1.45f, 0.0f, -0.10f },
            { "Top plan", { 0.0f, 0.0f, 0.0f }, 1.45f, 0.0f, -1.45f },
            { "Electronics", { -0.30f, 0.02f, 0.02f }, 0.62f, -0.50f, -0.80f },
            { "Thermal loop", { -0.13f, 0.06f, -0.06f }, 0.70f, 0.42f, -0.50f },
            { "Desalination", { 0.22f, 0.11f, -0.01f }, 0.78f, 0.50f, -0.38f },
        };
    }

    // ==================================================================== build
    void MarinaScene::OnEnter(GameContext& ctx) {
        m_savedSettings = Render::Settings();
        m_savedFov = ctx.camera.Settings().verticalFovRadians;
        m_savedMinDistance = ctx.camera.Settings().orbitMinDistance;
        ctx.camera.Settings().verticalFovRadians = 0.62f;   // ~35 degrees: less perspective distortion, CAD-like
        ctx.camera.Settings().orbitMinDistance = 0.08f;
        RegisterMarinaMaterials();
        ApplyEnvironment();
        Build();
        Showcase::SetCadInk(cadEdges, 0.55f, 1.0f);
        m_savedOverlay = ctx.overlayVisible;
        ctx.overlayVisible = false;     // presentation: no debug telemetry text (F1 brings it back)
        ctx.crosshairVisible = false;   // inspection scene: no first-person crosshair / aim label
        m_cameraNearSet = false;
        ApplyCamera(ctx, 0);
    }

    void MarinaScene::OnExit(GameContext& ctx) {
        Showcase::SetCadInk(false);
        Render::Settings() = m_savedSettings;
        ctx.camera.Settings().verticalFovRadians = m_savedFov;
        ctx.camera.Settings().orbitMinDistance = m_savedMinDistance;
        ctx.overlayVisible = m_savedOverlay;
        ctx.crosshairVisible = true;
    }

    void MarinaScene::ApplyEnvironment() {
        Render::RenderSettings& r = Render::Settings();
        r.skyColorZenith = { 0.56f, 0.58f, 0.62f };
        r.skyColorHorizon = { 0.66f, 0.67f, 0.70f };
        r.groundColor = { 0.50f, 0.51f, 0.53f };
        r.ambientUp = { 0.36f, 0.38f, 0.42f };
        r.ambientDown = { 0.20f, 0.20f, 0.21f };
        r.ambientIntensity = 1.0f;
        r.rimColor = { 0.9f, 0.92f, 1.0f };
        r.rimIntensity = 0.06f;
        r.fog.enabled = true;
        r.fog.color = r.skyColorHorizon;
        r.fog.distanceDensity = 0.06f;
        r.fog.heightDensity = 0.0f;
        r.fog.maxOpacity = 0.6f;
        r.exposure = 1.0f;
        r.bloom.threshold = 2.0f;
        r.bloom.intensity = 0.03f;
        r.ssao.radius = 0.02f;
        r.ssao.bias = 0.002f;
        r.ssao.intensity = 1.0f;
        r.shadowsEnabled = true;
        r.shadowDistance = 2.2f;
        r.clearColor = glm::vec4(r.skyColorHorizon, 1.0f);
    }

    void MarinaScene::Build() {
        m_routes.clear();
        Scene& s = *this;
        const RunStyle tubeStyle{ S::TUBE_OD, "Marina/ClearTube", "" };
        auto tubing = [&](const Circuit& c) { RunStyle st = tubeStyle; st.core = c.material; return st; };
        auto run = [&](Entity parent, const std::string& name, const std::vector<glm::vec3>& pts, const RunStyle& style, glm::vec3 color, bool fluid) {
            MarinaRun(s, parent, name, pts, style);
            m_routes.push_back({ name, color, style.od, fluid });
        };

        // ---------------------------------------------------------------- studio
        Entity studio = Group(s, "Studio", Entity());
        {
            Entity cyc = Group(s, "Cyclorama", studio, glm::vec3(0.0f, -2.0f * S::PLATE_HALF.y - S::FOOT_HEIGHT, 0.4f));
            cyc.Add<MeshRenderer>(MeshRenderer{ "meshes/hero/cyclorama.obj", "Marina/Studio" }).castShadows = false;
            cyc.Get<Transform>().scale = glm::vec3(0.25f);
            auto light = [&](const std::string& name, LightType type, glm::vec3 position, glm::vec3 target, glm::vec3 color, float intensity, bool shadows) {
                Entity e = Group(s, name, studio, position);
                e.Get<Transform>().rotation = glm::quatLookAt(glm::normalize(target - position), std::abs(glm::normalize(target - position).y) > 0.99f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0));
                Light l;
                l.type = type;
                l.color = color;
                l.intensity = intensity;
                l.range = 6.0f;
                l.outerConeAngle = 0.75f;
                l.innerConeAngle = 0.45f;
                l.castShadows = shadows;
                e.Add<Light>(l);
            };
            light("Key light", LightType::Directional, { 0.9f, 1.6f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.98f, 0.95f }, 2.6f, true);
            light("Fill light", LightType::Spot, { -1.1f, 0.8f, 1.1f }, { 0.0f, 0.05f, 0.0f }, { 0.85f, 0.90f, 1.0f }, 2.2f, false);
            light("Rim light", LightType::Spot, { 0.5f, 0.9f, -1.2f }, { 0.0f, 0.05f, 0.0f }, { 0.95f, 0.97f, 1.0f }, 2.6f, false);
        }

        Entity root = Group(s, ROOT, Entity());

        // ---------------------------------------------------------------- bench
        {
            Entity bench = Group(s, "0 Bench", root);
            Part(s, "Baseplate", bench, "baseplate.obj", "Marina/Plate");
            Part(s, "Feet", bench, "feet.obj", "Marina/Rubber");
            Part(s, "Electronics sub-plate", bench, "tray.obj", "Marina/AnodisedBlack", S::TRAY_CENTRE);
            Part(s, "Splash wall", bench, "divider.obj", "Marina/Aluminium", glm::vec3(S::DIVIDER_X, 0.0f, S::TRAY_CENTRE.z));
            Part(s, "Wiring duct", bench, "duct.obj", "Marina/DuctGrey", S::DUCT_CENTRE);
        }

        // ---------------------------------------------------------------- 1 computing unit
        {
            Entity sbc = Group(s, "1 Computing Unit - SBC", root, S::PI_ORIGIN);
            Part(s, "Case base", sbc, "sbc_case.obj", "Marina/AnodisedBlack");
            Part(s, "Board", sbc, "sbc_pcb.obj", "Marina/PCB/Green");
            Part(s, "IC packages", sbc, "sbc_ics.obj", "Marina/ICBlack");
            Part(s, "Connectors", sbc, "sbc_connectors.obj", "Marina/Tin");
            Part(s, "SoC water block", sbc, "waterblock.obj", "Marina/Aluminium");
            Entity fan = Group(s, "Cooling fan 30 mm", sbc, S::PI_FAN_CENTRE);
            Part(s, "Fan frame", fan, "fan_frame.obj", "Marina/DarkPolymer");
            Part(s, "Fan rotor", fan, "fan_rotor.obj", "Marina/DarkPolymer");
        }

        // ---------------------------------------------------------------- 3 electronics
        {
            Entity elec = Group(s, "3 Electronics", root);
            Entity uno = Group(s, "Controller A1 - Arduino UNO", elec, S::ARDUINO_ORIGIN);
            Part(s, "Board", uno, "uno_pcb.obj", "Marina/PCB/Teal");
            Part(s, "Headers and ICs", uno, "uno_parts.obj", "Marina/ICBlack");
            Part(s, "Connectors and standoffs", uno, "uno_metal.obj", "Marina/Tin");
            Entity bb = Group(s, "Breadboard", elec, S::BREADBOARD_ORIGIN);
            Part(s, "Body", bb, "breadboard.obj", "Marina/Breadboard");
            const struct { float z; const char* mat; } rails[] = { { -0.0258f, "Marina/RailBlue" }, { -0.0218f, "Marina/RailRed" }, { 0.0218f, "Marina/RailBlue" }, { 0.0258f, "Marina/RailRed" } };
            int n = 1;
            for (const auto& r : rails) Part(s, "Rail marking " + std::to_string(n++), bb, "breadboard_rail.obj", r.mat, { 0.0f, S::BREADBOARD_TOP, r.z }, false);
            Entity esp = Group(s, "Wireless A2 - ESP32-C3", bb, { S::ESP32_CENTRE.x, S::BREADBOARD_TOP, S::ESP32_CENTRE.z });
            Part(s, "Board", esp, "esp32c3_pcb.obj", "Marina/PCB/Black");
            Part(s, "Headers and buttons", esp, "esp32c3_parts.obj", "Marina/ICBlack");
            Part(s, "Module shield and USB-C", esp, "esp32c3_metal.obj", "Marina/Tin");
            Entity drv = Group(s, "Pump driver Q1 - MOSFET module", elec, S::DRIVER_ORIGIN);
            Part(s, "Board", drv, "driver_pcb.obj", "Marina/PCB/Blue");
            Part(s, "Header and MOSFET", drv, "driver_parts.obj", "Marina/ICBlack");
            Part(s, "Output terminal", drv, "driver_terminal.obj", "Marina/TerminalBlue");
            Part(s, "Pins and hardware", drv, "driver_metal.obj", "Marina/Tin");
        }

        // ---------------------------------------------------------------- 4 pump
        {
            Entity pump = Group(s, "4 Pump Assembly - P1", root, S::PUMP_ORIGIN);
            Part(s, "Bracket", pump, "pump_bracket.obj", "Marina/Aluminium");
            Part(s, "Motor", pump, "pump_motor.obj", "Marina/Stainless");
            Part(s, "Pump head", pump, "pump_head.obj", "Marina/DarkPolymer");
            Part(s, "Motor terminals", pump, "pump_terminals.obj", "Marina/Copper");
        }

        // ---------------------------------------------------------------- 5 heat exchanger
        {
            Entity hx = Group(s, "5 Heat Exchanger - HX1", root, S::HX_ORIGIN);
            Part(s, "Stand", hx, "hx_stand.obj", "Marina/Aluminium");
            Part(s, "Plate pack", hx, "hx_body.obj", "Marina/Stainless");
        }

        // ---------------------------------------------------------------- 6 membrane distillation unit
        {
            Entity md = Group(s, "6 Membrane Distillation - MD1", root, S::MD_ORIGIN);
            Part(s, "Cradles", md, "md_cradle.obj", "Marina/Aluminium");
            Part(s, "Hot-side housing", md, "md_hot_shell.obj", "Marina/Aluminium");
            Part(s, "Cold-side housing", md, "md_cold_shell.obj", "Marina/Aluminium");
            Part(s, "Membrane frame", md, "md_membrane.obj", "Marina/PTFE");
            Part(s, "Flange bolts", md, "md_bolts.obj", "Marina/Stainless");
            Part(s, "Hot-side band", md, "md_band.obj", "Marina/HotBand", { -0.045f, S::MD_CENTRE_Y, 0.0f });
            Part(s, "Cold-side band", md, "md_band.obj", "Marina/ColdBand", { 0.045f, S::MD_CENTRE_Y, 0.0f });
        }

        // ---------------------------------------------------------------- 7 seawater reservoir
        {
            Entity tank = Group(s, "7 Seawater Reservoir - TK1", root, S::TANK_ORIGIN);
            Part(s, "Seawater", tank, "tank_water.obj", "Marina/SeawaterBody", glm::vec3(0.0f), false);
            Part(s, "Vessel", tank, "tank_shell.obj", "Marina/ClearVessel", glm::vec3(0.0f), false);
            Part(s, "Lid", tank, "tank_lid.obj", "Marina/DarkPolymer");
            Part(s, "Fittings", tank, "tank_fittings.obj", "Marina/Stainless");
        }

        // ---------------------------------------------------------------- 8 condensation system
        {
            Entity cond = Group(s, "8 Condensation System", root, S::COND_ORIGIN);
            Entity cd = Group(s, "Condenser CD1", cond);
            Part(s, "Stand", cd, "cond_stand.obj", "Marina/Aluminium");
            Part(s, "Fin block", cd, "cond_fins.obj", "Marina/Aluminium");
            Part(s, "Coil", cd, "cond_coil.obj", "Marina/Copper");
            Entity tk2 = Group(s, "Freshwater Collection TK2", cond);
            Part(s, "Freshwater", tk2, "bottle_water.obj", "Marina/FreshwaterBody", glm::vec3(0.0f), false);
            Part(s, "Bottle", tk2, "bottle_shell.obj", "Marina/ClearVessel", glm::vec3(0.0f), false);
            Part(s, "Lid", tk2, "bottle_lid.obj", "Marina/DarkPolymer");
        }

        // ---------------------------------------------------------------- 2 cooling circuit (closed loop)
        {
            Entity loop = Group(s, "2 Cooling Circuit", root);
            const glm::vec3 wbOut = W(S::PI_ORIGIN, S::PI_WB_OUT), wbIn = W(S::PI_ORIGIN, S::PI_WB_IN);
            const glm::vec3 pIn = W(S::PUMP_ORIGIN, S::PUMP_IN), pOut = W(S::PUMP_ORIGIN, S::PUMP_OUT);
            const glm::vec3 hxIn = W(S::HX_ORIGIN, S::HX_COOLANT_IN), hxOut = W(S::HX_ORIGIN, S::HX_COOLANT_OUT);
            const RunStyle st = tubing(COOLANT);
            run(loop, "Coolant 1 - SBC to P1", { wbOut, { wbOut.x, 0.075f, wbOut.z }, { pIn.x, 0.075f, wbOut.z }, pIn }, st, COOLANT.color, true);
            run(loop, "Coolant 2 - P1 to HX1", { pOut, { pOut.x, 0.15f, pOut.z }, { pOut.x, 0.15f, -0.04f }, { hxIn.x, 0.15f, -0.04f }, { hxIn.x, hxIn.y, -0.04f }, hxIn },
                st, COOLANT.color, true);
            run(loop, "Coolant 3 - HX1 to SBC", { hxOut, { hxOut.x, hxOut.y, -0.045f }, { -0.13f, hxOut.y, -0.045f }, { -0.13f, 0.09f, -0.045f },
                                                  { wbIn.x, 0.09f, -0.045f }, { wbIn.x, 0.09f, wbIn.z }, wbIn }, st, COOLANT.color, true);
        }

        // ---------------------------------------------------------------- process tubing (hot / cold side)
        {
            Entity process = Group(s, "Process Tubing", root);
            Entity hot = Group(s, "Hot side - seawater", process);
            Entity cold = Group(s, "Cold side - distillate", process);
            const glm::vec3 tOut = W(S::TANK_ORIGIN, S::TANK_OUTLET), tRet = W(S::TANK_ORIGIN, S::TANK_RETURN);
            const glm::vec3 hxIn = W(S::HX_ORIGIN, S::HX_FEED_IN), hxOut = W(S::HX_ORIGIN, S::HX_FEED_OUT);
            const glm::vec3 mdIn = W(S::MD_ORIGIN, S::MD_HOT_IN), mdOut = W(S::MD_ORIGIN, S::MD_HOT_OUT), mdCold = W(S::MD_ORIGIN, S::MD_COLD_OUT);
            const glm::vec3 cIn = W(S::COND_ORIGIN, S::COND_IN), cOut = W(S::COND_ORIGIN, S::COND_OUT), bIn = W(S::COND_ORIGIN, S::BOTTLE_IN);
            run(hot, "Feed - TK1 to HX1", { tOut, { hxIn.x, tOut.y, tOut.z }, hxIn }, tubing(FEED), FEED.color, true);
            run(hot, "Heated feed - HX1 to MD1", { hxOut, { hxOut.x, hxOut.y, -0.04f }, { 0.08f, hxOut.y, -0.04f }, { 0.08f, mdIn.y, -0.04f }, { 0.08f, mdIn.y, mdIn.z }, mdIn },
                tubing(HOTFEED), HOTFEED.color, true);
            run(hot, "Brine - MD1 to TK1", { mdOut, { 0.07f, mdOut.y, mdOut.z }, { 0.07f, 0.20f, mdOut.z }, { 0.07f, 0.20f, tRet.z }, { tRet.x, 0.20f, tRet.z }, tRet },
                tubing(BRINE), BRINE.color, true);
            run(cold, "Vapour - MD1 to CD1", { mdCold, { 0.30f, mdCold.y, mdCold.z }, { 0.30f, 0.28f, mdCold.z }, { cIn.x, 0.28f, cIn.z }, cIn }, tubing(VAPOUR), VAPOUR.color, true);
            run(cold, "Condensate - CD1 to TK2", { cOut, bIn }, tubing(FRESH), FRESH.color, true);
        }

        // ---------------------------------------------------------------- 9 sensor wiring
        {
            Entity wiring = Group(s, "9 Sensor Wiring", root);
            Entity probes = Group(s, "Temperature probes", wiring);
            Entity cables = Group(s, "Probe cables", wiring);
            Entity jumpers = Group(s, "Jumper wires", wiring);
            const RunStyle cable{ S::CABLE_OD, "Marina/Cable", "" };
            auto wire = [&](const char* material) { return RunStyle{ S::WIRE_OD, material, "" }; };
            const glm::vec3 cableColour(0.5f);

            struct Probe { const char* name; const char* tag; const char* where; glm::vec3 mount; float high; };
            const Probe list[4] = {
                { "T1 - Seawater temperature", "T1", "seawater", W(S::TANK_ORIGIN, S::TANK_PROBE), 0.205f },
                { "T2 - Hot-side temperature", "T2", "hot side", W(S::MD_ORIGIN, S::MD_PROBE_HOT), 0.172f },
                { "T3 - Cold-side temperature", "T3", "cold side", W(S::MD_ORIGIN, S::MD_PROBE_COLD), 0.172f },
                { "T4 - Freshwater temperature", "T4", "freshwater", W(S::COND_ORIGIN, S::BOTTLE_PROBE), 0.152f },
            };
            const glm::vec3 landing = W(S::BREADBOARD_ORIGIN, glm::vec3(S::BB_PROBE_X, S::BREADBOARD_TOP, 0.0f));
            for (int k = 0; k < 4; ++k) {
                const Probe& p = list[k];
                Entity g = Group(s, p.name, probes, p.mount);
                Part(s, "Probe", g, "probe.obj", "Marina/Stainless");
                Part(s, "Cable gland", g, "probe_gland.obj", "Marina/DarkPolymer");
                // cable: up, back to the duct, along the duct (own lane), out onto the sub-plate, onto the bus row
                const glm::vec3 top = p.mount + glm::vec3(0.0f, S::PROBE_HEIGHT, 0.0f);
                const float laneZ = -0.2355f + 0.004f * float(k), trayX = -0.200f + 0.004f * float(k), riseZ = 0.168f + 0.007f * float(k);
                const float duct = 0.013f, over = 0.025f;
                run(cables, std::string("Cable ") + p.tag, { top, { top.x, p.high, top.z }, { top.x, p.high, laneZ }, { top.x, duct, laneZ }, { trayX, duct, laneZ },
                                                            { trayX, duct, riseZ }, { trayX, over, riseZ }, { landing.x, over, riseZ }, { landing.x, landing.y, riseZ } },
                    cable, cableColour, false);
            }

            const glm::vec3 fan5v = W(S::PI_ORIGIN, S::PI_FAN_5V_EXIT), fanGnd = W(S::PI_ORIGIN, S::PI_FAN_GND_EXIT);
            const glm::vec3 pin5v = W(S::PI_ORIGIN, S::PI_PIN_5V), pinGnd = W(S::PI_ORIGIN, S::PI_PIN_GND);
            run(jumpers, "Fan +5V - SBC GPIO", { fan5v, { fan5v.x, fan5v.y, pin5v.z }, pin5v }, wire("Marina/Wire/Red"), glm::vec3(0.5f), false);
            run(jumpers, "Fan GND - SBC GPIO", { fanGnd, { fanGnd.x, fanGnd.y, pinGnd.z }, pinGnd }, wire("Marina/Wire/Black"), glm::vec3(0.5f), false);

            const glm::vec3 d9 = W(S::ARDUINO_ORIGIN, S::ARD_D9), gnd = W(S::ARDUINO_ORIGIN, S::ARD_GND), d2 = W(S::ARDUINO_ORIGIN, S::ARD_D2);
            const glm::vec3 sda = W(S::ARDUINO_ORIGIN, S::ARD_SDA), scl = W(S::ARDUINO_ORIGIN, S::ARD_SCL);
            const glm::vec3 sig = W(S::DRIVER_ORIGIN, S::DRV_SIG), dgnd = W(S::DRIVER_ORIGIN, S::DRV_GND);
            const glm::vec3 outPos = W(S::DRIVER_ORIGIN, S::DRV_OUT_POS), outNeg = W(S::DRIVER_ORIGIN, S::DRV_OUT_NEG);
            const glm::vec3 termPos = W(S::PUMP_ORIGIN, S::PUMP_TERM_POS), termNeg = W(S::PUMP_ORIGIN, S::PUMP_TERM_NEG);
            const glm::vec3 bbSda = W(S::BREADBOARD_ORIGIN, S::BB_SDA), bbScl = W(S::BREADBOARD_ORIGIN, S::BB_SCL), bbBus = W(S::BREADBOARD_ORIGIN, S::BB_BUS);
            run(jumpers, "PWM - A1 D9 to Q1", { d9, { d9.x, 0.030f, d9.z }, { sig.x, 0.030f, d9.z }, { sig.x, 0.030f, sig.z }, sig }, wire("Marina/Wire/Yellow"), glm::vec3(0.5f), false);
            run(jumpers, "GND - A1 to Q1", { gnd, { gnd.x, 0.036f, gnd.z }, { dgnd.x, 0.036f, gnd.z }, { dgnd.x, 0.036f, dgnd.z }, dgnd }, wire("Marina/Wire/Black"), glm::vec3(0.5f), false);
            const float floor = S::TRAY_TOP + 0.0015f;
            run(jumpers, "Pump + - Q1 to P1", { outPos, { outPos.x, 0.028f, outPos.z }, { outPos.x, 0.028f, 0.012f }, { outPos.x, floor, 0.012f }, { outPos.x, floor, -0.238f },
                                                { -0.148f, floor, -0.238f }, { -0.148f, floor, termPos.z }, { -0.148f, termPos.y, termPos.z }, termPos },
                wire("Marina/Wire/Red"), glm::vec3(0.5f), false);
            run(jumpers, "Pump - - Q1 to P1", { outNeg, { outNeg.x, 0.028f, outNeg.z }, { outNeg.x, 0.028f, 0.012f }, { outNeg.x, floor, 0.012f }, { outNeg.x, floor, -0.2405f },
                                                { -0.143f, floor, -0.2405f }, { -0.143f, floor, termNeg.z }, { -0.143f, termNeg.y, termNeg.z }, termNeg },
                wire("Marina/Wire/Black"), glm::vec3(0.5f), false);
            run(jumpers, "I2C SDA - A2 to A1", { bbSda, { bbSda.x, 0.030f, bbSda.z }, { bbSda.x, 0.030f, sda.z }, sda }, wire("Marina/Wire/Green"), glm::vec3(0.5f), false);
            run(jumpers, "I2C SCL - A2 to A1", { bbScl, { bbScl.x, 0.030f, bbScl.z }, { bbScl.x, 0.030f, scl.z }, scl }, wire("Marina/Wire/White"), glm::vec3(0.5f), false);
            run(jumpers, "1-Wire bus - breadboard to A1 D2", { bbBus, { bbBus.x, 0.045f, bbBus.z }, { d2.x, 0.045f, bbBus.z }, { d2.x, 0.045f, d2.z }, d2 },
                wire("Marina/Wire/Blue"), glm::vec3(0.5f), false);
        }
    }

    void MarinaScene::ApplyCamera(GameContext& ctx, int preset) {
        m_camera = std::clamp(preset, 0, int(std::size(PRESETS)) - 1);
        const CameraPreset& p = PRESETS[m_camera];
        ctx.camera.SetOrbit(p.target, p.distance, p.yaw, p.pitch);
    }

    // ==================================================================== per frame
    void MarinaScene::Update(GameContext& ctx, float dt) {
        (void)ctx;
        if (!m_cameraNearSet) {   // the controller creates MainCamera on its first frame
            if (Camera* cam = FindByName("MainCamera").TryGet<Camera>()) {
                cam->nearPlane = 0.01f;
                cam->farPlane = 30.0f;
                m_cameraNearSet = true;
            }
        }
        m_flowPhase += dt * flowSpeed;
        if (spinFan) {
            if (Entity rotor = FindByName("Fan rotor")) {
                Transform& t = rotor.LocalTransform();
                t.rotation = glm::normalize(t.rotation * glm::angleAxis(dt * 2.0f * PI * 2.5f, glm::vec3(0, 1, 0)));   // visual only
            }
        }
    }

    bool MarinaScene::RoutePolyline(const std::string& group, std::vector<glm::vec3>& out) { return ProjectKit::RunPolyline(*this, group, out); }
    float MarinaScene::RouteGap(const std::string& group) { return ProjectKit::RunGap(*this, group); }

    void MarinaScene::DrawUI(GameContext&) {
        if (!showFlow) return;
        Entity camera = GetPrimaryCamera();
        if (!camera) return;
        const glm::vec3 eye = camera.WorldPosition();
        std::vector<glm::vec3> line;
        std::vector<float> along;
        for (const FlowRoute& route : m_routes) {
            if (!route.fluid || !RoutePolyline(route.group, line)) continue;
            along.assign(line.size(), 0.0f);
            for (size_t i = 1; i < line.size(); ++i) along[i] = along[i - 1] + glm::length(line[i] - line[i - 1]);
            const float total = along.back();
            if (total < 1e-4f) continue;
            auto at = [&](float s) {
                const size_t i = size_t(std::upper_bound(along.begin(), along.end(), s) - along.begin());
                const size_t k = std::clamp<size_t>(i, 1, line.size() - 1);
                const float span = std::max(along[k] - along[k - 1], 1e-6f);
                return glm::mix(line[k - 1], line[k], std::clamp((s - along[k - 1]) / span, 0.0f, 1.0f));
            };
            // Dashes travel along the run in its flow direction (schematic: constant speed, no data).
            const float spacing = 0.03f, dash = 0.011f, speed = 0.035f;
            const float offset = std::fmod(m_flowPhase * speed, spacing);
            const glm::vec4 color(route.color * 3.2f, 1.0f);
            for (float s0 = offset; s0 + dash <= total; s0 += spacing) {
                std::vector<Render::RibbonPoint> pts;
                for (int k = 0; k <= 3; ++k) {
                    const glm::vec3 p = at(s0 + dash * float(k) / 3.0f);
                    // lift the dash just in front of the fluid core (inside the clear wall)
                    const glm::vec3 toEye = glm::normalize(eye - p);
                    Render::RibbonPoint rp;
                    rp.position = p + toEye * (0.34f * route.od);
                    rp.width = 0.38f * route.od;
                    const float fade = std::sin(PI * float(k) / 3.0f) * 0.6f + 0.4f;
                    rp.color = glm::vec4(glm::vec3(color) * fade, 1.0f);
                    pts.push_back(rp);
                }
                Render::DrawRibbon(pts);
            }
        }
    }

    // ==================================================================== studio
    // ==================================================================== workspace panel
    void MarinaScene::OnAfterLoad(GameContext&) { m_cameraNearSet = false; }

    void MarinaScene::DrawWorkspacePanel(GameContext& ctx) {
        const ImVec4 heading(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f);
        const float bw = 98.0f * UI::Scale();
        ImGui::TextColored(heading, "CAMERA");
        for (int i = 0; i < int(std::size(PRESETS)); ++i) {
            if (i % 3) ImGui::SameLine();
            if (Retro::Button(PRESETS[i].name, bw, m_camera == i ? 1 : 0)) ApplyCamera(ctx, i);
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "DISPLAY");
        if (Retro::LedToggle("CAD edges", &cadEdges)) Showcase::SetCadInk(cadEdges, 0.55f, 1.0f);
        ImGui::SameLine();
        Retro::LedToggle("Fan spin", &spinFan);
        Retro::LedToggle("Flow direction", &showFlow);
        if (showFlow) Retro::EqSlider("flow speed", &flowSpeed, 0.0f, 3.0f, "%.1fx");
        if (showFlow) {
            for (const Circuit* c : CIRCUITS) {
                ImGui::ColorButton(c->name, ImVec4(c->color.r, c->color.g, c->color.b, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(14, 10));
                ImGui::SameLine();
                ImGui::TextUnformatted(c->name);
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "SUBSYSTEMS");
        std::vector<ProjectKit::SubsystemRow> rows;
        if (Entity root = FindByName(ROOT)) for (Entity e : SortedChildren(root)) rows.push_back({ e.GetName(), e.GetName() });
        ProjectKit::SubsystemList(*this, "##marina_bom", rows);
        ProjectKit::SelectedAssembly(*this, ROOT);

        ImGui::Spacing();
        ImGui::TextColored(heading, "SCENE FILE");
        ProjectKit::ProjectFileButtons(ctx, *this, m_status);
        ProjectKit::ScreenshotButton("project_marina", m_status);
    }

    void RegisterMarinaScene(SceneManager& scenes) {
        scenes.Register("ProjectMarina", [] { return std::make_unique<MarinaScene>(); });
    }
}
