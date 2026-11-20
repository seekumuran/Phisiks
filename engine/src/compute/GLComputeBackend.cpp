// OpenGL 4.6 implementation of Phisiks::Compute::IComputeBackend, plus the OpenCL/SYCL hooks.
#include "Phisiks/Compute/Compute.h"
#include "Phisiks/Core/Log.h"
#include "render/RendererInternal.h"
#include "render/gl/GLObjects.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace Phisiks::Compute {
    const char* ToString(BackendType type) {
        switch (type) {
            case BackendType::OpenGL: return "OpenGL";
            case BackendType::OpenCL: return "OpenCL";
            case BackendType::SYCL:   return "SYCL";
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
    }

    IComputeBackend* GetBackend(BackendType type) {
        if (type != BackendType::OpenGL || !Render::Internal::IsInitialized()) return nullptr;
        if (!g_gl) g_gl = std::make_unique<GLBackend>();
        return g_gl.get();
    }

    std::vector<DeviceInfo> EnumerateBackends() {
        std::vector<DeviceInfo> list;
        if (IComputeBackend* gl = GetBackend(BackendType::OpenGL)) list.push_back(gl->Info());
        else list.push_back({ BackendType::OpenGL, false, "", 0, 0, 0, "renderer not initialised (headless)" });
        list.push_back({ BackendType::OpenCL, false, "", 0, 0, 0, "interface hook only: no OpenCL backend is built" });
        list.push_back({ BackendType::SYCL, false, "", 0, 0, 0, "interface hook only: no SYCL backend is built" });
        return list;
    }

    namespace Internal {
        void ReleaseBackends() { g_gl.reset(); }   // called by the renderer before the GL context goes away
    }
}
