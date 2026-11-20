#include "render/gl/GLObjects.h"
#include "Phisiks/Core/Log.h"

#include <algorithm>

namespace Phisiks::Render::GL {

    void Label(GLenum identifier, GLuint name, const std::string& label) {
        if (glObjectLabel && name != 0 && !label.empty()) glObjectLabel(identifier, name, GLsizei(label.size()), label.c_str());
    }

    // ====================================================================== Buffer
    void Buffer::Create(const std::string& label, size_t size, const void* data) {
        Release();
        m_label = label;
        m_size = std::max<size_t>(size, 16);
        glCreateBuffers(1, &m_id);
        glNamedBufferStorage(m_id, GLsizeiptr(m_size), nullptr, GL_DYNAMIC_STORAGE_BIT);
        if (data && size) glNamedBufferSubData(m_id, 0, GLsizeiptr(size), data);
        Label(GL_BUFFER, m_id, label);
    }

    void Buffer::EnsureSize(size_t size) {
        if (m_id && size <= m_size) return;
        Create(m_label, std::max(size, m_size + m_size / 2));
    }

    void Buffer::Upload(const void* data, size_t size, size_t offset) {
        if (size == 0) return;
        if (offset == 0) EnsureSize(size);
        glNamedBufferSubData(m_id, GLintptr(offset), GLsizeiptr(size), data);
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
        glCreateTextures(desc.target, 1, &m_id);
        switch (desc.target) {
            case GL_TEXTURE_2D:
                glTextureStorage2D(m_id, desc.levels, desc.format, desc.width, desc.height);
                break;
            case GL_TEXTURE_2D_MULTISAMPLE:
                glTextureStorage2DMultisample(m_id, desc.samples, desc.format, desc.width, desc.height, GL_TRUE);
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                glTextureStorage3D(m_id, desc.levels, desc.format, desc.width, desc.height, desc.layers);
                break;
            default:
                Log::Error() << "GL::Texture '" << label << "': unsupported target";
                break;
        }
        if (desc.target != GL_TEXTURE_2D_MULTISAMPLE) {
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
    }

    void Texture::Upload2D(GLenum format, GLenum type, const void* pixels) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(m_id, 0, 0, 0, m_desc.width, m_desc.height, format, type, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void Texture::Release() {
        if (m_id) glDeleteTextures(1, &m_id);
        m_id = 0;
    }

    // ====================================================================== Framebuffer
    void Framebuffer::Create(const std::string& label) {
        Release();
        glCreateFramebuffers(1, &m_id);
        Label(GL_FRAMEBUFFER, m_id, label);
    }

    void Framebuffer::AttachColor(int index, const Texture& texture, int layer) {
        if (layer >= 0) glNamedFramebufferTextureLayer(m_id, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0, layer);
        else glNamedFramebufferTexture(m_id, GL_COLOR_ATTACHMENT0 + index, texture.Id(), 0);
    }

    void Framebuffer::AttachDepth(const Texture& texture, int layer) {
        if (layer >= 0) glNamedFramebufferTextureLayer(m_id, GL_DEPTH_ATTACHMENT, texture.Id(), 0, layer);
        else glNamedFramebufferTexture(m_id, GL_DEPTH_ATTACHMENT, texture.Id(), 0);
    }

    void Framebuffer::SetDrawBuffers(int colorCount) {
        if (colorCount == 0) {
            glNamedFramebufferDrawBuffer(m_id, GL_NONE);
            glNamedFramebufferReadBuffer(m_id, GL_NONE);
            return;
        }
        GLenum buffers[8];
        for (int i = 0; i < colorCount && i < 8; ++i) buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        glNamedFramebufferDrawBuffers(m_id, std::min(colorCount, 8), buffers);
    }

    bool Framebuffer::Check(const char* what) const {
        const GLenum status = glCheckNamedFramebufferStatus(m_id, GL_FRAMEBUFFER);
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
}
