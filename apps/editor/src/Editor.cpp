// PHISIKS editor: layout, menu bar, tool bar, scene files, dialogs (see Editor.h).
#include "Editor.h"
#include "EditorInternal.h"

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;
namespace fs = std::filesystem;

namespace Editor {
    namespace {
        AppConfig g_config;
        Internal::Layout g_layout;
        uint32_t g_selected = 0;              // id + 1
        std::string g_path;                   // current scene file (resolved), empty = unsaved
        std::string g_status;
        uint64_t g_sceneSerial = 0;
        bool g_edited = false;                // unsaved user edits
        bool g_replayDefault = true, g_replayDefaultKnown = false;
        bool g_stepPending = false;
        bool g_quitConfirmed = false;
        std::string g_pendingOpen;            // file to load once the empty scene is active

        // ---- deferred actions guarded by the unsaved-changes prompt
        enum class Action { None, Quit, New, Open, Scene, Reload };
        struct Pending { Action action = Action::None; std::string argument; };
        Pending g_pending;
        bool g_askUnsaved = false;
        bool g_proceedAfterSaveAs = false;

        // ---- file dialog
        enum class DialogMode { None, Open, SaveAs };
        DialogMode g_dialog = DialogMode::None;
        bool g_dialogOpenRequest = false;
        fs::path g_dialogDir;
        char g_dialogName[256] = {};
        char g_dialogDirText[1024] = {};
        std::string g_dialogError;

        bool g_showAbout = false, g_showControls = false;

        ImVec2 Centre() { const ImVec2 d = ImGui::GetIO().DisplaySize; return ImVec2(d.x * 0.5f, d.y * 0.5f); }

        std::string ShortPath(const std::string& path) {
            if (path.empty()) return "(unsaved)";
            std::error_code ec;
            const fs::path rel = fs::relative(path, fs::current_path(ec), ec);
            const std::string r = !ec && !rel.empty() && rel.string().rfind("..", 0) != 0 ? rel.generic_string() : fs::path(path).generic_string();
            return r;
        }

        void Proceed(GameContext& ctx, const Pending& p);

        void Guard(GameContext& ctx, Action action, const std::string& argument = std::string()) {
            const Pending p{ action, argument };
            if (!HasUnsavedChanges(ctx)) { Proceed(ctx, p); return; }
            g_pending = p;
            g_askUnsaved = true;
        }

        void OpenFileDialog(DialogMode mode) {
            g_dialog = mode;
            g_dialogOpenRequest = true;
            g_dialogError.clear();
            std::error_code ec;
            fs::path start = g_path.empty() ? fs::path() : fs::path(g_path).parent_path();
            if (start.empty() || !fs::is_directory(start, ec)) {
                start = fs::current_path(ec) / "scenes";
                if (!fs::is_directory(start, ec)) start = fs::current_path(ec);
            }
            g_dialogDir = start;
            std::snprintf(g_dialogDirText, sizeof(g_dialogDirText), "%s", g_dialogDir.string().c_str());
            const std::string name = g_path.empty() ? std::string("scene.scene.json") : fs::path(g_path).filename().string();
            std::snprintf(g_dialogName, sizeof(g_dialogName), "%s", mode == DialogMode::SaveAs ? name.c_str() : "");
        }

        void OpenPath(GameContext& ctx, const std::string& path) {
            WorkspaceExtension* ws = Internal::Workspace(ctx);
            Scene* scene = ctx.CurrentScene();
            const bool workspaceFile = ws && ws->DefaultScenePath() && fs::path(ResolveScenePath(ws->DefaultScenePath())) == fs::path(path);
            if (workspaceFile || (scene && scene->Name() == g_config.emptyScene)) { LoadScene(ctx, path, &g_status); return; }
            if (!ctx.scenes) return;
            g_pendingOpen = path;   // loads into the empty scene once it is active (OnSceneLoaded)
            ctx.scenes->Request(g_config.emptyScene);
        }

