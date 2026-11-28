#pragma once
// Minimal GLSL preprocessing done on the CPU before compiling (no GL dependency):
//   - `#include "name"` lines are replaced by the named source (recursively, each file once)
//   - `#define` lines from `defines` are inserted right after the `#version` line
// The expanded text is what the shader cache key hashes.

#include <functional>
#include <string>
#include <vector>

namespace Phisiks::Render::ShaderPreprocessor {
    // Returns the source for an include name, or nullptr if unknown.
    using Resolver = std::function<const std::string*(const std::string& name)>;

    struct Result {
        bool ok = false;
        std::string source;
        std::string error;
    };

    Result Expand(const std::string& source, const Resolver& resolve, const std::vector<std::string>& defines = {});

    // OpenGL 4.1 tier: GLSL 4.10 has no layout(binding = N). RewriteForGL41 turns a 4.60 source into
    // 4.10 (#version, PHISIKS_GL41 define, bindings stripped and recorded); GL::ApplyBindings sets
    // the recorded sampler units and uniform-block bindings on the linked program.
    struct ResourceBindings {
        std::vector<std::pair<std::string, int>> samplers;
        std::vector<std::pair<std::string, int>> blocks;
    };
    std::string RewriteForGL41(const std::string& source, ResourceBindings& bindings);
}
