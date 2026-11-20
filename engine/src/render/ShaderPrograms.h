#pragma once
// The renderer's programs and the embedded GLSL files they are built from (no GL dependency).

#include <string>
#include <vector>

namespace Phisiks::Render::Shaders {
    // Embedded engine/shaders/*.glsl by file name (nullptr if unknown).
    const std::string* Find(const std::string& fileName);
    std::vector<std::string> FileNames();

    enum class Stage { Vertex, Fragment };
    struct StageDef { Stage stage; const char* file; };
    struct ProgramDef {
        const char* name;
        std::vector<StageDef> stages;
        std::vector<std::string> defines;
    };

    enum ProgramId { FORWARD, SHADOW, SHADOW_POINT, SKY, TONEMAP, LINES, TEXT, PROGRAM_COUNT };
    const std::vector<ProgramDef>& Programs();   // indexed by ProgramId
}