        void Proceed(GameContext& ctx, const Pending& p) {
            switch (p.action) {
                case Action::Quit: g_quitConfirmed = true; ctx.quitRequested = true; break;
                case Action::New:
                    g_pendingOpen.clear();
                    if (ctx.scenes) { ctx.scenes->Request(g_config.emptyScene); if (ctx.CurrentScene() && ctx.CurrentScene()->Name() == g_config.emptyScene) ctx.scenes->RequestReload(); }
                    break;
                case Action::Open: OpenPath(ctx, p.argument); break;
                case Action::Scene: if (ctx.scenes) ctx.scenes->Request(p.argument); break;
                case Action::Reload: if (ctx.scenes) ctx.scenes->RequestReload(); break;
                case Action::None: break;
            }
        }

        void SaveCurrent(GameContext& ctx) {
            if (g_path.empty()) { OpenFileDialog(DialogMode::SaveAs); return; }
            SaveScene(ctx, g_path, &g_status);
        }

        // ------------------------------------------------------------------ dialogs
        void UnsavedDialog(GameContext& ctx) {
            if (g_askUnsaved) { ImGui::OpenPopup("Unsaved changes"); g_askUnsaved = false; }
            ImGui::SetNextWindowPos(Centre(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
            Scene* scene = ctx.CurrentScene();
            ImGui::TextColored(Internal::Amber(), "'%s' has unsaved changes.", scene ? scene->Name().c_str() : "scene");
            ImGui::TextDisabled("%s", ShortPath(g_path).c_str());
            ImGui::Spacing();
            const float w = 120.0f * g_layout.scale;
            if (!g_path.empty()) {
                if (Retro::Button("SAVE", w)) {
                    const Pending p = g_pending;
                    ImGui::CloseCurrentPopup();
                    if (SaveScene(ctx, g_path, &g_status)) Proceed(ctx, p);
                    g_pending = {};
                }
            }
            else if (Retro::Button("SAVE AS...", w)) {
                ImGui::CloseCurrentPopup();
                g_proceedAfterSaveAs = true;
                OpenFileDialog(DialogMode::SaveAs);
            }
            ImGui::SameLine();
            if (Retro::Button("DISCARD", w)) {
                const Pending p = g_pending;
                g_pending = {};
                g_edited = false;
                ImGui::CloseCurrentPopup();
                Proceed(ctx, p);
            }
            ImGui::SameLine();
            if (Retro::Button("CANCEL", w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_pending = {}; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        void FileDialog(GameContext& ctx) {
            const char* id = g_dialog == DialogMode::SaveAs ? "Save scene as" : "Open scene";
            if (g_dialogOpenRequest) { ImGui::OpenPopup(id); g_dialogOpenRequest = false; }
            const float s = g_layout.scale;
            ImGui::SetNextWindowSize(ImVec2(640.0f * s, 440.0f * s), ImGuiCond_Appearing);
            ImGui::SetNextWindowPos(Centre(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            std::error_code ec;
            if (Retro::Button("UP", 60.0f * s) && g_dialogDir.has_parent_path() && g_dialogDir.parent_path() != g_dialogDir) {
                g_dialogDir = g_dialogDir.parent_path();
                std::snprintf(g_dialogDirText, sizeof(g_dialogDirText), "%s", g_dialogDir.string().c_str());
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##dir", g_dialogDirText, sizeof(g_dialogDirText), ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (fs::is_directory(g_dialogDirText, ec)) g_dialogDir = g_dialogDirText;
                else g_dialogError = std::string("not a folder: ") + g_dialogDirText;
            }
            bool accept = false;
            const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f;
            if (ImGui::BeginChild("##entries", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
                std::vector<fs::directory_entry> dirs, files;
                for (fs::directory_iterator it(g_dialogDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                    const std::string name = it->path().filename().string();
                    if (name.empty() || name[0] == '.') continue;
                    if (it->is_directory(ec)) dirs.push_back(*it);
                    else if (it->path().extension() == ".json") files.push_back(*it);
                }
                auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename() < b.path().filename(); };
                std::sort(dirs.begin(), dirs.end(), byName);
                std::sort(files.begin(), files.end(), byName);
                for (const fs::directory_entry& d : dirs) {
                    const std::string label = "[" + d.path().filename().string() + "]";
                    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        g_dialogDir = d.path();
                        std::snprintf(g_dialogDirText, sizeof(g_dialogDirText), "%s", g_dialogDir.string().c_str());
                        break;
                    }
                }
                for (const fs::directory_entry& f : files) {
                    const std::string name = f.path().filename().string();
                    const bool selected = name == g_dialogName;
                    if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        std::snprintf(g_dialogName, sizeof(g_dialogName), "%s", name.c_str());
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) accept = true;
                    }
                }
                if (dirs.empty() && files.empty()) ImGui::TextDisabled("(no folders or .json files here)");
            }
            ImGui::EndChild();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##name", g_dialogName, sizeof(g_dialogName), ImGuiInputTextFlags_EnterReturnsTrue)) accept = true;
            if (!g_dialogError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", g_dialogError.c_str());
            if (Retro::Button(g_dialog == DialogMode::SaveAs ? "SAVE" : "OPEN", 110.0f * s)) accept = true;
            ImGui::SameLine();
            bool cancel = Retro::Button("CANCEL", 110.0f * s) || ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (accept && g_dialogName[0]) {
                fs::path file = g_dialogDir / g_dialogName;
                if (g_dialog == DialogMode::SaveAs) {
                    if (file.extension() != ".json") file += ".scene.json";
                    std::string status;
                    if (SaveScene(ctx, file.string(), &status)) {
                        g_status = status;
                        ImGui::CloseCurrentPopup();
                        g_dialog = DialogMode::None;
                        if (g_proceedAfterSaveAs) { g_proceedAfterSaveAs = false; const Pending p = g_pending; g_pending = {}; Proceed(ctx, p); }
                    }
                    else g_dialogError = status;
                }
                else {
                    if (fs::is_regular_file(file, ec)) {
                        ImGui::CloseCurrentPopup();
                        g_dialog = DialogMode::None;
                        Guard(ctx, Action::Open, fs::absolute(file, ec).string());
                    }
                    else g_dialogError = "no such file: " + file.string();
                }
            }
            if (cancel) { g_proceedAfterSaveAs = false; g_pending = {}; g_dialog = DialogMode::None; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        void InfoDialogs() {
            if (g_showAbout) { ImGui::OpenPopup("About PHISIKS"); g_showAbout = false; }
            if (g_showControls) { ImGui::OpenPopup("Controls"); g_showControls = false; }
            ImGui::SetNextWindowPos(Centre(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("About PHISIKS", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const Render::RendererInfo info = Render::GetRendererInfo();
                ImGui::TextColored(Internal::Amber(), "PHISIKS %s", PHISIKS_VERSION_STRING);
                ImGui::Text("General-purpose 3D engine and editor (C++20, OpenGL).");
                ImGui::TextDisabled("%s | OpenGL %d.%d (%s tier)", Platform::OperatingSystem(), info.contextVersion / 10, info.contextVersion % 10, info.compatTier ? "4.1" : "4.6");
                ImGui::TextDisabled("MIT licence. Third-party notices: THIRD_PARTY_NOTICES.md");
                if (Retro::Button("CLOSE", 100.0f * g_layout.scale) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SetNextWindowPos(Centre(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const char* rows[][2] = {
                    { "RMB drag / MMB drag / wheel", "orbit / pan / zoom (orbit camera)" },
                    { "F4", "orbit camera <-> first person;  V cycles camera modes" },
                    { "LMB", "select in the viewport; drag the gizmo to transform" },
                    { "F", "frame the selection" },
                    { "Delete", "delete the selection (asks first)" },
                    { "F3", "4-way view (perspective + top / front / side)" },
                    { "Tab", "hide / show the panels" },
                    { "Esc", "pause / resume the simulation" },
                    { "Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S", "new / open / save / save as" },
                    { "F1 / F2", "telemetry overlay / physics debug draw" },
                };
                if (ImGui::BeginTable("##controls", 2, ImGuiTableFlags_SizingFixedFit)) {
                    for (const auto& r : rows) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(Internal::Amber(), "%s", r[0]);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r[1]);
                    }
                    ImGui::EndTable();
                }
                if (Retro::Button("CLOSE", 100.0f * g_layout.scale) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        // ------------------------------------------------------------------ menu bar
        void MenuBar(GameContext& ctx, Scene* scene) {
            if (!Retro::BeginMenuBar(g_config.title.c_str())) return;
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New scene", "Ctrl+N")) Guard(ctx, Action::New);
                if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenFileDialog(DialogMode::Open);
                if (ImGui::MenuItem("Save", "Ctrl+S", false, scene != nullptr)) SaveCurrent(ctx);
                if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S", false, scene != nullptr)) OpenFileDialog(DialogMode::SaveAs);
                if (ImGui::MenuItem("Reopen from disk", nullptr, false, scene && !g_path.empty())) Guard(ctx, Action::Open, g_path);
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "Ctrl+Q")) Guard(ctx, Action::Quit);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                Entity sel = scene ? Internal::Selected(*scene) : Entity();
                if (ImGui::MenuItem("Delete", "Delete", false, bool(sel))) Internal::RequestDelete(sel);
                if (ImGui::MenuItem("Deselect", nullptr, false, bool(sel))) g_selected = 0;
                if (ImGui::MenuItem("Frame selection", "F", false, scene != nullptr)) Internal::FrameSelected(ctx, *scene);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create", scene != nullptr)) {
                Internal::CreateMenuItems(ctx, *scene, Entity());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Hide panels", "Tab")) UI::SetPanelsVisible(false);
                const bool quad = scene && IsQuadView(*scene);
                if (ImGui::MenuItem("4-way view", "F3", quad, scene != nullptr)) SetQuadView(*scene, !quad);
                if (ImGui::MenuItem("Telemetry overlay", "F1", ctx.overlayVisible)) ctx.overlayVisible = !ctx.overlayVisible;
                if (ImGui::MenuItem("Physics debug draw", "F2", ctx.physicsDebugDraw)) ctx.physicsDebugDraw = !ctx.physicsDebugDraw;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Simulation")) {
                const bool paused = ctx.state == GameState::Paused;
                if (ImGui::MenuItem(paused ? "Play" : "Pause", "Esc")) ctx.state = paused ? GameState::Running : GameState::Paused;
                if (ImGui::MenuItem("Step one frame", nullptr, false, paused)) { ctx.state = GameState::Running; g_stepPending = true; }
                if (ImGui::MenuItem("Reload scene", nullptr, false, scene != nullptr)) Guard(ctx, Action::Reload);
                ImGui::EndMenu();
            }
            if (g_config.menus) g_config.menus(ctx);
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Controls")) g_showControls = true;
                if (ImGui::MenuItem("About PHISIKS")) g_showAbout = true;
                ImGui::EndMenu();
            }
            if (!g_status.empty()) ImGui::TextDisabled("   %s", g_status.c_str());
            Retro::EndMenuBar();
        }

        // ------------------------------------------------------------------ tool bar
        void ToolBar(GameContext& ctx, Scene* scene) {
            const Internal::Layout& L = g_layout;
            ImGui::SetNextWindowPos(ImVec2(0.0f, L.menuBar), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(L.display.x, L.toolBar), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * L.scale, 4.0f * L.scale));
            ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            const float w = 70.0f * L.scale;
            int& op = Internal::GizmoOperation();
            int& mode = Internal::GizmoMode();
            if (Retro::Button("MOVE", w, op == ImGuizmo::TRANSLATE ? 1 : 0)) op = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (Retro::Button("ROTATE", w, op == ImGuizmo::ROTATE ? 1 : 0)) op = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (Retro::Button("SCALE", w, op == ImGuizmo::SCALE ? 1 : 0)) op = ImGuizmo::SCALE;
            ImGui::SameLine();
            if (Retro::Button(mode == ImGuizmo::WORLD ? "WORLD" : "LOCAL", w, 2)) mode = mode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            ImGui::SameLine(0.0f, 18.0f * L.scale);
            const bool paused = ctx.state == GameState::Paused;
            if (Retro::TransportButton("##tb_play", Retro::Glyph::Play, !paused, "Play simulation")) ctx.state = GameState::Running;
            ImGui::SameLine();
            if (Retro::TransportButton("##tb_pause", Retro::Glyph::Pause, paused, "Pause simulation (Esc)")) ctx.state = GameState::Paused;
            ImGui::SameLine();
            if (Retro::TransportButton("##tb_step", Retro::Glyph::StepForward, false, "Step one frame (while paused)") && paused) { ctx.state = GameState::Running; g_stepPending = true; }
            ImGui::SameLine();
            if (Retro::TransportButton("##tb_reload", Retro::Glyph::Stop, false, "Reload the scene (asks about unsaved changes)") && scene) Guard(ctx, Action::Reload);
            ImGui::SameLine(0.0f, 18.0f * L.scale);
            const bool quad = scene && IsQuadView(*scene);
            if (Retro::Button("4-WAY", w, quad ? 1 : 0) && scene) SetQuadView(*scene, !quad);
            ImGui::SameLine();
            if (Retro::Button("FRAME", w) && scene) Internal::FrameSelected(ctx, *scene);
            // status, right-aligned
            char status[256];
            const float fps = ctx.stats.cpuFrameMs > 0.0f ? 1000.0f / ctx.stats.cpuFrameMs : 0.0f;
            std::snprintf(status, sizeof(status), "%s%s  |  %s  |  %s  |  %.0f FPS", scene ? scene->Name().c_str() : "-", HasUnsavedChanges(ctx) ? " *" : "",
                          ToString(ctx.state), ToString(ctx.camera.GetMode()), double(fps));
            const float tw = ImGui::CalcTextSize(status).x;
            ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - tw, ImGui::GetCursorPosX() + 10.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Internal::Phosphor(), "%s", status);
            ImGui::End();
        }

        // Thin draggable handle between a panel and the viewport.
        void Splitter(const char* id, float x, float y, float w, float h, float& value, float sign, float lo, float hi, bool vertical) {
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
            ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            ImGui::InvisibleButton("##handle", ImVec2(w, h));
            const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
            if (active || hovered) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            if (active) value = std::clamp(value + sign * (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y), lo, hi);
            if (active || hovered) ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0xFF, 0xBE, 0x0B, 0x90));
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        void Shortcuts(GameContext& ctx, Scene* scene) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (io.KeyShift) OpenFileDialog(DialogMode::SaveAs); else if (scene) SaveCurrent(ctx); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenFileDialog(DialogMode::Open);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) Guard(ctx, Action::New);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) Guard(ctx, Action::Quit);
            if (!scene) return;
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) SetQuadView(*scene, !IsQuadView(*scene));
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) if (Entity sel = Internal::Selected(*scene)) Internal::RequestDelete(sel);
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && (Internal::MouseInViewport() || !io.WantCaptureMouse)) Internal::FrameSelected(ctx, *scene);
        }
    }

