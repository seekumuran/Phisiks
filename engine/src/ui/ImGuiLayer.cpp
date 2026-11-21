#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Platform/Window.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

namespace Phisiks::UI {
    namespace {
        bool g_active = false;
        bool g_inFrame = false;
        int g_vertices = 0;

        // Dark studio theme: charcoal panels, cool blue accents (matches the renderer backdrop).
        void ApplyTheme() {
            ImGui::StyleColorsDark();
            ImGuiStyle& s = ImGui::GetStyle();
            s.WindowRounding = 6.0f;
            s.FrameRounding = 4.0f;
            s.GrabRounding = 4.0f;
            s.TabRounding = 4.0f;
            s.WindowBorderSize = 1.0f;
            s.FramePadding = ImVec2(6, 4);
            s.ItemSpacing = ImVec2(8, 5);
            ImVec4* c = s.Colors;
            const ImVec4 bg(0.075f, 0.080f, 0.095f, 0.96f), panel(0.11f, 0.12f, 0.14f, 1.0f), accent(0.35f, 0.55f, 0.90f, 1.0f), accentDim(0.22f, 0.34f, 0.58f, 1.0f);
            c[ImGuiCol_WindowBg] = bg;
            c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = bg;
            c[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.30f, 0.8f);
            c[ImGuiCol_FrameBg] = panel;
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
            c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.065f, 0.08f, 1.0f);
            c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.17f, 1.0f);
            c[ImGuiCol_MenuBarBg] = ImVec4(0.06f, 0.065f, 0.08f, 1.0f);
            c[ImGuiCol_Header] = accentDim;
            c[ImGuiCol_HeaderHovered] = accent;
            c[ImGuiCol_HeaderActive] = accent;
            c[ImGuiCol_Button] = accentDim;
            c[ImGuiCol_ButtonHovered] = accent;
            c[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
            c[ImGuiCol_SliderGrab] = accent;
            c[ImGuiCol_SliderGrabActive] = ImVec4(0.5f, 0.7f, 1.0f, 1.0f);
            c[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
            c[ImGuiCol_Tab] = panel;
            c[ImGuiCol_TabHovered] = accent;
            c[ImGuiCol_PlotLines] = accent;
            c[ImGuiCol_PlotHistogram] = accent;
        }
    }

    bool Init() {
        if (g_active) return true;
        if (!Platform::HasWindow()) return false;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // layout is code-defined (no imgui.ini in the working directory)
        ApplyTheme();
        if (!ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(Platform::GetNativeHandle()), true) || !ImGui_ImplOpenGL3_Init("#version 410")) {
            Log::Error() << "UI: Dear ImGui backend initialisation failed";
            ImGui::DestroyContext();
            return false;
        }
        g_active = true;
        Log::Info() << "UI: Dear ImGui " << IMGUI_VERSION << " ready";
        return true;
    }

    void Shutdown() {
        if (!g_active) return;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        g_active = false;
    }

    bool IsActive() { return g_active; }

    void BeginFrame() {
        if (!g_active || g_inFrame) return;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        g_inFrame = true;
    }

    void EndFrame() {
        if (!g_active || !g_inFrame) return;
        ImGui::Render();
        ImDrawData* data = ImGui::GetDrawData();
        g_vertices = data ? data->TotalVtxCount : 0;
        ImGui_ImplOpenGL3_RenderDrawData(data);
        g_inFrame = false;
    }

    bool WantsMouse() { return g_active && ImGui::GetIO().WantCaptureMouse; }
    bool WantsKeyboard() { return g_active && ImGui::GetIO().WantCaptureKeyboard; }
    int LastDrawVertexCount() { return g_vertices; }
}
