// glTF 2.0 importer (clean-room, written from the Khronos glTF 2.0 specification).
// Supports: .gltf (JSON + external buffers/images or base64 data URIs) and .glb (JSON + BIN chunks),
// triangle primitives (indexed or not) with POSITION / NORMAL / TEXCOORD_0 / JOINTS_0 / WEIGHTS_0,
// node hierarchy (TRS or matrix), skins with inverse bind matrices, animation channels
// (translation / rotation / scale; STEP, LINEAR, CUBICSPLINE), metallic-roughness materials with a
// base-colour texture (PNG / JPEG via stb_image). Not supported: sparse accessors, morph targets,
// cameras / lights, KHR extensions (ignored, with a warning when required).
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Phisiks::Animation {
    namespace {
        using json = nlohmann::json;

        struct Context {
            const json* doc = nullptr;
            std::vector<std::vector<uint8_t>> buffers;
            std::string baseDirectory;
            std::string error;
        };

        bool Fail(Context& c, const std::string& message) {
            if (c.error.empty()) c.error = message;
            return false;
        }

        int Base64Value(char ch) {
            if (ch >= 'A' && ch <= 'Z') return ch - 'A';
            if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
            if (ch >= '0' && ch <= '9') return ch - '0' + 52;
            if (ch == '+' || ch == '-') return 62;
            if (ch == '/' || ch == '_') return 63;
            return -1;
        }

        bool DecodeBase64(const std::string& text, size_t start, std::vector<uint8_t>& out) {
            uint32_t accumulator = 0;
            int bits = 0;
            for (size_t i = start; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch == '=') break;
                const int v = Base64Value(ch);
                if (v < 0) { if (ch == '\r' || ch == '\n' || ch == ' ') continue; return false; }
                accumulator = (accumulator << 6) | uint32_t(v);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out.push_back(uint8_t((accumulator >> bits) & 0xFF));
                }
            }
            return true;
        }

        std::string UriDecode(const std::string& uri) {
            std::string out;
            for (size_t i = 0; i < uri.size(); ++i) {
                if (uri[i] == '%' && i + 2 < uri.size()) {
                    out.push_back(char(std::stoi(uri.substr(i + 1, 2), nullptr, 16)));
                    i += 2;
                }
                else out.push_back(uri[i]);
            }
            return out;
        }

        bool LoadUri(Context& c, const std::string& uri, std::vector<uint8_t>& out) {
            if (uri.rfind("data:", 0) == 0) {
                const size_t comma = uri.find(',');
                if (comma == std::string::npos || uri.find(";base64", 0) == std::string::npos || uri.find(";base64") > comma) return Fail(c, "unsupported data URI");
                if (!DecodeBase64(uri, comma + 1, out)) return Fail(c, "invalid base64 data");
                return true;
            }
            if (c.baseDirectory.empty() && uri.find('/') == std::string::npos && uri.find('\\') == std::string::npos) {
                if (Assets::ReadBytes(UriDecode(uri), out)) return true;
            }
            const std::string path = c.baseDirectory.empty() ? UriDecode(uri) : c.baseDirectory + "/" + UriDecode(uri);
            if (!Assets::ReadBytes(path, out)) return Fail(c, "cannot read '" + path + "'");
            return true;
        }

        int ComponentSize(int type) {
            switch (type) {
                case 5120: case 5121: return 1;
                case 5122: case 5123: return 2;
                case 5125: case 5126: return 4;
            }
            return 0;
        }
        int ComponentCount(const std::string& type) {
            if (type == "SCALAR") return 1;
            if (type == "VEC2") return 2;
            if (type == "VEC3") return 3;
            if (type == "VEC4") return 4;
            if (type == "MAT2") return 4;
            if (type == "MAT3") return 9;
            if (type == "MAT4") return 16;
            return 0;
        }

        float ReadComponent(const uint8_t* p, int type, bool normalized) {
            switch (type) {
                case 5120: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max(float(v) / 127.0f, -1.0f) : float(v); }
                case 5121: return normalized ? float(*p) / 255.0f : float(*p);
                case 5122: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max(float(v) / 32767.0f, -1.0f) : float(v); }
                case 5123: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? float(v) / 65535.0f : float(v); }
                case 5125: { uint32_t v; std::memcpy(&v, p, 4); return float(v); }
                case 5126: { float v; std::memcpy(&v, p, 4); return v; }
            }
            return 0.0f;
        }
        uint32_t ReadInteger(const uint8_t* p, int type) {
            switch (type) {
                case 5121: return *p;
                case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
                case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
                case 5120: { int8_t v; std::memcpy(&v, p, 1); return uint32_t(std::max<int>(v, 0)); }
                case 5122: { int16_t v; std::memcpy(&v, p, 2); return uint32_t(std::max<int>(v, 0)); }
            }
            return 0;
        }

        // Reads an accessor as floats (count x components) or integers.
        struct AccessorData { int count = 0, components = 0; std::vector<float> f; std::vector<uint32_t> u; };

        bool ReadAccessor(Context& c, int index, bool asIntegers, AccessorData& out) {
            const json& accessors = (*c.doc)["accessors"];
            if (index < 0 || index >= int(accessors.size())) return Fail(c, "accessor index out of range");
            const json& a = accessors[index];
            if (a.contains("sparse")) return Fail(c, "sparse accessors are not supported");
            const int type = a.value("componentType", 0);
            const int size = ComponentSize(type);
            out.count = a.value("count", 0);
            out.components = ComponentCount(a.value("type", std::string()));
            const bool normalized = a.value("normalized", false);
            if (!size || !out.components || out.count < 0) return Fail(c, "invalid accessor");
            const size_t values = size_t(out.count) * out.components;
            if (asIntegers) out.u.assign(values, 0u); else out.f.assign(values, 0.0f);
            if (!a.contains("bufferView")) return true;   // all zeros
            const json& views = (*c.doc)["bufferViews"];
            const int viewIndex = a["bufferView"].get<int>();
            if (viewIndex < 0 || viewIndex >= int(views.size())) return Fail(c, "bufferView index out of range");
            const json& view = views[viewIndex];
            const int bufferIndex = view.value("buffer", 0);
            if (bufferIndex < 0 || bufferIndex >= int(c.buffers.size())) return Fail(c, "buffer index out of range");
            const std::vector<uint8_t>& buffer = c.buffers[bufferIndex];
            const size_t offset = size_t(view.value("byteOffset", 0)) + size_t(a.value("byteOffset", 0));
            const size_t element = size_t(size) * out.components;
            size_t stride = size_t(view.value("byteStride", 0));
            if (stride == 0) stride = element;
            if (out.count > 0 && offset + stride * size_t(out.count - 1) + element > buffer.size()) return Fail(c, "accessor exceeds its buffer");
            // Matrix columns are padded to 4 bytes for 1- and 2-byte components (spec 3.6.2.4); MAT4 floats have none.
            for (int i = 0; i < out.count; ++i) {
                const uint8_t* base = buffer.data() + offset + stride * size_t(i);
                for (int k = 0; k < out.components; ++k) {
                    const uint8_t* p = base + size_t(k) * size;
                    if (asIntegers) out.u[size_t(i) * out.components + k] = ReadInteger(p, type);
                    else out.f[size_t(i) * out.components + k] = ReadComponent(p, type, normalized);
                }
            }
            return true;
        }

        bool DecodeImage(Context& c, const std::vector<uint8_t>& bytes, Image& out) {
            int w = 0, h = 0, n = 0;
            stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
            if (!pixels) return Fail(c, "image decode failed");
            out.width = w;
            out.height = h;
            out.rgba.assign(pixels, pixels + size_t(w) * h * 4);
            stbi_image_free(pixels);
            return true;
        }

        void ComputeNormals(Primitive& p) {
            for (Render::Vertex& v : p.vertices) v.normal = glm::vec3(0.0f);
            for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
                Render::Vertex& a = p.vertices[p.indices[i]];
                Render::Vertex& b = p.vertices[p.indices[i + 1]];
                Render::Vertex& d = p.vertices[p.indices[i + 2]];
                const glm::vec3 n = glm::cross(b.position - a.position, d.position - a.position);
                a.normal += n; b.normal += n; d.normal += n;
            }
            for (Render::Vertex& v : p.vertices) v.normal = glm::length(v.normal) > 1e-12f ? glm::normalize(v.normal) : glm::vec3(0, 1, 0);
        }

        Transform Decompose(const glm::mat4& m) {
            Transform t;
            t.position = glm::vec3(m[3]);
            t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            glm::mat3 r(glm::vec3(m[0]) / std::max(t.scale.x, 1e-12f), glm::vec3(m[1]) / std::max(t.scale.y, 1e-12f), glm::vec3(m[2]) / std::max(t.scale.z, 1e-12f));
            if (glm::determinant(r) < 0.0f) { t.scale.x = -t.scale.x; r[0] = -r[0]; }
            t.rotation = glm::normalize(glm::quat_cast(r));
            return t;
        }

        bool Parse(Context& c, Model& model) {
            const json& doc = *c.doc;
            const json& asset = doc.value("asset", json::object());
            const std::string version = asset.value("version", std::string());
            if (version.rfind("2.", 0) != 0) return Fail(c, "not a glTF 2.x asset (version '" + version + "')");
            if (doc.contains("extensionsRequired")) {
                for (const json& e : doc["extensionsRequired"]) Log::Warning() << "glTF: required extension '" << e.get<std::string>() << "' is not supported (ignored)";
            }

            // buffers (GLB BIN chunk is already in c.buffers[0] when present)
            if (doc.contains("buffers")) {
                const json& buffers = doc["buffers"];
                c.buffers.resize(std::max(c.buffers.size(), buffers.size()));
                for (size_t i = 0; i < buffers.size(); ++i) {
                    if (!buffers[i].contains("uri")) continue;   // GLB-stored
                    std::vector<uint8_t> data;
                    if (!LoadUri(c, buffers[i]["uri"].get<std::string>(), data)) return false;
                    c.buffers[i] = std::move(data);
                }
            }

            // images
            if (doc.contains("images")) {
                for (const json& img : doc["images"]) {
                    Image image;
                    image.name = img.value("name", std::string());
                    std::vector<uint8_t> bytes;
                    bool ok = false;
                    if (img.contains("uri")) ok = LoadUri(c, img["uri"].get<std::string>(), bytes);
                    else if (img.contains("bufferView")) {
                        const json& view = doc["bufferViews"][img["bufferView"].get<int>()];
                        const int b = view.value("buffer", 0);
                        const size_t off = size_t(view.value("byteOffset", 0)), len = size_t(view.value("byteLength", 0));
                        if (b >= 0 && b < int(c.buffers.size()) && off + len <= c.buffers[b].size()) {
                            bytes.assign(c.buffers[b].begin() + off, c.buffers[b].begin() + off + len);
                            ok = true;
                        }
                    }
                    if (ok) ok = DecodeImage(c, bytes, image);
                    if (!ok) { Log::Warning() << "glTF: image '" << image.name << "' skipped: " << c.error; c.error.clear(); }
                    model.images.push_back(std::move(image));
                }
            }

            // materials
            if (doc.contains("materials")) {
                for (const json& m : doc["materials"]) {
                    Material mat;
                    mat.name = m.value("name", std::string());
                    const json pbr = m.value("pbrMetallicRoughness", json::object());
                    if (pbr.contains("baseColorFactor")) {
                        const json& f = pbr["baseColorFactor"];
                        mat.baseColor = glm::vec4(f[0].get<float>(), f[1].get<float>(), f[2].get<float>(), f[3].get<float>());
                    }
                    mat.metallic = pbr.value("metallicFactor", 1.0f);
                    mat.roughness = pbr.value("roughnessFactor", 1.0f);
                    if (pbr.contains("baseColorTexture")) {
                        const int texture = pbr["baseColorTexture"].value("index", -1);
                        if (texture >= 0 && doc.contains("textures") && texture < int(doc["textures"].size())) mat.baseColorImage = doc["textures"][texture].value("source", -1);
                    }
                    if (m.contains("emissiveFactor")) {
                        const json& e = m["emissiveFactor"];
                        mat.emissive = glm::vec3(e[0].get<float>(), e[1].get<float>(), e[2].get<float>());
                    }
                    mat.doubleSided = m.value("doubleSided", false);
                    model.materials.push_back(mat);
                }
            }

            // meshes
            if (doc.contains("meshes")) {
                for (const json& m : doc["meshes"]) {
                    Mesh mesh;
                    mesh.name = m.value("name", std::string());
                    for (const json& p : m.value("primitives", json::array())) {
                        if (p.value("mode", 4) != 4) { Log::Warning() << "glTF: mesh '" << mesh.name << "': non-triangle primitive skipped"; continue; }
                        const json& attributes = p["attributes"];
                        if (!attributes.contains("POSITION")) return Fail(c, "primitive without POSITION");
                        Primitive prim;
                        prim.material = p.value("material", -1);
                        AccessorData pos;
                        if (!ReadAccessor(c, attributes["POSITION"].get<int>(), false, pos) || pos.components != 3) return Fail(c, "invalid POSITION accessor");
                        prim.vertices.resize(size_t(pos.count));
                        for (int i = 0; i < pos.count; ++i) prim.vertices[i] = { glm::vec3(pos.f[i * 3], pos.f[i * 3 + 1], pos.f[i * 3 + 2]), glm::vec3(0, 1, 0), glm::vec2(0.0f) };
                        bool hasNormals = false;
                        if (attributes.contains("NORMAL")) {
                            AccessorData n;
                            if (!ReadAccessor(c, attributes["NORMAL"].get<int>(), false, n) || n.count != pos.count || n.components != 3) return Fail(c, "invalid NORMAL accessor");
                            for (int i = 0; i < n.count; ++i) prim.vertices[i].normal = glm::vec3(n.f[i * 3], n.f[i * 3 + 1], n.f[i * 3 + 2]);
                            hasNormals = true;
                        }
                        if (attributes.contains("TEXCOORD_0")) {
                            AccessorData uv;
                            if (!ReadAccessor(c, attributes["TEXCOORD_0"].get<int>(), false, uv) || uv.count != pos.count || uv.components != 2) return Fail(c, "invalid TEXCOORD_0 accessor");
                            for (int i = 0; i < uv.count; ++i) prim.vertices[i].uv = glm::vec2(uv.f[i * 2], uv.f[i * 2 + 1]);
                        }
                        if (attributes.contains("JOINTS_0") && attributes.contains("WEIGHTS_0")) {
                            AccessorData j, w;
                            if (!ReadAccessor(c, attributes["JOINTS_0"].get<int>(), true, j) || j.count != pos.count || j.components != 4) return Fail(c, "invalid JOINTS_0 accessor");
                            if (!ReadAccessor(c, attributes["WEIGHTS_0"].get<int>(), false, w) || w.count != pos.count || w.components != 4) return Fail(c, "invalid WEIGHTS_0 accessor");
                            prim.joints.resize(size_t(pos.count));
                            prim.weights.resize(size_t(pos.count));
                            for (int i = 0; i < pos.count; ++i) {
                                prim.joints[i] = glm::uvec4(j.u[i * 4], j.u[i * 4 + 1], j.u[i * 4 + 2], j.u[i * 4 + 3]);
                                glm::vec4 weight(w.f[i * 4], w.f[i * 4 + 1], w.f[i * 4 + 2], w.f[i * 4 + 3]);
                                const float sum = weight.x + weight.y + weight.z + weight.w;
                                prim.weights[i] = sum > 1e-6f ? weight / sum : glm::vec4(1, 0, 0, 0);
                            }
                        }
                        if (p.contains("indices")) {
                            AccessorData idx;
                            if (!ReadAccessor(c, p["indices"].get<int>(), true, idx) || idx.components != 1) return Fail(c, "invalid index accessor");
                            prim.indices = std::move(idx.u);
                        }
                        else {
                            prim.indices.resize(prim.vertices.size());
                            for (size_t i = 0; i < prim.indices.size(); ++i) prim.indices[i] = uint32_t(i);
                        }
                        if (prim.indices.size() % 3) return Fail(c, "index count is not a multiple of 3");
                        for (uint32_t i : prim.indices) if (i >= prim.vertices.size()) return Fail(c, "vertex index out of range");
                        if (!hasNormals) ComputeNormals(prim);
                        mesh.primitives.push_back(std::move(prim));
                    }
                    model.meshes.push_back(std::move(mesh));
                }
            }

            // nodes
            if (doc.contains("nodes")) {
                const json& nodes = doc["nodes"];
                model.nodes.resize(nodes.size());
                for (size_t i = 0; i < nodes.size(); ++i) {
                    const json& n = nodes[i];
                    Node& node = model.nodes[i];
                    node.name = n.value("name", std::string("node") + std::to_string(i));
                    node.mesh = n.value("mesh", -1);
                    node.skin = n.value("skin", -1);
                    if (n.contains("matrix")) {
                        glm::mat4 m(1.0f);
                        for (int k = 0; k < 16; ++k) glm::value_ptr(m)[k] = n["matrix"][k].get<float>();   // column-major, like glm
                        node.local = Decompose(m);
                    }
                    else {
                        if (n.contains("translation")) node.local.position = glm::vec3(n["translation"][0].get<float>(), n["translation"][1].get<float>(), n["translation"][2].get<float>());
                        if (n.contains("rotation")) node.local.rotation = glm::normalize(glm::quat(n["rotation"][3].get<float>(), n["rotation"][0].get<float>(), n["rotation"][1].get<float>(), n["rotation"][2].get<float>()));
                        if (n.contains("scale")) node.local.scale = glm::vec3(n["scale"][0].get<float>(), n["scale"][1].get<float>(), n["scale"][2].get<float>());
                    }
                    for (const json& child : n.value("children", json::array())) {
                        const int ci = child.get<int>();
                        if (ci < 0 || ci >= int(nodes.size()) || ci == int(i)) return Fail(c, "invalid child index");
                        node.children.push_back(ci);
                    }
                    if (node.mesh >= int(model.meshes.size())) return Fail(c, "node mesh index out of range");
                }
                for (size_t i = 0; i < model.nodes.size(); ++i) {
                    for (int ci : model.nodes[i].children) {
                        if (model.nodes[ci].parent >= 0) return Fail(c, "node with two parents");
                        model.nodes[ci].parent = int(i);
                    }
                }
            }
            const int sceneIndex = doc.value("scene", 0);
            if (doc.contains("scenes") && sceneIndex < int(doc["scenes"].size())) {
                for (const json& n : doc["scenes"][sceneIndex].value("nodes", json::array())) model.roots.push_back(n.get<int>());
            }
            if (model.roots.empty()) for (size_t i = 0; i < model.nodes.size(); ++i) if (model.nodes[i].parent < 0) model.roots.push_back(int(i));

            // skins
            if (doc.contains("skins")) {
                for (const json& s : doc["skins"]) {
                    Skin skin;
                    skin.name = s.value("name", std::string());
                    for (const json& j : s["joints"]) {
                        const int node = j.get<int>();
                        if (node < 0 || node >= int(model.nodes.size())) return Fail(c, "skin joint index out of range");
                        skin.joints.push_back(node);
                    }
                    skin.inverseBind.assign(skin.joints.size(), glm::mat4(1.0f));
                    if (s.contains("inverseBindMatrices")) {
                        AccessorData m;
                        if (!ReadAccessor(c, s["inverseBindMatrices"].get<int>(), false, m) || m.components != 16 || m.count < int(skin.joints.size())) return Fail(c, "invalid inverseBindMatrices");
                        for (size_t k = 0; k < skin.joints.size(); ++k) std::memcpy(glm::value_ptr(skin.inverseBind[k]), &m.f[k * 16], 64);
                    }
                    model.skins.push_back(std::move(skin));
                }
            }

            // animations
            if (doc.contains("animations")) {
                int index = 0;
                for (const json& a : doc["animations"]) {
                    Clip clip;
                    clip.name = a.value("name", std::string("clip") + std::to_string(index++));
                    const json& samplers = a["samplers"];
                    for (const json& ch : a["channels"]) {
                        const json& target = ch["target"];
                        const std::string path = target.value("path", std::string());
                        if (path != "translation" && path != "rotation" && path != "scale") continue;   // morph weights: unsupported
                        if (!target.contains("node")) continue;
                        Channel channel;
                        channel.node = target["node"].get<int>();
                        if (channel.node < 0 || channel.node >= int(model.nodes.size())) return Fail(c, "animation node index out of range");
                        channel.path = path == "translation" ? TargetPath::Translation : path == "rotation" ? TargetPath::Rotation : TargetPath::Scale;
                        const json& sampler = samplers[ch["sampler"].get<int>()];
                        const std::string interp = sampler.value("interpolation", std::string("LINEAR"));
                        channel.interpolation = interp == "STEP" ? Interpolation::Step : interp == "CUBICSPLINE" ? Interpolation::CubicSpline : Interpolation::Linear;
                        AccessorData input, output;
                        if (!ReadAccessor(c, sampler["input"].get<int>(), false, input) || input.components != 1) return Fail(c, "invalid animation input");
                        if (!ReadAccessor(c, sampler["output"].get<int>(), false, output)) return Fail(c, "invalid animation output");
                        channel.times = input.f;
                        const int expected = channel.interpolation == Interpolation::CubicSpline ? input.count * 3 : input.count;
                        if (output.count != expected || (output.components != 3 && output.components != 4)) return Fail(c, "animation output count mismatch");
                        for (int k = 0; k < output.count; ++k) {
                            const float* v = &output.f[size_t(k) * output.components];
                            channel.values.push_back(output.components == 4 ? glm::vec4(v[0], v[1], v[2], v[3]) : glm::vec4(v[0], v[1], v[2], 0.0f));
                        }
                        if (!channel.times.empty()) clip.duration = std::max(clip.duration, channel.times.back());
                        clip.channels.push_back(std::move(channel));
                    }
                    model.clips.push_back(std::move(clip));
                }
            }
            return true;
        }
    }

    int Model::FindNode(const std::string& name) const {
        for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].name == name) return int(i);
        return -1;
    }
    int Model::FindClip(const std::string& name) const {
        for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == name) return int(i);
        return -1;
    }

    bool ParseGltf(const std::vector<uint8_t>& bytes, const std::string& baseDirectory, Model& out, std::string* error) {
        out = Model{};
        Context c;
        c.baseDirectory = baseDirectory;
        json doc;
        auto readU32 = [&](size_t offset) { uint32_t v; std::memcpy(&v, bytes.data() + offset, 4); return v; };
        try {
            if (bytes.size() >= 12 && readU32(0) == 0x46546C67u) {   // "glTF"
                if (readU32(4) != 2) { if (error) *error = "unsupported GLB version"; return false; }
                const size_t total = std::min<size_t>(readU32(8), bytes.size());
                size_t offset = 12;
                bool haveJson = false;
                while (offset + 8 <= total) {
                    const uint32_t length = readU32(offset), type = readU32(offset + 4);
                    if (offset + 8 + length > total) { if (error) *error = "truncated GLB chunk"; return false; }
                    const uint8_t* data = bytes.data() + offset + 8;
                    if (type == 0x4E4F534Au && !haveJson) { doc = json::parse(data, data + length); haveJson = true; }
                    else if (type == 0x004E4942u && c.buffers.empty()) c.buffers.emplace_back(data, data + length);
                    offset += 8 + ((length + 3) & ~3u);
                }
                if (!haveJson) { if (error) *error = "GLB without a JSON chunk"; return false; }
            }
            else {
                doc = json::parse(bytes.begin(), bytes.end());
            }
            c.doc = &doc;
            if (!Parse(c, out)) { if (error) *error = c.error; return false; }
        }
        catch (const std::exception& e) {
            if (error) *error = std::string("glTF parse error: ") + e.what();
            return false;
        }
        return true;
    }

    bool LoadGltf(const std::string& assetPath, Model& out, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!Assets::ReadBytes(assetPath, bytes)) { if (error) *error = "file not found: " + assetPath; return false; }
        const std::string resolved = Assets::Resolve(assetPath);
        const size_t slash = resolved.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? std::string(".") : resolved.substr(0, slash);
        return ParseGltf(bytes, base, out, error);
    }
}