    // ===================================================================== internal accessors
    namespace Internal {
        Layout& GetLayout() { return g_layout; }
        WorkspaceExtension* Workspace(GameContext& ctx) { return dynamic_cast<WorkspaceExtension*>(ctx.CurrentScene()); }
        Entity Selected(Scene& scene) { return g_selected ? scene.FromId(g_selected - 1) : Entity(); }
        ImVec4 Amber() { return ImVec4(Retro::AMBER[0], Retro::AMBER[1], Retro::AMBER[2], 1.0f); }
        ImVec4 Phosphor() { return ImVec4(Retro::PHOSPHOR[0], Retro::PHOSPHOR[1], Retro::PHOSPHOR[2], 1.0f); }
        ImVec4 Muted() { return ImVec4(0.54f, 0.56f, 0.64f, 1.0f); }
        void NoteEdit() { g_edited = true; }
    }

    // ===================================================================== public API
    void Configure(const AppConfig& config) { g_config = config; }
    const AppConfig& Config() { return g_config; }

    uint32_t SelectedEntity() { return g_selected; }
    void Select(uint32_t id) { g_selected = id; }
    std::string& ScenePath() { return g_path; }
    void MarkEdited() { g_edited = true; }
    void DrawSettingsPanel(GameContext& ctx) { Internal::SettingsPanelContents(ctx, ctx.CurrentScene()); }
    void ShowOutputTab(OutputTab tab) { Internal::SetOutputTab(int(tab)); }
    bool HasUnsavedChanges(GameContext& ctx) { return g_edited && ctx.CurrentScene() != nullptr; }
    glm::vec4 ViewportRect() { return g_layout.viewport; }

