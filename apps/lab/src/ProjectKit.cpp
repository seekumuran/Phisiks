#include "ProjectKit.h"
#include "Studio.h"
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

namespace Lab::ProjectKit {
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

    bool Project(Scene& scene, const glm::vec3& p, ImVec2& out) {
        Entity camera = scene.GetPrimaryCamera();
        if (!camera || !camera.Has<Camera>()) return false;
        const Camera& cam = camera.Get<Camera>();
        const ImGuiIO& io = ImGui::GetIO();
        const float aspect = io.DisplaySize.x / std::max(io.DisplaySize.y, 1.0f);
        const glm::mat4 view = glm::inverse(camera.WorldMatrix());
        const glm::mat4 proj = cam.projection == Projection::Orthographic
            ? glm::ortho(-0.5f * cam.orthoHeight * aspect, 0.5f * cam.orthoHeight * aspect, -0.5f * cam.orthoHeight, 0.5f * cam.orthoHeight, cam.nearPlane, cam.farPlane)
            : glm::perspective(cam.verticalFov, aspect, cam.nearPlane, cam.farPlane);
        const glm::vec4 c = proj * view * glm::vec4(p, 1.0f);
        if (c.w <= 1e-5f) return false;
        out = ImVec2((c.x / c.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - c.y / c.w * 0.5f) * io.DisplaySize.y);
        return true;
    }

    namespace {
        const ImU32 INK = IM_COL32(20, 24, 32, 235), PANEL = IM_COL32(16, 18, 24, 215), AMBER = IM_COL32(255, 190, 11, 255),
                    WHITE = IM_COL32(232, 236, 242, 255), GREY = IM_COL32(160, 168, 180, 255);
    }

