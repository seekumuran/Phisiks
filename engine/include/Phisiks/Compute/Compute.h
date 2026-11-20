#pragma once
// Phisiks::Compute: backend-neutral GPU compute interface.
//
// The OpenGL 4.6 backend (compute shaders + SSBOs) is implemented and is what the engine's own GPU
// systems use (culling, Hi-Z, particles, spatial hash, XPBD, field sampling). OpenCL and SYCL are
// interface hooks: GetBackend() returns nullptr for them until a backend is built.
//
//   auto* gpu = Compute::GetBackend(Compute::BackendType::OpenGL);
//   auto x = gpu->CreateBuffer(n * sizeof(float));
//   auto k = gpu->CreateKernel("scale", glslSource);      // binding points as declared in the source
//   gpu->Upload(x, data, bytes);
//   gpu->SetBuffer(k, 0, x);
//   gpu->SetUniform(k, "u_factor", 2.0f);
//   gpu->Dispatch(k, (n + 63) / 64);
//   gpu->Download(x, out, bytes);                        // waits for the GPU

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::Compute {
    enum class BackendType { OpenGL, OpenCL, SYCL };
    const char* ToString(BackendType type);

    using BufferId = uint32_t;   // 0 = invalid
    using KernelId = uint32_t;   // 0 = invalid

    struct DeviceInfo {
        BackendType backend = BackendType::OpenGL;
        bool available = false;
        std::string device;               // e.g. the GL renderer string
        uint32_t maxWorkGroupInvocations = 0;
        uint32_t maxSharedMemoryBytes = 0;
        uint32_t maxStorageBlocksPerKernel = 0;
        std::string note;                 // why a backend is unavailable
    };

    class IComputeBackend {
    public:
        virtual ~IComputeBackend() = default;
        virtual DeviceInfo Info() const = 0;
        virtual BufferId CreateBuffer(size_t bytes) = 0;
        virtual void DestroyBuffer(BufferId buffer) = 0;
        virtual void Upload(BufferId buffer, const void* data, size_t bytes, size_t offset = 0) = 0;
        virtual void Download(BufferId buffer, void* data, size_t bytes, size_t offset = 0) = 0;   // synchronises
        // OpenGL: `source` is a GLSL 4.60 compute shader (it is cached on disk like engine shaders).
        virtual KernelId CreateKernel(const std::string& name, const std::string& source) = 0;
        virtual void SetBuffer(KernelId kernel, uint32_t binding, BufferId buffer) = 0;
        virtual void SetUniform(KernelId kernel, const char* name, float value) = 0;
        virtual void SetUniform(KernelId kernel, const char* name, uint32_t value) = 0;
        virtual void Dispatch(KernelId kernel, uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) = 0;
        virtual void Barrier() = 0;       // makes writes of previous dispatches visible to the next ones
        virtual void Finish() = 0;
    };

    // OpenGL is available once the renderer is initialised (it shares the GL context).
    IComputeBackend* GetBackend(BackendType type);
    std::vector<DeviceInfo> EnumerateBackends();
}
