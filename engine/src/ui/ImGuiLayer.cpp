#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/UI/RetroDeck.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

namespace Phisiks::UI {
    namespace {
        bool g_active = false;
        bool g_inFrame = false;
        int g_vertices = 0;

        bool g_panelsVisible = true;
        float g_scale = 1.0f;

        // UI scale: the monitor's content scale (Windows / Linux report window sizes in pixels; macOS
        // in points, where the backend already renders at the framebuffer scale), overridable with
        // PHISIKS_UI_SCALE (e.g. 1.25).
        float ContentScale(GLFWwindow* window) {
            if (const char* env = std::getenv("PHISIKS_UI_SCALE")) {
                const float s = float(std::atof(env));
                if (s >= 0.5f && s <= 4.0f) return s;
            }
#if defined(__APPLE__)
            (void)window;
            return 1.0f;
#else
            float x = 1.0f, y = 1.0f;
            glfwGetWindowContentScale(window, &x, &y);
            return std::clamp(std::max(x, y), 1.0f, 4.0f);
#endif
        }
    }

    float Scale() { return g_scale; }

    bool Init() {
        if (g_active) return true;
        if (!Platform::HasWindow()) return false;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // layout is code-defined (no imgui.ini in the working directory)
        Retro::ApplyTheme();
        GLFWwindow* window = static_cast<GLFWwindow*>(Platform::GetNativeHandle());
        g_scale = ContentScale(window);
        if (g_scale > 1.01f) {   // crisp text at high DPI: the built-in font rasterised at the scaled size
            ImFontConfig font;
            font.SizePixels = std::round(13.0f * g_scale);
            io.Fonts->AddFontDefault(&font);
            ImGui::GetStyle().ScaleAllSizes(g_scale);
        }
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 410")) {
            Log::Error() << "UI: Dear ImGui backend initialisation failed";
            ImGui::DestroyContext();
            return false;
        }
        g_active = true;
        Log::Info() << "UI: Dear ImGui " << IMGUI_VERSION << " ready (scale " << g_scale << ")";
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
        Retro::ResetFrameStats();
        g_inFrame = true;
    }

    void EndFrame() {
        if (!g_active || !g_inFrame) return;
        ImGui::Render();
        ImDrawData* data = ImGui::GetDrawData();
        // Draw into the window: the renderer leaves its own framebuffers bound (4.6 blits by name).
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_vertices = data ? data->TotalVtxCount : 0;
        ImGui_ImplOpenGL3_RenderDrawData(data);
        g_inFrame = false;
    }

    bool WantsMouse() { return g_active && ImGui::GetIO().WantCaptureMouse; }
    bool WantsKeyboard() { return g_active && ImGui::GetIO().WantCaptureKeyboard; }
    int LastDrawVertexCount() { return g_vertices; }
    void SetPanelsVisible(bool visible) { g_panelsVisible = visible; }
    bool PanelsVisible() { return g_active && g_panelsVisible; }
}
