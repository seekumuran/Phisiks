#pragma once
// Application window + OpenGL core context (GLFW backend, hidden from users of this header):
// 4.6 where available, down to 4.1 (macOS, older iGPUs). The renderer picks its feature tier from
// GetRequestedGLVersion().
// Window events feed Phisiks::GetInputState() and Phisiks::GlobalEvents().

#include <glm/vec2.hpp>

#include <string>

namespace Phisiks::Platform {

    enum class CursorMode { Normal, Hidden, Captured };   // Captured = hidden + unbounded relative motion
    enum class CursorShape { Arrow, IBeam, Crosshair, Hand, ResizeHorizontal, ResizeVertical };

    struct WindowDesc {
        std::string title = "Phisiks";
        int width = 0;               // 0 = 75% of the primary monitor
        int height = 0;
        bool fullscreen = false;     // borderless, monitor-sized
        bool resizable = true;
        bool vsync = false;
        bool debugContext = true;    // OpenGL debug output (KHR_debug)
        bool startVisible = false;   // engines usually show the window after the first frame is ready
        int maxGLVersion = 46;       // 46 / 45 / 43 / 41: cap the context (41 = the macOS feature tier)
    };

    bool CreateMainWindow(const WindowDesc& desc);
    void DestroyMainWindow();
    bool HasWindow();

    void PollEvents();               // pumps OS events into InputState / GlobalEvents
    void SwapBuffers();
    void Show();

    bool ShouldClose();              // user closed the window or RequestClose() was called
    void RequestClose();
    bool CloseWasRequestedByApp();
    void CancelClose();              // the user's close request is ignored (the window stays open)
    bool HasFocus();
    bool IsMinimized();

    void SetFullscreen(bool fullscreen);
    bool IsFullscreen();
    void ToggleFullscreen();
    void SetTitle(const std::string& title);
    void SetVSync(bool enabled);

    void SetCursorMode(CursorMode mode);
    CursorMode GetCursorMode();
    void SetCursorShape(CursorShape shape);
    void SetCursorPosition(double x, double y);    // window coordinates

    glm::ivec2 GetWindowSize();          // logical window size
    glm::ivec2 GetFramebufferSize();     // drawable pixels
    glm::ivec2 GetWindowedSize();        // size used when not fullscreen
    glm::ivec2 GetMonitorSize();         // primary monitor video mode

    void MakeContextCurrent();
    using GLProcLoader = void* (*)(const char*);
    GLProcLoader GetGLProcLoader();      // pass to gladLoadGL

    void* GetNativeHandle();             // GLFWwindow*
    void* GetWin32Handle();              // HWND (nullptr elsewhere)
    int GetGLContextVersion();           // e.g. 46 (what the driver created)
    int GetRequestedGLVersion();         // the version the window asked for (<= maxGLVersion): the renderer's tier
}