    std::string ResolveScenePath(const std::string& path) {
        if (path.empty()) return path;
        std::error_code ec;
        const fs::path p(path);
        if (p.is_absolute()) return p.lexically_normal().string();
        const fs::path exe = fs::path(Platform::ExecutableDirectory()) / p;
        const fs::path cwd = fs::current_path(ec) / p;
        fs::path primary = fs::exists(exe, ec) ? exe : fs::exists(cwd, ec) ? cwd : (fs::is_directory(exe.parent_path(), ec) ? exe : cwd);
        if (Platform::IsWritableDirectory(primary.parent_path().string())) return primary.lexically_normal().string();
        // Read-only install: a per-user copy (falls back to reading the shipped file until it was saved).
        const std::string user = Platform::UserDataDirectory();
        if (user.empty()) return primary.lexically_normal().string();
        const fs::path copy = fs::path(user) / p;
        return (fs::exists(copy, ec) || !fs::exists(primary, ec) ? copy : primary).lexically_normal().string();
    }

    bool SaveScene(GameContext& ctx, const std::string& requested, std::string* status) {
        Scene* scene = ctx.CurrentScene();
        if (!scene) { if (status) *status = "no scene"; return false; }
        std::string path = requested;
        // A read-only shipped file is saved to the per-user copy instead (ResolveScenePath picks it).
        if (!Platform::IsWritableDirectory(fs::path(path).parent_path().string())) {
            const std::string user = Platform::UserDataDirectory();
            if (!user.empty()) path = (fs::path(user) / "scenes" / fs::path(path).filename()).string();
        }
        WorkspaceExtension* ws = Internal::Workspace(ctx);
        const bool quad = IsQuadView(*scene);
        if (quad) SetQuadView(*scene, false);            // editor cameras are not part of the document
        if (ws) ws->OnBeforeSave(ctx);
        const bool ok = SceneSerializer::Save(*scene, path);
        if (ws) ws->OnAfterSave(ctx);
        if (quad) SetQuadView(*scene, true);
        if (ok) { g_path = path; g_edited = false; }
        if (status) *status = ok ? "saved " + ShortPath(path) : "save failed: " + ShortPath(path) + " (see console)";
        return ok;
    }

