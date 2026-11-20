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
        static const std::vector<ProgramDef> programs = {
            { "forward",      { { Stage::Vertex, "mesh.vert.glsl" }, { Stage::Fragment, "forward.frag.glsl" } }, {} },
            { "shadow",       { { Stage::Vertex, "mesh.vert.glsl" }, { Stage::Fragment, "shadow.frag.glsl" } }, { "SHADOW_PASS" } },
            { "shadow_point", { { Stage::Vertex, "mesh.vert.glsl" }, { Stage::Fragment, "shadow.frag.glsl" } }, { "SHADOW_PASS", "POINT_SHADOW" } },
            { "sky",          { { Stage::Vertex, "fullscreen.vert.glsl" }, { Stage::Fragment, "sky.frag.glsl" } }, { "SKY" } },
            { "tonemap",      { { Stage::Vertex, "fullscreen.vert.glsl" }, { Stage::Fragment, "tonemap.frag.glsl" } }, {} },
            { "lines",        { { Stage::Vertex, "lines.vert.glsl" }, { Stage::Fragment, "lines.frag.glsl" } }, {} },
            { "text",         { { Stage::Vertex, "text.vert.glsl" }, { Stage::Fragment, "text.frag.glsl" } }, {} },
        };
        return programs;
    }
}
