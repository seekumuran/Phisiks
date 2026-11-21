#include "render/ShaderPrograms.h"

#include <unordered_map>

namespace Phisiks::Render::Shaders {
    namespace {
        struct Embedded { const char* name; const char* source; };
        const Embedded EMBEDDED[] = {
#include "phisiks/EmbeddedShaders.inc"
        };

        const std::unordered_map<std::string, std::string>& Table() {
            static const std::unordered_map<std::string, std::string> table = [] {
                std::unordered_map<std::string, std::string> t;
                for (const Embedded& e : EMBEDDED) t.emplace(e.name, e.source);
                return t;
            }();
            return table;
        }
    }

    const std::string* Find(const std::string& fileName) {
        auto it = Table().find(fileName);
        return it == Table().end() ? nullptr : &it->second;
    }

    std::vector<std::string> FileNames() {
        std::vector<std::string> names;
        for (const Embedded& e : EMBEDDED) names.push_back(e.name);
        return names;
    }

    const std::vector<ProgramDef>& Programs() {
        using S = Stage;
        auto compute = [](const char* name, const char* file, std::vector<std::string> defines = {}) {
            return ProgramDef{ name, { { S::Compute, file } }, std::move(defines) };
        };
        static const std::vector<ProgramDef> programs = {
            { "forward",         { { S::Vertex, "mesh.vert.glsl" }, { S::Fragment, "forward.frag.glsl" } }, {} },
            { "shadow",          { { S::Vertex, "mesh.vert.glsl" }, { S::Fragment, "shadow.frag.glsl" } }, { "SHADOW_PASS" } },
            { "shadow_point",    { { S::Vertex, "mesh.vert.glsl" }, { S::Fragment, "shadow.frag.glsl" } }, { "SHADOW_PASS", "POINT_SHADOW" } },
            { "sky",             { { S::Vertex, "fullscreen.vert.glsl" }, { S::Fragment, "sky.frag.glsl" } }, { "SKY" } },
            { "tonemap",         { { S::Vertex, "fullscreen.vert.glsl" }, { S::Fragment, "tonemap.frag.glsl" } }, {} },
            { "lines",           { { S::Vertex, "lines.vert.glsl" }, { S::Fragment, "lines.frag.glsl" } }, {} },
            { "text",            { { S::Vertex, "text.vert.glsl" }, { S::Fragment, "text.frag.glsl" } }, {} },
            { "sprites",         { { S::Vertex, "sprites.vert.glsl" }, { S::Fragment, "sprites.frag.glsl" } }, {} },
            { "softbody",        { { S::Vertex, "softbody.vert.glsl" }, { S::Fragment, "forward.frag.glsl" } }, {} },
            { "softbody_shadow", { { S::Vertex, "softbody.vert.glsl" }, { S::Fragment, "shadow.frag.glsl" } }, { "SHADOW_PASS" } },
            { "sdf",             { { S::Vertex, "fullscreen.vert.glsl" }, { S::Fragment, "sdf.frag.glsl" } }, {} },
            { "arrows",          { { S::Vertex, "arrows.vert.glsl" }, { S::Fragment, "lines.frag.glsl" } }, {} },
            { "easu",            { { S::Vertex, "fullscreen.vert.glsl" }, { S::Fragment, "easu.frag.glsl" } }, {} },
            { "rcas",            { { S::Vertex, "fullscreen.vert.glsl" }, { S::Fragment, "rcas.frag.glsl" } }, {} },
            { "gbuffer",         { { S::Vertex, "mesh.vert.glsl" }, { S::Fragment, "gbuffer.frag.glsl" } }, {} },
            { "gbuffer_soft",    { { S::Vertex, "softbody.vert.glsl" }, { S::Fragment, "gbuffer.frag.glsl" } }, { "SOFT_SURFACE" } },
            compute("cull", "cull.comp.glsl"),
            compute("hiz_init", "hiz.comp.glsl", { "HIZ_INIT" }),
            compute("hiz_downsample", "hiz.comp.glsl", { "HIZ_DOWNSAMPLE" }),
            compute("hash_count", "hash.comp.glsl", { "HASH_COUNT" }),
            compute("hash_scatter", "hash.comp.glsl", { "HASH_SCATTER" }),
            compute("hash_query", "hash.comp.glsl", { "HASH_QUERY" }),
            compute("scan_local", "scan.comp.glsl", { "SCAN_LOCAL" }),
            compute("scan_blocks", "scan.comp.glsl", { "SCAN_BLOCKS" }),
            compute("scan_add", "scan.comp.glsl", { "SCAN_ADD" }),
            compute("nbody", "particles.comp.glsl", { "NBODY" }),
            compute("particles_step", "particles.comp.glsl", { "PARTICLES_STEP" }),
            compute("xpbd_predict", "xpbd.comp.glsl", { "XPBD_PREDICT" }),
            compute("xpbd_solve", "xpbd.comp.glsl", { "XPBD_SOLVE" }),
            compute("xpbd_finish", "xpbd.comp.glsl", { "XPBD_FINISH" }),
            compute("xpbd_fused", "xpbd.comp.glsl", { "XPBD_FUSED" }),
            compute("xpbd_normals", "xpbd.comp.glsl", { "XPBD_NORMALS" }),
            compute("field_sample", "field_sample.comp.glsl"),
        };
        return programs;
    }
}
