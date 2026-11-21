#include "render/ShaderPreprocessor.h"

#include <regex>

#include <set>
#include <sstream>

namespace Phisiks::Render::ShaderPreprocessor {
    namespace {
        // Returns the include name if `line` is `#include "name"` (leading whitespace allowed).
        bool ParseInclude(const std::string& line, std::string& name) {
            size_t i = line.find_first_not_of(" \t");
            if (i == std::string::npos || line.compare(i, 8, "#include") != 0) return false;
            const size_t open = line.find('"', i + 8);
            if (open == std::string::npos) return false;
            const size_t close = line.find('"', open + 1);
            if (close == std::string::npos) return false;
            name = line.substr(open + 1, close - open - 1);
            return true;
        }

        bool ExpandInto(const std::string& source, const Resolver& resolve, std::set<std::string>& included,
                        std::vector<std::string>& stack, std::string& out, std::string& error) {
            std::istringstream in(source);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string name;
                if (!ParseInclude(line, name)) {
                    out += line;
                    out += '\n';
                    continue;
                }
                for (const std::string& open : stack) {
                    if (open == name) { error = "recursive #include \"" + name + "\""; return false; }
                }
                if (included.count(name)) continue;   // include-once
                const std::string* text = resolve ? resolve(name) : nullptr;
                if (!text) { error = "unknown #include \"" + name + "\""; return false; }
                included.insert(name);
                stack.push_back(name);
                if (!ExpandInto(*text, resolve, included, stack, out, error)) return false;
                stack.pop_back();
            }
            return true;
        }
    }

    Result Expand(const std::string& source, const Resolver& resolve, const std::vector<std::string>& defines) {
        Result result;
        std::string body;
        std::set<std::string> included;
        std::vector<std::string> stack;
        if (!ExpandInto(source, resolve, included, stack, body, result.error)) return result;

        // Defines go right after #version (which must stay the first directive).
        std::string defineBlock;
        for (const std::string& define : defines) defineBlock += "#define " + define + "\n";
        const size_t version = body.find("#version");
        if (version == std::string::npos) {
            result.source = defineBlock + body;
        }
        else {
            const size_t lineEnd = body.find('\n', version);
            const size_t insertAt = lineEnd == std::string::npos ? body.size() : lineEnd + 1;
            result.source = body.substr(0, insertAt) + defineBlock + body.substr(insertAt);
        }
        result.ok = true;
        return result;
    }
    std::string RewriteForGL41(const std::string& source, ResourceBindings& bindings) {
        std::string s = source;
        const size_t version = s.find("#version 460 core");
        if (version != std::string::npos) s.replace(version, 17, "#version 410 core\n#define PHISIKS_GL41 1");
        static const std::regex sampler(R"(layout\s*\(\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+(\w+)\s+(\w+)\s*;)");
        static const std::regex block(R"(layout\s*\(\s*(std140)\s*,\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+(\w+))");
        std::string out;
        std::smatch m;
        std::string rest = s;
        while (std::regex_search(rest, m, sampler)) {
            bindings.samplers.emplace_back(m[3].str(), std::stoi(m[1].str()));
            out += m.prefix().str() + "uniform " + m[2].str() + " " + m[3].str() + ";";
            rest = m.suffix().str();
        }
        out += rest;
        rest = out;
        out.clear();
        while (std::regex_search(rest, m, block)) {
            bindings.blocks.emplace_back(m[3].str(), std::stoi(m[2].str()));
            out += m.prefix().str() + "layout(" + m[1].str() + ") uniform " + m[3].str();
            rest = m.suffix().str();
        }
        out += rest;
        return out;
    }

}
