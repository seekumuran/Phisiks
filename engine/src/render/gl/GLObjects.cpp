#include "render/gl/GLObjects.h"
#include "Phisiks/Core/Log.h"

#include <algorithm>

namespace Phisiks::Render::GL {
    namespace {
        Tier g_tier = Tier::GL46;

        // 4.1 tier edits buffers through GL_COPY_WRITE_BUFFER (does not disturb VAO / indexed bindings).
        void BindScratch(GLuint buffer) { glBindBuffer(GL_COPY_WRITE_BUFFER, buffer); }

        // External format/type matching a sized internal format (4.1 tier glTexImage*).
        void ExternalFormat(GLenum internal, GLenum& format, GLenum& type) {
            switch (internal) {
                case GL_RGBA16F: format = GL_RGBA; type = GL_HALF_FLOAT; return;
                case GL_RGBA32F: format = GL_RGBA; type = GL_FLOAT; return;
                case GL_R32F: format = GL_RED; type = GL_FLOAT; return;
                case GL_R16F: format = GL_RED; type = GL_HALF_FLOAT; return;
                case GL_R32UI: format = GL_RED_INTEGER; type = GL_UNSIGNED_INT; return;
                case GL_R8: format = GL_RED; type = GL_UNSIGNED_BYTE; return;
                case GL_DEPTH_COMPONENT32F: format = GL_DEPTH_COMPONENT; type = GL_FLOAT; return;
                case GL_DEPTH_COMPONENT24: format = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_INT; return;
                default: format = GL_RGBA; type = GL_UNSIGNED_BYTE; return;   // RGBA8, SRGB8_ALPHA8
            }
        }

