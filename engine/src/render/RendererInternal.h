#pragma once
// Engine-side renderer control (the scene backend and the Application drive these).

#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Components.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Render::Internal {
    bool Init();                    // needs a current GL 4.6 context; loads GL entry points
    void Shutdown();
    bool IsInitialized();

    uint64_t CreateObject(const MeshRenderer& renderer, const glm::mat4& world, uint32_t entityId);
    void UpdateObject(uint64_t handle, const MeshRenderer& renderer, const glm::mat4& world);
    void DestroyObject(uint64_t handle);

    uint64_t CreateLight(const Light& light, const glm::vec3& position, const glm::vec3& direction);
    void UpdateLight(uint64_t handle, const Light& light, const glm::vec3& position, const glm::vec3& direction);
    void DestroyLight(uint64_t handle);

    struct CameraInput {
        glm::mat4 view;             // world -> camera
        Camera camera;
    };
    void SetCameras(const std::vector<CameraInput>& cameras);   // rendered in this order

    void RenderFrame();             // renders into the default framebuffer (caller presents)

    // GL program of a Shaders::ProgramId (compute systems dispatch the renderer's programs).
    uint32_t ProgramHandle(int programId);
    // Builds (and caches on disk) a compute program from GLSL source (#include of engine files allowed).
    uint32_t BuildComputeProgram(const std::string& name, const std::string& source);
    // The last presented window image (scene + overlay text), RGBA8, rows bottom-up.
    bool ReadPresented(std::vector<uint8_t>& rgba, int& width, int& height);
    RenderStats Stats();
}
