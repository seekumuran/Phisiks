#pragma once
// The renderer's programs and the embedded GLSL files they are built from (no GL dependency).

#include <string>
#include <vector>

namespace Phisiks::Render::Shaders {
    // Embedded engine/shaders/*.glsl by file name (nullptr if unknown). A hot-reload override
    // (SetOverride) takes precedence over the embedded copy.
    const std::string* Find(const std::string& fileName);
    std::vector<std::string> FileNames();
    void SetOverride(const std::string& fileName, const std::string& source);
    void ClearOverrides();
    size_t OverrideCount();

    enum class Stage { Vertex, Fragment, Compute };
    struct StageDef { Stage stage; const char* file; };
    struct ProgramDef {
        const char* name;
        std::vector<StageDef> stages;
        std::vector<std::string> defines;
    };

    enum ProgramId {
        // graphics
        FORWARD, SHADOW, SHADOW_POINT, SKY, TONEMAP, LINES, TEXT,
        SPRITES, SOFTBODY, SOFTBODY_SHADOW, SDF, ARROWS,
        EASU, RCAS, GBUFFER, GBUFFER_SOFT,
        RIBBON, DECAL, SSAO, SSAO_BLUR, SSAO_APPLY, BLOOM_PREFILTER, BLOOM_DOWN, BLOOM_UP,
        // compute
        CULL, HIZ_INIT, HIZ_DOWNSAMPLE,
        HASH_COUNT, HASH_SCATTER, HASH_QUERY, SCAN_LOCAL, SCAN_BLOCKS, SCAN_ADD,
        NBODY, PARTICLES_STEP,
        XPBD_PREDICT, XPBD_SOLVE, XPBD_FINISH, XPBD_FUSED, XPBD_NORMALS,
        FIELD_SAMPLE,
        PROGRAM_COUNT
    };
    const std::vector<ProgramDef>& Programs();   // indexed by ProgramId
    inline bool IsCompute(int id) { return id >= CULL && id < PROGRAM_COUNT; }   // not built on the 4.1 tier
}
