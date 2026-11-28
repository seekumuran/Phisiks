#pragma once
// Thin RAII wrappers over OpenGL objects plus the state helpers the renderer uses, for two feature
// tiers selected once at startup (SetTier):
//   GL46  OpenGL 4.6 core: direct state access, compute, SSBOs, indirect multi-draw (default)
//   GL41  OpenGL 4.1 core (macOS, older iGPUs): bind-to-edit, texture buffers instead of SSBOs,
//         no compute (the engine's GPU systems run on the CPU compute backend)
// Every helper takes the object name and does the right thing for the active tier. Move-only types.

#include "render/ShaderPreprocessor.h"

#include <glad/gl.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Phisiks::Render::GL {

    enum class Tier { GL46, GL41 };
    void SetTier(Tier tier);
    Tier GetTier();
    inline bool Compat() { return GetTier() == Tier::GL41; }

    void Label(GLenum identifier, GLuint name, const std::string& label);   // KHR_debug object label (4.6 tier)

    // ------------------------------------------------------------------ state helpers
    GLuint CreateBufferObject(size_t size, const void* data, GLenum usageHint = GL_DYNAMIC_DRAW);
    void BufferSubData(GLuint buffer, size_t offset, size_t size, const void* data);
    void GetBufferSubData(GLuint buffer, size_t offset, size_t size, void* data);
    void ClearBufferZero(GLuint buffer, size_t size);

    void BindTexture(GLuint unit, GLenum target, GLuint texture);
    void ClearColor(GLuint framebuffer, int drawBuffer, const float* rgba);
    void ClearColorUint(GLuint framebuffer, int drawBuffer, const GLuint* value);
    void ClearDepth(GLuint framebuffer, float depth);
    void ReadBuffer(GLuint framebuffer, GLenum attachment);
    void Blit(GLuint source, GLuint destination, int srcW, int srcH, int dstW, int dstH, GLbitfield mask, GLenum filter);

    struct VertexAttrib { GLuint index; GLint size; GLenum type; size_t offset; };
    GLuint CreateVertexArray();
    // Sets the vertex buffer and attribute layout of `vao` (binding 0). Leaves the bound VAO unchanged.
    void SetVertexBuffer(GLuint vao, GLuint buffer, GLsizei stride, std::initializer_list<VertexAttrib> attribs);
    void SetElementBuffer(GLuint vao, GLuint buffer);

    // ------------------------------------------------------------------ buffer
    // Dynamic buffer that grows on demand (contents are discarded when it grows).
    class Buffer {
    public:
        Buffer() = default;
        ~Buffer() { Release(); }
        Buffer(Buffer&& o) noexcept { *this = std::move(o); }
        Buffer& operator=(Buffer&& o) noexcept { std::swap(m_id, o.m_id); std::swap(m_size, o.m_size); std::swap(m_label, o.m_label); return *this; }
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        void Create(const std::string& label, size_t size, const void* data = nullptr);
        void EnsureSize(size_t size);                 // grows to at least `size` (x1.5)
        void Upload(const void* data, size_t size, size_t offset = 0);   // grows if needed (offset 0 only)
        void Release();
        GLuint Id() const { return m_id; }
        size_t Size() const { return m_size; }

    private:
        GLuint m_id = 0;
        size_t m_size = 0;
        std::string m_label;
    };

    // ------------------------------------------------------------------ textures
    struct TextureDesc {
        GLenum target = GL_TEXTURE_2D;   // 2D, 2D_MULTISAMPLE, 2D_ARRAY, CUBE_MAP_ARRAY
        GLenum format = GL_RGBA8;        // sized internal format
        int width = 1, height = 1;
        int layers = 1;                  // array layers (cube arrays: number of cubes * 6)
        int samples = 1;                 // multisample targets
        int levels = 1;
        GLenum minFilter = GL_LINEAR, magFilter = GL_LINEAR;
        GLenum wrap = GL_CLAMP_TO_EDGE;
        bool depthCompare = false;       // shadow sampler (GL_COMPARE_REF_TO_TEXTURE, LEQUAL)
    };

    class Texture {
    public:
        Texture() = default;
        ~Texture() { Release(); }
        Texture(Texture&& o) noexcept { *this = std::move(o); }
        Texture& operator=(Texture&& o) noexcept { std::swap(m_id, o.m_id); std::swap(m_desc, o.m_desc); return *this; }
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        void Create(const std::string& label, const TextureDesc& desc);
        void Upload2D(GLenum format, GLenum type, const void* pixels);   // level 0, whole image
        void UploadLayer(int layer, GLenum format, GLenum type, const void* pixels);   // array textures, level 0
        void GenerateMipmaps();
        void SetParameter(GLenum name, float value);
        void Read(GLenum format, GLenum type, size_t bytes, void* out) const;   // level 0 (synchronous)
        void Bind(GLuint unit) const { BindTexture(unit, m_desc.target, m_id); }
        void Release();
        GLuint Id() const { return m_id; }
        const TextureDesc& Desc() const { return m_desc; }

    private:
        GLuint m_id = 0;
        TextureDesc m_desc;
    };

    // Texture-buffer view (4.1 tier storage access): re-pointed at a buffer before each use.
    class TextureBuffer {
    public:
        ~TextureBuffer() { Release(); }
        void Bind(GLuint unit, GLenum format, GLuint buffer);
        void Release();
    private:
        GLuint m_id = 0;
    };

    // ------------------------------------------------------------------ framebuffer
    class Framebuffer {
    public:
        Framebuffer() = default;
        ~Framebuffer() { Release(); }
        Framebuffer(Framebuffer&& o) noexcept { std::swap(m_id, o.m_id); }
        Framebuffer& operator=(Framebuffer&& o) noexcept { std::swap(m_id, o.m_id); return *this; }
        Framebuffer(const Framebuffer&) = delete;
        Framebuffer& operator=(const Framebuffer&) = delete;

        void Create(const std::string& label);
        void AttachColor(int index, const Texture& texture, int layer = -1);   // layer >= 0: one layer of an array
        void AttachDepth(const Texture& texture, int layer = -1);
        void SetDrawBuffers(int colorCount);                                   // 0 = depth only
        bool Check(const char* what) const;                                    // logs incomplete status
        void Release();
        GLuint Id() const { return m_id; }

    private:
        GLuint m_id = 0;
    };

    // ------------------------------------------------------------------ program
    struct StageSource {
        GLenum stage;                    // GL_VERTEX_SHADER / GL_FRAGMENT_SHADER / GL_GEOMETRY_SHADER / GL_COMPUTE_SHADER
        std::string source;              // fully preprocessed
    };

    class Program {
    public:
        Program() = default;
        ~Program() { Release(); }
        Program(Program&& o) noexcept { std::swap(m_id, o.m_id); std::swap(m_name, o.m_name); }
        Program& operator=(Program&& o) noexcept { std::swap(m_id, o.m_id); std::swap(m_name, o.m_name); return *this; }
        Program(const Program&) = delete;
        Program& operator=(const Program&) = delete;

        void Adopt(GLuint id, const std::string& name) { Release(); m_id = id; m_name = name; }
        void Release();
        void Use() const { glUseProgram(m_id); }
        GLuint Id() const { return m_id; }
        bool Valid() const { return m_id != 0; }
        GLint Uniform(const char* name) const { return glGetUniformLocation(m_id, name); }
        const std::string& Name() const { return m_name; }

    private:
        GLuint m_id = 0;
        std::string m_name;
    };

    // Compiles + links from source. `retrievable` asks the driver to keep the binary for the cache.
    // Returns 0 on failure (errors are logged with the program name and stage).
    GLuint BuildProgram(const std::string& name, const std::vector<StageSource>& stages, bool retrievable);

    // 4.1 tier: sets the sampler units / block bindings recorded by ShaderPreprocessor::RewriteForGL41.
    void ApplyBindings(GLuint program, const ShaderPreprocessor::ResourceBindings& bindings);
}
