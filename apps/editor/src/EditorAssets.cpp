// Project folder and asset browser (the ASSETS tab of the bottom dock).
//
// A project is a folder with "assets/" (the AssetDatabase root) and "scenes/". The browser lists
// the project's assets by folder, with search and a type filter, shows their metadata (id, type,
// size, content hash, source, revision, scene references) and a preview (mesh: triangles and
// bounds; texture: size and mean colour), imports files (File > Import asset... or a path), and
// assigns assets to the selected entity or adds them to the scene. Assignments are undo steps and
// store asset references ("asset:<guid>/<name>"), which survive renaming and moving the file.
#include "Editor.h"
#include "EditorInternal.h"

#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace fs = std::filesystem;

namespace Editor {
    namespace {
        AssetDatabase g_db;
        std::string g_project;              // project folder (absolute), "" = none yet
        std::string g_folder;               // browser folder ("" = all)
        char g_search[128] = {};
        int g_typeFilter = -1;              // AssetType, -1 = all
        uint64_t g_asset = 0;               // selected asset
        char g_rename[256] = {};
        char g_newFolder[128] = {};
        char g_importPath[1024] = {};
        int g_moveTarget = 0;
        bool g_stlMillimetres = true;       // STL has no units: most CAD exports are millimetres
        std::string g_status;
        AssetRefreshReport g_lastRefresh;
        double g_refreshClock = 0.0;
        // preview cache
        uint64_t g_previewGuid = 0;
        int g_previewRevision = 0;
        std::string g_previewText;
        ImVec4 g_previewColour{ 0, 0, 0, 0 };
        // missing references of the current scene (refreshed with the database)
        std::vector<AssetReference> g_missing;
        uint64_t g_missingScene = 0;

        std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; }
        std::string Stem(const std::string& name) {
            const std::string lower = Lower(name);
            if (lower.size() > 11 && lower.compare(lower.size() - 11, 11, ".scene.json") == 0) return name.substr(0, name.size() - 11);
            return fs::path(name).stem().string();
        }
        std::string Bytes(uint64_t n) {
            char t[32];
            if (n >= (1u << 20)) std::snprintf(t, sizeof(t), "%.1f MiB", double(n) / double(1u << 20));
            else if (n >= 1024) std::snprintf(t, sizeof(t), "%.1f KiB", double(n) / 1024.0);
            else std::snprintf(t, sizeof(t), "%llu B", static_cast<unsigned long long>(n));
            return t;
        }

        void EnsureProject() {
            if (g_db.IsOpen()) return;
            if (g_project.empty()) {
                const std::string user = Platform::UserDataDirectory();
                g_project = (fs::path(user.empty() ? fs::current_path().string() : user) / "projects" / "Default").lexically_normal().generic_string();
            }
            std::string error;
            if (!OpenProject(g_project, &error)) g_status = error;
        }

        void RefreshMissing(GameContext& ctx) {
            Scene* scene = ctx.CurrentScene();
            g_missing.clear();
            g_missingScene = scene ? scene->Serial() : 0;
            if (!scene) return;
            for (AssetReference& r : AssetDatabase::CollectReferences(*scene)) if (!r.resolved) g_missing.push_back(std::move(r));
        }

        void Preview(const AssetInfo& a) {
            if (g_previewGuid == a.guid && g_previewRevision == a.revision) return;
            g_previewGuid = a.guid;
            g_previewRevision = a.revision;
            g_previewText.clear();
            g_previewColour = ImVec4(0, 0, 0, 0);
            const std::string path = g_db.AbsolutePath(a.guid);
            if (path.empty()) { g_previewText = "file missing"; return; }
            if (a.type == AssetType::Mesh) {
                Render::MeshData mesh;
                std::string error;
                if (!Assets::LoadMesh(path, mesh, &error)) { g_previewText = "cannot load: " + error; return; }
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (const Render::Vertex& v : mesh.vertices) { lo = glm::min(lo, v.position); hi = glm::max(hi, v.position); }
                const glm::vec3 size = hi - lo;
                char t[256];
                std::snprintf(t, sizeof(t), "%zu triangles, %zu vertices\nbounds %.4g x %.4g x %.4g (file units%s)", mesh.indices.size() / 3, mesh.vertices.size(),
                              double(size.x), double(size.y), double(size.z), Lower(a.name).size() > 4 && Lower(a.name).rfind(".stl") == a.name.size() - 4 ? "; STL has no units" : ", metres");
                g_previewText = t;
            }
            else if (a.type == AssetType::Texture) {
                Assets::Image image;
                if (!Assets::LoadImageFile(path, image)) { g_previewText = "cannot decode"; return; }
                double r = 0, g = 0, b = 0;
                const size_t n = size_t(image.width) * size_t(image.height);
                for (size_t i = 0; i < n; ++i) { r += image.rgba[i * 4]; g += image.rgba[i * 4 + 1]; b += image.rgba[i * 4 + 2]; }
                if (n) g_previewColour = ImVec4(float(r / n / 255.0), float(g / n / 255.0), float(b / n / 255.0), 1.0f);
                g_previewText = std::to_string(image.width) + " x " + std::to_string(image.height) + " px (shown on materials at 512 x 512)";
            }
            else if (a.type == AssetType::Model) g_previewText = "glTF 2.0 model: placed with a SkinnedModel component (clips, skinning)";
            else if (a.type == AssetType::Audio) g_previewText = "WAV clip, " + Bytes(a.size);
            else if (a.type == AssetType::Cad) g_previewText = "CAD exchange file (B-rep): open it in a CAD workspace (Workspace > CAD part)";
            else if (a.type == AssetType::CadDocument) g_previewText = "PHISIKS parametric part: open it in a CAD workspace";
            else g_previewText = ToString(a.type);
        }