    void DrawCallouts(Scene& scene, const std::vector<Callout>& callouts, const std::function<bool(const Callout&)>& filter) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        // Greedy layout: labels nearest the top are placed first; each takes the first candidate
        // position that overlaps no placed label and no studio panel, and stays on screen.
        // A label with no free spot is skipped rather than overprinted.
        struct Item { const Callout* c; ImVec2 a, size; std::string head; };
        std::vector<Item> items;
        for (const Callout& c : callouts) {
            if (filter && !filter(c)) continue;
            Entity e = scene.FindByName(c.entity);
            if (!e) continue;
            int total = 0;
            if (CountVisible(e, total) == 0 && total > 0) continue;   // hidden subsystem
            Item it{ &c, ImVec2(), ImVec2(), c.tag.empty() ? c.title : (c.title.empty() ? c.tag : c.tag + "  " + c.title) };
            if (!Project(scene, glm::vec3(e.WorldMatrix() * glm::vec4(c.anchor, 1.0f)), it.a)) continue;
            const ImVec2 hs = ImGui::CalcTextSize(it.head.c_str());
            const ImVec2 ns = c.note.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(c.note.c_str());
            it.size = ImVec2(std::max(hs.x, ns.x) + 14.0f, hs.y + (c.note.empty() ? 0.0f : ns.y + 1.0f) + 6.0f);
            items.push_back(it);
        }
        std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.a.y < y.a.y; });
        std::vector<ImVec4> placed;   // x0, y0, x1, y1
        if (UI::PanelsVisible()) {   // studio columns and the timeline
            placed.push_back(ImVec4(0.0f, 0.0f, 345.0f, display.y));
            placed.push_back(ImVec4(display.x - 375.0f, 0.0f, display.x, display.y));
            placed.push_back(ImVec4(0.0f, display.y - 110.0f, display.x, display.y));
        }
        auto overlaps = [&](const ImVec4& r) {
            if (r.x < 4.0f || r.y < 26.0f || r.z > display.x - 4.0f || r.w > display.y - 4.0f) return true;
            for (const ImVec4& p : placed) if (r.x < p.z + 3.0f && r.z + 3.0f > p.x && r.y < p.w + 3.0f && r.w + 3.0f > p.y) return true;
            return false;
        };
        for (const Item& it : items) {
            const bool preferLeft = it.a.x < display.x * 0.5f;
            ImVec4 best(0, 0, 0, 0);
            bool found = false;
            const float rises[] = { -34.0f, -60.0f, -86.0f, 30.0f, -112.0f, 56.0f, -138.0f, 82.0f, -164.0f };
            for (float dy : rises) {
                for (int side = 0; side < 2 && !found; ++side) {
                    const bool left = (side == 0) == preferLeft;
                    const float x0 = left ? it.a.x - 22.0f - it.size.x : it.a.x + 22.0f;
                    const ImVec4 r(x0, it.a.y + dy - it.size.y * 0.5f, x0 + it.size.x, it.a.y + dy + it.size.y * 0.5f);
                    if (!overlaps(r)) { best = r; found = true; }
                }
                if (found) break;
            }
            if (!found) continue;
            placed.push_back(best);
            const Callout& c = *it.c;
            const bool boxLeft = best.z <= it.a.x;
            const ImVec2 join(boxLeft ? best.z : best.x, (best.y + best.w) * 0.5f);
            draw->AddLine(it.a, join, INK, 1.4f);
            draw->AddCircleFilled(it.a, 2.8f, INK);
            draw->AddRectFilled(ImVec2(best.x, best.y), ImVec2(best.z, best.w), PANEL, 2.0f);
            draw->AddRectFilled(ImVec2(best.x, best.y), ImVec2(best.x + 2.0f, best.w), AMBER);
            float x = best.x + 7.0f;
            if (!c.tag.empty()) {
                draw->AddText(ImVec2(x, best.y + 3.0f), AMBER, c.tag.c_str());
                x += ImGui::CalcTextSize((c.tag + "  ").c_str()).x;
            }
            if (!c.title.empty()) draw->AddText(ImVec2(x, best.y + 3.0f), WHITE, c.title.c_str());
            if (!c.note.empty()) draw->AddText(ImVec2(best.x + 7.0f, best.y + 4.0f + ImGui::GetTextLineHeight()), GREY, c.note.c_str());
        }
    }

    void DrawTitleBlock(const std::vector<std::string>& lines) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        float y = display.y - 16.0f - float(lines.size()) * 18.0f - 8.0f;
        float width = 0.0f;
        for (const std::string& line : lines) width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
        const float x = display.x - 16.0f - 10.0f - width;
        draw->AddRectFilled(ImVec2(x - 10.0f, y - 8.0f), ImVec2(display.x - 16.0f, display.y - 16.0f), PANEL, 2.0f);
        draw->AddRectFilled(ImVec2(x - 10.0f, y - 8.0f), ImVec2(x - 7.0f, display.y - 16.0f), AMBER);
        for (size_t i = 0; i < lines.size(); ++i, y += 18.0f)
            draw->AddText(ImVec2(x, y), i == 0 ? AMBER : (i + 1 == lines.size() ? GREY : WHITE), lines[i].c_str());
    }

    void DrawLegend(const std::vector<LegendEntry>& entries, const char* footer) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        float width = footer ? ImGui::CalcTextSize(footer).x : 0.0f;
        for (const LegendEntry& e : entries) width = std::max(width, ImGui::CalcTextSize(e.text.c_str()).x + 26.0f);
        float ly = 16.0f;
        draw->AddRectFilled(ImVec2(16.0f, ly - 6.0f), ImVec2(16.0f + width + 24.0f, ly + float(entries.size()) * 18.0f + (footer ? 30.0f : 6.0f)), PANEL, 2.0f);
        for (const LegendEntry& e : entries) {
            draw->AddRectFilled(ImVec2(26.0f, ly + 4.0f), ImVec2(44.0f, ly + 12.0f), ImGui::ColorConvertFloat4ToU32(ImVec4(e.color.r, e.color.g, e.color.b, 1.0f)), 1.0f);
            draw->AddText(ImVec2(52.0f, ly), WHITE, e.text.c_str());
            ly += 18.0f;
        }
        if (footer) draw->AddText(ImVec2(26.0f, ly + 4.0f), GREY, footer);
    }

    void SubsystemList(Scene& scene, const char* id, const std::vector<SubsystemRow>& rows, float lines) {
        const uint32_t selected = Studio::SelectedEntity();
        if (Retro::BeginPlaylist(id, lines * ImGui::GetTextLineHeightWithSpacing())) {
            int row = 0;
            for (const SubsystemRow& r : rows) {
                Entity e = scene.FindByName(r.entity);
                if (!e) continue;
                bool visible = AnyVisible(e);
                ImGui::PushID(r.entity.c_str());
                if (ImGui::Checkbox("##vis", &visible)) SetVisible(e, visible);
                ImGui::SameLine();
                if (Retro::PlaylistRow(row++, r.label.c_str(), nullptr, selected == e.Id() + 1)) Studio::Select(e.Id() + 1);
                ImGui::PopID();
            }
        }
        Retro::EndPlaylist();
    }

    void SelectedAssembly(Scene& scene, const std::string& rootName) {
        const uint32_t selected = Studio::SelectedEntity();
        if (Entity sel = selected ? scene.FromId(selected - 1) : Entity()) {
            ImGui::TextWrapped("Selected: %s", sel.GetName().c_str());
            if (Entity parent = sel.GetParent(); parent && parent.GetName() != rootName) {
                if (Retro::Button("Select parent assembly", 200.0f)) Studio::Select(parent.Id() + 1);
            }
        }
    }

    void ProjectFileButtons(GameContext& ctx, Scene& scene, std::string& status, const std::function<void()>& beforeSave,
                            const std::function<void()>& afterSave, const std::function<void()>& afterLoad) {
        ImGui::TextDisabled("%s", Studio::ScenePath().c_str());
        if (Retro::Button("Save project", 98.0f)) {
            if (beforeSave) beforeSave();
            status = SceneSerializer::Save(scene, Studio::ScenePath()) ? "saved" : "save failed";
            if (afterSave) afterSave();
        }
        ImGui::SameLine();
        if (Retro::Button("Reopen", 98.0f)) {
            std::string error;
            status = SceneSerializer::Load(scene, Studio::ScenePath(), &error) ? "reopened" : "load failed: " + error;
            Studio::Select(0);
            ctx.replay.Clear();
            if (afterLoad) afterLoad();
        }
        ImGui::SameLine();
        if (Retro::Button("Rebuild", 98.0f) && ctx.scenes) ctx.scenes->RequestReload();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    }

    void ScreenshotButton(const std::string& prefix, std::string& status) {
        if (!Retro::Button("Screenshot (PNG)", 200.0f)) return;
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
        std::error_code ec;
        std::filesystem::create_directories("screenshots", ec);
        const std::string path = "screenshots/" + prefix + "_" + stamp + ".png";
        status = CaptureScreenshot(path) ? "screenshot: " + path : "screenshot failed";
    }
}