    bool LoadScene(GameContext& ctx, const std::string& path, std::string* status) {
        Scene* scene = ctx.CurrentScene();
        if (!scene) { if (status) *status = "no scene"; return false; }
        std::string error;
        const bool quad = IsQuadView(*scene);
        if (!SceneSerializer::Load(*scene, path, &error)) {
            if (status) *status = "open failed (scene unchanged): " + error;
            return false;
        }
        g_selected = 0;
        ctx.replay.Clear();
        if (WorkspaceExtension* ws = Internal::Workspace(ctx)) ws->OnAfterLoad(ctx);
        if (quad) SetQuadView(*scene, true);
        Internal::ResetPanelState();
        g_path = path;
        g_edited = false;
        if (status) *status = "opened " + ShortPath(path);
        return true;
    }

    void RequestScene(GameContext& ctx, const std::string& sceneName) { Guard(ctx, Action::Scene, sceneName); }
    void RequestReload(GameContext& ctx) { Guard(ctx, Action::Reload); }

    void OnSceneLoaded(GameContext& ctx) {
        Scene* scene = ctx.CurrentScene();
        if (!scene || scene->Serial() == g_sceneSerial) return;
        g_sceneSerial = scene->Serial();
        g_selected = 0;
        g_edited = false;
        Internal::ResetPanelState();
        if (!g_replayDefaultKnown) { g_replayDefault = ctx.replay.Enabled(); g_replayDefaultKnown = true; }
        ctx.replay.Clear();
        WorkspaceExtension* ws = Internal::Workspace(ctx);
        ctx.replay.SetEnabled(ws && !ws->SupportsReplay() ? false : g_replayDefault);
        g_path = ws && ws->DefaultScenePath() ? ResolveScenePath(ws->DefaultScenePath()) : std::string();
        if (!g_pendingOpen.empty()) {
            const std::string path = g_pendingOpen;
            g_pendingOpen.clear();
            LoadScene(ctx, path, &g_status);
        }
    }

