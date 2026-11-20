#include "render/ShaderCache.h"
#include "render/ShaderCacheFormat.h"
#include "Phisiks/Core/Log.h"

#include <cstdlib>
#include <system_error>

namespace Phisiks::Render {
    namespace fs = std::filesystem;
    namespace Format = ShaderCacheFormat;

    namespace {
        std::string GLString(GLenum name) {
            const GLubyte* s = glGetString(name);
            return s ? reinterpret_cast<const char*>(s) : "";
        }

        fs::path DefaultDirectory() {
            if (const char* custom = std::getenv("PHISIKS_SHADER_CACHE_DIR"); custom && *custom) return fs::path(custom);
#ifdef _WIN32
            if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) return fs::path(local) / "Phisiks" / "ShaderCache";
#endif
            return fs::path(".phisiks_cache") / "shaders";
        }

        fs::path JournalPath(const fs::path& directory) { return directory / "compile.journal"; }

        // Inserts lines right after the #version line.
        std::string InsertAfterVersion(const std::string& source, const std::string& lines) {
            const size_t version = source.find("#version");
            if (version == std::string::npos) return lines + source;
            const size_t end = source.find('\n', version);
            const size_t at = end == std::string::npos ? source.size() : end + 1;
            return source.substr(0, at) + lines + source.substr(at);
        }
    }

    void ShaderCache::Init() {
        m_stats = {};
        m_directory = DefaultDirectory();
        std::error_code ec;
        fs::create_directories(m_directory, ec);
        m_stats.directory = m_directory.string();
        m_driverIdentity = GLString(GL_VENDOR) + "|" + GLString(GL_RENDERER) + "|" + GLString(GL_VERSION) + "|" + GLString(GL_SHADING_LANGUAGE_VERSION);

        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        m_binarySupported = formats > 0;
        if (!m_binarySupported) Log::Warning() << "Shader cache: driver exposes no program binary formats; compiling every run";

        std::vector<uint8_t> journal;
        if (Format::ReadFile(JournalPath(m_directory), journal) && !journal.empty()) {
            m_safeModeProgram.assign(journal.begin(), journal.end());
            m_stats.recoveredFromCrash = true;
            Log::Warning() << "Shader cache: the previous run stopped while compiling program '" << m_safeModeProgram
                           << "' (driver crash?). Rebuilding it in safe mode (optimizations off).";
            fs::remove(JournalPath(m_directory), ec);
        }
        Log::Info() << "Shader cache: " << m_stats.directory << (m_binarySupported ? "" : " (binaries unsupported)");
    }

    void ShaderCache::BeginJournal(const std::string& name) {
        Format::WriteFileAtomic(JournalPath(m_directory), std::vector<uint8_t>(name.begin(), name.end()));
    }

    void ShaderCache::EndJournal() {
        std::error_code ec;
        fs::remove(JournalPath(m_directory), ec);
    }

    GLuint ShaderCache::GetProgram(const std::string& name, std::vector<GL::StageSource> stages) {
        ++m_stats.programs;
        if (name == m_safeModeProgram) {
            for (GL::StageSource& stage : stages) stage.source = InsertAfterVersion(stage.source, "#pragma optimize(off)\n#define PHISIKS_SAFE_MODE 1\n");
        }

        std::vector<std::string> sources;
        for (const GL::StageSource& stage : stages) sources.push_back(stage.source);
        const uint64_t key = Format::ComputeProgramKey(sources, m_driverIdentity);
        const fs::path file = m_directory / Format::KeyToFileName(key);

        // 1. Cached binary
        if (m_binarySupported) {
            std::vector<uint8_t> bytes;
            if (Format::ReadFile(file, bytes)) {
                Format::ProgramBlob blob;
                const Format::ParseResult result = Format::Parse(bytes.data(), bytes.size(), key, blob);
                if (result == Format::ParseResult::Ok) {
                    const GLuint program = glCreateProgram();
                    glProgramBinary(program, blob.binaryFormat, blob.binary.data(), GLsizei(blob.binary.size()));
                    GLint linked = GL_FALSE;
                    glGetProgramiv(program, GL_LINK_STATUS, &linked);
                    if (linked) {
                        ++m_stats.loadedFromCache;
                        GL::Label(GL_PROGRAM, program, name);
                        return program;
                    }
                    glDeleteProgram(program);
                    Log::Warning() << "Shader cache: driver rejected the cached binary of '" << name << "'; recompiling";
                }
                else {
                    Log::Warning() << "Shader cache: '" << name << "' cache file invalid (" << Format::ToString(result) << "); recompiling";
                }
                ++m_stats.rejected;
            }
        }

        // 2. Compile from source (journaled)
        Log::Info() << "Shader cache: compiling '" << name << "'";
        BeginJournal(name);
        const GLuint program = GL::BuildProgram(name, stages, m_binarySupported);
        EndJournal();
        if (!program) {
            ++m_stats.failures;
            return 0;
        }
        ++m_stats.compiled;

        // 3. Store the binary
        if (m_binarySupported) {
            GLint length = 0;
            glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
            if (length > 0) {
                Format::ProgramBlob blob;
                blob.key = key;
                blob.binary.resize(size_t(length));
                GLenum format = 0;
                glGetProgramBinary(program, length, nullptr, &format, blob.binary.data());
                blob.binaryFormat = format;
                if (Format::WriteFileAtomic(file, Format::Serialize(blob))) ++m_stats.written;
                else Log::Warning() << "Shader cache: could not write " << file.string();
            }
        }
        return program;
    }
}
