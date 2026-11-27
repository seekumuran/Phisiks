// PHISIKS editor panels: scene hierarchy, inspector, settings, console / telemetry / timeline dock.
#include "EditorInternal.h"

#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Editor::Internal {
    namespace {
        // ---- hierarchy
        char g_filter[128] = {};
        uint32_t g_scrollTo = 0;
        // ---- inspector: Euler angles shown for the selected entity (re-derived only when the rotation
        // changed elsewhere, so a field can be dragged past +-90 degrees without flipping)
        uint32_t g_eulerEntity = 0;
        glm::quat g_eulerQuat{ 1, 0, 0, 0 };
        glm::vec3 g_euler{ 0.0f };
        // ---- delete confirmation
        uint32_t g_deleteId = 0;
        bool g_deleteOpen = false;
        // ---- settings (physics sliders mirror the engine)
        float g_friction = 0.6f;
        int g_quality = 1;
        // ---- console
        std::vector<Log::Record> g_lines;
        uint64_t g_seen = 0, g_clearedAt = 0;
        bool g_showInfo = true, g_showWarn = true, g_showError = true, g_follow = true;
        char g_consoleFilter[96] = {};
        int g_dockTab = 0;
        // ---- telemetry history
        std::array<float, 240> g_frameNorm{};
        size_t g_cursor = 0;
        // ---- mesh list for the inspector (assets found on disk, refreshed on demand)
        std::vector<std::string> g_meshes;
        bool g_meshesScanned = false;

        bool Edited(bool changed) { if (changed) NoteEdit(); return changed; }
        bool DragVec3(const char* label, glm::vec3& v, float speed = 0.05f) { return ImGui::DragFloat3(label, &v.x, speed); }

        bool ContainsNoCase(const std::string& text, const char* needle) {
            if (!needle || !*needle) return true;
            std::string a = text, b = needle;
            std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return a.find(b) != std::string::npos;
        }

        int CountDescendants(const Entity& e) {
            int n = 0;
            for (Entity c : e.GetChildren()) n += 1 + CountDescendants(c);
            return n;
        }

        bool IsEditorCamera(const Entity& e) {
            const std::string& n = e.GetName();
            return n == "TopView" || n == "FrontView" || n == "SideView";
        }

        void EntityContextMenu(GameContext& ctx, Scene& scene, Entity e) {
            if (!ImGui::BeginPopupContextItem("##ctx")) return;
            ImGui::TextColored(Amber(), "%s", e.GetName().c_str());
            ImGui::Separator();
            if (ImGui::BeginMenu("Create child")) { CreateMenuItems(ctx, scene, e); ImGui::EndMenu(); }
            if (ImGui::MenuItem("Unparent", nullptr, false, bool(e.GetParent()))) { e.ClearParent(true); NoteEdit(); }
            if (ImGui::MenuItem("Frame", "F")) { Select(e.Id() + 1); FrameSelected(ctx, scene); }
            if (ImGui::MenuItem("Delete", "Delete")) RequestDelete(e);
            ImGui::EndPopup();
        }

        void DragDropTarget(Entity e) {
            if (!ImGui::BeginDragDropTarget()) return;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PHISIKS_ENTITY")) {
                const uint32_t id = *static_cast<const uint32_t*>(payload->Data);
                if (Entity child = e.GetScene()->FromId(id); child && !(child == e)) {
                    if (child.SetParent(e, true)) NoteEdit();
                }
            }
            ImGui::EndDragDropTarget();
        }

        void TreeNode(GameContext& ctx, Scene& scene, Entity e) {
            const std::vector<Entity> children = e.GetChildren();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            const bool selected = SelectedEntity() == e.Id() + 1;
            if (selected) flags |= ImGuiTreeNodeFlags_Selected;
            if (g_scrollTo && selected) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            ImGui::PushID(int(e.Id()));
            if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            const bool open = ImGui::TreeNodeEx("##node", flags, "%s", e.GetName().c_str());
            if (selected) ImGui::PopStyleColor();
            if (selected && g_scrollTo) { ImGui::SetScrollHereY(0.4f); g_scrollTo = 0; }
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(e.Id() + 1);
            if (ImGui::BeginDragDropSource()) {
                const uint32_t id = e.Id();
                ImGui::SetDragDropPayload("PHISIKS_ENTITY", &id, sizeof(id));
                ImGui::Text("Parent '%s' to ...", e.GetName().c_str());
                ImGui::EndDragDropSource();
            }
            DragDropTarget(e);
            EntityContextMenu(ctx, scene, e);
            if (!children.empty()) {
                char count[16];
                std::snprintf(count, sizeof(count), "%d", CountDescendants(e));
                const float w = ImGui::CalcTextSize(count).x;
                ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - w - 4.0f, 0.0f));
                ImGui::TextColored(Muted(), "%s", count);
            }
            if (open) {
                for (Entity c : children) TreeNode(ctx, scene, c);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // ------------------------------------------------------------------ inspector sections
        void MaterialEditor(const std::string& name) {
            const Render::MaterialDesc* found = Render::FindMaterial(name.empty() ? "Default" : name);
            if (!found) { ImGui::TextDisabled("material '%s' is not registered", name.c_str()); return; }
            Render::MaterialDesc m = *found;
            bool changed = false;
            changed |= ImGui::ColorEdit3("base colour", &m.baseColor.x);
            changed |= ImGui::SliderFloat("roughness", &m.roughness, 0.02f, 1.0f);
            changed |= ImGui::SliderFloat("metallic", &m.metallic, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("opacity", &m.opacity, 0.02f, 1.0f);
            changed |= ImGui::ColorEdit3("emissive", &m.emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            int pattern = int(m.pattern);
            if (ImGui::Combo("pattern", &pattern, "None\0Checker\0Grid\0Noise\0Wood\0Tiles\0CalibrationGrid\0Splat\0")) { m.pattern = Render::Pattern(pattern); changed = true; }
            if (m.pattern == Render::Pattern::Splat) {
                for (int k = 0; k < 4; ++k) {
                    ImGui::PushID(k);
                    changed |= ImGui::ColorEdit3("layer", &m.splat[k].color.x);
                    ImGui::PopID();
                }
            }
            else if (m.pattern != Render::Pattern::None) {
                changed |= ImGui::ColorEdit3("pattern colour", &m.patternColor.x);
                changed |= ImGui::SliderFloat("pattern scale", &m.patternScale, 0.1f, 20.0f);
            }
            if (changed) { Render::RegisterMaterial(name.empty() ? "Default" : name, m); NoteEdit(); }
            ImGui::TextDisabled("shared material: edits apply to every part that uses it");
        }

        void ScanMeshes() {
            namespace fs = std::filesystem;
            g_meshes = { "Cube", "Sphere", "Plane" };
            std::set<std::string> found;
            std::error_code ec;
            for (const fs::path& root : { fs::path(Platform::ExecutableDirectory()) / "assets", fs::current_path(ec) / "assets",
                                          fs::path(Platform::ExecutableDirectory()) / "projects" / "assets", fs::current_path(ec) / "projects" / "assets" }) {
                if (!fs::is_directory(root, ec)) continue;
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file(ec) && it->path().extension() == ".obj") found.insert(fs::relative(it->path(), root, ec).generic_string());
                }
            }
            g_meshes.insert(g_meshes.end(), found.begin(), found.end());
            g_meshesScanned = true;
        }

        void MeshField(MeshRenderer& m) {
            if (!g_meshesScanned) ScanMeshes();
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s", m.mesh.c_str());
            if (ImGui::InputText("mesh", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) { m.mesh = buffer; NoteEdit(); }
            if (ImGui::BeginCombo("##meshlist", "choose a mesh...", ImGuiComboFlags_HeightLarge)) {
                for (const std::string& name : g_meshes) if (ImGui::Selectable(name.c_str(), name == m.mesh)) { m.mesh = name; NoteEdit(); }
                ImGui::Separator();
                if (ImGui::Selectable("(rescan asset folders)")) g_meshesScanned = false;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("OBJ files under assets/ (or projects/assets/), plus the built-in Cube / Sphere / Plane");
        }

        void AddComponentMenu(Entity e) {
            if (!ImGui::BeginCombo("##addcomponent", "add component...")) return;
            if (!e.Has<MeshRenderer>() && ImGui::Selectable("Mesh renderer")) { e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" }); NoteEdit(); }
            if (!e.Has<Light>() && ImGui::Selectable("Light")) { Light l; l.type = LightType::Point; l.range = 8.0f; l.intensity = 4.0f; e.Add<Light>(l); NoteEdit(); }
            if (!e.Has<Collider>() && ImGui::Selectable("Collider (box)")) {
                Collider c;
                c.shape.shape = Physics::ShapeType::Box;
                c.shape.halfExtents = glm::vec3(0.5f);
                e.Add<Collider>(c);
                e.MarkPhysicsDirty();
                NoteEdit();
            }
            if (!e.Has<Rigidbody>() && ImGui::Selectable("Rigidbody")) {
                if (!e.Has<Collider>()) { Collider c; c.shape.halfExtents = glm::vec3(0.5f); e.Add<Collider>(c); }
                e.Add<Rigidbody>(Rigidbody{});
                e.MarkPhysicsDirty();
                NoteEdit();
            }
            ImGui::EndCombo();
        }
    }

    void ResetPanelState() {
        g_eulerEntity = 0;
        g_deleteId = 0;
        g_scrollTo = 0;
    }

    void RequestDelete(Entity e) {
        if (!e) return;
        g_deleteId = e.Id() + 1;
        g_deleteOpen = true;
    }

    void DrawDeleteConfirmation(Scene& scene) {
        if (g_deleteOpen) { ImGui::OpenPopup("Delete entity"); g_deleteOpen = false; }
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Delete entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        Entity e = g_deleteId ? scene.FromId(g_deleteId - 1) : Entity();
        if (!e) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        const int children = CountDescendants(e);
        ImGui::Text("Delete '%s'%s?", e.GetName().c_str(), children ? (" and its " + std::to_string(children) + " descendants").c_str() : "");
        ImGui::TextDisabled("There is no undo; reopen the scene file to get it back.");
        const float w = 110.0f * GetLayout().scale;
        if (Retro::Button("DELETE", w)) {
            if (SelectedEntity() == g_deleteId || (SelectedEntity() && scene.FromId(SelectedEntity() - 1).IsDescendantOf(e))) Select(0);
            e.Destroy();
            NoteEdit();
            g_deleteId = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Retro::Button("CANCEL", w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_deleteId = 0; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    void CreateMenuItems(GameContext& ctx, Scene& scene, Entity parent) {
        // New objects appear at the orbit target / in front of the camera, under `parent` when given.
        const glm::vec3 at = parent ? glm::vec3(0.0f) : ctx.camera.GetPosition() + ctx.camera.GetForward() * std::min(ctx.camera.GetOrbitDistance(), 8.0f);
        auto make = [&](const char* name) {
            Entity e = scene.CreateEntity(name);
            if (parent) e.SetParent(parent, false);
            e.LocalTransform().position = at;
            Select(e.Id() + 1);
            g_scrollTo = e.Id() + 1;
            NoteEdit();
            return e;
        };
        if (ImGui::MenuItem("Empty")) make("Empty");
        ImGui::Separator();
        if (ImGui::MenuItem("Cube")) make("Cube").Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
        if (ImGui::MenuItem("Sphere")) make("Sphere").Add<MeshRenderer>(MeshRenderer{ "Sphere", "" });
        if (ImGui::MenuItem("Plane")) make("Plane").Add<MeshRenderer>(MeshRenderer{ "Plane", "" });
        if (ImGui::MenuItem("Rigid cube (dynamic)")) {
            Entity e = make("Rigid cube");
            e.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
            Collider c;
            c.shape.halfExtents = glm::vec3(0.5f);
            e.Add<Collider>(c);
            e.Add<Rigidbody>(Rigidbody{});
        }
        ImGui::Separator();
        auto light = [&](const char* name, LightType type) {
            Entity e = make(name);
            Light l;
            l.type = type;
            l.intensity = type == LightType::Directional ? 2.0f : 6.0f;
            l.range = 10.0f;
            e.Add<Light>(l);
            if (type != LightType::Point) e.LocalTransform().rotation = glm::quatLookAt(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), glm::vec3(0, 0, -1));
        };
        if (ImGui::MenuItem("Directional light")) light("Directional light", LightType::Directional);
        if (ImGui::MenuItem("Point light")) light("Point light", LightType::Point);
        if (ImGui::MenuItem("Spot light")) light("Spot light", LightType::Spot);
    }

    // ====================================================================== hierarchy
    void HierarchyPanel(GameContext& ctx, Scene& scene, float x, float y, float w, float h) {
        if (!Retro::BeginDockedPanel("SCENE##hierarchy", x, y, w, h)) { Retro::EndPanel(); return; }
        ImGui::TextColored(Amber(), "%s", scene.Name().c_str());
        ImGui::SameLine();
        ImGui::TextColored(Muted(), "%zu entities", scene.EntityCount());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "search by name", g_filter, sizeof(g_filter));
        if (Retro::BeginPlaylist("##hierarchy", -1.0f)) {
            Retro::PushPlaylistTreeStyle();
            if (g_filter[0]) {   // flat list of matches
                std::vector<Entity> matches;
                scene.Each<Transform>([&](Entity e, Transform&) { if (!scene.IsPendingDestroy(e) && ContainsNoCase(e.GetName(), g_filter)) matches.push_back(e); });
                std::sort(matches.begin(), matches.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
                for (Entity e : matches) {
                    ImGui::PushID(int(e.Id()));
                    if (ImGui::Selectable(e.GetName().c_str(), SelectedEntity() == e.Id() + 1)) Select(e.Id() + 1);
                    EntityContextMenu(ctx, scene, e);
                    if (Entity p = e.GetParent()) { ImGui::SameLine(); ImGui::TextColored(Muted(), "in %s", p.GetName().c_str()); }
                    ImGui::PopID();
                }
                if (matches.empty()) ImGui::TextDisabled("no entity matches");
            }
            else {
                std::vector<Entity> roots;
                scene.Each<Transform>([&](Entity e, Transform&) { if (!e.GetParent() && !scene.IsPendingDestroy(e) && !IsEditorCamera(e)) roots.push_back(e); });
                std::sort(roots.begin(), roots.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
                for (Entity e : roots) TreeNode(ctx, scene, e);
                // drop here to unparent
                ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 20.0f)));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PHISIKS_ENTITY")) {
                        if (Entity child = scene.FromId(*static_cast<const uint32_t*>(payload->Data))) { child.ClearParent(true); NoteEdit(); }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("##empty_ctx")) {
                    if (ImGui::BeginMenu("Create")) { CreateMenuItems(ctx, scene, Entity()); ImGui::EndMenu(); }
                    ImGui::EndPopup();
                }
            }
            Retro::PopPlaylistTreeStyle();
        }
        Retro::EndPlaylist();
        Retro::EndPanel();
    }

    // ====================================================================== inspector
    void InspectorPanel(GameContext& ctx, Scene& scene, float x, float y, float w, float h) {
        (void)ctx;
        if (!Retro::BeginDockedPanel("INSPECTOR##inspector", x, y, w, h)) { Retro::EndPanel(); return; }
        Entity e = Selected(scene);
        if (!e) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Click an object in the viewport or the scene list. Create adds new objects.");
            ImGui::PopStyleColor();
            Retro::EndPanel();
            return;
        }
        char name[128];
        std::snprintf(name, sizeof(name), "%s", e.GetName().c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##name", name, sizeof(name))) { e.SetName(name); NoteEdit(); }
        ImGui::TextColored(Muted(), "id %u%s%s", e.Id(), e.GetParent() ? "   parent: " : "", e.GetParent() ? e.GetParent().GetName().c_str() : "");
        if (Retro::Button("DELETE", 90.0f * GetLayout().scale)) RequestDelete(e);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        AddComponentMenu(e);

        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            Transform& t = e.LocalTransform();
            if (g_eulerEntity != e.Id() + 1 || std::abs(std::abs(glm::dot(g_eulerQuat, t.rotation)) - 1.0f) > 1e-6f) {
                g_euler = glm::degrees(glm::eulerAngles(t.rotation));
                g_eulerEntity = e.Id() + 1;
            }
            bool moved = Edited(DragVec3("position", t.position));
            if (Edited(DragVec3("rotation", g_euler, 0.5f))) { t.rotation = glm::quat(glm::radians(g_euler)); moved = true; }
            g_eulerQuat = t.rotation;
            const bool scaled = Edited(DragVec3("scale", t.scale, 0.01f));
            if (moved && e.GetBody().Valid()) e.SetWorldPose(e.WorldPosition(), e.WorldRotation());
            if (scaled) e.MarkPhysicsDirty();
            ImGui::TextDisabled("world (%.3f, %.3f, %.3f)", double(e.WorldPosition().x), double(e.WorldPosition().y), double(e.WorldPosition().z));
        }
        if (MeshRenderer* m = e.TryGet<MeshRenderer>(); m && ImGui::CollapsingHeader("Mesh renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            MeshField(*m);
            char material[128];
            std::snprintf(material, sizeof(material), "%s", m->material.c_str());
            if (ImGui::InputText("material", material, sizeof(material), ImGuiInputTextFlags_EnterReturnsTrue)) { m->material = material; NoteEdit(); }
            Edited(ImGui::Checkbox("visible", &m->visible)); ImGui::SameLine();
            Edited(ImGui::Checkbox("casts shadows", &m->castShadows));
            Edited(ImGui::DragFloat2("uv scale", &m->uvScale.x, 0.05f, 0.01f, 100.0f));
            if (ImGui::TreeNodeEx("Material", ImGuiTreeNodeFlags_DefaultOpen)) { MaterialEditor(m->material); ImGui::TreePop(); }
        }
        if (Light* l = e.TryGet<Light>(); l && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            int type = int(l->type);
            if (Edited(ImGui::Combo("type", &type, "Directional\0Point\0Spot\0"))) l->type = LightType(type);
            Edited(ImGui::ColorEdit3("colour", &l->color.x));
            Edited(ImGui::DragFloat("intensity", &l->intensity, 0.1f, 0.0f, 1000.0f));
            if (l->type != LightType::Directional) Edited(ImGui::DragFloat("range", &l->range, 0.1f, 0.1f, 200.0f));
            if (l->type == LightType::Spot) {
                Edited(ImGui::SliderAngle("inner cone", &l->innerConeAngle, 1.0f, 89.0f));
                Edited(ImGui::SliderAngle("outer cone", &l->outerConeAngle, 1.0f, 89.0f));
            }
            Edited(ImGui::Checkbox("shadows", &l->castShadows));
        }
        if (Camera* c = e.TryGet<Camera>(); c && ImGui::CollapsingHeader("Camera")) {
            ImGui::Text("%s, near %.3f, far %.1f", c->projection == Projection::Orthographic ? "orthographic" : "perspective", double(c->nearPlane), double(c->farPlane));
            if (e.GetName() == "MainCamera") ImGui::TextDisabled("driven by the editor camera controller");
        }
        if (Collider* c = e.TryGet<Collider>(); c && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool dirty = false;
            int shape = int(c->shape.shape);
            if (c->shape.shape == Physics::ShapeType::HeightField) ImGui::Text("height field %ux%u, %.2f m cells", c->shape.heightfieldColumns, c->shape.heightfieldRows, double(c->shape.heightfieldCellSize));
            else if (ImGui::Combo("shape", &shape, "Box\0Sphere\0Capsule\0ConvexMesh\0TriangleMesh\0")) { c->shape.shape = Physics::ShapeType(shape); dirty = true; }
            if (c->shape.shape == Physics::ShapeType::Box) dirty |= DragVec3("half extents", c->shape.halfExtents, 0.01f);
            if (c->shape.shape == Physics::ShapeType::Sphere || c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("radius", &c->shape.radius, 0.01f, 0.01f, 50.0f);
            if (c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("half height", &c->shape.halfHeight, 0.01f, 0.01f, 50.0f);
            dirty |= ImGui::SliderFloat("static friction", &c->shape.material.staticFriction, 0.0f, 1.5f);
            dirty |= ImGui::SliderFloat("dynamic friction", &c->shape.material.dynamicFriction, 0.0f, 1.5f);
            dirty |= ImGui::SliderFloat("restitution", &c->shape.material.restitution, 0.0f, 1.0f);
            dirty |= ImGui::Checkbox("trigger", &c->shape.isTrigger);
            if (Edited(dirty)) e.MarkPhysicsDirty();
        }
        if (Rigidbody* r = e.TryGet<Rigidbody>(); r && ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool dirty = false;
            int type = int(r->type);
            if (ImGui::Combo("body type", &type, "Static\0Dynamic\0Kinematic\0")) { r->type = Physics::BodyType(type); dirty = true; }
            dirty |= ImGui::DragFloat("mass (kg)", &r->mass, 0.05f, 0.01f, 10000.0f);
            dirty |= ImGui::SliderFloat("linear damping", &r->linearDamping, 0.0f, 5.0f);
            dirty |= ImGui::SliderFloat("angular damping", &r->angularDamping, 0.0f, 5.0f);
            dirty |= ImGui::Checkbox("gravity", &r->useGravity);
            dirty |= ImGui::Checkbox("continuous collision", &r->continuousCollision);
            if (Edited(dirty)) e.MarkPhysicsDirty();
            const Physics::BodyId body = e.GetBody();
            if (body.Valid()) {
                const glm::vec3 v = Physics::GetLinearVelocity(body);
                ImGui::TextColored(Phosphor(), "v (%.2f, %.2f, %.2f) m/s%s", double(v.x), double(v.y), double(v.z), Physics::IsSleeping(body) ? "  sleeping" : "");
                if (Retro::Button("IMPULSE UP")) Physics::AddImpulse(body, glm::vec3(0.0f, 4.0f * Physics::GetMass(body), 0.0f));
            }
        }
        if (Joint* j = e.TryGet<Joint>(); j && ImGui::CollapsingHeader("Joint", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const char* TYPES[] = { "Hinge", "Ball-and-socket", "Prismatic", "Spring-damper" };
            ImGui::Text("%s to %s%s", TYPES[int(j->type)], j->connectedTo.empty() ? "the world" : j->connectedTo.c_str(), Physics::IsValid(j->runtime) ? "" : " (not created)");
            const Physics::JointState s = Physics::GetJointState(j->runtime);
            const bool angular = j->type == Physics::JointType::Hinge;
            ImGui::TextColored(Phosphor(), "position %.3f %s, speed %.3f %s", angular ? double(glm::degrees(s.position)) : double(s.position), angular ? "deg" : "m",
                               angular ? double(glm::degrees(s.speed)) : double(s.speed), angular ? "deg/s" : "m/s");
            if (j->type == Physics::JointType::Hinge || j->type == Physics::JointType::Prismatic) {
                bool changed = Retro::LedToggle("MOTOR", &j->enableMotor);
                changed |= ImGui::SliderFloat("motor speed", &j->motorSpeed, -6.0f, 6.0f);
                changed |= ImGui::SliderFloat("max force", &j->maxMotorForce, 0.0f, 2000.0f, "%.0f");
                if (changed) Physics::SetJointMotor(j->runtime, j->enableMotor, j->motorSpeed, j->maxMotorForce);
            }
            ImGui::TextDisabled("joints are not stored in scene files");
        }
        if (SkinnedModel* m = e.TryGet<SkinnedModel>(); m && ImGui::CollapsingHeader("Skinned model (glTF)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped("%s  clip '%s'  %u vertices%s%s", m->path.c_str(), m->clip.c_str(), m->vertexCount, m->error.empty() ? "" : "  ERROR: ", m->error.c_str());
            Retro::LedToggle("PLAY", &m->playing);
            ImGui::SameLine();
            bool dq = m->method == Animation::SkinningMethod::DualQuaternion;
            if (Retro::LedToggle("DUAL QUAT", &dq)) m->method = dq ? Animation::SkinningMethod::DualQuaternion : Animation::SkinningMethod::Linear;
            ImGui::SliderFloat("speed", &m->speed, 0.0f, 3.0f);
            ImGui::DragFloat("time (s)", &m->time, 0.01f);
        }
        if (CameraPath* p = e.TryGet<CameraPath>(); p && ImGui::CollapsingHeader("Camera path", ImGuiTreeNodeFlags_DefaultOpen)) {
            Retro::LedToggle("FLY", &p->playing);
            ImGui::SameLine();
            ImGui::Text("%s, %zu points, %.1f m", p->type == Animation::SplineType::CatmullRom ? "Catmull-Rom" : "Bezier", p->points.size(), double(p->positionSpline.Length()));
            ImGui::SliderFloat("duration (s)", &p->duration, 2.0f, 60.0f, "%.1f");
        }
        if (NavAgent* n = e.TryGet<NavAgent>(); n && ImGui::CollapsingHeader("Nav agent", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("target (%.1f, %.1f, %.1f)%s, %zu waypoints, %u plans", double(n->target.x), double(n->target.y), double(n->target.z), n->arrived ? " ARRIVED" : "", n->path.size(), n->replans);
            ImGui::SliderFloat("max speed", &n->maxSpeed, 0.2f, 6.0f);
            ImGui::SliderFloat("avoidance", &n->avoidance, 0.0f, 6.0f);
        }
        if (Script* s = e.TryGet<Script>(); s && ImGui::CollapsingHeader("Lua script", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s->file.empty()) ImGui::Text("file %s (re-runs on save)", s->file.c_str());
            else ImGui::TextDisabled("inline source, %zu bytes", s->source.size());
            if (!s->error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", s->error.c_str());
            Retro::LedToggle("ENABLED", &s->enabled);
            ImGui::SameLine();
            if (Retro::Button("RELOAD ALL")) Scripting::Reload(scene);
        }
        if (AudioSource* a = e.TryGet<AudioSource>(); a && ImGui::CollapsingHeader("Audio source", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%s%s", a->clip.c_str(), a->isPlaying ? "  [playing]" : "");
            Edited(ImGui::SliderFloat("volume", &a->volume, 0.0f, 1.0f));
            Audio::SpatialInfo info;
            if (a->spatial && Audio::GetSpatialInfo(a->voice, info)) {
                Retro::LedMeter("L", info.gainLeft, nullptr);
                Retro::LedMeter("R", info.gainRight, nullptr);
                ImGui::Text("%.1f m, gain %.2f, pan %+.2f, Doppler x%.3f", double(info.distance), double(info.distanceGain), double(info.pan), double(info.dopplerPitch));
            }
            if (Retro::Button("PLAY")) a->Play();
        }
        if (Terrain* t = e.TryGet<Terrain>(); t && ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%u x %u samples, %.2f m cells, %u triangles", t->generator.resolution, t->generator.resolution, double(t->generator.cellSize), t->triangles);
        }
        if (Decal* d = e.TryGet<Decal>(); d && ImGui::CollapsingHeader("Decal", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("visible", &d->visible);
            ImGui::ColorEdit4("colour", &d->color.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            DragVec3("size", d->size, 0.02f);
        }
        if (Trail* t = e.TryGet<Trail>(); t && ImGui::CollapsingHeader("Trail", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("lifetime", &t->lifetime, 0.1f, 5.0f);
            ImGui::SliderFloat("width", &t->width, 0.01f, 0.6f);
            ImGui::Text("%zu points", t->points.size());
        }
        if (Physics::ForceField* f = e.TryGet<Physics::ForceField>(); f && ImGui::CollapsingHeader("Force field", ImGuiTreeNodeFlags_DefaultOpen)) {
            int type = int(f->type);
            if (Edited(ImGui::Combo("field", &type, "Uniform\0SphericalGravity\0Vortex\0NBody\0"))) f->type = Physics::FieldType(type);
            if (f->type == Physics::FieldType::Uniform) Edited(DragVec3("vector", f->vector, 0.05f));
            else Edited(ImGui::DragFloat("strength", &f->strength, 0.05f));
            if (f->type == Physics::FieldType::SphericalGravity || f->type == Physics::FieldType::Vortex) Edited(ImGui::DragFloat("radius", &f->radius, 0.02f, 0.01f, 100.0f));
            if (f->type == Physics::FieldType::Vortex) { Edited(ImGui::DragFloat("falloff", &f->falloff, 0.02f, 0.0f, 4.0f)); Edited(DragVec3("axis", f->axis, 0.02f)); }
            Edited(ImGui::DragFloat("influence radius", &f->influenceRadius, 0.05f, 0.0f, 200.0f));
            Edited(ImGui::Checkbox("rigid", &f->affectsRigidBodies)); ImGui::SameLine();
            Edited(ImGui::Checkbox("particles", &f->affectsParticles)); ImGui::SameLine();
            Edited(ImGui::Checkbox("soft", &f->affectsSoftBodies));
        }
        if (Physics::Advanced::ParticleSystem* p = e.TryGet<Physics::Advanced::ParticleSystem>(); p && ImGui::CollapsingHeader("Particle system", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("edits apply when the system is re-created (reload)");
            int count = int(p->count);
            if (Edited(ImGui::DragInt("pool size", &count, 16.0f, 1, 200000))) p->count = uint32_t(count);
            Edited(ImGui::DragFloat("emission rate (/s)", &p->emissionRate, 5.0f, 0.0f, 100000.0f));
            Edited(ImGui::DragFloat("lifetime (s)", &p->lifetime, 0.05f, 0.0f, 120.0f));
            Edited(ImGui::DragFloat("emit speed", &p->emitSpeed, 0.05f, 0.0f, 100.0f));
            Edited(ImGui::SliderFloat("cone (rad)", &p->emitSpread, 0.0f, 3.14f));
            Edited(ImGui::ColorEdit3("colour", &p->color.x));
            Edited(ImGui::Checkbox("push rigid bodies", &p->pushRigidBodies));
        }
        if (Physics::Advanced::SoftBody* s = e.TryGet<Physics::Advanced::SoftBody>(); s && ImGui::CollapsingHeader("Soft body", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("edits apply when the body is re-created (reload)");
            Edited(ImGui::DragFloat("compliance", &s->compliance, 1e-5f, 0.0f, 0.1f, "%.6f"));
            Edited(ImGui::SliderInt("substeps", &s->substeps, 1, 40));
            Edited(ImGui::SliderFloat("damping", &s->damping, 0.0f, 5.0f));
            Edited(ImGui::Checkbox("push rigid bodies", &s->pushRigidBodies));
        }
        Retro::EndPanel();
    }

    // ====================================================================== settings (non-workspace scenes)
    void SettingsPanelContents(GameContext& ctx, Scene* scene) {
        ImGui::SeparatorText("PHYSICS");
        const Physics::Settings& ps = Physics::GetSettings();
        float gravity = ps.gravity.y;
        if (Retro::EqSlider("GRAVITY", &gravity, -30.0f, 5.0f, "%.2f")) Physics::SetGravity(glm::vec3(ps.gravity.x, gravity, ps.gravity.z));
        int substeps = int(ps.substeps);
        if (Retro::EqSliderInt("SUBSTEPS", &substeps, 1, 16)) Physics::SetSubsteps(uint32_t(substeps));
        if (Retro::EqSlider("FRICTION", &g_friction, 0.0f, 1.5f, "%.2f") && scene) {
            scene->Each<Collider>([&](Entity e, Collider& c) {
                c.shape.material.staticFriction = g_friction;
                c.shape.material.dynamicFriction = g_friction * 0.83f;
                e.MarkPhysicsDirty();
            });
            NoteEdit();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the friction of every collider in the scene");
        Retro::LedToggle("DEBUG DRAW", &ctx.physicsDebugDraw);
        ImGui::SameLine();
        ImGui::TextColored(Muted(), "%s", Physics::BackendName());

        ImGui::SeparatorText("RENDER");
        Render::RenderSettings& r = Render::Settings();
        if (ImGui::Combo("quality", &g_quality, "Low (integrated GPU)\0Medium\0High\0")) Render::ApplyQualityPreset(Render::QualityPreset(g_quality));
        int upscaler = int(r.upscaler);
        if (ImGui::Combo("upscaler", &upscaler, "Bilinear\0FSR 1 (EASU + RCAS)\0")) r.upscaler = Render::Upscaler(upscaler);
        Retro::EqSlider("RENDER SCALE", &r.renderScale, 0.5f, 1.0f, "%.2f");
        Retro::EqSlider("EXPOSURE", &r.exposure, 0.1f, 4.0f, "%.2f");
        Retro::EqSlider("BLOOM", &r.bloom.intensity, 0.0f, 0.3f, "%.2f");
        Retro::EqSlider("SHARPNESS", &r.sharpness, 0.0f, 2.0f, "%.2f");
        Retro::EqSlider("SSAO RADIUS", &r.ssao.radius, 0.1f, 2.0f, "%.2f");
        auto led = [&](const char* label, bool* v, bool last) {
            Retro::LedToggle(label, v);
            if (!last) ImGui::SameLine();
        };
        led("CULL", &r.gpuCulling, false); led("HI-Z", &r.occlusionCulling, false); led("SHADOW", &r.shadowsEnabled, false); led("FOG", &r.fog.enabled, true);
        led("BLOOM", &r.bloom.enabled, false); led("SSAO", &r.ssao.enabled, false); led("DECAL", &r.decals, true);
        const Render::ShaderReloadStatus reload = Render::GetShaderReloadStatus();
        if (Retro::Button(reload.enabled ? "RELOAD SHADERS" : "WATCH SHADERS", 0.0f, reload.enabled ? (reload.failures ? 2 : 1) : 0)) {
            if (!reload.enabled) Render::EnableShaderHotReload("engine/shaders");
            else Render::ReloadShadersNow();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Watches the engine GLSL sources and rebuilds changed programs (%u reloads, %u failed)%s%s",
                                                      reload.reloads, reload.failures, reload.lastError.empty() ? "" : "\n", reload.lastError.c_str());
    }

    // ====================================================================== bottom dock
    namespace {
        void Console() {
            const uint64_t total = Log::LineCount();
            if (total != g_seen) { Log::CopyRecent(g_lines); g_seen = total; }
            Retro::LedToggle("INFO", &g_showInfo); ImGui::SameLine();
            Retro::LedToggle("WARN", &g_showWarn); ImGui::SameLine();
            Retro::LedToggle("ERROR", &g_showError); ImGui::SameLine();
            Retro::LedToggle("FOLLOW", &g_follow); ImGui::SameLine();
            if (Retro::Button("CLEAR")) g_clearedAt = total;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##cfilter", "filter", g_consoleFilter, sizeof(g_consoleFilter));
            if (ImGui::BeginChild("##console", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const Log::Record& line : g_lines) {
                    if (line.sequence <= g_clearedAt) continue;
                    if ((line.level == Log::Level::Info && !g_showInfo) || (line.level == Log::Level::Warning && !g_showWarn) || (line.level == Log::Level::Error && !g_showError)) continue;
                    if (!ContainsNoCase(line.text, g_consoleFilter)) continue;
                    const ImVec4 colour = line.level == Log::Level::Error ? ImVec4(1.0f, 0.38f, 0.32f, 1.0f) : line.level == Log::Level::Warning ? Amber() : ImVec4(0.78f, 0.86f, 0.80f, 1.0f);
                    ImGui::TextColored(Muted(), "%9.3f", line.seconds);
                    ImGui::SameLine();
                    ImGui::TextColored(colour, "%s", line.text.c_str());
                }
                if (g_follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
                else if (g_follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
            }
            ImGui::EndChild();
        }

        void Telemetry(GameContext& ctx) {
            const double t = ctx.stats.simulationTime;
            const int minutes = int(t / 60.0);
            char big[32], small[96], value[32];
            std::snprintf(big, sizeof(big), "%02d:%04.1f", minutes % 100, t - minutes * 60.0);
            const float fps = ctx.stats.cpuFrameMs > 0.0f ? 1000.0f / ctx.stats.cpuFrameMs : 0.0f;
            std::snprintf(small, sizeof(small), "FPS %3.0f  STEP %llu  %s", double(fps), static_cast<unsigned long long>(ctx.stats.totalFixedSteps), ToString(ctx.state));
            if (ImGui::BeginTable("##telemetry", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                Retro::Lcd(big, small);
                ImGui::TableSetColumnIndex(1);
                std::snprintf(value, sizeof(value), "%.1f ms", double(ctx.stats.cpuFrameMs));
                Retro::LedMeter("CPU", ctx.stats.cpuFrameMs / 33.3f, value);
                std::snprintf(value, sizeof(value), "%.1f ms", double(std::max(ctx.stats.gpuFrameMs, 0.0f)));
                Retro::LedMeter("GPU", std::max(ctx.stats.gpuFrameMs, 0.0f) / 33.3f, value);
                std::snprintf(value, sizeof(value), "%.2f ms", double(ctx.stats.cpuPhysicsMs));
                Retro::LedMeter("PHYS", ctx.stats.cpuPhysicsMs / 16.7f, value);
                const double ram = double(Platform::ProcessMemoryBytes()), total = std::max(double(Platform::SystemMemoryBytes()), 1.0);
                std::snprintf(value, sizeof(value), "%.0f MB", ram / 1048576.0);
                Retro::LedMeter("RAM", float(ram / total) * 4.0f, value);   // full scale = 25 % of system memory
                ImGui::EndTable();
            }
            Retro::Analyzer("##frames", g_frameNorm.data(), int(g_frameNorm.size()), int(g_cursor), 36.0f * GetLayout().scale);
            const Render::RendererInfo info = Render::GetRendererInfo();
            const RenderStats rs = GetRenderStats();
            const Physics::Stats ps = Physics::GetStats();
            ImGui::TextColored(Muted(), "%s | OpenGL %d.%d (%s tier) | %s | %dx%d -> %dx%d | drawn %u, culled %u | bodies %u, joints %u", Platform::OperatingSystem(),
                               info.contextVersion / 10, info.contextVersion % 10, info.compatTier ? "4.1" : "4.6", Physics::BackendName(), rs.internalWidth, rs.internalHeight,
                               rs.outputWidth, rs.outputHeight, rs.instancesDrawn, rs.instancesCulled, ps.dynamicBodies, ps.joints);
        }

        void Timeline(GameContext& ctx, Scene* scene) {
            WorkspaceExtension* ws = Workspace(ctx);
            if (ws && !ws->SupportsReplay()) {
                ImGui::TextColored(Muted(), "Replay timeline: not available in this workspace.");
                ImGui::TextDisabled("Its state (exploded views, mission playback) is not only entity transforms; use the workspace controls.");
                return;
            }
            Replay& replay = ctx.replay;
            int index = int(replay.Scrubbed() ? replay.ScrubIndex() : (replay.Size() ? replay.Size() - 1 : 0));
            const int last = int(replay.Size()) - 1;
            auto restore = [&](int i) { if (scene && last >= 0) { ctx.state = GameState::Paused; replay.Restore(*scene, size_t(std::clamp(i, 0, last))); } };
            if (Retro::TransportButton("##back", Retro::Glyph::StepBack, false, "Step back one recorded step")) restore(index - 1);
            ImGui::SameLine();
            if (Retro::TransportButton("##rewind", Retro::Glyph::Stop, false, "Rewind to the first recorded step")) restore(0);
            ImGui::SameLine();
            if (Retro::TransportButton("##forward", Retro::Glyph::StepForward, false, "Step forward one recorded step")) restore(index + 1);
            ImGui::SameLine();
            const bool enabled = replay.Enabled();
            if (Retro::TransportButton("##record", Retro::Glyph::Record, enabled, "Record every fixed step")) replay.SetEnabled(!enabled);
            ImGui::SameLine();
            if (Retro::TransportButton("##eject", Retro::Glyph::Eject, false, "Clear the recording")) replay.Clear();
            ImGui::SameLine();
            ImGui::TextColored(Phosphor(), "%zu steps  %.1f MB", replay.Size(), double(replay.MemoryBytes()) / 1048576.0);
            if (last >= 0) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##scrub", &index, 0, last, "step %d")) restore(index);
                if (replay.Scrubbed()) ImGui::TextDisabled("step %llu, t = %.2f s: Play continues from here (newer steps are discarded)",
                                                           static_cast<unsigned long long>(replay.At(size_t(index)).step), replay.At(size_t(index)).simulationTime);
            }
            else ImGui::TextDisabled(enabled ? "Recording: press Play to record steps." : "Recording is off.");
            ImGui::TextDisabled("Restores rigid bodies and entity transforms; particles and soft bodies keep their state.");
        }
    }

    void SetOutputTab(int tab) { g_dockTab = std::clamp(tab, 0, 2); }

    void BottomDock(GameContext& ctx, Scene* scene, float x, float y, float w, float h) {
        g_frameNorm[g_cursor] = ctx.stats.cpuFrameMs / 33.3f;
        g_cursor = (g_cursor + 1) % g_frameNorm.size();
        if (!Retro::BeginDockedPanel("OUTPUT##dock", x, y, w, h, ImGuiWindowFlags_NoScrollbar)) { Retro::EndPanel(); return; }
        const char* tabs[] = { "CONSOLE", "TELEMETRY", "TIMELINE" };
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (Retro::Button(tabs[i], 100.0f * GetLayout().scale, g_dockTab == i ? 1 : 0)) g_dockTab = i;
        }
        ImGui::Separator();
        if (g_dockTab == 0) Console();
        else if (g_dockTab == 1) Telemetry(ctx);
        else Timeline(ctx, scene);
        Retro::EndPanel();
    }
}