    bool OnQuitRequested(GameContext& ctx) {
        if (g_quitConfirmed || !HasUnsavedChanges(ctx) || !UI::IsActive()) return true;
        g_pending = { Action::Quit, std::string() };
        g_askUnsaved = true;
        UI::SetPanelsVisible(true);
        ctx.state = GameState::Paused;
        return false;
    }

    void Draw(GameContext& ctx) {
        if (!UI::IsActive()) return;
        OnSceneLoaded(ctx);
        if (g_stepPending && ctx.state == GameState::Running) { ctx.state = GameState::Paused; g_stepPending = false; }
        Internal::Layout& L = g_layout;
        L.scale = UI::Scale();
        L.display = ImGui::GetIO().DisplaySize;
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) UI::SetPanelsVisible(!UI::PanelsVisible());

        Scene* scene = ctx.CurrentScene();
        WorkspaceExtension* ws = Internal::Workspace(ctx);
        if (ws) ws->DrawViewportOverlay(ctx);
        const bool panels = UI::PanelsVisible();
        if (scene) Internal::ApplyViewportLayout(ctx, *scene, panels);
        if (!panels) { L.viewport = glm::vec4(0.0f, 0.0f, L.display.x, L.display.y); if (scene) Internal::DrawGizmo(ctx, *scene); return; }

