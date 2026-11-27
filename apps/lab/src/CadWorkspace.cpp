#include "CadWorkspace.h"
#include "Showcase.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace Kit = Lab::ProjectKit;

namespace Lab {
    namespace {
        constexpr float PI = Kit::PI;
        constexpr float FOV = 0.62f;
        const char* const VIEW_NAMES[CadWorkspace::ViewCount] = { "Perspective", "Front", "Rear", "Left", "Right", "Top", "Bottom" };
    }

    // ==================================================================== lifecycle
    void CadWorkspace::OnEnter(GameContext& ctx) {
        m_savedSettings = Render::Settings();
        CameraSettings& cam = ctx.camera.Settings();
        m_savedFov = cam.verticalFovRadians;
        m_savedMinDistance = cam.orbitMinDistance;
        m_savedMinPitch = cam.minPitch;
        m_savedMaxPitch = cam.maxPitch;
        m_savedOverlay = ctx.overlayVisible;
        cam.verticalFovRadians = FOV;
        cam.orbitMinDistance = 0.15f;
        cam.minPitch = -0.5f * PI + 1e-3f;   // true top / bottom views
        cam.maxPitch = 0.5f * PI - 1e-3f;
        ctx.overlayVisible = false;
        ctx.crosshairVisible = false;
        Kit::Material("CAD/Ground", { 0.80f, 0.81f, 0.83f }, 0.92f, 0.0f, glm::vec3(0.0f), 1.0f, 2, { 0.70f, 0.71f, 0.74f }, 10.0f, 0.015f);
        ApplyEnvironment();
        Build();
        Showcase::SetCadInk(cadEdges, 0.45f, 1.0f);
        m_cameraNearSet = false;
        ApplyView(ctx, Perspective);
    }

    void CadWorkspace::OnExit(GameContext& ctx) {
        Showcase::SetCadInk(false);
        Render::Settings() = m_savedSettings;
        CameraSettings& cam = ctx.camera.Settings();
        cam.verticalFovRadians = m_savedFov;
        cam.orbitMinDistance = m_savedMinDistance;
        cam.minPitch = m_savedMinPitch;
        cam.maxPitch = m_savedMaxPitch;
        ctx.overlayVisible = m_savedOverlay;
        ctx.crosshairVisible = true;
    }

    void CadWorkspace::ApplyEnvironment() {   // neutral engineering studio: light grey, no fog
        Render::RenderSettings& r = Render::Settings();
        r.skyColorZenith = { 0.78f, 0.80f, 0.83f };
        r.skyColorHorizon = { 0.88f, 0.89f, 0.90f };
        r.groundColor = { 0.80f, 0.81f, 0.83f };
        r.ambientUp = { 0.46f, 0.48f, 0.52f };
        r.ambientDown = { 0.30f, 0.30f, 0.31f };
        r.ambientIntensity = 1.0f;
        r.rimColor = { 1.0f, 1.0f, 1.0f };
        r.rimIntensity = 0.06f;
        r.fog.enabled = false;
        r.exposure = 1.0f;
        r.bloom.threshold = 2.0f;
        r.bloom.intensity = 0.02f;
        r.ssao.radius = 0.06f;
        r.ssao.bias = 0.005f;
        r.shadowsEnabled = true;
        r.shadowDistance = 4.0f;
        r.clearColor = glm::vec4(r.skyColorHorizon, 1.0f);
    }

    // ==================================================================== build
    void CadWorkspace::Build() {
        Scene& s = *this;
        // studio: lights and a ground reference grid only (toggle "Ground"; hidden in the bottom view)
        Entity studio = Kit::Group(s, STUDIO, Entity());
        Entity ground = Kit::Part(s, "Ground grid", studio, "Cube", "CAD/Ground", { 0.0f, -0.001f, 0.0f }, false);
        ground.Get<Transform>().scale = glm::vec3(3.0f, 0.002f, 3.0f);
        auto light = [&](const char* name, glm::vec3 position, glm::vec3 color, float intensity, bool shadows) {
            Entity e = Kit::Group(s, name, studio, position);
            e.Get<Transform>().rotation = glm::quatLookAt(glm::normalize(-position), glm::vec3(0, 1, 0));
            Light l;
            l.type = LightType::Directional;
            l.color = color;
            l.intensity = intensity;
            l.castShadows = shadows;
            e.Add<Light>(l);
        };
        light("Key light", { 2.5f, 4.0f, 3.0f }, { 1.0f, 0.97f, 0.92f }, 2.4f, true);
        light("Fill light", { -3.0f, 2.5f, -2.0f }, { 0.80f, 0.86f, 1.0f }, 0.7f, false);
        light("Under light", { 0.5f, -3.0f, 1.0f }, { 0.9f, 0.9f, 0.95f }, 0.35f, false);   // lets the bottom view read

        m_built = BuildAssembly();
        m_explode = m_explodeTarget = 0.0f;
        OnAssemblyBuilt();
    }

