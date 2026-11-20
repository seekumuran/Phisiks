#include "render/ShaderPreprocessor.h"

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
}