        MenuBar(ctx, scene);
        L.menuBar = Retro::MenuBarHeight();
        L.toolBar = std::round(ImGui::GetFrameHeight() + 10.0f * L.scale);
        ToolBar(ctx, scene);

        // Column sizes stay usable when the window shrinks.
        const float top = L.menuBar + L.toolBar;
        const float minView = 240.0f * L.scale;
        L.left = std::clamp(L.left, 200.0f * L.scale, std::max(200.0f * L.scale, (L.display.x - minView) * 0.45f));
        L.right = std::clamp(L.right, 240.0f * L.scale, std::max(240.0f * L.scale, L.display.x - L.left - minView));
        L.bottom = std::clamp(L.bottom, 110.0f * L.scale, std::max(110.0f * L.scale, (L.display.y - top) * 0.6f));
        const float centreW = std::max(L.display.x - L.left - L.right, 1.0f);
        const float centreH = std::max(L.display.y - top - L.bottom, 1.0f);
        L.viewport = glm::vec4(L.left, top, centreW, centreH);

        const float columnH = L.display.y - top;
        const float hierH = std::floor(columnH * L.leftSplit);
        if (scene) {
            Internal::HierarchyPanel(ctx, *scene, 0.0f, top, L.left, hierH);
            const std::string title = ws ? std::string(ws->WorkspaceTitle()) + "##workspace"
                                         : (g_config.sidePanel ? g_config.sidePanelTitle + "##side" : std::string("SETTINGS##side"));
            if (Retro::BeginDockedPanel(title.c_str(), 0.0f, top + hierH, L.left, columnH - hierH)) {
                if (ws) ws->DrawWorkspacePanel(ctx);
                else if (g_config.sidePanel) g_config.sidePanel(ctx);
                else Internal::SettingsPanelContents(ctx, scene);
            }
            Retro::EndPanel();
            Internal::InspectorPanel(ctx, *scene, L.display.x - L.right, top, L.right, columnH);
        }
        Internal::BottomDock(ctx, scene, L.left, top + centreH, centreW, L.bottom);

        const float grip = 5.0f * L.scale;
        Splitter("##split_left", L.left - grip * 0.5f, top, grip, columnH, L.left, 1.0f, 200.0f * L.scale, L.display.x * 0.45f, true);
        Splitter("##split_right", L.display.x - L.right - grip * 0.5f, top, grip, columnH, L.right, -1.0f, 240.0f * L.scale, L.display.x * 0.5f, true);
        Splitter("##split_bottom", L.left, top + centreH - grip * 0.5f, centreW, grip, L.bottom, -1.0f, 110.0f * L.scale, (L.display.y - top) * 0.7f, false);
        float split = hierH;
        Splitter("##split_hier", 0.0f, top + hierH - grip * 0.5f, L.left, grip, split, 1.0f, 120.0f * L.scale, columnH - 120.0f * L.scale, false);
        L.leftSplit = std::clamp(split / std::max(columnH, 1.0f), 0.15f, 0.85f);

        if (scene) {
            Internal::DrawViewportLabels(*scene);
            Internal::DrawGizmo(ctx, *scene);
            Internal::HandleViewportClick(ctx, *scene);
            Internal::DrawDeleteConfirmation(*scene);
        }
        Shortcuts(ctx, scene);
        UnsavedDialog(ctx);
        if (g_dialog != DialogMode::None) FileDialog(ctx);
        InfoDialogs();
    }
}
