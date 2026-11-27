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
namespace Kit = Projects::ProjectKit;

namespace Projects {
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

    // ==================================================================== persistence hooks
    // Every save / load path (File menu, the panel buttons) runs these through the editor, so the
    // scene file always holds the assembled layout and the explode state matches the parts.
    void CadWorkspace::OnBeforeSave(GameContext&) {
        m_savedExplode = m_explode;
        SetExplodeNow(0.0f);
    }

    void CadWorkspace::OnAfterSave(GameContext&) { SetExplodeNow(m_savedExplode); }

    void CadWorkspace::OnAfterLoad(GameContext&) {
        m_explode = m_explodeTarget = 0.0f;
        m_cameraNearSet = false;
        SetGroundVisible();
    }

    // ==================================================================== panel
    void CadWorkspace::DrawWorkspacePanel(GameContext& ctx) {
        const Description& w = Workspace();
        const ImVec4 heading(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f);
        const float bw = Editor::RowButtonWidth(3), bw2 = Editor::RowButtonWidth(2);

        ImGui::TextColored(heading, "VIEW");
        if (Retro::Button(VIEW_NAMES[Perspective], Editor::RowButtonWidth(1), m_view == Perspective ? 1 : 0)) ApplyView(ctx, Perspective);
        for (int i = Front; i < ViewCount; ++i) {   // the six orthographic views, three per row
            if ((i - Front) % 3) ImGui::SameLine();
            if (Retro::Button(VIEW_NAMES[i], bw, m_view == i ? 1 : 0)) ApplyView(ctx, i);
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "EXPLODE");
        if (Retro::Button("Assembled", bw2, m_explodeTarget < 0.5f ? 1 : 0)) { m_explodeTarget = 0.0f; ApplyView(ctx, m_view); }
        ImGui::SameLine();
        if (Retro::Button("Exploded", bw2, m_explodeTarget >= 0.5f ? 1 : 0)) { m_explodeTarget = 1.0f; ApplyView(ctx, m_view); }
        float e = m_explodeTarget;
        if (Retro::EqSlider("explode", &e, 0.0f, 1.0f)) m_explodeTarget = e;

        DrawAssemblyPanel(ctx);

        ImGui::Spacing();
        ImGui::TextColored(heading, "DISPLAY");
        if (Retro::LedToggle("CAD edges", &cadEdges)) Showcase::SetCadInk(cadEdges, 0.45f, 1.0f);
        ImGui::SameLine();
        if (Retro::LedToggle("Ground", &showGround)) SetGroundVisible();

        ImGui::Spacing();
        ImGui::TextColored(heading, "SUBSYSTEMS");
        Kit::SubsystemList(*this, "##cad_subsystems", w.subsystems, 9.5f);
        const uint32_t selected = Editor::SelectedEntity();
        Entity sel = selected ? FromId(selected - 1) : Entity();
        if (Retro::Button("Hide selected", bw2) && sel) { Kit::SetVisible(sel, false); Editor::MarkEdited(); }
        ImGui::SameLine();
        if (Retro::Button("Show all", bw2)) if (Entity root = FindByName(w.rootName)) {
            Kit::SetVisible(root, true);
            ApplyExplode(m_explode, m_explode);   // wiring stays hidden while exploded
            Editor::MarkEdited();
        }
        if (sel) {   // selection: part, subsystem (the group directly under the assembly), material
            const Entity root = FindByName(w.rootName);
            std::vector<Entity> chain;
            for (Entity c = sel; c && !(c == root); c = c.GetParent()) chain.push_back(c);
            const bool inAssembly = root && (sel == root || sel.IsDescendantOf(root));
            std::string subsystem = "-";
            if (inAssembly && !chain.empty()) {
                size_t top = chain.size() - 1;
                if (root.GetChildren().size() == 1 && top > 0) --top;   // skip a single body group (e.g. "Rover body")
                subsystem = chain[top].GetName();
            }
            if (ImGui::BeginTable("##cad_sel", 2, ImGuiTableFlags_SizingFixedFit)) {
                auto row = [&](const char* k, const std::string& v) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(heading, "%s", k);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v.c_str());
                };
                row("part", sel.GetName());
                row("subsystem", subsystem);
                if (const MeshRenderer* mr = sel.TryGet<MeshRenderer>()) row("material", mr->material);
                ImGui::EndTable();
            }
            if (Entity parent = sel.GetParent(); parent && !(parent == root) && inAssembly)
                if (Retro::Button("Select parent assembly", Editor::RowButtonWidth(1))) Editor::Select(parent.Id() + 1);
        }

        ImGui::Spacing();
        ImGui::TextColored(heading, "SCENE FILE");
        Kit::ProjectFileButtons(ctx, *this, m_status);
        Kit::ScreenshotButton(w.screenshotPrefix, m_status);
    }
}
