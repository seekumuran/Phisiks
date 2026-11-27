// Phisiks::Compute backends: OpenGL 4.6 compute, the multi-threaded CPU backend, OpenCL/SYCL hooks.
#include "Phisiks/Compute/Compute.h"
#include "Phisiks/Core/Log.h"
#include "render/RendererInternal.h"
#include "render/gl/GLObjects.h"
#include "compute/ThreadPool.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Phisiks::Compute {
    const char* ToString(BackendType type) {
        switch (type) {
            case BackendType::OpenGL: return "OpenGL";
            case BackendType::OpenCL: return "OpenCL";
            case BackendType::SYCL:   return "SYCL";
            case BackendType::CPU:    return "CPU";
        }
        return "?";
    }

    namespace {
        class GLBackend final : public IComputeBackend {
        public:
            ~GLBackend() override {
                for (auto& [id, kernel] : m_kernels) glDeleteProgram(kernel.program);
                for (auto& [id, buffer] : m_buffers) glDeleteBuffers(1, &buffer);
            }

            DeviceInfo Info() const override {
                DeviceInfo info;
                info.backend = BackendType::OpenGL;
                info.available = true;
                info.device = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                GLint v = 0;
                glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &v);
                info.maxWorkGroupInvocations = uint32_t(v);
                glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &v);
                info.maxSharedMemoryBytes = uint32_t(v);
                glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &v);
                info.maxStorageBlocksPerKernel = uint32_t(v);
                return info;
            }

            BufferId CreateBuffer(size_t bytes) override {
                GLuint id = 0;
                glCreateBuffers(1, &id);
                glNamedBufferStorage(id, GLsizeiptr(std::max<size_t>(bytes, 4)), nullptr, GL_DYNAMIC_STORAGE_BIT);
                const BufferId handle = ++m_next;
                m_buffers[handle] = id;
                return handle;
            }
            void DestroyBuffer(BufferId buffer) override {
                auto it = m_buffers.find(buffer);
                if (it == m_buffers.end()) return;
                glDeleteBuffers(1, &it->second);
                m_buffers.erase(it);
            }
            void Upload(BufferId buffer, const void* data, size_t bytes, size_t offset) override {
                if (GLuint id = Gl(buffer)) glNamedBufferSubData(id, GLintptr(offset), GLsizeiptr(bytes), data);
            }
            void Download(BufferId buffer, void* data, size_t bytes, size_t offset) override {
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                if (GLuint id = Gl(buffer)) glGetNamedBufferSubData(id, GLintptr(offset), GLsizeiptr(bytes), data);
            }

            KernelId CreateKernel(const std::string& name, const std::string& source) override {
                const GLuint program = Render::Internal::BuildComputeProgram("user:" + name, source);
                if (!program) return 0;
                const KernelId handle = ++m_next;
                m_kernels[handle] = Kernel{ program, {} };
                return handle;
            }
            void SetBuffer(KernelId kernel, uint32_t binding, BufferId buffer) override {
                auto it = m_kernels.find(kernel);
                if (it != m_kernels.end()) it->second.bindings[binding] = buffer;
            }
            void SetUniform(KernelId kernel, const char* name, float value) override {
                if (const Kernel* k = Find(kernel)) glProgramUniform1f(k->program, glGetUniformLocation(k->program, name), value);
            }
            void SetUniform(KernelId kernel, const char* name, uint32_t value) override {
                if (const Kernel* k = Find(kernel)) glProgramUniform1ui(k->program, glGetUniformLocation(k->program, name), value);
            }
            void Dispatch(KernelId kernel, uint32_t x, uint32_t y, uint32_t z) override {
                const Kernel* k = Find(kernel);
                if (!k) return;
                for (const auto& [binding, buffer] : k->bindings) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, Gl(buffer));
                glUseProgram(k->program);
                glDispatchCompute(x, y, z);
            }
            void Barrier() override { glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT); }
            void Finish() override { glFinish(); }

        private:
            struct Kernel { GLuint program; std::unordered_map<uint32_t, BufferId> bindings; };
            GLuint Gl(BufferId b) const { auto it = m_buffers.find(b); return it == m_buffers.end() ? 0 : it->second; }
            const Kernel* Find(KernelId k) const { auto it = m_kernels.find(k); return it == m_kernels.end() ? nullptr : &it->second; }
            std::unordered_map<BufferId, GLuint> m_buffers;
            std::unordered_map<KernelId, Kernel> m_kernels;
            uint32_t m_next = 0;
        };

        std::unique_ptr<GLBackend> g_gl;

        // ------------------------------------------------------------------ CPU
        struct CpuKernelDef { uint32_t groupSize = 64; CpuKernelFunction function; };
        std::mutex g_registryMutex;
        std::map<std::string, CpuKernelDef>& Registry() { static std::map<std::string, CpuKernelDef> r; return r; }

        class CpuBackend final : public IComputeBackend {
        public:
            DeviceInfo Info() const override {
                DeviceInfo info;
                info.backend = BackendType::CPU;
                info.available = true;
                info.device = "CPU (" + std::to_string(ThreadPool::Get().Workers() + 1) + " threads)";
                info.maxWorkGroupInvocations = 1024;
                info.maxSharedMemoryBytes = 0;
                info.maxStorageBlocksPerKernel = 16;
                return info;
            }
            BufferId CreateBuffer(size_t bytes) override {
                const BufferId id = ++m_next;
                m_buffers[id].assign(std::max<size_t>(bytes, 4), 0);
                return id;
            }
            void DestroyBuffer(BufferId buffer) override { m_buffers.erase(buffer); }
            void Upload(BufferId buffer, const void* data, size_t bytes, size_t offset) override {
                auto it = m_buffers.find(buffer);
                if (it == m_buffers.end() || offset + bytes > it->second.size()) return;
                std::memcpy(it->second.data() + offset, data, bytes);
            }
            void Download(BufferId buffer, void* data, size_t bytes, size_t offset) override {
                auto it = m_buffers.find(buffer);
                if (it == m_buffers.end() || offset + bytes > it->second.size()) return;
                std::memcpy(data, it->second.data() + offset, bytes);
            }
            KernelId CreateKernel(const std::string& name, const std::string&) override {
                std::lock_guard<std::mutex> lock(g_registryMutex);
                auto it = Registry().find(name);
                if (it == Registry().end()) { Log::Error() << "Compute (CPU): no kernel registered as '" << name << "'"; return 0; }
                const KernelId id = ++m_next;
                m_kernels[id].def = it->second;
                return id;
            }
            void SetBuffer(KernelId kernel, uint32_t binding, BufferId buffer) override {
                if (auto it = m_kernels.find(kernel); it != m_kernels.end()) it->second.bindings[binding] = buffer;
            }
            void SetUniform(KernelId kernel, const char* name, float value) override {
                if (auto it = m_kernels.find(kernel); it != m_kernels.end()) it->second.floats[name] = value;
            }
            void SetUniform(KernelId kernel, const char* name, uint32_t value) override {
                if (auto it = m_kernels.find(kernel); it != m_kernels.end()) it->second.uints[name] = value;
            }
            void Dispatch(KernelId kernel, uint32_t x, uint32_t y, uint32_t z) override {
                auto it = m_kernels.find(kernel);
                if (it == m_kernels.end()) return;
                Kernel& k = it->second;
                std::vector<void*> buffers(16, nullptr);
                std::vector<size_t> sizes(16, 0);
                for (const auto& [binding, id] : k.bindings) {
                    auto b = m_buffers.find(id);
                    if (b == m_buffers.end() || binding >= 16) continue;
                    buffers[binding] = b->second.data();
                    sizes[binding] = b->second.size();
                }
                const std::function<float(const char*)> uf = [&k](const char* n) { auto f = k.floats.find(n); return f == k.floats.end() ? 0.0f : f->second; };
                const std::function<uint32_t(const char*)> uu = [&k](const char* n) { auto u = k.uints.find(n); return u == k.uints.end() ? 0u : u->second; };
                const uint32_t sizeX = std::max(k.def.groupSize, 1u);
                const size_t width = size_t(x) * sizeX;
                const size_t total = width * y * z;
                ThreadPool::Get().ParallelFor(total, 1024, [&](size_t, size_t begin, size_t end) {
                    CpuInvocation inv;
                    inv.buffers = &buffers;
                    inv.bufferSizes = &sizes;
                    inv.uniformF = &uf;
                    inv.uniformU = &uu;
                    for (size_t i = begin; i < end; ++i) {
                        inv.globalId[0] = uint32_t(i % width);
                        inv.globalId[1] = uint32_t((i / width) % y);
                        inv.globalId[2] = uint32_t(i / (width * y));
                        k.def.function(inv);
                    }
                });
            }
            void Barrier() override {}   // Dispatch returns when every invocation finished
            void Finish() override {}

        private:
            struct Kernel {
                CpuKernelDef def;
                std::unordered_map<uint32_t, BufferId> bindings;
                std::unordered_map<std::string, float> floats;
                std::unordered_map<std::string, uint32_t> uints;
            };
            std::unordered_map<BufferId, std::vector<uint8_t>> m_buffers;
            std::unordered_map<KernelId, Kernel> m_kernels;
            uint32_t m_next = 0;
        };
        std::unique_ptr<CpuBackend> g_cpu;
    }

    void RegisterCpuKernel(const std::string& name, uint32_t workGroupSize, CpuKernelFunction kernel) {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        Registry()[name] = CpuKernelDef{ std::max(workGroupSize, 1u), std::move(kernel) };
    }

    IComputeBackend* GetBackend(BackendType type) {
        if (type == BackendType::CPU) {
            if (!g_cpu) g_cpu = std::make_unique<CpuBackend>();
            return g_cpu.get();
        }
        if (type != BackendType::OpenGL || !Render::Internal::IsInitialized() || Render::GL::Compat()) return nullptr;
        if (!g_gl) g_gl = std::make_unique<GLBackend>();
        return g_gl.get();
    }

    std::vector<DeviceInfo> EnumerateBackends() {
        std::vector<DeviceInfo> list;
        if (IComputeBackend* gl = GetBackend(BackendType::OpenGL)) list.push_back(gl->Info());
        else list.push_back({ BackendType::OpenGL, false, "", 0, 0, 0,
                              Render::Internal::IsInitialized() ? "OpenGL 4.1 tier: no compute shaders (CPU backend used)" : "renderer not initialised (headless)" });
        list.push_back({ BackendType::OpenCL, false, "", 0, 0, 0, "interface hook only: no OpenCL backend is built" });
        list.push_back({ BackendType::SYCL, false, "", 0, 0, 0, "interface hook only: no SYCL backend is built" });
        list.push_back(GetBackend(BackendType::CPU)->Info());
        return list;
    }

    namespace Internal {
        void ReleaseBackends() { g_gl.reset(); }   // called by the renderer before the GL context goes away
    }
}