    void CadWorkspace::ApplyExplode(float from, float to) {
        const float delta = to - from;
        if (std::abs(delta) > 1e-6f) {
            for (const ExplodePart& p : m_built.explode) if (Entity e = FindByName(p.entity)) e.LocalTransform().position += p.offset * delta;
            if (Entity root = FindByName(Workspace().rootName)) root.LocalTransform().position += Workspace().explodeRootLift * delta;
        }
        for (const std::string& w : m_built.wiring) if (Entity e = FindByName(w)) Kit::SetVisible(e, to < 0.01f);   // wiring follows the assembled layout
    }

    void CadWorkspace::SetExplodeNow(float t) {
        ApplyExplode(m_explode, t);
        m_explode = m_explodeTarget = t;
    }

    void CadWorkspace::SetGroundVisible() {
        if (Entity g = FindByName("Ground grid")) Kit::SetVisible(g, showGround && m_view != Bottom);
    }

    // ==================================================================== camera
    void CadWorkspace::ApplyView(GameContext& ctx, int view) {
        const Description& w = Workspace();
        m_view = std::clamp(view, 0, ViewCount - 1);
        const float top = 0.5f * PI - 1e-3f;
        float yaw = 0.0f, pitch = 0.0f;
        switch (m_view) {   // forward = (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch)
            case Perspective: yaw = w.perspectiveYaw; pitch = w.perspectivePitch; break;
            case Front: yaw = 0.5f * PI; break;            // looking -X at the front
            case Rear: yaw = -0.5f * PI; break;
            case Left: yaw = PI; break;                     // left side = -Z
            case Right: yaw = 0.0f; break;                  // right side = +Z
            case Top: yaw = -0.5f * PI; pitch = -top; break;   // front towards the top of the screen
            case Bottom: yaw = -0.5f * PI; pitch = top; break;
            default: break;
        }
        const float ex = std::max(m_explode, m_explodeTarget);   // frame the layout being shown / animated to
        const float grow = 1.0f + w.explodeGrow * ex;
        const glm::vec3 shift = (w.explodeTargetShift + w.explodeRootLift) * ex;
        const glm::vec3 target = (m_view == Top || m_view == Bottom ? w.planCentre : w.centre) + shift;
        const float distance = (m_view == Perspective ? w.perspectiveDistance : w.orthoDistance) * grow;
        ctx.camera.SetOrbit(target, distance, yaw, pitch);
        SetGroundVisible();
    }

    // ==================================================================== frame
    void CadWorkspace::Update(GameContext& ctx, float dt) {
        if (Entity camera = FindByName("MainCamera")) {
            if (Camera* cam = camera.TryGet<Camera>()) {
                if (!m_cameraNearSet) { cam->nearPlane = 0.01f; cam->farPlane = 60.0f; m_cameraNearSet = true; }
                cam->projection = m_view == Perspective ? Projection::Perspective : Projection::Orthographic;
                // orthographic extent follows the orbit distance, so mouse-wheel zoom works in every view
                cam->orthoHeight = 2.0f * std::tan(0.5f * FOV) * ctx.camera.GetOrbitDistance();
            }
        }
        const float d = m_explodeTarget - m_explode;
        if (std::abs(d) > 1e-4f) {
            const float next = m_explode + std::clamp(d, -dt * 1.6f, dt * 1.6f);
            ApplyExplode(m_explode, next);
            m_explode = next;
        }
    }

    // ==================================================================== studio
    void CadWorkspace::DrawStudioOverlay(GameContext&) {
        if (showLabels && m_view == Perspective) Kit::DrawCallouts(*this, m_built.callouts);   // principal views stay clean
        if (!UI::PanelsVisible()) {
            std::vector<std::string> lines = Workspace().titleBlock;
            lines.insert(lines.end() - (lines.empty() ? 0 : 1), std::string("View: ") + VIEW_NAMES[m_view]);
            Kit::DrawTitleBlock(lines);
        }
    }

