#pragma once
// Phisiks::Compute: backend-neutral GPU compute interface.
//
// Backends:
//   OpenGL  compute shaders + SSBOs (OpenGL 4.6 tier): what the engine's GPU systems use (culling,
//           Hi-Z, particles, spatial hash, XPBD, field sampling) when the GPU supports it.
//   CPU     multi-threaded C++ kernels on a worker pool, always available (also headless). On the
//           OpenGL 4.1 tier (macOS, older iGPUs) the engine runs particles, XPBD and field sampling
//           on it. Kernels are C++ functions registered by name (RegisterCpuKernel); the rest of the
//           API (buffers, bindings, uniforms, Dispatch in work groups) is identical.
//   OpenCL, SYCL  interface hooks: GetBackend() returns nullptr until a backend is built.
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
#include <functional>
#include <string>
#include <vector>

namespace Phisiks::Compute {
    enum class BackendType { OpenGL, OpenCL, SYCL, CPU };
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
        // CPU: `name` selects a kernel registered with RegisterCpuKernel (`source` is ignored).
        virtual KernelId CreateKernel(const std::string& name, const std::string& source) = 0;
        virtual void SetBuffer(KernelId kernel, uint32_t binding, BufferId buffer) = 0;
        virtual void SetUniform(KernelId kernel, const char* name, float value) = 0;
        virtual void SetUniform(KernelId kernel, const char* name, uint32_t value) = 0;
        virtual void Dispatch(KernelId kernel, uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) = 0;
        virtual void Barrier() = 0;       // makes writes of previous dispatches visible to the next ones
        virtual void Finish() = 0;
    };

    // OpenGL is available once the renderer is initialised on the 4.6 tier (it shares the GL
    // context); CPU is always available.
    IComputeBackend* GetBackend(BackendType type);
    std::vector<DeviceInfo> EnumerateBackends();   // OpenGL, OpenCL, SYCL, CPU

    // CPU kernels: invoked once per invocation (work group size x groups), in parallel.
    struct CpuInvocation {
        uint32_t globalId[3] = { 0, 0, 0 };
        std::vector<void*>* buffers = nullptr;              // by binding (nullptr if unbound)
        std::vector<size_t>* bufferSizes = nullptr;
        const std::function<float(const char*)>* uniformF = nullptr;
        const std::function<uint32_t(const char*)>* uniformU = nullptr;
        template<class T> T* Buffer(uint32_t binding) const { return binding < buffers->size() ? static_cast<T*>((*buffers)[binding]) : nullptr; }
        float F(const char* name) const { return (*uniformF)(name); }
        uint32_t U(const char* name) const { return (*uniformU)(name); }
    };
    using CpuKernelFunction = std::function<void(const CpuInvocation&)>;
    void RegisterCpuKernel(const std::string& name, uint32_t workGroupSize, CpuKernelFunction kernel);
}