        GLint BoundVao() { GLint vao = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao); return vao; }
    }

    void SetTier(Tier tier) { g_tier = tier; }
    Tier GetTier() { return g_tier; }

    void Label(GLenum identifier, GLuint name, const std::string& label) {
        if (!Compat() && glObjectLabel && name != 0 && !label.empty()) glObjectLabel(identifier, name, GLsizei(label.size()), label.c_str());
    }

    // ====================================================================== state helpers
    GLuint CreateBufferObject(size_t size, const void* data, GLenum usageHint) {
        GLuint id = 0;
        if (!Compat()) {
            glCreateBuffers(1, &id);
            glNamedBufferStorage(id, GLsizeiptr(size), data, GL_DYNAMIC_STORAGE_BIT);
        }
        else {
            glGenBuffers(1, &id);
            BindScratch(id);
            glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(size), data, usageHint);
        }
        return id;
    }

    void BufferSubData(GLuint buffer, size_t offset, size_t size, const void* data) {
        if (size == 0) return;
        if (!Compat()) glNamedBufferSubData(buffer, GLintptr(offset), GLsizeiptr(size), data);
        else { BindScratch(buffer); glBufferSubData(GL_COPY_WRITE_BUFFER, GLintptr(offset), GLsizeiptr(size), data); }
    }

    void GetBufferSubData(GLuint buffer, size_t offset, size_t size, void* data) {
        if (size == 0) return;
        if (!Compat()) glGetNamedBufferSubData(buffer, GLintptr(offset), GLsizeiptr(size), data);
        else { BindScratch(buffer); glGetBufferSubData(GL_COPY_WRITE_BUFFER, GLintptr(offset), GLsizeiptr(size), data); }
    }

    void ClearBufferZero(GLuint buffer, size_t size) {
        if (!Compat()) {
            const uint32_t zero = 0;
            glClearNamedBufferData(buffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            return;
        }
        const std::vector<uint8_t> zeros(size, 0);
        BufferSubData(buffer, 0, size, zeros.data());
    }

    void BindTexture(GLuint unit, GLenum target, GLuint texture) {
        if (!Compat()) { glBindTextureUnit(unit, texture); return; }
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(target, texture);
        glActiveTexture(GL_TEXTURE0);
    }

    void ClearColor(GLuint fbo, int drawBuffer, const float* rgba) {
        if (!Compat()) { glClearNamedFramebufferfv(fbo, GL_COLOR, drawBuffer, rgba); return; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glClearBufferfv(GL_COLOR, drawBuffer, rgba);
    }
    void ClearColorUint(GLuint fbo, int drawBuffer, const GLuint* value) {
        if (!Compat()) { glClearNamedFramebufferuiv(fbo, GL_COLOR, drawBuffer, value); return; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glClearBufferuiv(GL_COLOR, drawBuffer, value);
    }
    void ClearDepth(GLuint fbo, float depth) {
        if (!Compat()) { glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &depth); return; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glClearBufferfv(GL_DEPTH, 0, &depth);
    }
    void ReadBuffer(GLuint fbo, GLenum attachment) {
        if (!Compat()) { glNamedFramebufferReadBuffer(fbo, attachment); return; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadBuffer(attachment);
    }
    void Blit(GLuint src, GLuint dst, int sw, int sh, int dw, int dh, GLbitfield mask, GLenum filter) {
        if (!Compat()) { glBlitNamedFramebuffer(src, dst, 0, 0, sw, sh, 0, 0, dw, dh, mask, filter); return; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
        glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, mask, filter);
    }

    GLuint CreateVertexArray() {
        GLuint vao = 0;
        if (!Compat()) glCreateVertexArrays(1, &vao);
        else glGenVertexArrays(1, &vao);
        return vao;
    }

    void SetVertexBuffer(GLuint vao, GLuint buffer, GLsizei stride, std::initializer_list<VertexAttrib> attribs) {
        if (!Compat()) {
            for (const VertexAttrib& a : attribs) {
                glEnableVertexArrayAttrib(vao, a.index);
                glVertexArrayAttribFormat(vao, a.index, a.size, a.type, GL_FALSE, GLuint(a.offset));
                glVertexArrayAttribBinding(vao, a.index, 0);
            }
            glVertexArrayVertexBuffer(vao, 0, buffer, 0, stride);
            return;
        }
        const GLint previous = BoundVao();
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        for (const VertexAttrib& a : attribs) {
            glEnableVertexAttribArray(a.index);
            glVertexAttribPointer(a.index, a.size, a.type, GL_FALSE, stride, reinterpret_cast<const void*>(a.offset));
        }
        glBindVertexArray(GLuint(previous));
    }

    void SetElementBuffer(GLuint vao, GLuint buffer) {
        if (!Compat()) { glVertexArrayElementBuffer(vao, buffer); return; }
        const GLint previous = BoundVao();
        glBindVertexArray(vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        glBindVertexArray(GLuint(previous));
    }

    // ====================================================================== Buffer
    void Buffer::Create(const std::string& label, size_t size, const void* data) {
        Release();
        m_label = label;
        m_size = std::max<size_t>(size, 16);
        m_id = CreateBufferObject(m_size, nullptr);
        if (data && size) BufferSubData(m_id, 0, size, data);
        Label(GL_BUFFER, m_id, label);
    }

    void Buffer::EnsureSize(size_t size) {
        if (m_id && size <= m_size) return;
        Create(m_label, std::max(size, m_size + m_size / 2));
    }

    void Buffer::Upload(const void* data, size_t size, size_t offset) {
        if (size == 0) return;
        if (offset == 0) EnsureSize(size);
        BufferSubData(m_id, offset, size, data);
    }

    void Buffer::Release() {
        if (m_id) glDeleteBuffers(1, &m_id);
        m_id = 0;
        m_size = 0;
    }

    // ====================================================================== Texture
    void Texture::Create(const std::string& label, const TextureDesc& desc) {
        Release();
        m_desc = desc;
        const GLenum t = desc.target;
        if (!Compat()) {
            glCreateTextures(t, 1, &m_id);
            switch (t) {
                case GL_TEXTURE_2D: glTextureStorage2D(m_id, desc.levels, desc.format, desc.width, desc.height); break;
                case GL_TEXTURE_2D_MULTISAMPLE: glTextureStorage2DMultisample(m_id, desc.samples, desc.format, desc.width, desc.height, GL_TRUE); break;
                case GL_TEXTURE_2D_ARRAY:
                case GL_TEXTURE_CUBE_MAP_ARRAY: glTextureStorage3D(m_id, desc.levels, desc.format, desc.width, desc.height, desc.layers); break;
                default: Log::Error() << "GL::Texture '" << label << "': unsupported target"; break;
            }
            if (t != GL_TEXTURE_2D_MULTISAMPLE) {
                glTextureParameteri(m_id, GL_TEXTURE_MIN_FILTER, GLint(desc.minFilter));
                glTextureParameteri(m_id, GL_TEXTURE_MAG_FILTER, GLint(desc.magFilter));
                glTextureParameteri(m_id, GL_TEXTURE_WRAP_S, GLint(desc.wrap));
                glTextureParameteri(m_id, GL_TEXTURE_WRAP_T, GLint(desc.wrap));
                glTextureParameteri(m_id, GL_TEXTURE_WRAP_R, GLint(desc.wrap));
                if (desc.depthCompare) {
                    glTextureParameteri(m_id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                    glTextureParameteri(m_id, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
                }
            }
            Label(GL_TEXTURE, m_id, label);
            return;
        }
        glGenTextures(1, &m_id);
        glBindTexture(t, m_id);
        GLenum format, type;
        ExternalFormat(desc.format, format, type);
        switch (t) {
            case GL_TEXTURE_2D:
                for (int level = 0; level < desc.levels; ++level)
                    glTexImage2D(t, level, GLint(desc.format), std::max(desc.width >> level, 1), std::max(desc.height >> level, 1), 0, format, type, nullptr);
                break;
            case GL_TEXTURE_2D_MULTISAMPLE:
                glTexImage2DMultisample(t, desc.samples, desc.format, desc.width, desc.height, GL_TRUE);
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                for (int level = 0; level < desc.levels; ++level)
                    glTexImage3D(t, level, GLint(desc.format), std::max(desc.width >> level, 1), std::max(desc.height >> level, 1), desc.layers, 0, format, type, nullptr);
                break;
            default: Log::Error() << "GL::Texture '" << label << "': unsupported target"; break;
        }
        if (t != GL_TEXTURE_2D_MULTISAMPLE) {
            glTexParameteri(t, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(t, GL_TEXTURE_MAX_LEVEL, desc.levels - 1);
            glTexParameteri(t, GL_TEXTURE_MIN_FILTER, GLint(desc.minFilter));
            glTexParameteri(t, GL_TEXTURE_MAG_FILTER, GLint(desc.magFilter));
            glTexParameteri(t, GL_TEXTURE_WRAP_S, GLint(desc.wrap));
            glTexParameteri(t, GL_TEXTURE_WRAP_T, GLint(desc.wrap));
            glTexParameteri(t, GL_TEXTURE_WRAP_R, GLint(desc.wrap));
            if (desc.depthCompare) {
                glTexParameteri(t, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                glTexParameteri(t, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            }
        }
        glBindTexture(t, 0);
    }

    void Texture::Upload2D(GLenum format, GLenum type, const void* pixels) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!Compat()) glTextureSubImage2D(m_id, 0, 0, 0, m_desc.width, m_desc.height, format, type, pixels);
        else {
            glBindTexture(GL_TEXTURE_2D, m_id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_desc.width, m_desc.height, format, type, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void Texture::UploadLayer(int layer, GLenum format, GLenum type, const void* pixels) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!Compat()) glTextureSubImage3D(m_id, 0, 0, 0, layer, m_desc.width, m_desc.height, 1, format, type, pixels);
        else {
            glBindTexture(m_desc.target, m_id);
            glTexSubImage3D(m_desc.target, 0, 0, 0, layer, m_desc.width, m_desc.height, 1, format, type, pixels);
            glBindTexture(m_desc.target, 0);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void Texture::GenerateMipmaps() {
        if (!Compat()) { glGenerateTextureMipmap(m_id); return; }
        glBindTexture(m_desc.target, m_id);
        glGenerateMipmap(m_desc.target);
        glBindTexture(m_desc.target, 0);
    }

    void Texture::SetParameter(GLenum name, float value) {
        if (!Compat()) { glTextureParameterf(m_id, name, value); return; }
        glBindTexture(m_desc.target, m_id);
        glTexParameterf(m_desc.target, name, value);
        glBindTexture(m_desc.target, 0);
    }

    void Texture::Read(GLenum format, GLenum type, size_t bytes, void* out) const {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        if (!Compat()) { glGetTextureImage(m_id, 0, format, type, GLsizei(bytes), out); return; }
        glBindTexture(m_desc.target, m_id);
        glGetTexImage(m_desc.target, 0, format, type, out);
        glBindTexture(m_desc.target, 0);
    }

    void Texture::Release() {
        if (m_id) glDeleteTextures(1, &m_id);
        m_id = 0;
    }

    void TextureBuffer::Bind(GLuint unit, GLenum format, GLuint buffer) {
        if (!m_id) glGenTextures(1, &m_id);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_BUFFER, m_id);
        glTexBuffer(GL_TEXTURE_BUFFER, format, buffer);
        glActiveTexture(GL_TEXTURE0);
    }
    void TextureBuffer::Release() {
        if (m_id) glDeleteTextures(1, &m_id);
        m_id = 0;
    }

    // ====================================================================== Framebuffer
    void Framebuffer::Create(const std::string& label) {
        Release();
        if (!Compat()) glCreateFramebuffers(1, &m_id);
        else {
            glGenFramebuffers(1, &m_id);
            glBindFramebuffer(GL_FRAMEBUFFER, m_id);   // creates the object
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        Label(GL_FRAMEBUFFER, m_id, label);
    }

    void Framebuffer::AttachColor(int index, const Texture& texture, int layer) {
        if (!Compat()) {
            if (layer >= 0) glNamedFramebufferTextureLayer(m_id, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0, layer);
            else glNamedFramebufferTexture(m_id, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_id);
        if (layer >= 0) glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0, layer);
        else glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0);
    }

    void Framebuffer::AttachDepth(const Texture& texture, int layer) {
        if (!Compat()) {
            if (layer >= 0) glNamedFramebufferTextureLayer(m_id, GL_DEPTH_ATTACHMENT, texture.Id(), 0, layer);
            else glNamedFramebufferTexture(m_id, GL_DEPTH_ATTACHMENT, texture.Id(), 0);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_id);
        if (layer >= 0) glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture.Id(), 0, layer);
        else glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture.Id(), 0);
    }

    void Framebuffer::SetDrawBuffers(int colorCount) {
        GLenum buffers[8];
        for (int i = 0; i < colorCount && i < 8; ++i) buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        if (!Compat()) {
            if (colorCount == 0) {
                glNamedFramebufferDrawBuffer(m_id, GL_NONE);
                glNamedFramebufferReadBuffer(m_id, GL_NONE);
                return;
            }
            glNamedFramebufferDrawBuffers(m_id, std::min(colorCount, 8), buffers);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_id);
        if (colorCount == 0) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); return; }
        glDrawBuffers(std::min(colorCount, 8), buffers);
    }

    bool Framebuffer::Check(const char* what) const {
        GLenum status;
        if (!Compat()) status = glCheckNamedFramebufferStatus(m_id, GL_FRAMEBUFFER);
        else {
            glBindFramebuffer(GL_FRAMEBUFFER, m_id);
            status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            Log::Error() << "Framebuffer '" << what << "' incomplete (0x" << std::hex << status << std::dec << ")";
            return false;
        }
        return true;
    }

    void Framebuffer::Release() {
        if (m_id) glDeleteFramebuffers(1, &m_id);
        m_id = 0;
    }

    // ====================================================================== Program
    void Program::Release() {
        if (m_id) glDeleteProgram(m_id);
        m_id = 0;
    }

    namespace {
        const char* StageName(GLenum stage) {
            switch (stage) {
                case GL_VERTEX_SHADER:   return "vertex";
                case GL_FRAGMENT_SHADER: return "fragment";
                case GL_GEOMETRY_SHADER: return "geometry";
                case GL_COMPUTE_SHADER:  return "compute";
            }
            return "?";
        }

        std::string InfoLog(GLuint object, bool program) {
            GLint length = 0;
            if (program) glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
            else glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
            std::string log(size_t(std::max(length, 1)), '\0');
            if (program) glGetProgramInfoLog(object, length, nullptr, log.data());
            else glGetShaderInfoLog(object, length, nullptr, log.data());
            while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
            return log;
        }
    }

    GLuint BuildProgram(const std::string& name, const std::vector<StageSource>& stages, bool retrievable) {
        const GLuint program = glCreateProgram();
        std::vector<GLuint> shaders;
        bool ok = true;
        for (const StageSource& stage : stages) {
            const GLuint shader = glCreateShader(stage.stage);
            const char* text = stage.source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (!compiled) {
                Log::Error() << "Shader '" << name << "' (" << StageName(stage.stage) << ") failed to compile:\n" << InfoLog(shader, false);
                ok = false;
            }
            glAttachShader(program, shader);
            shaders.push_back(shader);
        }
        if (ok) {
            if (retrievable) glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) {
                Log::Error() << "Program '" << name << "' failed to link:\n" << InfoLog(program, true);
                ok = false;
            }
        }
        for (GLuint shader : shaders) {
            glDetachShader(program, shader);
            glDeleteShader(shader);
        }
        if (!ok) {
            glDeleteProgram(program);
            return 0;
        }
        Label(GL_PROGRAM, program, name);
        return program;
    }

    void ApplyBindings(GLuint program, const ShaderPreprocessor::ResourceBindings& bindings) {
        if (!program) return;
        GLint previous = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
        glUseProgram(program);
        for (const auto& [name, unit] : bindings.samplers) {
            const GLint location = glGetUniformLocation(program, name.c_str());
            if (location >= 0) glUniform1i(location, unit);
        }
        for (const auto& [name, binding] : bindings.blocks) {
            const GLuint index = glGetUniformBlockIndex(program, name.c_str());
            if (index != GL_INVALID_INDEX) glUniformBlockBinding(program, index, GLuint(binding));
        }
        glUseProgram(GLuint(previous));
    }
}
