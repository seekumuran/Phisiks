#pragma once
// Persistent on-disk program-binary cache (glGetProgramBinary / glProgramBinary).
//
// Warm start: every program is created from its cached driver binary; no GLSL is compiled.
// Cold start / driver or source change: programs are compiled one at a time, in a fixed order,
// during Init (never mid-game), and their binaries are stored atomically.
// Crash safety: before compiling, the program name is written to a journal file which is removed
// once it linked. If a run dies inside the driver compiler, the next run finds the journal, logs
// the program that was being compiled, and rebuilds that one program in safe mode
// (`#pragma optimize(off)`, PHISIKS_SAFE_MODE defined).

#include "Phisiks/Render/Renderer.h"
#include "render/gl/GLObjects.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Phisiks::Render {
    class ShaderCache {
    public:
        // Directory: $PHISIKS_SHADER_CACHE_DIR, else %LOCALAPPDATA%/Phisiks/ShaderCache,
        // else ./.phisiks_cache/shaders. Call with a current GL context.
        void Init();
        // Returns 0 on failure (logged). Stages must be fully preprocessed.
        GLuint GetProgram(const std::string& name, std::vector<GL::StageSource> stages);
        ShaderCacheStats Stats() const { return m_stats; }

    private:
        void BeginJournal(const std::string& name);
        void EndJournal();

        std::filesystem::path m_directory;
        std::string m_driverIdentity;
        std::string m_safeModeProgram;   // program a previous run crashed while compiling
        bool m_binarySupported = false;
        ShaderCacheStats m_stats;
    };
}
