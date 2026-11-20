#include "Phisiks/Platform/Window.h"
#include "Phisiks/Core/Events.h"
#include "Phisiks/Core/InputState.h"
#include "Phisiks/Core/Log.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <array>
#include <cmath>

namespace Phisiks::Platform {
    namespace {
        struct State {
            GLFWwindow* window = nullptr;
            std::array<GLFWcursor*, 6> cursors{};
            WindowDesc desc;
            glm::ivec2 windowedSize{ 0 };
            glm::ivec2 windowedPosition{ 0 };
            bool fullscreen = false;
            bool appRequestedClose = false;
            bool focused = false;
            CursorMode cursorMode = CursorMode::Normal;
        } g;

        void OnKey(GLFWwindow*, int key, int /*scancode*/, int action, int /*mods*/) {
            if (action == GLFW_REPEAT) return;
            GetInputState().OnKey(key, action == GLFW_PRESS);
        }
        void OnMouseButton(GLFWwindow*, int button, int action, int /*mods*/) {
            GetInputState().OnMouseButton(button, action == GLFW_PRESS);
        }
        void OnCursor(GLFWwindow*, double x, double y) {
            GetInputState().OnCursorPosition(x, y);
        }
        void OnScroll(GLFWwindow*, double dx, double dy) {
            GetInputState().OnScroll(dx, dy);
        }
        void OnFocus(GLFWwindow*, int focused) {
            g.focused = focused != 0;
            if (!g.focused) GetInputState().OnFocusLost();
            GlobalEvents().Enqueue(WindowFocusEvent{ g.focused });
        }
        void OnFramebufferSize(GLFWwindow*, int width, int height) {
            GlobalEvents().Enqueue(WindowResizedEvent{ width, height });
        }
        void OnClose(GLFWwindow*) {
            GlobalEvents().Enqueue(WindowCloseRequestedEvent{});
        }
        void OnError(int code, const char* message) {
            Log::Error() << "GLFW error " << code << ": " << message;
        }

        const GLFWvidmode* PrimaryMode() {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            return monitor ? glfwGetVideoMode(monitor) : nullptr;
        }
    }

    bool CreateMainWindow(const WindowDesc& desc) {
        if (g.window) return true;
        glfwSetErrorCallback(OnError);
        if (!glfwInit()) {
            Log::Error() << "Platform: glfwInit failed";
            return false;
        }

        const GLFWvidmode* mode = PrimaryMode();
        const glm::ivec2 monitor = mode ? glm::ivec2(mode->width, mode->height) : glm::ivec2(1280, 720);
        g.desc = desc;
        g.windowedSize = glm::ivec2(desc.width > 0 ? desc.width : int(std::lround(monitor.x * 0.75)),
                                    desc.height > 0 ? desc.height : int(std::lround(monitor.y * 0.75)));

        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, desc.debugContext ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, desc.startVisible ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 0);

        g.window = glfwCreateWindow(g.windowedSize.x, g.windowedSize.y, desc.title.c_str(), nullptr, nullptr);
        if (!g.window) {
            Log::Error() << "Platform: could not create an OpenGL 4.6 core window";
            glfwTerminate();
            return false;
        }
        if (mode) {
            g.windowedPosition = (monitor - g.windowedSize) / 2;
            glfwSetWindowPos(g.window, g.windowedPosition.x, g.windowedPosition.y);
        }

        glfwSetKeyCallback(g.window, OnKey);
        glfwSetMouseButtonCallback(g.window, OnMouseButton);
        glfwSetCursorPosCallback(g.window, OnCursor);
        glfwSetScrollCallback(g.window, OnScroll);
        glfwSetWindowFocusCallback(g.window, OnFocus);
        glfwSetFramebufferSizeCallback(g.window, OnFramebufferSize);
        glfwSetWindowCloseCallback(g.window, OnClose);

        const int shapes[6] = { GLFW_ARROW_CURSOR, GLFW_IBEAM_CURSOR, GLFW_CROSSHAIR_CURSOR, GLFW_HAND_CURSOR, GLFW_HRESIZE_CURSOR, GLFW_VRESIZE_CURSOR };
        for (int i = 0; i < 6; ++i) g.cursors[i] = glfwCreateStandardCursor(shapes[i]);

