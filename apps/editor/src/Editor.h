#pragma once
// PHISIKS editor: the Dear ImGui development environment on top of the engine (library PhisiksEditor).
//
// Layout: menu bar, tool bar, scene hierarchy (left), inspector (right), workspace / settings panel
// (left, below the hierarchy), console + telemetry + replay timeline (bottom), and the 3D viewport in
// the remaining centre area (the main camera's viewport is fitted to it). Tab hides every panel.
//
// An application built on the editor (the PHISIKS executable, the sample browser, the separate
// programs in examples/) calls Configure() once, then OnSceneLoaded / Draw / OnQuitRequested from
// its IGame. A scene can bring its own panel and persistence hooks by also deriving from
// WorkspaceExtension (found by dynamic_cast).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Phisiks/Scene/SceneSerializer.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Phisiks/Assets/AssetDatabase.h"

namespace Phisiks {
    struct GameContext;
    class Entity;
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
        // Component types (SceneSerializer::UnsavedComponent names) that scene files do not store but
        // this workspace re-creates itself after every load, so a save does not lose them and the
        // editor does not warn about them.
        virtual std::vector<std::string> ComponentsRestoredAfterLoad() const { return {}; }
    };

    struct AppConfig {
        std::string title = "PHISIKS";                         // menu bar title plate
        std::string emptyScene = "Untitled";                   // File > New / Open target (RegisterEmptyScene)
        bool insetViewport = true;                             // false: the 3D view fills the window under the panels
        std::function<void(Phisiks::GameContext&)> menus;      // extra menus between Simulation and Help
        // Scenes that start in play mode when loaded (samples, demos); empty = every scene starts in
        // edit mode (the PHISIKS editor and the example projects).
        std::function<bool(const std::string& sceneName)> playOnLoad;
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
    // SaveScene refuses (returns false, file untouched) when the scene holds components that scene
    // files do not store (ComponentsLostOnSave), unless `allowComponentLoss` says the caller has
    // confirmed it. Interactive saves go through RequestSave, which asks the user.
    bool SaveScene(Phisiks::GameContext& ctx, const std::string& path, std::string* status = nullptr, bool allowComponentLoss = false);
    bool LoadScene(Phisiks::GameContext& ctx, const std::string& path, std::string* status = nullptr);
    // Components of the current scene a save would drop: SceneSerializer::UnsavedComponents minus
    // the types the scene's workspace restores after loading.
    std::vector<Phisiks::SceneSerializer::UnsavedComponent> ComponentsLostOnSave(Phisiks::GameContext& ctx);
    // The save behind File > Save / Save as and the workspace Save buttons: writes at once, or, when
    // components would be lost, opens the "Components will not be saved" dialog (list, counts,
    // SAVE WITHOUT THEM / CANCEL) and writes nothing until the user confirms.
    enum class SaveRequest { Saved, Failed, AwaitingConfirmation };
    SaveRequest RequestSave(Phisiks::GameContext& ctx, const std::string& path, std::string* status = nullptr);
    bool SaveConfirmationPending();
    bool SaveConfirmationVisible();                    // the dialog was drawn in the last UI frame
    // The dialog's buttons: `confirm` writes the file without the listed components; otherwise the
    // save is cancelled and the file is left as it was.
    void ResolveSaveConfirmation(Phisiks::GameContext& ctx, bool confirm);
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
    enum class OutputTab { Console = 0, Telemetry = 1, Timeline = 2, Assets = 3 };
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

    // ---- project and assets (EditorAssets.cpp)
    // A project is a folder with assets/ (the AssetDatabase root) and scenes/ (--project <dir>, or
    // the ASSETS tab). Without one, <user data>/projects/Default is used when assets are first needed.
    bool OpenProject(const std::string& directory, std::string* status = nullptr);
    const std::string& ProjectDirectory();
    Phisiks::AssetDatabase& ProjectAssets();          // the open project's database (opens the default)
    Phisiks::AssetImportResult ImportAsset(const std::string& file, const std::string& folder = std::string(), std::string* status = nullptr);
    // Assigns a project asset to an entity, as one undo step, with an asset reference:
    // mesh -> MeshRenderer.mesh; texture -> a material of the entity (copied from its current one) with
    // the texture; audio -> AudioSource.clip; model -> SkinnedModel.path; script -> Script.file.
    bool AssignAsset(Phisiks::GameContext& ctx, Phisiks::Entity entity, uint64_t asset, std::string* status = nullptr);
    // A new entity showing a mesh or model asset at the orbit target (one undo step; selected).
    Phisiks::Entity InstantiateAsset(Phisiks::GameContext& ctx, uint64_t asset, std::string* status = nullptr);

    // ---- CAD parts (EditorCad.cpp)
    // A CAD part is an entity with a cadModel component (a Phisiks/Cad/Document.h document). The
    // editor regenerates it into the entity's mesh whenever its document changes; editing it in the
    // inspector / viewport rewrites the document, so CAD edits are undoable and saved with the scene.
    Phisiks::Entity CreateCadPart(Phisiks::GameContext& ctx, const std::string& name, const std::string& documentJson = std::string(), std::string* status = nullptr);
    bool SetCadDocument(Phisiks::GameContext& ctx, Phisiks::Entity entity, const std::string& documentJson, std::string* status = nullptr);   // one undo step
    // The part's state as JSON: kernel, unit, ok / first error, parameters, features (built, skipped,
    // error, sketch degrees of freedom), bodies, volume, area, bounds, face / edge counts, validity.
    std::string CadPartReport(Phisiks::Entity entity);
    // Writes the part into <project>/assets/exports/<entity name>.<ext>: "step", "iges", "stl", "brep"
    // or "phcad" (the parametric document). Only that folder is written.
    bool ExportCadPart(Phisiks::Entity entity, const std::string& format, std::string* path = nullptr, std::string* status = nullptr);
    // Automation (UI tests, demos): the window-pixel rect (x, y, w, h) of a CAD workspace control drawn
    // in the last frame: "add feature", "add:<item>", "EDIT SKETCH", "FINISH SKETCH", "tool:<name>",
    // "constrain:<button>", "feature:<name>", "input:<label>", "dim:<constraint id>".
    bool UiItemRect(const std::string& id, glm::vec4& rect);

    // ---- undo / redo (EditorHistory.cpp)
    // Every edit made in edit mode becomes one undo step: inspector values, gizmo moves / rotations /
    // scales, renames, create / delete / duplicate, re-parenting, components added or removed,
    // materials, asset assignments, workspace tools that call MarkEdited, and code edits wrapped in
    // BeginEdit / EndEdit (the agent interface). The editor diffs the authored document (what Save
    // writes) when an interaction ends, so a gizmo drag or a value drag is one step. Not recorded:
    // anything while playing (discarded by Stop), selection, camera and view settings. Loading a file
    // or switching scenes clears the history.
    struct HistoryState {
        bool canUndo = false, canRedo = false;
        std::string undoLabel, redoLabel;
        size_t undoCount = 0, redoCount = 0;
        size_t bytes = 0;              // memory held by the history
        double lastCaptureMs = 0.0;    // cost of the last document capture
    };
    HistoryState History();
    bool Undo(Phisiks::GameContext& ctx);
    bool Redo(Phisiks::GameContext& ctx);
    void ClearHistory();
    void SetHistoryLimits(size_t maxEntries, size_t maxBytes);   // defaults: 256 steps, 64 MiB
    // A code edit as one labelled step: BeginEdit, change the scene, EndEdit (false: nothing changed).
    // BeginEdit fails while playing.
    bool BeginEdit(Phisiks::GameContext& ctx, const std::string& label);
    bool EndEdit(Phisiks::GameContext& ctx);
    // Automation (tests, the agent interface): behave as if a widget were held, so edits over several
    // frames coalesce into one step when it is released.
    void SetInteractionHeld(bool held);

    // ---- edit / play mode (EditorHistory.cpp)
    // Edit: the simulation is stopped, edits are recorded and can be saved. Play: the simulation runs
    // (and can be paused / stepped); EnterPlay captured the edit state first and StopPlay restores it,
    // so nothing done while playing reaches the document. While playing, Save, Undo / Redo, New /
    // Open are unavailable; inspector edits still work and are discarded by Stop.
    enum class Mode { Edit, Play };
    Mode CurrentMode();
    bool EnterPlay(Phisiks::GameContext& ctx, std::string* status = nullptr, bool setRunning = true);
    bool StopPlay(Phisiks::GameContext& ctx, std::string* status = nullptr);

    // ---- integration
    // Called at the start of every editor frame (UI active), before the panels: in-process services
    // such as the agent interface (PAI) handle their requests here, on the main thread.
    void AddFrameHook(std::function<void(Phisiks::GameContext&)> hook);
    // The 4-way view cameras are editor-owned (not part of the document; hidden from tools).
    bool IsEditorEntity(const Phisiks::Entity& entity);

    // An empty, editable scene (ground grid, sun, orbit camera), the editor's default document.
    void RegisterEmptyScene(Phisiks::SceneManager& scenes, const std::string& name = "Untitled");
}
