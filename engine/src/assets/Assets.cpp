#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "platform/Paths.h"

#include <stb_image.h>
#include <stb_image_write.h>   // implementations compiled in assets/StbImplementation.cpp

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <thread>
#include <unordered_map>


namespace Phisiks::Assets {
    namespace fs = std::filesystem;

    namespace {
        struct Job {
            Ticket ticket;
            std::string path;
        };
        struct Result {
            Status status = Status::Pending;
            Image image;
        };

        struct State {
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<Job> queue;
            std::unordered_map<Ticket, Result> results;
            std::thread worker;
            bool stop = false;
            Ticket nextTicket = 1;
            std::vector<fs::path> extraRoots;
            Stats stats;
        };
        State& S() {
            static State state;
            return state;
        }

        fs::path ExecutableDirectory() { return Platform::Paths::ExecutableDirectory(); }

        bool DecodeImage(const std::vector<uint8_t>& bytes, Image& out) {
            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
            if (!pixels) return false;
            out.width = w;
            out.height = h;
            out.rgba.assign(pixels, pixels + size_t(w) * h * 4);
            stbi_image_free(pixels);
            return true;
        }

        void WorkerLoop() {
            State& s = S();
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(s.mutex);
                    s.wake.wait(lock, [&] { return s.stop || !s.queue.empty(); });
                    if (s.stop) return;
                    job = std::move(s.queue.front());
                    s.queue.pop_front();
                }
                Result result;
                std::vector<uint8_t> bytes;
                result.status = ReadBytes(job.path, bytes) && DecodeImage(bytes, result.image) ? Status::Ready : Status::Failed;
                std::lock_guard<std::mutex> lock(s.mutex);
                if (result.status == Status::Ready) ++s.stats.imagesDecoded;
                else ++s.stats.failures;
                s.results[job.ticket] = std::move(result);
            }
        }
    }

    void AddSearchPath(const std::string& directory) {
        std::lock_guard<std::mutex> lock(S().mutex);
        S().extraRoots.push_back(fs::path(directory));
    }

    void RemoveSearchPath(const std::string& directory) {
        std::lock_guard<std::mutex> lock(S().mutex);
        auto& roots = S().extraRoots;
        roots.erase(std::remove(roots.begin(), roots.end(), fs::path(directory)), roots.end());
    }

    namespace {
        std::mutex g_resolverMutex;
        ReferenceResolver g_resolver;
        std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; }
        std::string Extension(const std::string& path) {
            const size_t slash = path.find_last_of("/\\");
            const size_t dot = path.find_last_of('.');
            return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : Lower(path.substr(dot));
        }
    }

    void SetReferenceResolver(ReferenceResolver resolver) {
        std::lock_guard<std::mutex> lock(g_resolverMutex);
        g_resolver = std::move(resolver);
    }
    bool IsReference(const std::string& path) { return path.rfind("asset:", 0) == 0; }

    std::string Resolve(const std::string& path) {
        std::error_code ec;
        if (IsReference(path)) {
            ReferenceResolver resolver;
            { std::lock_guard<std::mutex> lock(g_resolverMutex); resolver = g_resolver; }
            const std::string resolved = resolver ? resolver(path) : std::string();
            return !resolved.empty() && fs::is_regular_file(resolved, ec) ? resolved : std::string();
        }
        const fs::path p(path);
        if (p.is_absolute()) return fs::is_regular_file(p, ec) ? p.string() : std::string();
        std::vector<fs::path> roots;
        {
            std::lock_guard<std::mutex> lock(S().mutex);
            roots = S().extraRoots;
        }
        // The executable's own folder first (an extracted release archive works from any working
        // directory), then the working directory (a development build run from the repository root).
        roots.push_back(ExecutableDirectory() / "assets");
        roots.push_back(fs::current_path(ec) / "assets");
        roots.push_back(fs::current_path(ec));
        for (const fs::path& root : roots) {
            const fs::path candidate = root / p;
            if (fs::is_regular_file(candidate, ec)) return candidate.string();
        }
        return {};
    }

    std::string ResolveDirectory(const std::string& path) {
        std::error_code ec;
        const fs::path p(path);
        if (p.is_absolute()) return fs::is_directory(p, ec) ? p.string() : std::string();
        for (const fs::path& root : { ExecutableDirectory(), fs::current_path(ec) }) {
            const fs::path candidate = root / p;
            if (fs::is_directory(candidate, ec)) return candidate.string();
        }
        return {};
    }

    bool ReadBytes(const std::string& path, std::vector<uint8_t>& out) {
        const std::string resolved = Resolve(path);
        if (resolved.empty()) return false;
        std::ifstream file(fs::path(resolved), std::ios::binary | std::ios::ate);
        if (!file) return false;
        const std::streamsize size = file.tellg();
        out.resize(size_t(std::max<std::streamsize>(size, 0)));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(out.data()), size)) return false;
        std::lock_guard<std::mutex> lock(S().mutex);
        ++S().stats.filesRead;
        S().stats.bytesRead += uint64_t(size);
        return true;
    }

    bool LoadImageFile(const std::string& path, Image& out) {
        std::vector<uint8_t> bytes;
        return ReadBytes(path, bytes) && DecodeImage(bytes, out);
    }

    bool WritePng(const std::string& path, const Image& image) {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        return stbi_write_png(path.c_str(), image.width, image.height, 4, image.rgba.data(), image.width * 4) != 0;
    }

    void ResizeImage(const Image& in, int width, int height, Image& out) {
        out.width = width;
        out.height = height;
        out.rgba.assign(size_t(width) * height * 4, 0);
        if (in.width <= 0 || in.height <= 0) return;
        for (int y = 0; y < height; ++y) {
            const float sy = std::clamp((y + 0.5f) * in.height / height - 0.5f, 0.0f, float(in.height - 1));
            const int y0 = int(sy), y1 = std::min(y0 + 1, in.height - 1);
            const float ty = sy - y0;
            for (int x = 0; x < width; ++x) {
                const float sx = std::clamp((x + 0.5f) * in.width / width - 0.5f, 0.0f, float(in.width - 1));
                const int x0 = int(sx), x1 = std::min(x0 + 1, in.width - 1);
                const float tx = sx - x0;
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int xx, int yy) { return float(in.rgba[(size_t(yy) * in.width + xx) * 4 + c]); };
                    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                    out.rgba[(size_t(y) * width + x) * 4 + c] = uint8_t(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
                }
            }
        }
    }

    Ticket RequestImage(const std::string& path) {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.worker.joinable()) {
            s.stop = false;
            s.worker = std::thread(WorkerLoop);
        }
        const Ticket ticket = s.nextTicket++;
        s.results[ticket] = Result{};
        s.queue.push_back({ ticket, path });
        ++s.stats.imagesRequested;
        s.wake.notify_one();
        return ticket;
    }

    Status PollImage(Ticket ticket, Image& out) {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.results.find(ticket);
        if (it == s.results.end()) return Status::Failed;
        if (it->second.status == Status::Pending) return Status::Pending;
        const Status status = it->second.status;
        out = std::move(it->second.image);
        s.results.erase(it);
        return status;
    }

    // ---------------------------------------------------------------------------------- OBJ
    bool ParseObj(const std::string& text, Render::MeshData& out, std::string* error) {
        out = {};
        std::vector<glm::vec3> positions, normals;
        std::vector<glm::vec2> uvs;
        std::map<std::tuple<int, int, int>, uint32_t> vertexOf;
        bool missingNormals = false;
        std::istringstream in(text);
        std::string line;
        int lineNumber = 0;
        auto fail = [&](const std::string& message) {
            if (error) *error = "line " + std::to_string(lineNumber) + ": " + message;
            out = {};
            return false;
        };
        auto fixIndex = [](int index, size_t count) { return index < 0 ? int(count) + index : index - 1; };   // OBJ is 1-based; negative = relative

        while (std::getline(in, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(line);
            std::string tag;
            if (!(ls >> tag) || tag[0] == '#') continue;
            if (tag == "v") { glm::vec3 v; if (!(ls >> v.x >> v.y >> v.z)) return fail("bad vertex"); positions.push_back(v); }
            else if (tag == "vn") { glm::vec3 n; if (!(ls >> n.x >> n.y >> n.z)) return fail("bad normal"); normals.push_back(n); }
            else if (tag == "vt") { glm::vec2 t; if (!(ls >> t.x >> t.y)) return fail("bad uv"); uvs.push_back(t); }
            else if (tag == "f") {
                std::vector<uint32_t> face;
                std::string corner;
                while (ls >> corner) {
                    int vi = 0, ti = 0, ni = 0;
                    const size_t s1 = corner.find('/');
                    try {
                        vi = std::stoi(corner.substr(0, s1));
                        if (s1 != std::string::npos) {
                            const size_t s2 = corner.find('/', s1 + 1);
                            const std::string t = corner.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                            if (!t.empty()) ti = std::stoi(t);
                            if (s2 != std::string::npos && s2 + 1 < corner.size()) ni = std::stoi(corner.substr(s2 + 1));
                        }
                    }
                    catch (...) { return fail("bad face index '" + corner + "'"); }
                    const int v = fixIndex(vi, positions.size());
                    const int t = ti ? fixIndex(ti, uvs.size()) : -1;
                    const int n = ni ? fixIndex(ni, normals.size()) : -1;
                    if (v < 0 || v >= int(positions.size()) || t >= int(uvs.size()) || n >= int(normals.size())) return fail("face index out of range");
                    if (n < 0) missingNormals = true;
                    const auto key = std::make_tuple(v, t, n);
                    auto found = vertexOf.find(key);
                    if (found == vertexOf.end()) {
                        Render::Vertex vertex{ positions[v], n >= 0 ? normals[n] : glm::vec3(0.0f), t >= 0 ? uvs[t] : glm::vec2(0.0f) };
                        found = vertexOf.emplace(key, uint32_t(out.vertices.size())).first;
                        out.vertices.push_back(vertex);
                    }
                    face.push_back(found->second);
                }
                if (face.size() < 3) return fail("face with fewer than 3 corners");
                for (size_t i = 1; i + 1 < face.size(); ++i) {   // triangle fan
                    out.indices.push_back(face[0]);
                    out.indices.push_back(face[i]);
                    out.indices.push_back(face[i + 1]);
                }
            }
        }
        if (out.indices.empty()) return fail("no faces");
        if (missingNormals) {
            // Area-weighted face normals accumulated per position (smooth shading).
            std::vector<glm::vec3> accum(positions.size(), glm::vec3(0.0f));
            std::vector<int> positionOf(out.vertices.size());
            for (const auto& [key, index] : vertexOf) positionOf[index] = std::get<0>(key);
            for (size_t i = 0; i < out.indices.size(); i += 3) {
                const glm::vec3& a = out.vertices[out.indices[i]].position;
                const glm::vec3& b = out.vertices[out.indices[i + 1]].position;
                const glm::vec3& c = out.vertices[out.indices[i + 2]].position;
                const glm::vec3 n = glm::cross(b - a, c - a);
                for (int k = 0; k < 3; ++k) accum[positionOf[out.indices[i + k]]] += n;
            }
            for (size_t i = 0; i < out.vertices.size(); ++i) {
                if (glm::length(out.vertices[i].normal) > 0.0f) continue;
                const glm::vec3 n = accum[positionOf[i]];
                out.vertices[i].normal = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 1, 0);
            }
        }
        return true;
    }

    bool LoadObj(const std::string& path, Render::MeshData& out, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!ReadBytes(path, bytes)) {
            if (error) *error = "file not found";
            return false;
        }
        return ParseObj(std::string(bytes.begin(), bytes.end()), out, error);
    }

    bool ParseStl(const std::vector<uint8_t>& bytes, Render::MeshData& out, std::string* error) {
        out = {};
        auto fail = [&](const std::string& why) { if (error) *error = why; out = {}; return false; };
        auto emit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            glm::vec3 n = glm::cross(b - a, c - a);
            const float len = glm::length(n);
            if (!(len > 0.0f) || !std::isfinite(len)) return;   // degenerate facet: skipped
            n /= len;
            const uint32_t base = uint32_t(out.vertices.size());
            for (const glm::vec3& p : { a, b, c }) out.vertices.push_back({ p, n, glm::vec2(0.0f) });
            out.indices.insert(out.indices.end(), { base, base + 1, base + 2 });
        };
        // binary: 80-byte header, uint32 count, 50 bytes per facet (the size must match exactly)
        if (bytes.size() >= 84) {
            uint32_t count = 0;
            std::memcpy(&count, bytes.data() + 80, 4);
            if (uint64_t(84) + uint64_t(count) * 50 == bytes.size()) {
                if (count > 50000000u) return fail("too many facets");
                out.vertices.reserve(size_t(count) * 3);
                out.indices.reserve(size_t(count) * 3);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t* f = bytes.data() + 84 + size_t(i) * 50;
                    float v[12];
                    std::memcpy(v, f, sizeof(v));
                    emit({ v[3], v[4], v[5] }, { v[6], v[7], v[8] }, { v[9], v[10], v[11] });
                }
                if (out.indices.empty()) return fail("no valid facets");
                return true;
            }
        }
        // ASCII: "solid ... facet normal ... outer loop vertex x y z (x3) endloop endfacet ... endsolid"
        const std::string text(bytes.begin(), bytes.end());
        std::istringstream in(text);
        std::string word;
        if (!(in >> word) || Lower(word) != "solid") return fail("not an STL file (binary size mismatch, no 'solid' keyword)");
        std::vector<glm::vec3> loop;
        while (in >> word) {
            const std::string w = Lower(word);
            if (w == "vertex") {
                glm::vec3 p;
                if (!(in >> p.x >> p.y >> p.z)) return fail("malformed vertex");
                loop.push_back(p);
            }
            else if (w == "endloop") {
                if (loop.size() != 3) return fail("facet with " + std::to_string(loop.size()) + " vertices");
                emit(loop[0], loop[1], loop[2]);
                loop.clear();
            }
        }
        if (out.indices.empty()) return fail("no valid facets");
        return true;
    }

    bool LoadStl(const std::string& path, Render::MeshData& out, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!ReadBytes(path, bytes)) { if (error) *error = "file not found"; return false; }
        return ParseStl(bytes, out, error);
    }

    bool IsMeshFile(const std::string& path) {
        const std::string ext = Extension(path);
        return ext == ".obj" || ext == ".stl";
    }

    bool LoadMesh(const std::string& path, Render::MeshData& out, std::string* error) {
        const std::string ext = Extension(path);
        if (ext == ".stl") return LoadStl(path, out, error);
        if (ext == ".obj") return LoadObj(path, out, error);
        if (error) *error = "unsupported mesh format '" + ext + "' (OBJ and STL are supported)";
        return false;
    }

    Stats GetStats() {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        Stats stats = s.stats;
        stats.pending = 0;
        for (const auto& [ticket, result] : s.results) if (result.status == Status::Pending) ++stats.pending;
        return stats;
    }

    void Shutdown() {
        State& s = S();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.stop = true;
            s.queue.clear();
        }
        s.wake.notify_all();
        if (s.worker.joinable()) s.worker.join();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.results.clear();
    }
}