    void CadWorkspace::DrawStudioPanel(GameContext& ctx) {
        const Description& w = Workspace();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (!Retro::BeginPanel(w.panelTitle, 10.0f, 30.0f, 330.0f, std::max(display.y - 340.0f, 300.0f))) { Retro::EndPanel(); return; }
        const ImVec4 heading(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f);
        Retro::Lcd(w.panelTitle, w.lcdSubtitle);

        ImGui::TextColored(heading, "VIEW");
        for (int i = 0; i < ViewCount; ++i) {
            if (i % 3) ImGui::SameLine();
            if (Retro::Button(VIEW_NAMES[i], 98.0f, m_view == i ? 1 : 0)) ApplyView(ctx, i);
        }
        ImGui::TextDisabled("principal views: orthographic, no callouts");

        ImGui::Spacing();
        ImGui::TextColored(heading, "EXPLODE");
        if (Retro::Button("Assembled", 98.0f, m_explodeTarget < 0.5f ? 1 : 0)) { m_explodeTarget = 0.0f; ApplyView(ctx, m_view); }
        ImGui::SameLine();
        if (Retro::Button("Exploded", 98.0f, m_explodeTarget >= 0.5f ? 1 : 0)) { m_explodeTarget = 1.0f; ApplyView(ctx, m_view); }
        float e = m_explodeTarget;
        if (Retro::EqSlider("explode", &e, 0.0f, 1.0f)) m_explodeTarget = e;

        DrawAssemblyPanel(ctx);

        ImGui::Spacing();
        ImGui::TextColored(heading, "DISPLAY");
        Retro::LedToggle("Callouts", &showLabels);
        ImGui::SameLine();
        if (Retro::LedToggle("CAD edges", &cadEdges)) Showcase::SetCadInk(cadEdges, 0.45f, 1.0f);
        ImGui::SameLine();
        if (Retro::LedToggle("Ground", &showGround)) SetGroundVisible();

        ImGui::Spacing();
        ImGui::TextColored(heading, "SUBSYSTEMS");
        ImGui::TextDisabled("click to select | box = visible");
        Kit::SubsystemList(*this, "##cad_subsystems", w.subsystems, 9.5f);
        // individual components: hide the selected part (or group) / show everything again
        const uint32_t selected = Studio::SelectedEntity();
        Entity sel = selected ? FromId(selected - 1) : Entity();
        if (Retro::Button("Hide selected", 98.0f) && sel) Kit::SetVisible(sel, false);
        ImGui::SameLine();
        if (Retro::Button("Show all", 98.0f)) if (Entity root = FindByName(w.rootName)) {
            Kit::SetVisible(root, true);
            ApplyExplode(m_explode, m_explode);   // wiring stays hidden while exploded
        }
        // selection: name, subsystem (the group directly under the assembly), material; transform and
        // material are edited in the studio inspector
        if (sel) {
            const Entity root = FindByName(w.rootName);
            std::vector<Entity> chain;
            for (Entity e = sel; e && !(e == root); e = e.GetParent()) chain.push_back(e);
            const bool inAssembly = root && (sel == root || sel.IsDescendantOf(root));
            std::string subsystem = "-";
            if (inAssembly && !chain.empty()) {
                size_t top = chain.size() - 1;
                if (root.GetChildren().size() == 1 && top > 0) --top;   // skip a single body group (e.g. "Rover body")
                subsystem = chain[top].GetName();
            }
            ImGui::TextWrapped("Part: %s", sel.GetName().c_str());
            ImGui::TextWrapped("Subsystem: %s", subsystem.c_str());
            if (const MeshRenderer* mr = sel.TryGet<MeshRenderer>()) ImGui::TextWrapped("Material: %s", mr->material.c_str());
            if (Entity parent = sel.GetParent(); parent && !(parent == root) && inAssembly)
                if (Retro::Button("Select parent assembly", 200.0f)) Studio::Select(parent.Id() + 1);
            ImGui::TextDisabled("transform + material: inspector (right)");
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "PROJECT FILE");
        float saved = 0.0f;
        Kit::ProjectFileButtons(ctx, *this, m_status,
            [&] { saved = m_explode; SetExplodeNow(0.0f); },   // saved assembled
            [&] { SetExplodeNow(saved); },
            [this] { m_explode = m_explodeTarget = 0.0f; m_cameraNearSet = false; });
        Kit::ScreenshotButton(w.screenshotPrefix, m_status);
        ImGui::TextDisabled("same assembly as %s", w.projectScene);
        Retro::EndPanel();
    }
}
