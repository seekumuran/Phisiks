// Forward PBR renderer (OpenGL 4.6, clean-room).
//
// Frame:
//   1. stream in textures/meshes requested on demand; upload materials and lights
//   2. build one instance buffer + indirect command buffer: shadow casters, then every view's
//      frustum-culled objects (sorted by mesh: one DrawElementsIndirectCommand per mesh)
//   3. spot / point shadow maps (shared by all views)
//   4. per view (camera): directional shadow fit -> viewport/scissor into its target set ->
//      forward PBR pass (HDR + entity id) -> sky -> debug lines
//   5. per target set: MSAA resolve -> tonemap into LDR; the window set is blitted to the
//      default framebuffer and overlay text is drawn on top.
#include "render/RendererInternal.h"
#include "render/RenderMath.h"
#include "render/ShaderCache.h"
#include "render/ShaderPrograms.h"
#include "render/ShaderPreprocessor.h"
#include "render/gl/GLObjects.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Platform/Window.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Phisiks::Render {
    namespace {
        constexpr int MAX_SPOT_SHADOWS = 4;
        constexpr int MAX_POINT_SHADOWS = 4;
        constexpr int TEXTURE_SIZE = 512;
        constexpr int TEXTURE_LEVELS = 10;   // 512 -> 1

        // ---- GPU layouts (must match engine/shaders/common.glsl)
        struct FrameDataGpu {
            glm::mat4 view, projection, viewProjection, inverseViewProjection;
            glm::vec4 cameraPosition, skyZenith, skyHorizon, groundColor, fogColor, fogParams, fogParams2;
            glm::vec4 ambientUp, ambientDown, rim;
            glm::mat4 dirShadowMatrix;
            glm::mat4 spotShadowMatrices[MAX_SPOT_SHADOWS];
        };
        static_assert(sizeof(FrameDataGpu) == 4 * 64 + 10 * 16 + 64 + MAX_SPOT_SHADOWS * 64, "std140 layout");
        struct LightGpu { glm::vec4 positionRange, directionType, colorIntensity, params; };
        struct InstanceGpu { glm::mat4 model, normalMatrix; glm::vec4 scaleMaterial; glm::uvec4 ids; glm::vec4 uvScale; };
        static_assert(sizeof(InstanceGpu) == 176, "std430 layout");
        struct MaterialGpu { glm::vec4 baseRoughness, patternMetallic, emissivePattern, params, textureParams; };
        static_assert(sizeof(MaterialGpu) == 80, "std430 layout");
        struct DrawCommand { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };
        struct LineVertex { glm::vec3 position; glm::vec4 color; };
        struct TextVertex { glm::vec2 position; glm::vec2 texel; glm::vec4 color; };

        struct MeshEntry { uint32_t firstIndex = 0, indexCount = 0; int32_t baseVertex = 0; float radius = 0.0f; };
        struct Object {
            MeshRenderer renderer;
            glm::mat4 world{ 1.0f };
            uint32_t entityId = 0;
        };
        struct LightEntry { Light light; glm::vec3 position{ 0.0f }; glm::vec3 direction{ 0, -1, 0 }; };

        // HDR/id/depth (MSAA) -> resolved -> LDR. One for the window, one per offscreen target.
        struct TargetSet {
            glm::ivec2 size{ 0 };
            int samples = 0;
            GL::Texture hdrMain, idMain, depthMain, hdrResolved, idResolved, ldr;
            GL::Framebuffer mainFbo, resolveColorFbo, resolveIdFbo, ldrFbo;
            GL::Texture present;               // window only: LDR scene + overlay text (what is shown)
            GL::Framebuffer presentFbo;
            bool usedThisFrame = false;
            bool rendered = false;
        };

        struct State {
            RenderSettings settings;
            ShaderCache shaderCache;
            GL::Program programs[Shaders::PROGRAM_COUNT];
            GLint uboAlignment = 256;

            // meshes
            GLuint meshVao = 0;
            GL::Buffer vertexBuffer, indexBuffer;
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            std::vector<MeshEntry> meshes;
            std::unordered_map<std::string, uint32_t> meshByName;
            bool meshesDirty = true;

            // materials + albedo textures (one texture array layer per texture file)
            std::vector<MaterialDesc> materials;
            std::unordered_map<std::string, uint32_t> materialByName;
            GL::Texture albedoArray;
            int albedoCapacity = 0;
            int albedoUsed = 0;
            std::unordered_map<std::string, int> textureLayer;          // -1 = failed
            std::unordered_map<std::string, Assets::Ticket> textureTickets;

            // scene
            std::unordered_map<uint64_t, Object> objects;
            std::unordered_map<uint64_t, LightEntry> lights;
            uint64_t nextHandle = 0;
            std::vector<Internal::CameraInput> cameras;
            std::unordered_set<std::string> warned;

            // per-frame buffers
            GL::Buffer frameUbo, lightBuffer, instanceBuffer, materialBuffer, indirectBuffer, lineBuffer, textBuffer;
            GLuint lineVao = 0, textVao = 0, emptyVao = 0;
            std::vector<LineVertex> lines;
            std::vector<TextVertex> text;

            // targets
            TargetSet window;
            std::unordered_map<std::string, TargetSet> offscreen;
            int shadowSize = 0;
            GL::Texture dirShadow, spotShadows, pointShadows;
            GL::Framebuffer shadowFbo;
            GL::Texture fontAtlas;

            RenderStats frameStats;
            uint32_t glErrors = 0;
            uint32_t loggedDebugMessages = 0;
        };
        std::unique_ptr<State> g;

        void GLAD_API_PTR OnDebugMessage(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || !g) return;
            if (type == GL_DEBUG_TYPE_ERROR) ++g->glErrors;
            if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
                if (g->loggedDebugMessages++ < 20) Log::Error() << "OpenGL [" << id << "]: " << message;
            }
        }

        void WarnOnce(const std::string& key, const std::string& message) {
            if (g->warned.insert(key).second) Log::Warning() << message;
        }

        // ------------------------------------------------------------------ setup
        bool BuildPrograms() {
            const auto resolver = [](const std::string& name) { return Shaders::Find(name); };
            const std::vector<Shaders::ProgramDef>& defs = Shaders::Programs();
            for (size_t i = 0; i < defs.size(); ++i) {
                std::vector<GL::StageSource> stages;
                for (const Shaders::StageDef& stage : defs[i].stages) {
                    const std::string* source = Shaders::Find(stage.file);
                    if (!source) {
                        Log::Error() << "Renderer: embedded shader '" << stage.file << "' missing";
                        return false;
                    }
                    const ShaderPreprocessor::Result expanded = ShaderPreprocessor::Expand(*source, resolver, defs[i].defines);
                    if (!expanded.ok) {
                        Log::Error() << "Renderer: " << stage.file << ": " << expanded.error;
                        return false;
                    }
                    stages.push_back({ stage.stage == Shaders::Stage::Vertex ? GLenum(GL_VERTEX_SHADER) : GLenum(GL_FRAGMENT_SHADER), expanded.source });
                }
                const GLuint program = g->shaderCache.GetProgram(defs[i].name, std::move(stages));
                if (!program) return false;
                g->programs[i].Adopt(program, defs[i].name);
            }
            return true;
        }

        void UploadMeshesIfDirty() {
            if (!g->meshesDirty) return;
            g->vertexBuffer.Upload(g->vertices.data(), g->vertices.size() * sizeof(Vertex));
            g->indexBuffer.Upload(g->indices.data(), g->indices.size() * sizeof(uint32_t));
            glVertexArrayVertexBuffer(g->meshVao, 0, g->vertexBuffer.Id(), 0, sizeof(Vertex));
            glVertexArrayElementBuffer(g->meshVao, g->indexBuffer.Id());
            g->meshesDirty = false;
        }

        void EnsureTargetSet(TargetSet& t, glm::ivec2 size, const std::string& label) {
            const int samples = std::clamp(g->settings.msaaSamples, 1, 8);
            if (size == t.size && samples == t.samples) return;
            t.size = size;
            t.samples = samples;
            t.rendered = false;

            GL::TextureDesc d;
            d.width = size.x;
            d.height = size.y;
            d.target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
            d.samples = samples;
            d.minFilter = d.magFilter = GL_NEAREST;
            d.format = GL_RGBA16F;            t.hdrMain.Create(label + " HDR", d);
            d.format = GL_R32UI;              t.idMain.Create(label + " ids", d);
            d.format = GL_DEPTH_COMPONENT32F; t.depthMain.Create(label + " depth", d);
            t.mainFbo.Create(label + " main");
            t.mainFbo.AttachColor(0, t.hdrMain);
            t.mainFbo.AttachColor(1, t.idMain);
            t.mainFbo.AttachDepth(t.depthMain);
            t.mainFbo.SetDrawBuffers(2);
            t.mainFbo.Check((label + " main").c_str());

            d.target = GL_TEXTURE_2D;
            d.samples = 1;
            d.format = GL_RGBA16F;
            d.minFilter = d.magFilter = GL_LINEAR;
            t.hdrResolved.Create(label + " HDR resolved", d);
            d.format = GL_R32UI;
            d.minFilter = d.magFilter = GL_NEAREST;
            t.idResolved.Create(label + " ids resolved", d);
            d.format = GL_RGBA8;
            d.minFilter = d.magFilter = GL_LINEAR;
            t.ldr.Create(label + " LDR", d);

            t.resolveColorFbo.Create(label + " resolve colour");
            t.resolveColorFbo.AttachColor(0, t.hdrResolved);
            t.resolveColorFbo.SetDrawBuffers(1);
            t.resolveIdFbo.Create(label + " resolve ids");
            t.resolveIdFbo.AttachColor(0, t.idResolved);
            t.resolveIdFbo.SetDrawBuffers(1);
            t.ldrFbo.Create(label + " LDR");
            t.ldrFbo.AttachColor(0, t.ldr);
            t.ldrFbo.SetDrawBuffers(1);
            if (&t == &g->window) {
                d.format = GL_RGBA8;
                t.present.Create("Presented frame", d);
                t.presentFbo.Create("Present");
                t.presentFbo.AttachColor(0, t.present);
                t.presentFbo.SetDrawBuffers(1);
            }
            Log::Info() << "Renderer: target '" << label << "' " << size.x << "x" << size.y << ", MSAA " << samples << "x";
        }

        void EnsureShadowTargets() {
            const int size = std::clamp(g->settings.shadowMapSize, 256, 8192);
            if (size == g->shadowSize) return;
            g->shadowSize = size;
            GL::TextureDesc d;
            d.format = GL_DEPTH_COMPONENT32F;
            d.minFilter = d.magFilter = GL_LINEAR;   // bilinear depth comparison
            d.depthCompare = true;
            d.width = d.height = size;
            g->dirShadow.Create("Shadow (directional)", d);
            d.target = GL_TEXTURE_2D_ARRAY;
            d.width = d.height = size / 2;
            d.layers = MAX_SPOT_SHADOWS;
            g->spotShadows.Create("Shadow (spot)", d);
            d.target = GL_TEXTURE_CUBE_MAP_ARRAY;
            d.width = d.height = size / 4;
            d.layers = MAX_POINT_SHADOWS * 6;
            g->pointShadows.Create("Shadow (point)", d);
            g->shadowFbo.Create("Shadow");
            g->shadowFbo.SetDrawBuffers(0);
        }

        void EnsureAlbedoCapacity(int layers) {
            if (layers <= g->albedoCapacity) return;
            const int capacity = std::max({ 4, layers, g->albedoCapacity * 2 });
            GL::TextureDesc d;
            d.target = GL_TEXTURE_2D_ARRAY;
            d.format = GL_SRGB8_ALPHA8;
            d.width = d.height = TEXTURE_SIZE;
            d.layers = capacity;
            d.levels = TEXTURE_LEVELS;
            d.minFilter = GL_LINEAR_MIPMAP_LINEAR;
            d.magFilter = GL_LINEAR;
            d.wrap = GL_REPEAT;
            GL::Texture grown;
            grown.Create("Albedo textures", d);
            glTextureParameterf(grown.Id(), GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
            if (g->albedoUsed > 0) {
                for (int level = 0; level < TEXTURE_LEVELS; ++level) {
                    const int s = std::max(TEXTURE_SIZE >> level, 1);
                    glCopyImageSubData(g->albedoArray.Id(), GL_TEXTURE_2D_ARRAY, level, 0, 0, 0,
                                       grown.Id(), GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, s, s, g->albedoUsed);
                }
            }
            g->albedoArray = std::move(grown);
            g->albedoCapacity = capacity;
        }

        // Requests textures referenced by materials and uploads the ones the worker finished.
        void StreamTextures() {
            for (const MaterialDesc& m : g->materials) {
                const std::string& path = m.albedoTexture;
                if (path.empty() || g->textureLayer.count(path) || g->textureTickets.count(path)) continue;
                g->textureTickets[path] = Assets::RequestImage(path);
            }
            for (auto it = g->textureTickets.begin(); it != g->textureTickets.end();) {
                Assets::Image image;
                const Assets::Status status = Assets::PollImage(it->second, image);
                if (status == Assets::Status::Pending) { ++it; continue; }
                if (status == Assets::Status::Failed) {
                    Log::Error() << "Renderer: texture '" << it->first << "' could not be loaded";
                    g->textureLayer[it->first] = -1;
                }
                else {
                    Assets::Image sized;
                    if (image.width != TEXTURE_SIZE || image.height != TEXTURE_SIZE) Assets::ResizeImage(image, TEXTURE_SIZE, TEXTURE_SIZE, sized);
                    const Assets::Image& upload = sized.rgba.empty() ? image : sized;
                    EnsureAlbedoCapacity(g->albedoUsed + 1);
                    const int layer = g->albedoUsed++;
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTextureSubImage3D(g->albedoArray.Id(), 0, 0, 0, layer, TEXTURE_SIZE, TEXTURE_SIZE, 1, GL_RGBA, GL_UNSIGNED_BYTE, upload.rgba.data());
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glGenerateTextureMipmap(g->albedoArray.Id());
                    g->textureLayer[it->first] = layer;
                    Log::Info() << "Renderer: texture '" << it->first << "' (" << image.width << "x" << image.height << ") -> layer " << layer;
                }
                it = g->textureTickets.erase(it);
            }
        }

        // Mesh lookup; ".obj" names load from the asset folder the first time they are used.
        bool ResolveMesh(const std::string& name, uint32_t& index) {
            auto it = g->meshByName.find(name);
            if (it != g->meshByName.end()) { index = it->second; return true; }
            const bool isObj = name.size() > 4 && name.compare(name.size() - 4, 4, ".obj") == 0;
            if (isObj && !g->warned.count("mesh:" + name)) {
                MeshData data;
                std::string error;
                if (Assets::LoadObj(name, data, &error) && RegisterMesh(name, data)) {
                    Log::Info() << "Renderer: mesh '" << name << "' loaded (" << data.vertices.size() << " vertices, " << data.indices.size() / 3 << " triangles)";
                    index = g->meshByName[name];
                    return true;
                }
                WarnOnce("mesh:" + name, "Renderer: mesh '" + name + "' could not be loaded: " + error);
                return false;
            }
            WarnOnce("mesh:" + name, "Renderer: unknown mesh '" + name + "'");
            return false;
        }

        void SetupVertexArrays() {
            glCreateVertexArrays(1, &g->meshVao);
            const GLuint vao = g->meshVao;
            glEnableVertexArrayAttrib(vao, 0);
            glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
            glVertexArrayAttribBinding(vao, 0, 0);
            glEnableVertexArrayAttrib(vao, 1);
            glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
            glVertexArrayAttribBinding(vao, 1, 0);
            glEnableVertexArrayAttrib(vao, 2);
            glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
            glVertexArrayAttribBinding(vao, 2, 0);

            glCreateVertexArrays(1, &g->lineVao);
            glEnableVertexArrayAttrib(g->lineVao, 0);
            glVertexArrayAttribFormat(g->lineVao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(LineVertex, position));
            glVertexArrayAttribBinding(g->lineVao, 0, 0);
            glEnableVertexArrayAttrib(g->lineVao, 1);
            glVertexArrayAttribFormat(g->lineVao, 1, 4, GL_FLOAT, GL_FALSE, offsetof(LineVertex, color));
            glVertexArrayAttribBinding(g->lineVao, 1, 0);

            glCreateVertexArrays(1, &g->textVao);
            glEnableVertexArrayAttrib(g->textVao, 0);
            glVertexArrayAttribFormat(g->textVao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(TextVertex, position));
            glVertexArrayAttribBinding(g->textVao, 0, 0);
            glEnableVertexArrayAttrib(g->textVao, 1);
            glVertexArrayAttribFormat(g->textVao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(TextVertex, texel));
            glVertexArrayAttribBinding(g->textVao, 1, 0);
            glEnableVertexArrayAttrib(g->textVao, 2);
            glVertexArrayAttribFormat(g->textVao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(TextVertex, color));
            glVertexArrayAttribBinding(g->textVao, 2, 0);

            glCreateVertexArrays(1, &g->emptyVao);
        }

        MaterialGpu ToGpu(const MaterialDesc& m) {
            MaterialGpu out;
            out.baseRoughness = glm::vec4(m.baseColor, m.roughness);
            out.patternMetallic = glm::vec4(m.patternColor, m.metallic);
            out.emissivePattern = glm::vec4(m.emissive, float(uint32_t(m.pattern)));
            out.params = glm::vec4(m.patternScale, m.lineWidth, m.patternRoughness, m.patternGlow);
            int layer = -1;
            if (!m.albedoTexture.empty()) {
                auto it = g->textureLayer.find(m.albedoTexture);
                if (it != g->textureLayer.end()) layer = it->second;
            }
            out.textureParams = glm::vec4(float(layer), m.textureScale, 0.0f, 0.0f);
            return out;
        }

        float MaxScale(const glm::mat4& m) {
            return std::max({ glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])) });
        }

        glm::mat4 MakeProjection(const Camera& c, float aspect, float farOverride = -1.0f) {
            const float farPlane = farOverride > 0.0f ? farOverride : c.farPlane;
            if (c.projection == Projection::Orthographic) {
                const float h = c.orthoHeight * 0.5f, w = h * aspect;
                return glm::ortho(-w, w, -h, h, c.nearPlane, farPlane);
            }
            return glm::perspective(c.verticalFov, aspect, c.nearPlane, farPlane);
        }

        std::array<glm::vec3, 8> CornersOf(const glm::mat4& viewProjection) {
            const glm::mat4 inverse = glm::inverse(viewProjection);
            std::array<glm::vec3, 8> corners;
            int i = 0;
            for (float z : { -1.0f, 1.0f })
                for (float y : { -1.0f, 1.0f })
                    for (float x : { -1.0f, 1.0f }) {
                        const glm::vec4 p = inverse * glm::vec4(x, y, z, 1.0f);
                        corners[i++] = glm::vec3(p) / p.w;
                    }
            return corners;
        }

        void DrawTextPass(GLuint framebuffer, glm::ivec2 size) {
            if (g->text.empty()) return;
            g->textBuffer.Upload(g->text.data(), g->text.size() * sizeof(TextVertex));
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glViewport(0, 0, size.x, size.y);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            const GL::Program& program = g->programs[Shaders::TEXT];
            program.Use();
            glUniform2f(program.Uniform("u_screenSize"), float(size.x), float(size.y));
            glBindTextureUnit(0, g->fontAtlas.Id());
            glBindVertexArray(g->textVao);
            glVertexArrayVertexBuffer(g->textVao, 0, g->textBuffer.Id(), 0, sizeof(TextVertex));
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(g->text.size()));
            glDisable(GL_BLEND);
        }

        void DrawCommands(size_t firstCommand, size_t count) {
            if (count == 0) return;
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(firstCommand * sizeof(DrawCommand)), GLsizei(count), 0);
        }

        bool Capture(const TargetSet& t, FrameCapture& out) {
            if (!t.rendered || t.size.x <= 0) return false;
            out.width = t.size.x;
            out.height = t.size.y;
            const size_t pixels = size_t(out.width) * size_t(out.height);
            out.rgba.resize(pixels * 4);
            out.entityIds.resize(pixels);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glGetTextureImage(t.ldr.Id(), 0, GL_RGBA, GL_UNSIGNED_BYTE, GLsizei(out.rgba.size()), out.rgba.data());
            glGetTextureImage(t.idResolved.Id(), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, GLsizei(out.entityIds.size() * 4), out.entityIds.data());
            return true;
        }
    }

    // ====================================================================== public API
    RenderSettings& Settings() {
        static RenderSettings headless;   // usable before Init / in tests / headless runs
        return g ? g->settings : headless;
    }

    bool RegisterMaterial(const std::string& name, const MaterialDesc& desc) {
        if (!g || name.empty()) return false;
        auto it = g->materialByName.find(name);
        if (it != g->materialByName.end()) g->materials[it->second] = desc;
        else {
            g->materialByName[name] = uint32_t(g->materials.size());
            g->materials.push_back(desc);
        }
        return true;
    }
    bool HasMaterial(const std::string& name) { return g && g->materialByName.count(name) != 0; }
    const MaterialDesc* FindMaterial(const std::string& name) {
        if (!g) return nullptr;
        auto it = g->materialByName.find(name);
        return it == g->materialByName.end() ? nullptr : &g->materials[it->second];
    }

    bool RegisterMesh(const std::string& name, const MeshData& data) {
        if (!g || name.empty() || data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0) return false;
        for (uint32_t index : data.indices) if (index >= data.vertices.size()) return false;
        if (g->meshByName.count(name)) {
            Log::Warning() << "Render::RegisterMesh: '" << name << "' already exists";
            return false;
        }
        MeshEntry entry;
        entry.firstIndex = uint32_t(g->indices.size());
        entry.indexCount = uint32_t(data.indices.size());
        entry.baseVertex = int32_t(g->vertices.size());
        entry.radius = RenderMath::BoundingRadius(data);
        g->vertices.insert(g->vertices.end(), data.vertices.begin(), data.vertices.end());
        g->indices.insert(g->indices.end(), data.indices.begin(), data.indices.end());
        g->meshByName[name] = uint32_t(g->meshes.size());
        g->meshes.push_back(entry);
        g->meshesDirty = true;
        return true;
    }
    bool HasMesh(const std::string& name) { return g && g->meshByName.count(name) != 0; }

    void DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color) {
        if (!g) return;
        g->lines.push_back({ a, color });
        g->lines.push_back({ b, color });
    }

    void DrawAABB(const glm::vec3& lo, const glm::vec3& hi, const glm::vec4& color) {
        const glm::vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                                 { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        const int edges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : edges) DrawLine(c[e[0]], c[e[1]], color);
    }

    void DrawCross(const glm::vec3& p, float size, const glm::vec4& color) {
        DrawLine(p - glm::vec3(size, 0, 0), p + glm::vec3(size, 0, 0), color);
        DrawLine(p - glm::vec3(0, size, 0), p + glm::vec3(0, size, 0), color);
        DrawLine(p - glm::vec3(0, 0, size), p + glm::vec3(0, 0, size), color);
    }

    namespace {
        void EmitText(const std::string& text, glm::vec2 origin, const glm::vec4& color, float scale) {
            using namespace RenderMath::Font;
            glm::vec2 pen = origin;
            for (char c : text) {
                if (c == '\n') { pen.x = origin.x; pen.y += LINE_HEIGHT * scale; continue; }
                if (c != ' ') {
                    int code = static_cast<unsigned char>(c);
                    if (code < 32 || code > 126) code = '?';
                    const int cell = code - 32;
                    const glm::vec2 t0(float((cell % COLUMNS) * CELL_W), float((cell / COLUMNS) * CELL_H));
                    const glm::vec2 t1 = t0 + glm::vec2(GLYPH_W, GLYPH_H);
                    const glm::vec2 p0 = pen, p1 = pen + glm::vec2(GLYPH_W, GLYPH_H) * scale;
                    const TextVertex quad[6] = {
                        { { p0.x, p0.y }, { t0.x, t0.y }, color }, { { p1.x, p0.y }, { t1.x, t0.y }, color }, { { p1.x, p1.y }, { t1.x, t1.y }, color },
                        { { p0.x, p0.y }, { t0.x, t0.y }, color }, { { p1.x, p1.y }, { t1.x, t1.y }, color }, { { p0.x, p1.y }, { t0.x, t1.y }, color },
                    };
                    g->text.insert(g->text.end(), quad, quad + 6);
                }
                pen.x += ADVANCE * scale;
            }
        }
    }

    void DrawScreenText(const std::string& text, glm::vec2 position, const glm::vec4& color, float scale) {
        if (!g || text.empty()) return;
        const float shadow = std::max(1.0f, std::floor(scale * 0.5f));
        EmitText(text, position + glm::vec2(shadow), glm::vec4(0.0f, 0.0f, 0.0f, color.a * 0.8f), scale);
        EmitText(text, position, color, scale);
    }

    glm::vec2 MeasureText(const std::string& text, float scale) { return RenderMath::Font::Measure(text, scale); }

    glm::ivec2 GetFramebufferSize() { return Platform::HasWindow() ? Platform::GetFramebufferSize() : glm::ivec2(0); }

    bool CaptureFrame(FrameCapture& out) { return g && Capture(g->window, out); }

    bool CaptureTarget(const std::string& name, FrameCapture& out) {
        if (!g) return false;
        auto it = g->offscreen.find(name);
        return it != g->offscreen.end() && Capture(it->second, out);
    }

    uint32_t GetTargetTexture(const std::string& name) {
        if (!g) return 0;
        auto it = g->offscreen.find(name);
        return it != g->offscreen.end() && it->second.rendered ? it->second.ldr.Id() : 0;
    }

    ShaderCacheStats GetShaderCacheStats() { return g ? g->shaderCache.Stats() : ShaderCacheStats{}; }
    uint32_t GetGLErrorCount() { return g ? g->glErrors : 0; }

    // ====================================================================== internal
    namespace Internal {
        bool IsInitialized() { return g != nullptr; }

        bool Init() {
            if (g) return true;
            if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(Platform::GetGLProcLoader()))) {
                Log::Error() << "Renderer: failed to load OpenGL entry points";
                return false;
            }
            GLint major = 0, minor = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            Log::Info() << "Renderer: OpenGL " << major << "." << minor << " - " << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            if (major < 4 || (major == 4 && minor < 6)) {
                Log::Error() << "Renderer: OpenGL 4.6 is required";
                return false;
            }

            g = std::make_unique<State>();
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(OnDebugMessage, nullptr);
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g->uboAlignment);

            g->shaderCache.Init();
            const double start = Time::NowSeconds();
            if (!BuildPrograms()) {
                Log::Error() << "Renderer: shader programs failed to build";
                g.reset();
                return false;
            }
            const ShaderCacheStats cache = g->shaderCache.Stats();
            Log::Info() << "Renderer: " << cache.programs << " programs ready in " << int((Time::NowSeconds() - start) * 1000.0) << " ms ("
                        << cache.loadedFromCache << " from cache, " << cache.compiled << " compiled, " << cache.rejected << " stale)";

            SetupVertexArrays();
            RegisterMesh("Cube", RenderMath::MakeCube());
            RegisterMesh("Sphere", RenderMath::MakeSphere());
            RegisterMesh("Plane", RenderMath::MakePlane());
            RegisterMaterial("Default", MaterialDesc{});

            g->frameUbo.Create("FrameData", 4096);
            g->lightBuffer.Create("Lights", sizeof(LightGpu) * 16);
            g->instanceBuffer.Create("Instances", sizeof(InstanceGpu) * 256);
            g->materialBuffer.Create("Materials", sizeof(MaterialGpu) * 32);
            g->indirectBuffer.Create("Draw commands", sizeof(DrawCommand) * 16);
            g->lineBuffer.Create("Debug lines", sizeof(LineVertex) * 1024);
            g->textBuffer.Create("Text", sizeof(TextVertex) * 4096);

            GL::TextureDesc font;
            font.format = GL_R8;
            font.width = RenderMath::Font::ATLAS_W;
            font.height = RenderMath::Font::ATLAS_H;
            font.minFilter = font.magFilter = GL_NEAREST;
            g->fontAtlas.Create("Font", font);
            const std::vector<uint8_t> atlas = RenderMath::Font::BuildAtlas();
            g->fontAtlas.Upload2D(GL_RED, GL_UNSIGNED_BYTE, atlas.data());

            EnsureAlbedoCapacity(4);
            EnsureShadowTargets();
            return true;
        }

        void Shutdown() {
            if (!g) return;
            glFinish();
            glDeleteVertexArrays(1, &g->meshVao);
            glDeleteVertexArrays(1, &g->lineVao);
            glDeleteVertexArrays(1, &g->textVao);
            glDeleteVertexArrays(1, &g->emptyVao);
            glDebugMessageCallback(nullptr, nullptr);
            Log::Info() << "Renderer: shut down (" << g->glErrors << " OpenGL errors reported)";
            g.reset();
        }

        uint64_t CreateObject(const MeshRenderer& renderer, const glm::mat4& world, uint32_t entityId) {
            if (!g) return 0;
            const uint64_t handle = ++g->nextHandle;
            g->objects[handle] = Object{ renderer, world, entityId };
            return handle;
        }
        void UpdateObject(uint64_t handle, const MeshRenderer& renderer, const glm::mat4& world) {
            if (!g) return;
            auto it = g->objects.find(handle);
            if (it == g->objects.end()) return;
            it->second.renderer = renderer;
            it->second.world = world;
        }
        void DestroyObject(uint64_t handle) { if (g) g->objects.erase(handle); }

        uint64_t CreateLight(const Light& light, const glm::vec3& position, const glm::vec3& direction) {
            if (!g) return 0;
            const uint64_t handle = ++g->nextHandle;
            g->lights[handle] = LightEntry{ light, position, direction };
            return handle;
        }
        void UpdateLight(uint64_t handle, const Light& light, const glm::vec3& position, const glm::vec3& direction) {
            if (!g) return;
            auto it = g->lights.find(handle);
            if (it != g->lights.end()) it->second = LightEntry{ light, position, direction };
        }
        void DestroyLight(uint64_t handle) { if (g) g->lights.erase(handle); }

        void SetCameras(const std::vector<CameraInput>& cameras) { if (g) g->cameras = cameras; }

        bool ReadPresented(std::vector<uint8_t>& rgba, int& width, int& height) {
            if (!g || !g->window.rendered || g->window.present.Id() == 0) return false;
            width = g->window.size.x;
            height = g->window.size.y;
            rgba.resize(size_t(width) * height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glGetTextureImage(g->window.present.Id(), 0, GL_RGBA, GL_UNSIGNED_BYTE, GLsizei(rgba.size()), rgba.data());
            return true;
        }

        RenderStats Stats() {
            if (!g) return {};
            RenderStats s = g->frameStats;
            s.meshObjects = uint32_t(g->objects.size());
            s.planes = 0;
            for (const auto& [handle, object] : g->objects) if (object.renderer.mesh == "Plane") ++s.planes;
            s.lights = uint32_t(g->lights.size());
            s.cameraBound = !g->cameras.empty();
            s.meshesLoaded = uint32_t(g->meshes.size());
            s.texturesResident = 0;
            for (const auto& [path, layer] : g->textureLayer) if (layer >= 0) ++s.texturesResident;
            s.texturesPending = uint32_t(g->textureTickets.size());
            return s;
        }

        void RenderFrame() {
            if (!g) return;
            const glm::ivec2 windowSize = GetFramebufferSize();
            if (windowSize.x <= 0 || windowSize.y <= 0) {   // minimized
                g->lines.clear();
                g->text.clear();
                return;
            }
            const RenderSettings& settings = g->settings;
            RenderStats stats;
            EnsureTargetSet(g->window, windowSize, "Window");
            EnsureShadowTargets();
            StreamTextures();

            // ---------------------------------------------------------- resolve objects (meshes load on demand)
            struct Item { uint32_t mesh; uint32_t material; const Object* object; glm::vec3 center; float radius; };
            std::vector<Item> items;
            items.reserve(g->objects.size());
            for (const auto& [handle, object] : g->objects) {
                if (!object.renderer.visible) continue;
                uint32_t mesh = 0;
                if (!ResolveMesh(object.renderer.mesh, mesh)) continue;
                uint32_t material = 0;
                if (!object.renderer.material.empty()) {
                    auto m = g->materialByName.find(object.renderer.material);
                    if (m != g->materialByName.end()) material = m->second;
                    else WarnOnce("material:" + object.renderer.material, "Renderer: unknown material '" + object.renderer.material + "' (using Default)");
                }
                items.push_back({ mesh, material, &object, glm::vec3(object.world[3]), g->meshes[mesh].radius * MaxScale(object.world) });
            }
            UploadMeshesIfDirty();

            // ---------------------------------------------------------- materials
            std::vector<MaterialGpu> materials;
            materials.reserve(g->materials.size());
            for (const MaterialDesc& m : g->materials) materials.push_back(ToGpu(m));
            g->materialBuffer.Upload(materials.data(), materials.size() * sizeof(MaterialGpu));

            // ---------------------------------------------------------- lights + shadow assignment
            const bool shadows = settings.shadowsEnabled;
            std::vector<LightGpu> lights;
            const LightEntry* dirShadowLight = nullptr;
            std::vector<const LightEntry*> spotShadowLights, pointShadowLights;
            for (const auto& [handle, entry] : g->lights) {
                const Light& l = entry.light;
                LightGpu gpu;
                gpu.positionRange = glm::vec4(entry.position, l.range);
                gpu.directionType = glm::vec4(glm::normalize(entry.direction), float(l.type == LightType::Directional ? 0 : l.type == LightType::Point ? 1 : 2));
                gpu.colorIntensity = glm::vec4(l.color, l.intensity);
                gpu.params = glm::vec4(std::cos(l.innerConeAngle), std::cos(l.outerConeAngle), -1.0f, 0.0f);
                if (shadows && l.castShadows) {
                    if (l.type == LightType::Directional && !dirShadowLight) { dirShadowLight = &entry; gpu.params.z = 0.0f; }
                    else if (l.type == LightType::Spot && spotShadowLights.size() < MAX_SPOT_SHADOWS) { gpu.params.z = float(spotShadowLights.size()); spotShadowLights.push_back(&entry); }
                    else if (l.type == LightType::Point && pointShadowLights.size() < MAX_POINT_SHADOWS) { gpu.params.z = float(pointShadowLights.size()); pointShadowLights.push_back(&entry); }
                }
                lights.push_back(gpu);
            }
            if (lights.empty()) lights.push_back(LightGpu{});   // keep the buffer non-empty
            g->lightBuffer.Upload(lights.data(), lights.size() * sizeof(LightGpu));

            // ---------------------------------------------------------- views
            struct View {
                const Internal::CameraInput* input;
                TargetSet* target;
                glm::ivec4 rect;           // pixels within the target
                glm::mat4 projection, viewProjection;
                size_t firstCommand = 0, commandCount = 0;
                FrameDataGpu frame;
            };
            std::vector<View> views;
            g->window.usedThisFrame = true;
            for (auto& [name, set] : g->offscreen) set.usedThisFrame = false;
            for (const Internal::CameraInput& input : g->cameras) {
                const Camera& cam = input.camera;
                TargetSet* target = &g->window;
                glm::ivec2 size = windowSize;
                if (!cam.target.empty()) {
                    target = &g->offscreen[cam.target];
                    size = glm::max(cam.targetSize, glm::ivec2(16));
                    EnsureTargetSet(*target, size, cam.target);
                    target->usedThisFrame = true;
                }
                const glm::vec4 vp = glm::clamp(cam.viewport, glm::vec4(0.0f), glm::vec4(1.0f));
                glm::ivec4 rect(int(vp.x * size.x), int(vp.y * size.y), int(vp.z * size.x), int(vp.w * size.y));
                if (rect.z <= 0 || rect.w <= 0) continue;
                View v{ &input, target, rect };
                const float aspect = float(rect.z) / float(rect.w);
                v.projection = MakeProjection(cam, aspect);
                v.viewProjection = v.projection * input.view;
                views.push_back(v);
            }

            // ---------------------------------------------------------- instances + indirect commands
            std::vector<InstanceGpu> instances;
            std::vector<DrawCommand> commands;
            auto append = [&](std::vector<const Item*>& list) {
                std::sort(list.begin(), list.end(), [](const Item* a, const Item* b) { return a->mesh < b->mesh; });
                const size_t first = commands.size();
                for (const Item* item : list) {
                    const Object& o = *item->object;
                    InstanceGpu gpu;
                    gpu.model = o.world;
                    gpu.normalMatrix = glm::mat4(glm::inverseTranspose(glm::mat3(o.world)));
                    gpu.scaleMaterial = glm::vec4(glm::length(glm::vec3(o.world[0])), glm::length(glm::vec3(o.world[1])), glm::length(glm::vec3(o.world[2])), float(item->material));
                    gpu.ids = glm::uvec4(o.entityId + 1, 0, 0, 0);
                    gpu.uvScale = glm::vec4(o.renderer.uvScale, 0.0f, 0.0f);
                    const MeshEntry& mesh = g->meshes[item->mesh];
                    if (commands.size() == first || commands.back().firstIndex != mesh.firstIndex) {
                        commands.push_back({ mesh.indexCount, 0, mesh.firstIndex, mesh.baseVertex, uint32_t(instances.size()) });
                    }
                    ++commands.back().instanceCount;
                    instances.push_back(gpu);
                }
                return std::pair<size_t, size_t>(first, commands.size() - first);
            };
            std::vector<const Item*> casters;
            for (const Item& item : items) if (item.object->renderer.castShadows) casters.push_back(&item);
            const auto [casterFirst, casterCount] = append(casters);
            bool firstWindowView = true;
            for (View& v : views) {
                const RenderMath::Frustum frustum = RenderMath::ExtractFrustum(v.viewProjection);
                std::vector<const Item*> visible;
                for (const Item& item : items) {
                    if (frustum.IntersectsSphere(item.center, item.radius)) visible.push_back(&item);
                    else ++stats.instancesCulled;
                }
                stats.instancesDrawn += uint32_t(visible.size());
                std::tie(v.firstCommand, v.commandCount) = append(visible);
                for (size_t i = v.firstCommand; i < v.firstCommand + v.commandCount; ++i) stats.trianglesDrawn += commands[i].count / 3 * commands[i].instanceCount;
                if (firstWindowView && v.target == &g->window) { stats.drawCommands = uint32_t(v.commandCount); firstWindowView = false; }
            }
            stats.views = uint32_t(views.size());
            if (instances.empty()) instances.push_back(InstanceGpu{});
            g->instanceBuffer.Upload(instances.data(), instances.size() * sizeof(InstanceGpu));
            if (!commands.empty()) g->indirectBuffer.Upload(commands.data(), commands.size() * sizeof(DrawCommand));

            // ---------------------------------------------------------- per-view frame data
            glm::mat4 spotMatrices[MAX_SPOT_SHADOWS]{};
            for (size_t i = 0; i < spotShadowLights.size(); ++i) {
                const LightEntry& e = *spotShadowLights[i];
                spotMatrices[i] = RenderMath::SpotShadowMatrix(e.position, e.direction, e.light.outerConeAngle, e.light.range);
            }
            const size_t stride = (sizeof(FrameDataGpu) + size_t(g->uboAlignment) - 1) / size_t(g->uboAlignment) * size_t(g->uboAlignment);
            std::vector<uint8_t> frameBytes(std::max<size_t>(views.size(), 1) * stride, 0);
            if (views.empty()) {   // tonemap still runs (clear colour): it needs the exposure
                FrameDataGpu f{};
                f.skyHorizon = glm::vec4(settings.skyColorHorizon, settings.exposure);
                std::memcpy(frameBytes.data(), &f, sizeof(f));
            }
            for (size_t i = 0; i < views.size(); ++i) {
                View& v = views[i];
                const Camera& cam = v.input->camera;
                const bool ortho = cam.projection == Projection::Orthographic;
                const glm::mat4 cameraWorld = glm::inverse(v.input->view);
                FrameDataGpu& f = v.frame;
                f = {};
                f.view = v.input->view;
                f.projection = v.projection;
                f.viewProjection = v.viewProjection;
                f.inverseViewProjection = glm::inverse(v.viewProjection);
                f.cameraPosition = glm::vec4(glm::vec3(cameraWorld[3]), Time::TotalTime());
                f.skyZenith = glm::vec4(settings.skyColorZenith, settings.ambientIntensity);
                f.skyHorizon = glm::vec4(settings.skyColorHorizon, settings.exposure);
                f.groundColor = glm::vec4(settings.groundColor, float(g->lights.size()));
                f.fogColor = glm::vec4(settings.fog.color, settings.fog.enabled && !ortho ? 1.0f : 0.0f);
                f.fogParams = glm::vec4(settings.fog.distanceDensity, settings.fog.heightDensity, settings.fog.heightFalloff, settings.fog.heightBase);
                f.fogParams2 = glm::vec4(settings.fog.maxOpacity, shadows ? 1.0f : 0.0f, 0.0f, ortho ? 1.0f : 0.0f);
                f.ambientUp = glm::vec4(settings.ambientUp, 0.0f);
                f.ambientDown = glm::vec4(settings.ambientDown, 0.0f);
                f.rim = glm::vec4(settings.rimColor * settings.rimIntensity, settings.rimPower);
                if (dirShadowLight) {
                    const float aspect = float(v.rect.z) / float(v.rect.w);
                    const float shadowFar = ortho ? std::min(cam.farPlane, cam.nearPlane + 2.0f * settings.shadowDistance)
                                                  : std::min(cam.farPlane, settings.shadowDistance);
                    const auto corners = CornersOf(MakeProjection(cam, aspect, shadowFar) * v.input->view);
                    f.dirShadowMatrix = RenderMath::DirectionalShadowMatrix(dirShadowLight->direction, corners, g->shadowSize);
                    const float row0 = glm::length(glm::vec3(f.dirShadowMatrix[0][0], f.dirShadowMatrix[1][0], f.dirShadowMatrix[2][0]));
                    f.fogParams2.z = 2.0f / (row0 * float(g->shadowSize));   // world size of one shadow texel
                }
                for (int s = 0; s < MAX_SPOT_SHADOWS; ++s) f.spotShadowMatrices[s] = spotMatrices[s];
                std::memcpy(frameBytes.data() + i * stride, &f, sizeof(f));
            }
            g->frameUbo.Upload(frameBytes.data(), frameBytes.size());

            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g->lightBuffer.Id());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g->instanceBuffer.Id());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g->materialBuffer.Id());
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g->indirectBuffer.Id());
            glBindVertexArray(g->meshVao);
            if (!views.empty()) glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), 0, sizeof(FrameDataGpu));

            // ---------------------------------------------------------- shadow maps
            const float one = 1.0f;
            auto renderShadowView = [&](const GL::Program& program, const glm::mat4& viewProj, int size) {
                g->shadowFbo.SetDrawBuffers(0);
                glBindFramebuffer(GL_FRAMEBUFFER, g->shadowFbo.Id());
                glViewport(0, 0, size, size);
                glClearNamedFramebufferfv(g->shadowFbo.Id(), GL_DEPTH, 0, &one);
                glUniformMatrix4fv(program.Uniform("u_lightViewProjection"), 1, GL_FALSE, &viewProj[0][0]);
                DrawCommands(casterFirst, casterCount);
                ++stats.shadowViews;
            };
            auto beginShadowPass = [&] {
                glBindVertexArray(g->meshVao);   // earlier passes (sky, lines) leave other vertex arrays bound
                glDisable(GL_SCISSOR_TEST);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_CULL_FACE);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.5f, 2.0f);
            };
            if (shadows && casterCount > 0 && !views.empty()) {
                beginShadowPass();
                const GL::Program& shadow = g->programs[Shaders::SHADOW];
                shadow.Use();
                for (size_t i = 0; i < spotShadowLights.size(); ++i) {
                    g->shadowFbo.AttachDepth(g->spotShadows, int(i));
                    renderShadowView(shadow, spotMatrices[i], g->shadowSize / 2);
                }
                if (!pointShadowLights.empty()) {
                    glDisable(GL_POLYGON_OFFSET_FILL);   // linear distance is written explicitly
                    const GL::Program& point = g->programs[Shaders::SHADOW_POINT];
                    point.Use();
                    for (size_t i = 0; i < pointShadowLights.size(); ++i) {
                        const LightEntry& e = *pointShadowLights[i];
                        const glm::vec4 positionRange(e.position, e.light.range);
                        glUniform4fv(point.Uniform("u_lightPositionRange"), 1, &positionRange[0]);
                        const auto faces = RenderMath::PointShadowMatrices(e.position, 0.05f, e.light.range);
                        for (int face = 0; face < 6; ++face) {
                            g->shadowFbo.AttachDepth(g->pointShadows, int(i) * 6 + face);
                            renderShadowView(point, faces[face], g->shadowSize / 4);
                        }
                    }
                }
                glDisable(GL_POLYGON_OFFSET_FILL);
            }

            // ---------------------------------------------------------- clear targets used this frame
            const glm::vec4 clear(settings.clearColor.r, settings.clearColor.g, settings.clearColor.b, 1.0f);
            const GLuint noEntity[4] = { 0, 0, 0, 0 };
            glDisable(GL_SCISSOR_TEST);
            glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            auto clearSet = [&](TargetSet& t) {
                glClearNamedFramebufferfv(t.mainFbo.Id(), GL_COLOR, 0, &clear[0]);
                glClearNamedFramebufferuiv(t.mainFbo.Id(), GL_COLOR, 1, noEntity);
                glClearNamedFramebufferfv(t.mainFbo.Id(), GL_DEPTH, 0, &one);
            };
            clearSet(g->window);
            for (auto& [name, set] : g->offscreen) if (set.usedThisFrame) clearSet(set);

            // ---------------------------------------------------------- views
            for (size_t i = 0; i < views.size(); ++i) {
                View& v = views[i];
                glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), GLintptr(i * stride), sizeof(FrameDataGpu));
                if (shadows && dirShadowLight && casterCount > 0) {
                    beginShadowPass();
                    const GL::Program& shadow = g->programs[Shaders::SHADOW];
                    shadow.Use();
                    g->shadowFbo.AttachDepth(g->dirShadow);
                    renderShadowView(shadow, v.frame.dirShadowMatrix, g->shadowSize);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                }

                const GLuint fbo = v.target->mainFbo.Id();
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glViewport(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                glScissor(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                glEnable(GL_SCISSOR_TEST);
                glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &one);   // views may overlap: each starts with empty depth
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glBindTextureUnit(4, g->dirShadow.Id());
                glBindTextureUnit(5, g->spotShadows.Id());
                glBindTextureUnit(6, g->pointShadows.Id());
                glBindTextureUnit(7, g->fontAtlas.Id());
                glBindTextureUnit(8, g->albedoArray.Id());
                glBindVertexArray(g->meshVao);
                g->programs[Shaders::FORWARD].Use();
                DrawCommands(v.firstCommand, v.commandCount);

                // sky (far plane, only where nothing was drawn) and debug lines: colour only
                glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);
                glDepthFunc(GL_LEQUAL);
                glDisable(GL_CULL_FACE);
                g->programs[Shaders::SKY].Use();
                glBindVertexArray(g->emptyVao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                if (!g->lines.empty() && v.target == &g->window) {
                    g->lineBuffer.Upload(g->lines.data(), g->lines.size() * sizeof(LineVertex));
                    g->programs[Shaders::LINES].Use();
                    glBindVertexArray(g->lineVao);
                    glVertexArrayVertexBuffer(g->lineVao, 0, g->lineBuffer.Id(), 0, sizeof(LineVertex));
                    glDrawArrays(GL_LINES, 0, GLsizei(g->lines.size()));
                }
                glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glDepthFunc(GL_LESS);
                glDisable(GL_SCISSOR_TEST);
            }

            // ---------------------------------------------------------- resolve + tonemap every used target
            auto finish = [&](TargetSet& t) {
                const GLuint fbo = t.mainFbo.Id();
                const glm::ivec2 s = t.size;
                glNamedFramebufferReadBuffer(fbo, GL_COLOR_ATTACHMENT0);
                glBlitNamedFramebuffer(fbo, t.resolveColorFbo.Id(), 0, 0, s.x, s.y, 0, 0, s.x, s.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glNamedFramebufferReadBuffer(fbo, GL_COLOR_ATTACHMENT1);
                glBlitNamedFramebuffer(fbo, t.resolveIdFbo.Id(), 0, 0, s.x, s.y, 0, 0, s.x, s.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glNamedFramebufferReadBuffer(fbo, GL_COLOR_ATTACHMENT0);
                glBindFramebuffer(GL_FRAMEBUFFER, t.ldrFbo.Id());
                glViewport(0, 0, s.x, s.y);
                glDisable(GL_DEPTH_TEST);
                g->programs[Shaders::TONEMAP].Use();
                glBindTextureUnit(0, t.hdrResolved.Id());
                glBindVertexArray(g->emptyVao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                t.rendered = true;
            };
            if (views.empty()) glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), 0, sizeof(FrameDataGpu));   // exposure for tonemap
            for (auto& [name, set] : g->offscreen) if (set.usedThisFrame) finish(set);
            finish(g->window);

            // Compose the presented image (scene + overlay text) in a renderer-owned texture, then show it.
            const GLuint present = g->window.presentFbo.Id();
            glBlitNamedFramebuffer(g->window.ldrFbo.Id(), present, 0, 0, windowSize.x, windowSize.y, 0, 0, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            DrawTextPass(present, windowSize);
            glBlitNamedFramebuffer(present, 0, 0, 0, windowSize.x, windowSize.y, 0, 0, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
            g->lines.clear();
            g->text.clear();
            g->frameStats = stats;
        }
    }
}
