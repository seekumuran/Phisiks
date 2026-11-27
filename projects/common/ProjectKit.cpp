#include "ProjectKit.h"
#include "Editor.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Projects::ProjectKit {
    void Material(const std::string& name, glm::vec3 base, float roughness, float metallic, glm::vec3 emissive, float opacity, int pattern,
                  glm::vec3 patternColor, float patternScale, float lineWidth) {
        Render::MaterialDesc d;
        d.baseColor = base;
        d.roughness = roughness;
        d.metallic = metallic;
        d.emissive = emissive;
        d.opacity = opacity;
        d.pattern = Render::Pattern(pattern);
        d.patternColor = patternColor;
        d.patternScale = patternScale;
        d.lineWidth = lineWidth;
        Render::RegisterMaterial(name, d);
    }

    Entity Group(Scene& scene, const std::string& name, Entity parent, const glm::vec3& position) {
        Entity e = scene.CreateEntity(name);
        if (parent) e.SetParent(parent, false);
        e.Get<Transform>().position = position;
        return e;
    }

    Entity Part(Scene& scene, const std::string& name, Entity parent, const std::string& mesh, const std::string& material, const glm::vec3& position, bool castShadows) {
        Entity e = Group(scene, name, parent, position);
        MeshRenderer r{ mesh, material };
        r.castShadows = castShadows;
        e.Add<MeshRenderer>(r);
        return e;
    }

    glm::quat FromY(const glm::vec3& dir) {
        const glm::vec3 d = glm::normalize(dir);
        const float c = glm::dot(glm::vec3(0, 1, 0), d);
        if (c > 0.9999f) return glm::quat(1, 0, 0, 0);
        if (c < -0.9999f) return glm::angleAxis(PI, glm::vec3(1, 0, 0));
        return glm::angleAxis(std::acos(c), glm::normalize(glm::cross(glm::vec3(0, 1, 0), d)));
    }

    Entity BuildRun(Scene& scene, Entity parent, const std::string& name, std::vector<glm::vec3> pts, const RunStyle& style) {
        const bool tube = !style.core.empty();
        if (style.sleeve > 0.0f) {
            pts.front() -= glm::normalize(pts[1] - pts[0]) * style.sleeve;
            pts.back() += glm::normalize(pts.back() - pts[pts.size() - 2]) * style.sleeve;
        }
        Entity group = Group(scene, name, parent, pts.front());
        const glm::vec3 origin = pts.front();
        const float R = BEND_RATIO * style.od;
        const std::string kit = KIT;
        int number = 1;
        auto label = [&](const char* kind) {
            char text[32];
            std::snprintf(text, sizeof(text), "%02d %s", number++, kind);
            return std::string(text);
        };
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const glm::vec3 d = glm::normalize(pts[i + 1] - pts[i]);
            const glm::vec3 a = pts[i] + (i > 0 ? d * R : glm::vec3(0.0f));
            const glm::vec3 b = pts[i + 1] - (i + 2 < pts.size() ? d * R : glm::vec3(0.0f));
            const float length = glm::dot(b - a, d);
            if (length < -1e-5f) Log::Warning() << scene.Name() << ": run '" << name << "' segment " << i << " is shorter than its bends";
            if (length > 1e-5f) {
                Entity piece = Part(scene, label(tube ? "Tube" : "Wire"), group, kit + (tube ? "tube_straight.obj" : "rod.obj"), style.shell, (a + b) * 0.5f - origin, !tube);
                Transform& t = piece.Get<Transform>();
                t.rotation = FromY(d);
                t.scale = glm::vec3(style.od, length, style.od);
                if (tube) Part(scene, "Fluid", piece, kit + "rod.obj", style.core).Get<Transform>().scale = glm::vec3(CORE_RATIO, 1.0f, CORE_RATIO);
            }
            if (i + 2 < pts.size()) {   // bend at pts[i + 1]: in along d, out along the next direction
                const glm::vec3 e = glm::normalize(pts[i + 2] - pts[i + 1]);
                if (std::abs(glm::dot(d, e)) > 1e-3f) Log::Warning() << scene.Name() << ": run '" << name << "' corner " << i + 1 << " is not 90 degrees";
                Entity bend = Part(scene, label("Bend"), group, kit + "tube_bend.obj", style.shell, pts[i + 1] - origin, !tube);
                Transform& t = bend.Get<Transform>();
                t.rotation = glm::quat_cast(glm::mat3(d, e, glm::cross(d, e)));
                t.scale = glm::vec3(style.od);
                if (tube) Part(scene, "Fluid", bend, kit + "tube_bend_core.obj", style.core);
            }
        }
        return group;
    }

    namespace {
        bool IsBend(const MeshRenderer& r) { return r.mesh.find("tube_bend") != std::string::npos; }
    }

    bool RunPolyline(Scene& scene, const std::string& group, std::vector<glm::vec3>& out) {
        out.clear();
        Entity g = scene.FindByName(group);
        if (!g) return false;
        for (Entity piece : SortedChildren(g)) {
            const MeshRenderer* r = piece.TryGet<MeshRenderer>();
            if (!r || !r->visible) continue;
            const glm::mat4 m = piece.WorldMatrix();
            auto add = [&](const glm::vec3& local) {
                const glm::vec3 p = glm::vec3(m * glm::vec4(local, 1.0f));
                if (out.empty() || glm::length(out.back() - p) > 1e-6f) out.push_back(p);
            };
            if (IsBend(*r)) {
                for (int i = 0; i <= 8; ++i) {
                    const float a = -0.5f * PI + 0.5f * PI * float(i) / 8.0f;
                    add(glm::vec3(-BEND_RATIO + BEND_RATIO * std::cos(a), BEND_RATIO + BEND_RATIO * std::sin(a), 0.0f));
                }
            } else {
                add(glm::vec3(0.0f, -0.5f, 0.0f));
                add(glm::vec3(0.0f, 0.5f, 0.0f));
            }
        }
        return out.size() >= 2;
    }

    float RunGap(Scene& scene, const std::string& group) {
        Entity g = scene.FindByName(group);
        if (!g) return 1e9f;
        float gap = 0.0f;
        bool first = true;
        glm::vec3 previousEnd(0.0f);
        for (Entity piece : SortedChildren(g)) {
            const MeshRenderer* r = piece.TryGet<MeshRenderer>();
            if (!r) continue;
            const glm::mat4 m = piece.WorldMatrix();
            const bool bend = IsBend(*r);
            const glm::vec3 a = glm::vec3(m * glm::vec4(bend ? glm::vec3(-BEND_RATIO, 0.0f, 0.0f) : glm::vec3(0.0f, -0.5f, 0.0f), 1.0f));
            const glm::vec3 b = glm::vec3(m * glm::vec4(bend ? glm::vec3(0.0f, BEND_RATIO, 0.0f) : glm::vec3(0.0f, 0.5f, 0.0f), 1.0f));
            if (!first) gap = std::max(gap, glm::length(a - previousEnd));
            previousEnd = b;
            first = false;
        }
        return first ? 1e9f : gap;
    }

    void SetVisible(Entity e, bool visible) {
        if (MeshRenderer* r = e.TryGet<MeshRenderer>()) r->visible = visible;
        for (Entity c : e.GetChildren()) SetVisible(c, visible);
    }
    int CountVisible(Entity e, int& total) {
        int visible = 0;
        if (const MeshRenderer* r = e.TryGet<MeshRenderer>()) { ++total; if (r->visible) ++visible; }
        for (Entity c : e.GetChildren()) visible += CountVisible(c, total);
        return visible;
    }
    bool AnyVisible(Entity e) {
        int total = 0;
        return CountVisible(e, total) > 0 || total == 0;
    }
    std::vector<Entity> SortedChildren(Entity e) {
        std::vector<Entity> children = e.GetChildren();
        std::sort(children.begin(), children.end(), [](const Entity& a, const Entity& b) { return a.GetName() < b.GetName(); });
        return children;
    }

    void SubsystemList(Scene& scene, const char* id, const std::vector<SubsystemRow>& rows, float lines) {
        const uint32_t selected = Editor::SelectedEntity();
        if (Retro::BeginPlaylist(id, lines * ImGui::GetTextLineHeightWithSpacing())) {
            int row = 0;
            for (const SubsystemRow& r : rows) {
                Entity e = scene.FindByName(r.entity);
                if (!e) continue;
                bool visible = AnyVisible(e);
                ImGui::PushID(r.entity.c_str());
                if (ImGui::Checkbox("##vis", &visible)) { SetVisible(e, visible); Editor::MarkEdited(); }
                ImGui::SameLine();
                if (Retro::PlaylistRow(row++, r.label.c_str(), nullptr, selected == e.Id() + 1)) Editor::Select(e.Id() + 1);
                ImGui::PopID();
            }
        }
        Retro::EndPlaylist();
    }

    void SelectedAssembly(Scene& scene, const std::string& rootName) {
        const uint32_t selected = Editor::SelectedEntity();
        if (Entity sel = selected ? scene.FromId(selected - 1) : Entity()) {
            ImGui::TextWrapped("Selected: %s", sel.GetName().c_str());
            if (Entity parent = sel.GetParent(); parent && parent.GetName() != rootName) {
                if (Retro::Button("Select parent assembly", 200.0f)) Editor::Select(parent.Id() + 1);
            }
        }
    }

    // Every path goes through Editor::SaveScene / LoadScene, so the workspace's persistence hooks
    // (collapse exploded views, reset runtime state) run the same way as for File > Save / Open.
    void ProjectFileButtons(GameContext& ctx, Scene& scene, std::string& status) {
        (void)scene;
        auto* ws = dynamic_cast<Editor::WorkspaceExtension*>(ctx.CurrentScene());
        const std::string path = ws && ws->DefaultScenePath() ? Editor::ResolveScenePath(ws->DefaultScenePath()) : Editor::ScenePath();
        ImGui::TextDisabled("%s", std::filesystem::path(path).filename().string().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        if (Retro::Button("Save", 98.0f) && !path.empty()) Editor::SaveScene(ctx, path, &status);
        ImGui::SameLine();
        if (Retro::Button("Reopen", 98.0f) && !path.empty()) {
            if (Editor::HasUnsavedChanges(ctx)) Editor::RequestReload(ctx);   // asks first; reload rebuilds, then Reopen loads the file
            else Editor::LoadScene(ctx, path, &status);
        }
        ImGui::SameLine();
        if (Retro::Button("Rebuild", 98.0f)) Editor::RequestReload(ctx);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rebuild the scene from code (the saved file is not read)");
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    }

    void ScreenshotButton(const std::string& prefix, std::string& status) {
        if (!Retro::Button("Screenshot (PNG)", 200.0f)) return;
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
        const std::string path = Editor::ResolveScenePath("screenshots/" + prefix + "_" + stamp + ".png");
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        status = CaptureScreenshot(path) ? "screenshot: " + path : "screenshot failed";
    }
}
