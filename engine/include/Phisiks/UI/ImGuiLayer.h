#pragma once
// Dear ImGui integration (GLFW + OpenGL3 backends, GLSL 410: works on both renderer tiers).
// The Application drives it when AppOptions::enableUI is set: NewFrame before IGame::OnGui, draw
// after the scene was presented (the UI is not part of captured frames). Include <imgui.h> /
// <ImGuizmo.h> in game code to build windows inside OnGui.
namespace Phisiks::UI {
    bool Init();               // needs the window and renderer; applies the retro deck theme (RetroDeck.h)
    void Shutdown();
    bool IsActive();
    void BeginFrame();
    void EndFrame();           // renders into the default framebuffer
    bool WantsMouse();         // the cursor is over / dragging a UI element (games should ignore the mouse)
    bool WantsKeyboard();
    int LastDrawVertexCount(); // vertices of the last rendered UI frame (tests)
    // Editor panels shown (the game decides what it draws; the engine HUD hides its telemetry text
    // while they are, so nothing overlaps behind the panels).
    void SetPanelsVisible(bool visible);
    bool PanelsVisible();      // IsActive() and not hidden
    float Scale();             // UI scale factor (monitor DPI or PHISIKS_UI_SCALE; 1 = 96 dpi): multiply layout pixels by it
}