        glfwMakeContextCurrent(g.window);
        SetVSync(desc.vsync);
        if (desc.fullscreen) SetFullscreen(true);
        g.focused = glfwGetWindowAttrib(g.window, GLFW_FOCUSED) != 0;
        Log::Info() << "Platform: window " << g.windowedSize.x << "x" << g.windowedSize.y << " (monitor " << monitor.x << "x" << monitor.y << ")";
        return true;
    }

    void DestroyMainWindow() {
        if (!g.window) return;
        for (GLFWcursor*& cursor : g.cursors) {
            if (cursor) glfwDestroyCursor(cursor);
            cursor = nullptr;
        }
        glfwDestroyWindow(g.window);
        g.window = nullptr;
        glfwTerminate();
    }

    bool HasWindow() { return g.window != nullptr; }

    void PollEvents() {
        glfwPollEvents();
        if (g.window) g.focused = glfwGetWindowAttrib(g.window, GLFW_FOCUSED) != 0;
    }

    void SwapBuffers() { if (g.window) glfwSwapBuffers(g.window); }
    void Show()        { if (g.window) glfwShowWindow(g.window); }

    bool ShouldClose()            { return !g.window || g.appRequestedClose || glfwWindowShouldClose(g.window); }
    void RequestClose()           { g.appRequestedClose = true; }
    bool CloseWasRequestedByApp() { return g.appRequestedClose; }
    bool HasFocus()               { return g.focused; }

    bool IsMinimized() {
        const glm::ivec2 size = GetFramebufferSize();
        return size.x == 0 || size.y == 0;
    }

    void SetFullscreen(bool fullscreen) {
        if (!g.window || fullscreen == g.fullscreen) return;
        const GLFWvidmode* mode = PrimaryMode();
        if (fullscreen && mode) {
            glfwGetWindowPos(g.window, &g.windowedPosition.x, &g.windowedPosition.y);
            glfwGetWindowSize(g.window, &g.windowedSize.x, &g.windowedSize.y);
            // Borderless window covering the monitor (fast alt-tab, no mode switch).
            glfwSetWindowAttrib(g.window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowMonitor(g.window, nullptr, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);
        }
        else {
            glfwSetWindowAttrib(g.window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(g.window, nullptr, g.windowedPosition.x, g.windowedPosition.y, g.windowedSize.x, g.windowedSize.y, GLFW_DONT_CARE);
        }
        g.fullscreen = fullscreen;
    }

    bool IsFullscreen()      { return g.fullscreen; }
    void ToggleFullscreen()  { SetFullscreen(!g.fullscreen); }

    void SetTitle(const std::string& title) {
        g.desc.title = title;
        if (g.window) glfwSetWindowTitle(g.window, title.c_str());
    }

    void SetVSync(bool enabled) {
        g.desc.vsync = enabled;
        if (g.window) glfwSwapInterval(enabled ? 1 : 0);
    }

    void SetCursorMode(CursorMode mode) {
        if (!g.window) return;
        g.cursorMode = mode;
        const int glfwMode = mode == CursorMode::Normal ? GLFW_CURSOR_NORMAL
                           : mode == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_DISABLED;
        glfwSetInputMode(g.window, GLFW_CURSOR, glfwMode);
        if (mode == CursorMode::Captured && glfwRawMouseMotionSupported()) {
            glfwSetInputMode(g.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        GetInputState().ResetCursorTracking();   // avoid a jump when the cursor mode changes
    }

    CursorMode GetCursorMode() { return g.cursorMode; }

    void SetCursorShape(CursorShape shape) {
        const int index = static_cast<int>(shape);
        if (g.window && index >= 0 && index < int(g.cursors.size())) glfwSetCursor(g.window, g.cursors[index]);
    }

    void SetCursorPosition(double x, double y) {
        if (!g.window) return;
        glfwSetCursorPos(g.window, x, y);
        GetInputState().ResetCursorTracking();
    }

    glm::ivec2 GetWindowSize() {
        glm::ivec2 size(0);
        if (g.window) glfwGetWindowSize(g.window, &size.x, &size.y);
        return size;
    }

    glm::ivec2 GetFramebufferSize() {
        glm::ivec2 size(0);
        if (g.window) glfwGetFramebufferSize(g.window, &size.x, &size.y);
        return size;
    }

    glm::ivec2 GetWindowedSize() { return g.windowedSize; }

    glm::ivec2 GetMonitorSize() {
        const GLFWvidmode* mode = PrimaryMode();
        return mode ? glm::ivec2(mode->width, mode->height) : glm::ivec2(0);
    }

    void MakeContextCurrent() { if (g.window) glfwMakeContextCurrent(g.window); }

    GLProcLoader GetGLProcLoader() {
        return reinterpret_cast<GLProcLoader>(glfwGetProcAddress);
    }

    void* GetNativeHandle() { return g.window; }
    void* GetWin32Handle()  { return g.window ? glfwGetWin32Window(g.window) : nullptr; }
}
