#pragma once
// Editor-private state and panel functions shared by Editor.cpp, EditorPanels.cpp and EditorViewport.cpp.
#include "Editor.h"

#include "Phisiks/Scene/Scene.h"

#include <imgui.h>

#include <string>

namespace Phisiks { struct GameContext; }

namespace Editor::Internal {
    struct Layout {
        float scale = 1.0f;
        float menuBar = 0.0f, toolBar = 0.0f;
        float left = 300.0f, right = 340.0f, bottom = 220.0f;   // column widths / dock height (scaled pixels)
        float leftSplit = 0.46f;                                 // hierarchy share of the left column
        ImVec2 display{ 0.0f, 0.0f };
        glm::vec4 viewport{ 0.0f };                             // x, y (top-left), w, h
    };
    Layout& GetLayout();

    WorkspaceExtension* Workspace(Phisiks::GameContext& ctx);
    Phisiks::Entity Selected(Phisiks::Scene& scene);

    // Colours of the console identity (from Phisiks/UI/RetroDeck.h).
    ImVec4 Amber();
    ImVec4 Phosphor();
    ImVec4 Muted();

    // ---- panels (EditorPanels.cpp)
    void HierarchyPanel(Phisiks::GameContext& ctx, Phisiks::Scene& scene, float x, float y, float w, float h);
    void InspectorPanel(Phisiks::GameContext& ctx, Phisiks::Scene& scene, float x, float y, float w, float h);
    void SettingsPanelContents(Phisiks::GameContext& ctx, Phisiks::Scene* scene);
    void BottomDock(Phisiks::GameContext& ctx, Phisiks::Scene* scene, float x, float y, float w, float h);
    void NoteEdit();                    // marks the scene as edited (unsaved changes)
    void SetOutputTab(int tab);         // 0 console, 1 telemetry, 2 timeline
    void ResetPanelState();             // on scene change (euler cache, filters)
    void RequestDelete(Phisiks::Entity e);   // confirmation popup
    void DrawDeleteConfirmation(Phisiks::Scene& scene);
    void CreateMenuItems(Phisiks::GameContext& ctx, Phisiks::Scene& scene, Phisiks::Entity parent);

    // ---- viewport (EditorViewport.cpp)
    void ApplyViewportLayout(Phisiks::GameContext& ctx, Phisiks::Scene& scene, bool panels);
    void DrawGizmo(Phisiks::GameContext& ctx, Phisiks::Scene& scene);
    void HandleViewportClick(Phisiks::GameContext& ctx, Phisiks::Scene& scene);
    void DrawViewportLabels(Phisiks::Scene& scene);
    void FrameSelected(Phisiks::GameContext& ctx, Phisiks::Scene& scene);
    bool MouseInViewport();
    int& GizmoOperation();              // ImGuizmo::OPERATION
    int& GizmoMode();                   // ImGuizmo::MODE
}