        bool Assignable(AssetType t) { return t == AssetType::Mesh || t == AssetType::Texture || t == AssetType::Audio || t == AssetType::Model || t == AssetType::Script; }
    }

    // ==================================================================== public API
    bool OpenProject(const std::string& directory, std::string* status) {
        std::error_code ec;
        const fs::path dir = fs::absolute(fs::path(directory), ec).lexically_normal();
        fs::create_directories(dir / "assets", ec);
        fs::create_directories(dir / "scenes", ec);
        const fs::path marker = dir / "phisiks.project";
        if (!fs::exists(marker, ec)) {
            std::FILE* f = std::fopen(marker.string().c_str(), "wb");
            if (f) { std::fprintf(f, "{\n  \"format\": \"phisiks-project\",\n  \"version\": 1,\n  \"name\": \"%s\"\n}\n", dir.filename().string().c_str()); std::fclose(f); }
        }
        std::string error;
        if (!g_db.Open((dir / "assets").string(), &error)) { if (status) *status = "project: " + error; return false; }
        g_project = dir.generic_string();
        g_folder.clear();
        g_asset = 0;
        g_missingScene = 0;
        if (status) *status = "project " + g_project;
        Log::Info() << "Editor: project " << g_project;
        return true;
    }

    const std::string& ProjectDirectory() { return g_project; }
    AssetDatabase& ProjectAssets() { EnsureProject(); return g_db; }

    bool AssignAsset(GameContext& ctx, Entity entity, uint64_t guid, std::string* status) {
        AssetInfo a;
        if (!entity || !ProjectAssets().Find(guid, a)) { if (status) *status = "nothing to assign"; return false; }
        if (a.missing) { if (status) *status = "'" + a.name + "' is missing"; return false; }
        if (!Assignable(a.type)) { if (status) *status = std::string("a ") + ToString(a.type) + " asset cannot be assigned to an entity"; return false; }
        const std::string ref = g_db.Reference(guid);
        const bool recorded = BeginEdit(ctx, "Assign '" + a.name + "' to '" + entity.GetName() + "'");
        switch (a.type) {
            case AssetType::Mesh: {
                if (!entity.Has<MeshRenderer>()) entity.Add<MeshRenderer>();
                entity.Get<MeshRenderer>().mesh = ref;
                break;
            }
            case AssetType::Texture: {
                // A material of this entity (copied from its current one), so other parts sharing the old material keep it.
                if (!entity.Has<MeshRenderer>()) entity.Add<MeshRenderer>();
                MeshRenderer& r = entity.Get<MeshRenderer>();
                Render::MaterialDesc m;
                if (const Render::MaterialDesc* current = r.material.empty() ? nullptr : Render::FindMaterial(r.material)) m = *current;
                m.albedoTexture = ref;
                m.textureUsesMeshUV = true;
                if (m.baseColor == glm::vec3(0.0f)) m.baseColor = glm::vec3(1.0f);
                const std::string name = entity.GetName() + " / " + Stem(a.name);
                Render::RegisterMaterial(name, m);
                r.material = name;
                break;
            }
            case AssetType::Audio: {
                if (!entity.Has<AudioSource>()) entity.Add<AudioSource>();
                entity.Get<AudioSource>().clip = ref;
                break;
            }
            case AssetType::Model: {
                SkinnedModel model;
                if (const SkinnedModel* current = entity.TryGet<SkinnedModel>()) model = *current;
                model.path = ref;
                model.loaded = false;
                model.error.clear();
                entity.Add<SkinnedModel>(model);
                break;
            }
            case AssetType::Script: {
                Script s;
                if (const Script* current = entity.TryGet<Script>()) s = *current;
                s.file = ref;
                s.source.clear();
                s.started = false;
                entity.Add<Script>(s);
                break;
            }
            default: break;
        }
        if (recorded) EndEdit(ctx); else MarkEdited();
        if (status) *status = "assigned '" + a.name + "' to '" + entity.GetName() + "'";
        return true;
    }

    Entity InstantiateAsset(GameContext& ctx, uint64_t guid, std::string* status) {
        Scene* scene = ctx.CurrentScene();
        AssetInfo a;
        if (!scene || !ProjectAssets().Find(guid, a) || a.missing) { if (status) *status = "nothing to add"; return {}; }
        if (a.type != AssetType::Mesh && a.type != AssetType::Model) { if (status) *status = "only meshes and models can be added to the scene"; return {}; }
        const bool recorded = BeginEdit(ctx, "Add '" + a.name + "'");
        Entity e = scene->CreateEntity(Stem(a.name));
        e.LocalTransform().position = ctx.camera.GetOrbitTarget();
        if (a.type == AssetType::Mesh) {
            e.Add<MeshRenderer>(MeshRenderer{ g_db.Reference(guid), "" });
            if (g_stlMillimetres && Lower(a.name).size() > 4 && Lower(a.name).rfind(".stl") == a.name.size() - 4) e.LocalTransform().scale = glm::vec3(0.001f);
        }
        else {
            SkinnedModel m;
            m.path = g_db.Reference(guid);
            e.Add<SkinnedModel>(m);
        }
        if (recorded) EndEdit(ctx); else MarkEdited();
        Select(e.Id() + 1);
        if (status) *status = "added '" + a.name + "'";
        return e;
    }

    AssetImportResult ImportAsset(const std::string& file, const std::string& folder, std::string* status) {
        const AssetImportResult r = ProjectAssets().Import(file, folder);
        if (status) *status = !r.ok ? "import failed: " + r.error : r.duplicate ? "already in the project (the existing asset is selected)" : "imported " + fs::path(file).filename().string();
        if (r.ok) g_asset = r.guid;
        return r;
    }

    // ==================================================================== the browser (bottom dock tab)
    namespace Internal {
        size_t MissingAssetCount() { return g_missing.size(); }
        const std::string& BrowserFolder() { return g_folder; }

        void AssetBrowser(GameContext& ctx, Scene* scene) {
            EnsureProject();
            const float s = GetLayout().scale;
            // periodic refresh (picks up files added or changed outside PHISIKS)
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (g_db.IsOpen() && now - g_refreshClock > 3.0) { g_lastRefresh = g_db.Refresh(); g_refreshClock = now; RefreshMissing(ctx); }
            if (scene && scene->Serial() != g_missingScene) RefreshMissing(ctx);

            // header: project, import
            ImGui::TextColored(Muted(), "PROJECT");
            ImGui::SameLine();
            ImGui::TextUnformatted(g_project.empty() ? "(none)" : g_project.c_str());
            ImGui::SameLine();
            ImGui::TextColored(Muted(), "  %zu assets, refresh %.1f ms", g_lastRefresh.assets, g_lastRefresh.milliseconds);
            ImGui::SetNextItemWidth(std::max(120.0f * s, ImGui::GetContentRegionAvail().x - 330.0f * s));
            const bool enter = ImGui::InputTextWithHint("##import", "file to import (OBJ, STL, glTF/GLB, PNG/JPG/TGA/BMP, WAV, Lua, STEP/IGES, scene)", g_importPath, sizeof(g_importPath), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((Retro::Button("IMPORT", 80.0f * s) || enter) && g_importPath[0]) ImportAsset(g_importPath, g_folder, &g_status);
            ImGui::SameLine();
            if (Retro::Button("BROWSE...", 90.0f * s)) Internal::OpenImportDialog();
            ImGui::SameLine();
            if (Retro::Button("REFRESH", 80.0f * s)) { g_lastRefresh = g_db.Refresh(); RefreshMissing(ctx); g_status = "refreshed: " + std::to_string(g_lastRefresh.added) + " new, " + std::to_string(g_lastRefresh.moved) + " moved, " + std::to_string(g_lastRefresh.changed) + " changed, " + std::to_string(g_lastRefresh.missing) + " missing"; }
            if (!g_status.empty()) ImGui::TextColored(Amber(), "%s", g_status.c_str());

            const float avail = ImGui::GetContentRegionAvail().y;
            // folders
            ImGui::BeginChild("##folders", ImVec2(170.0f * s, avail), ImGuiChildFlags_Borders);
            if (ImGui::Selectable("(all assets)", g_folder.empty())) g_folder.clear();
            for (const std::string& f : g_db.Folders()) {
                const int depth = int(std::count(f.begin(), f.end(), '/'));
                const std::string label = std::string(size_t(depth) * 2, ' ') + fs::path(f).filename().string() + "##" + f;
                if (ImGui::Selectable(label.c_str(), g_folder == f)) g_folder = f;
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##newfolder", "new folder", g_newFolder, sizeof(g_newFolder), ImGuiInputTextFlags_EnterReturnsTrue) && g_newFolder[0]) {
                std::string error;
                const std::string rel = g_folder.empty() ? std::string(g_newFolder) : g_folder + "/" + g_newFolder;
                if (g_db.CreateFolder(rel, &error)) { g_folder = rel; g_newFolder[0] = 0; } else g_status = error;
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // list
            const float detailsW = 330.0f * s;
            ImGui::BeginChild("##assetlist", ImVec2(std::max(100.0f, ImGui::GetContentRegionAvail().x - detailsW), avail), ImGuiChildFlags_Borders);
            ImGui::SetNextItemWidth(160.0f * s);
            ImGui::InputTextWithHint("##search", "search", g_search, sizeof(g_search));
            ImGui::SameLine();
            const char* types[] = { "all types", "mesh", "model", "texture", "audio", "scene", "script", "cad", "cad-document" };
            ImGui::SetNextItemWidth(120.0f * s);
            int filter = g_typeFilter + 1;
            if (ImGui::Combo("##type", &filter, types, IM_ARRAYSIZE(types))) g_typeFilter = filter - 1;
            const std::string needle = Lower(g_search);
            if (ImGui::BeginTable("##assets", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                for (const AssetInfo& a : g_db.List(g_folder, true)) {
                    if (g_typeFilter >= 0 && int(a.type) != g_typeFilter) continue;
                    if (!needle.empty() && Lower(a.path).find(needle) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string label = a.path + "##" + std::to_string(a.guid);
                    if (ImGui::Selectable(label.c_str(), g_asset == a.guid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        g_asset = a.guid;
                        std::snprintf(g_rename, sizeof(g_rename), "%s", a.name.c_str());
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && scene) {
                            if (Entity sel = Internal::Selected(*scene); sel && Assignable(a.type)) AssignAsset(ctx, sel, a.guid, &g_status);
                            else if (a.type == AssetType::Mesh || a.type == AssetType::Model) InstantiateAsset(ctx, a.guid, &g_status);
                        }
                    }
                    if (ImGui::BeginDragDropSource()) {   // drop on an entity in the hierarchy: assign
                        ImGui::SetDragDropPayload("PHISIKS_ASSET", &a.guid, sizeof(a.guid));
                        ImGui::Text("%s", a.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::TableNextColumn(); ImGui::TextColored(Muted(), "%s", ToString(a.type));
                    ImGui::TableNextColumn(); ImGui::TextColored(Muted(), "%s", Bytes(a.size).c_str());
                    ImGui::TableNextColumn();
                    if (a.missing) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "MISSING");
                    else if (!a.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", a.error.c_str());
                    else ImGui::TextColored(Phosphor(), "rev %d", a.revision);
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // details
            ImGui::BeginChild("##assetdetails", ImVec2(0.0f, avail), ImGuiChildFlags_Borders);
            AssetInfo a;
            if (g_asset && g_db.Find(g_asset, a)) {
                ImGui::TextColored(Amber(), "%s", a.name.c_str());
                char id[24];
                std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(a.guid));
                ImGui::TextColored(Muted(), "id %s  |  %s  |  rev %d", id, ToString(a.type), a.revision);
                ImGui::TextWrapped("path: %s", a.path.c_str());
                if (!a.source.empty()) ImGui::TextWrapped("source: %s", a.source.c_str());
                ImGui::TextColored(Muted(), "%s  |  hash %016llx  |  %s", Bytes(a.size).c_str(), static_cast<unsigned long long>(a.hash), a.imported.c_str());
                if (a.missing) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "The file is missing: restore it, or Reimport from the source.");
                Preview(a);
                if (g_previewColour.w > 0.0f) { ImGui::ColorButton("##mean", g_previewColour, ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f * s, 18.0f * s)); ImGui::SameLine(); }
                ImGui::TextWrapped("%s", g_previewText.c_str());
                size_t uses = 0;
                if (scene) for (const AssetReference& r : AssetDatabase::CollectReferences(*scene)) uses += r.asset == a.guid;
                ImGui::TextColored(Muted(), "used %zu time%s in this scene", uses, uses == 1 ? "" : "s");
                ImGui::Separator();
                Entity sel = scene ? Internal::Selected(*scene) : Entity();
                const float bw = RowButtonWidth(2);
                ImGui::BeginDisabled(!sel || !Assignable(a.type) || a.missing);
                if (Retro::Button("ASSIGN TO SELECTED", bw)) AssignAsset(ctx, sel, a.guid, &g_status);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!scene || (a.type != AssetType::Mesh && a.type != AssetType::Model) || a.missing);
                if (Retro::Button("ADD TO SCENE", bw)) InstantiateAsset(ctx, a.guid, &g_status);
                ImGui::EndDisabled();
                if (Lower(a.name).size() > 4 && Lower(a.name).rfind(".stl") == a.name.size() - 4) ImGui::Checkbox("STL in millimetres (scale 0.001)", &g_stlMillimetres);
                ImGui::BeginDisabled(a.source.empty());
                if (Retro::Button("REIMPORT", bw)) {
                    const AssetImportResult r = g_db.Reimport(a.guid);
                    g_status = r.ok ? "reimported '" + a.name + "'" : "reimport failed: " + r.error;
                    RefreshMissing(ctx);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (Retro::Button("COPY REFERENCE", bw)) ImGui::SetClipboardText(g_db.Reference(a.guid).c_str());
                ImGui::SetNextItemWidth(-90.0f * s);
                ImGui::InputText("##rename", g_rename, sizeof(g_rename));
                ImGui::SameLine();
                if (Retro::Button("RENAME", 80.0f * s) && g_rename[0]) {
                    std::string error;
                    const std::string parent = fs::path(a.path).parent_path().generic_string();
                    if (g_db.Move(a.guid, parent.empty() ? std::string(g_rename) : parent + "/" + g_rename, &error)) g_status = "renamed to " + std::string(g_rename) + " (references keep working)";
                    else g_status = "rename failed: " + error;
                }
                std::vector<std::string> folders = g_db.Folders();
                folders.insert(folders.begin(), "(project root)");
                g_moveTarget = std::clamp(g_moveTarget, 0, int(folders.size()) - 1);
                ImGui::SetNextItemWidth(-90.0f * s);
                if (ImGui::BeginCombo("##move", folders[size_t(g_moveTarget)].c_str())) {
                    for (int i = 0; i < int(folders.size()); ++i) if (ImGui::Selectable(folders[size_t(i)].c_str(), i == g_moveTarget)) g_moveTarget = i;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (Retro::Button("MOVE", 80.0f * s)) {
                    std::string error;
                    const std::string dest = g_moveTarget == 0 ? a.name : folders[size_t(g_moveTarget)] + "/" + a.name;
                    if (g_db.Move(a.guid, dest, &error)) g_status = "moved to " + dest + " (references keep working)";
                    else g_status = "move failed: " + error;
                }
            }
            else ImGui::TextDisabled("Select an asset. Double-click assigns it to the selected entity (or adds a mesh / model); drag it onto an entity in the hierarchy.");
            if (!g_missing.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%zu MISSING REFERENCE%s IN THIS SCENE", g_missing.size(), g_missing.size() == 1 ? "" : "S");
                for (const AssetReference& r : g_missing) ImGui::BulletText("%s  %s.%s = %s", r.entityName.empty() ? "(material)" : r.entityName.c_str(), r.component.c_str(), r.field.c_str(), r.value.c_str());
            }
            ImGui::EndChild();
        }

        bool AcceptAssetDrop(GameContext& ctx, Entity target) {
            if (!ImGui::BeginDragDropTarget()) return false;
            bool done = false;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PHISIKS_ASSET")) {
                uint64_t guid = 0;
                std::memcpy(&guid, payload->Data, sizeof(guid));
                done = AssignAsset(ctx, target, guid, &g_status);
            }
            ImGui::EndDragDropTarget();
            return done;
        }
    }
}
