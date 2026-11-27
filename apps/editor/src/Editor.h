#pragma once
// PHISIKS editor: the Dear ImGui development environment on top of the engine (library PhisiksEditor).
//
// Layout: menu bar, tool bar, scene hierarchy (left), inspector (right), workspace / settings panel
// (left, below the hierarchy), console + telemetry + replay timeline (bottom), and the 3D viewport in
// the remaining centre area (the main camera's viewport is fitted to it). Tab hides every panel.
//
// An application built on the editor (the PHISIKS executable, the sample browser, the separate
// projects in projects/) calls Configure() once, then OnSceneLoaded / Draw / OnQuitRequested from
// its IGame. A scene can bring its own panel and persistence hooks by also deriving from
// WorkspaceExtension (found by dynamic_cast).
#include <cstdint>
#include <functional>
#include <string>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Phisiks {
    struct GameContext;
    class Scene;
    class SceneManager;
}

namespace Editor {
    // A scene with its own tools. The hierarchy, inspector, gizmo, viewport and console stay as they are.
    class WorkspaceExtension {
    public:
        virtual ~WorkspaceExtension() = default;
        virtual const char* WorkspaceTitle() const = 0;                       // left panel title
        virtual void DrawWorkspacePanel(Phisiks::GameContext& ctx) = 0;        // left panel contents (below the hierarchy)
        virtual void DrawViewportOverlay(Phisiks::GameContext&) {}            // every UI frame, also while Tab hides the panels
        // Scene file opened / saved by File > Save, Reopen (relative paths resolve with ResolveScenePath).
        virtual const char* DefaultScenePath() const { return nullptr; }
        // Replay timeline: a workspace whose state is not only entity transforms (explode offsets,
        // mission state) returns false; the timeline is then disabled in that scene.
        virtual bool SupportsReplay() const { return false; }
        // Persistence hooks, called by every save / load path (File menu, workspace buttons):
        // e.g. collapse an exploded view before writing, reset runtime state after reading.
        virtual void OnBeforeSave(Phisiks::GameContext&) {}
        virtual void OnAfterSave(Phisiks::GameContext&) {}
        virtual void OnAfterLoad(Phisiks::GameContext&) {}
    };

    struct AppConfig {
        std::string title = "PHISIKS";                         // menu bar title plate
        std::string emptyScene = "Untitled";                   // File > New / Open target (RegisterEmptyScene)
        bool insetViewport = true;                             // false: the 3D view fills the window under the panels
        std::function<void(Phisiks::GameContext&)> menus;      // extra menus between Simulation and Help
        // Left panel for scenes that are not workspaces (default: render / physics settings).
        std::string sidePanelTitle;
        std::function<void(Phisiks::GameContext&)> sidePanel;
    };
    void Configure(const AppConfig& config);
    const AppConfig& Config();

    // ---- IGame hooks
    void OnSceneLoaded(Phisiks::GameContext& ctx);     // call when ctx.CurrentScene() changed
    void Draw(Phisiks::GameContext& ctx);              // inside IGame::OnGui
    bool OnQuitRequested(Phisiks::GameContext& ctx);   // IGame::OnQuitRequested (asks about unsaved changes)

    // ---- selection
    uint32_t SelectedEntity();                         // Entity::Id() + 1, 0 = none
    void Select(uint32_t entityIdPlusOne);

    // ---- scenes and files
    // The scene file of the current scene (resolved absolute path, or empty for an unsaved scene).
    std::string& ScenePath();
    // Resolves a relative scene path the way assets are found: next to the executable first (a
    // release package), then the working directory (a repository checkout); if that folder is not
    // writable, a per-user copy under Platform::UserDataDirectory() is used instead.
    std::string ResolveScenePath(const std::string& path);
    // Save / load through the workspace hooks, with the editor's bookkeeping (selection, replay,
    // unsaved-changes snapshot). `status` receives a one-line result.
    bool SaveScene(Phisiks::GameContext& ctx, const std::string& path, std::string* status = nullptr);
    bool LoadScene(Phisiks::GameContext& ctx, const std::string& path, std::string* status = nullptr);
    // Scene switch / reload that asks about unsaved changes first.
    void RequestScene(Phisiks::GameContext& ctx, const std::string& sceneName);
    void RequestReload(Phisiks::GameContext& ctx);
    bool HasUnsavedChanges(Phisiks::GameContext& ctx);
    void MarkEdited();                                 // an edit the editor cannot see (workspace tools)
    // The render / physics settings controls (the default left panel), for apps with their own panel.
    void DrawSettingsPanel(Phisiks::GameContext& ctx);
    // Width for `perRow` buttons sharing the remaining width of the current panel row.
    float RowButtonWidth(int perRow);
    // Bottom dock tab shown (the dock also has buttons for it).
    enum class OutputTab { Console = 0, Telemetry = 1, Timeline = 2 };
    void ShowOutputTab(OutputTab tab);

    // ---- viewport
    // Centre area in window pixels (x, y from the top-left, width, height).
    glm::vec4 ViewportRect();
    struct QuadFraming {
        glm::vec3 center{ 0.0f };
        float distance = 40.0f;     // camera distance from the centre along each axis
        float topHeight = 24.0f;    // orthographic height of the top view (m)
        float sideHeight = 12.0f;   // orthographic height of the front / side views (m)
    };
    // 4-way view: perspective + orthographic top / front / side. Without a framing, it fits the
    // scene's renderable objects.
    void SetQuadView(Phisiks::Scene& scene, bool enabled, const QuadFraming* framing = nullptr);
    bool IsQuadView(Phisiks::Scene& scene);

    // An empty, editable scene (ground grid, sun, orbit camera), the editor's default document.
    void RegisterEmptyScene(Phisiks::SceneManager& scenes, const std::string& name = "Untitled");
}
