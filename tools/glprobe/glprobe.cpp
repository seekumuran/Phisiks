// glprobe: reports whether this machine meets the Phisiks renderer requirements.
// Creates the same context the engine requests (GL 4.6 core, debug) and checks extensions.
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>

static bool HasExtension(const char* name) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        if (ext && std::strcmp(ext, name) == 0) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    const bool listAll = argc > 1 && std::strcmp(argv[1], "--all") == 0;
    if (!glfwInit()) { std::printf("FAIL: glfwInit\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "glprobe", nullptr, nullptr);
    if (!window) { std::printf("FAIL: could not create an OpenGL 4.6 core context\n"); glfwTerminate(); return 3; }
    glfwMakeContextCurrent(window);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { std::printf("FAIL: gladLoadGL\n"); return 4; }

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    std::printf("GL_VENDOR   : %s\n", glGetString(GL_VENDOR));
    std::printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER));
    std::printf("GL_VERSION  : %s (%d.%d)\n", glGetString(GL_VERSION), major, minor);

    if (listAll) {
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) std::printf("  %s\n", glGetStringi(GL_EXTENSIONS, i));
    }

    // The Phisiks renderer needs OpenGL 4.6 core only (DSA, multi-draw indirect, gl_BaseInstance,
    // cube map arrays, KHR_debug, anisotropic filtering are all core). Program binaries are
    // optional: without them the shader cache compiles every run.
    const bool versionOk = major > 4 || (major == 4 && minor >= 6);
    GLint binaryFormats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
    std::printf("%-36s %s\n", "OpenGL 4.6 core", versionOk ? "yes" : "NO");
    std::printf("%-36s %d%s\n", "program binary formats", binaryFormats, binaryFormats > 0 ? " (shader cache enabled)" : " (no shader cache)");
    std::printf("RESULT: %s\n", versionOk ? "PASS" : "FAIL (OpenGL 4.6 required)");
    glfwDestroyWindow(window);
    glfwTerminate();
    return versionOk ? 0 : 1;
}
