// Forward PBR renderer (OpenGL 4.6 tier, or the 4.1 tier: see render/gl/GLObjects.h; clean-room).
//
// Frame:
//   1. stream in textures/meshes requested on demand; upload materials and lights
//   2. build one instance buffer + indirect command buffer: shadow casters, then every view's
//      frustum-culled objects (sorted by mesh: one DrawElementsIndirectCommand per mesh)
//   3. spot / point shadow maps (shared by all views)
//   4. per view (camera): directional shadow fit -> viewport/scissor into its target set ->
//      forward PBR pass (HDR + entity id) -> sky -> debug lines
//      (+ additive VFX ribbons after the sky)
//   5. per target set: MSAA resolve -> [window: depth resolve, projected decals, SSAO, bloom mip
//      chain] -> tonemap into LDR. The window set renders at renderScale x the window size and is
//      upscaled (EASU + RCAS, or bilinear) to the window, then overlay text is drawn on top and the
//      result is blitted to the default framebuffer.
// Shader hot reload: engine/shaders edits replace the embedded sources and rebuild the programs.
// 4.1 tier: CPU culling, one instanced draw per command (u_baseInstance), storage read through
// texture buffers, no Hi-Z; particles / XPBD / fields come from the CPU compute backend.
#include "render/RendererInternal.h"
#include "render/RenderMath.h"
#include "render/ShaderCache.h"
#include "render/ShaderPrograms.h"
#include "render/ShaderPreprocessor.h"
#include "render/gl/GLObjects.h"
#include "render/gl/RingBuffer.h"
#include "compute/AdvancedWorld.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Render/Upscaler.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Phisiks::Compute::Internal { void ReleaseBackends(); }

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
        struct MaterialGpu { glm::vec4 baseRoughness, patternMetallic, emissivePattern, params, textureParams, splat[4], splatDetail; };
        static_assert(sizeof(MaterialGpu) == 160, "std430 layout");
        struct RibbonVertex { glm::vec4 positionSide, tangentWidth, color; };
        struct DrawCommand { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };
        struct CullObjectGpu { glm::vec4 sphere; glm::uvec4 meshInstance; };
        struct SdfPrimitiveGpu { glm::vec4 positionShape, rotation, params, op; };
        struct LineVertex { glm::vec3 position; glm::vec4 color; };
        struct TextVertex { glm::vec2 position; glm::vec2 texel; glm::vec4 color; };

        struct MeshEntry { uint32_t firstIndex = 0, indexCount = 0; int32_t baseVertex = 0; float radius = 0.0f; uint32_t vertexCount = 0; };
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
            glm::ivec2 outputSize{ 0 };        // window only: upscaled image (when size < outputSize)
            GL::Texture upscaled, output;
            GL::Framebuffer upscaledFbo, outputFbo;
            // window only: post-processing
            GL::Texture depthResolved, aoRaw, aoBlur;
            GL::Framebuffer depthResolveFbo, aoRawFbo, aoBlurFbo;
            glm::ivec2 postSize{ 0 }, aoSize{ 0 };
            std::vector<GL::Texture> bloomMips;
            std::vector<GL::Framebuffer> bloomFbos;
            std::vector<glm::ivec2> bloomSizes;
            bool upscaledThisFrame = false;
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

            // persistent-mapped per-frame uploads (instances, cull objects) + remap lists
            GL::RingBuffer ring;
            GLint ssboAlignment = 16;
            GL::Buffer remapBuffer;

            // GPU culling statistics: one counter buffer per frame in flight, read back (never waiting) as
            // soon as its fence has signalled. The driver may queue ~3 frames, so keep more slots than that.
            static constexpr int CULL_SLOTS = 6;
            GLuint cullStats[CULL_SLOTS] = {};
            GLsync cullFences[CULL_SLOTS] = {};
            uint64_t cullFrame = 0;
            uint32_t gpuVisible = 0, gpuFrustumCulled = 0, gpuOcclusionCulled = 0;

            // Hi-Z pyramid of the window (previous frame) for occlusion culling
            GL::Texture hizDepth, hiz;
            GL::Framebuffer hizDepthFbo;
            glm::ivec2 hizSize{ 0 };
            int hizLevels = 0;
            bool hizValid = false;
            std::vector<glm::mat4> hizViewProjections;   // window views of the frame the pyramid was built from
            std::vector<glm::ivec4> hizRects;

            // visualisers
            SdfVisualization sdf;
            GL::Buffer sdfBuffer;
            FieldVisualization fieldViz;

            RenderStats frameStats;
            uint32_t glErrors = 0;
            uint32_t loggedDebugMessages = 0;

            // feature tier
            bool compat = false;
            int contextVersion = 0;
            std::string rendererName, vendorName;
            GL::TextureBuffer tboLights, tboInstances, tboMaterials, tboRemap, tboAux, tboAux2;
            std::vector<DrawCommand> cpuCommands;              // 4.1: drawn one by one
            std::vector<std::vector<uint8_t>> albedoImages;    // 4.1: re-uploaded when the array grows

            // VFX primitives of this frame
            std::vector<RibbonVertex> ribbons;
            uint32_t ribbonStrips = 0;
            GL::Buffer ribbonBuffer;
            GLuint ribbonVao = 0;
            std::vector<DecalDesc> decals;
            GL::Texture black;                 // 1x1 (bloom off)

            // shader hot reload
            std::string hotReloadDirectory;
            std::map<std::string, std::filesystem::file_time_type> hotReloadStamps;
            double hotReloadNextPoll = 0.0;
            ShaderReloadStatus reloadStatus;

            // synthetic vision (aux G-buffer of the first window view)
            bool visionRequested = false, visionReady = false;
            VisionCapture vision;
            glm::ivec2 auxSize{ 0 };
            GL::Texture auxNormal, auxDepth, auxId, auxZ;
            GL::Framebuffer auxFbo;
        };
        std::unique_ptr<State> g;

        void GLAD_API_PTR OnDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || !g) return;
            // Compile failures are reported (and handled) through the compile status: a hot-reloaded
            // shader with a typo is not an API error.
            if (type == GL_DEBUG_TYPE_ERROR && source != GL_DEBUG_SOURCE_SHADER_COMPILER) ++g->glErrors;
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
                if (g->compat && Shaders::IsCompute(int(i))) continue;   // 4.1: CPU compute backend instead
                std::vector<GL::StageSource> stages;
                ShaderPreprocessor::ResourceBindings bindings;
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
                    const GLenum glStage = stage.stage == Shaders::Stage::Vertex ? GLenum(GL_VERTEX_SHADER)
                                        : stage.stage == Shaders::Stage::Fragment ? GLenum(GL_FRAGMENT_SHADER) : GLenum(GL_COMPUTE_SHADER);
                    stages.push_back({ glStage, g->compat ? ShaderPreprocessor::RewriteForGL41(expanded.source, bindings) : expanded.source });
                }
                const GLuint program = g->shaderCache.GetProgram(defs[i].name, std::move(stages));
                if (!program) return false;
                if (g->compat) GL::ApplyBindings(program, bindings);
                g->programs[i].Adopt(program, defs[i].name);
            }
            return true;
        }

        void UploadMeshesIfDirty() {
            if (!g->meshesDirty) return;
            g->vertexBuffer.Upload(g->vertices.data(), g->vertices.size() * sizeof(Vertex));
            g->indexBuffer.Upload(g->indices.data(), g->indices.size() * sizeof(uint32_t));
            GL::SetVertexBuffer(g->meshVao, g->vertexBuffer.Id(), sizeof(Vertex), {
                { 0, 3, GL_FLOAT, offsetof(Vertex, position) }, { 1, 3, GL_FLOAT, offsetof(Vertex, normal) }, { 2, 2, GL_FLOAT, offsetof(Vertex, uv) } });
            GL::SetElementBuffer(g->meshVao, g->indexBuffer.Id());
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

        // Window only: EASU output + RCAS output at the window size (when the scene renders smaller).
        void EnsureUpscaleTargets(TargetSet& t, glm::ivec2 outputSize) {
            if (t.outputSize == outputSize && t.output.Id() != 0) return;
            t.outputSize = outputSize;
            GL::TextureDesc d;
            d.width = outputSize.x;
            d.height = outputSize.y;
            d.format = GL_RGBA8;
            d.minFilter = d.magFilter = GL_NEAREST;
            t.upscaled.Create("Upscaled (EASU)", d);
            t.output.Create("Upscaled (RCAS)", d);
            t.upscaledFbo.Create("Upscaled (EASU)");
            t.upscaledFbo.AttachColor(0, t.upscaled);
            t.upscaledFbo.SetDrawBuffers(1);
            t.outputFbo.Create("Upscaled (RCAS)");
            t.outputFbo.AttachColor(0, t.output);
            t.outputFbo.SetDrawBuffers(1);
            if (!t.present.Id() || t.present.Desc().width != outputSize.x || t.present.Desc().height != outputSize.y) {
                d.minFilter = d.magFilter = GL_LINEAR;
                t.present.Create("Presented frame", d);
                t.presentFbo.Create("Present");
                t.presentFbo.AttachColor(0, t.present);
                t.presentFbo.SetDrawBuffers(1);
            }
        }

        glm::ivec2 SceneSize(glm::ivec2 window) {
            const float scale = std::clamp(g->settings.renderScale, 0.25f, 1.0f);
            if (scale >= 0.999f) return window;
            return glm::max(glm::ivec2(glm::round(glm::vec2(window) * scale)), glm::ivec2(16));
        }

        // EASU (or bilinear) from `source` (sourceSize) into `upscaledFbo`, then RCAS into `outputFbo`.
        void RunUpscaler(GLuint sourceTexture, glm::ivec2 sourceSize, GLuint upscaledFbo, GLuint upscaledTexture, GLuint outputFbo, glm::ivec2 outputSize,
                         Upscaler upscaler, float sharpness, GLuint sourceFbo) {
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glBindVertexArray(g->emptyVao);
            if (upscaler == Upscaler::FSR1) {
                glBindFramebuffer(GL_FRAMEBUFFER, upscaledFbo);
                glViewport(0, 0, outputSize.x, outputSize.y);
                const GL::Program& easu = g->programs[Shaders::EASU];
                easu.Use();
                glUniform2f(easu.Uniform("u_inputSize"), float(sourceSize.x), float(sourceSize.y));
                glUniform2f(easu.Uniform("u_outputSize"), float(outputSize.x), float(outputSize.y));
                GL::BindTexture(0, GL_TEXTURE_2D, sourceTexture);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindFramebuffer(GL_FRAMEBUFFER, outputFbo);
                const GL::Program& rcas = g->programs[Shaders::RCAS];
                rcas.Use();
                glUniform1f(rcas.Uniform("u_sharpness"), std::exp2(-std::max(sharpness, 0.0f)));
                GL::BindTexture(0, GL_TEXTURE_2D, upscaledTexture);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            else {
                GL::Blit(sourceFbo, outputFbo, sourceSize.x, sourceSize.y, outputSize.x, outputSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            }
        }

        void EnsureAuxTargets(glm::ivec2 size) {
            if (g->auxSize == size) return;
            g->auxSize = size;
            GL::TextureDesc d;
            d.width = size.x;
            d.height = size.y;
            d.minFilter = d.magFilter = GL_NEAREST;
            d.format = GL_RGBA16F;            g->auxNormal.Create("Vision normals", d);
            d.format = GL_R32F;               g->auxZ.Create("Vision linear depth", d);
            d.format = GL_R32UI;              g->auxId.Create("Vision ids", d);
            d.format = GL_DEPTH_COMPONENT32F; g->auxDepth.Create("Vision depth", d);
            g->auxFbo.Create("Vision G-buffer");
            g->auxFbo.AttachColor(0, g->auxNormal);
            g->auxFbo.AttachColor(1, g->auxZ);
            g->auxFbo.AttachColor(2, g->auxId);
            g->auxFbo.AttachDepth(g->auxDepth);
            g->auxFbo.SetDrawBuffers(3);
            g->auxFbo.Check("Vision G-buffer");
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
            if (!g->compat) grown.SetParameter(GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
            if (g->compat) {   // no glCopyImageSubData on 4.1: re-upload the kept images
                for (int layer = 0; layer < g->albedoUsed && layer < int(g->albedoImages.size()); ++layer)
                    grown.UploadLayer(layer, GL_RGBA, GL_UNSIGNED_BYTE, g->albedoImages[layer].data());
                if (g->albedoUsed > 0) grown.GenerateMipmaps();
            }
            else if (g->albedoUsed > 0) {
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
                    g->albedoArray.UploadLayer(layer, GL_RGBA, GL_UNSIGNED_BYTE, upload.rgba.data());
                    g->albedoArray.GenerateMipmaps();
                    if (g->compat) g->albedoImages.push_back(upload.rgba);
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

        // Attribute layouts are set with the vertex buffer (GL::SetVertexBuffer) where it is bound.
        void SetupVertexArrays() {
            g->meshVao = GL::CreateVertexArray();
            g->ribbonVao = GL::CreateVertexArray();
            g->lineVao = GL::CreateVertexArray();
            g->textVao = GL::CreateVertexArray();
            g->emptyVao = GL::CreateVertexArray();
        }

        void SetLineBuffer() {
            GL::SetVertexBuffer(g->lineVao, g->lineBuffer.Id(), sizeof(LineVertex), {
                { 0, 3, GL_FLOAT, offsetof(LineVertex, position) }, { 1, 4, GL_FLOAT, offsetof(LineVertex, color) } });
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
            out.textureParams = glm::vec4(float(layer), m.textureScale, m.textureUsesMeshUV ? 1.0f : 0.0f, 0.0f);
            for (int k = 0; k < 4; ++k) out.splat[k] = glm::vec4(m.splat[k].color, m.splat[k].roughness);
            out.splatDetail = glm::vec4(m.splat[0].detail, m.splat[1].detail, m.splat[2].detail, m.splat[3].detail);
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

        uint32_t FloatBits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

        void BindSoftBuffers(const Advanced::SoftBodyDraw& d) {
            if (g->compat) {
                g->tboAux.Bind(13, GL_RGBA32F, d.positions);
                g->tboAux2.Bind(14, GL_RGBA32F, d.normals);
            }
            else {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, d.positions);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, d.normals);
            }
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
            g->fontAtlas.Bind(0);
            GL::SetVertexBuffer(g->textVao, g->textBuffer.Id(), sizeof(TextVertex), {
                { 0, 2, GL_FLOAT, offsetof(TextVertex, position) }, { 1, 2, GL_FLOAT, offsetof(TextVertex, texel) }, { 2, 4, GL_FLOAT, offsetof(TextVertex, color) } });
            glBindVertexArray(g->textVao);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(g->text.size()));
            glDisable(GL_BLEND);
        }

        void DrawCommands(const GL::Program& program, size_t firstCommand, size_t count) {
            if (count == 0) return;
            if (!g->compat) {
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(firstCommand * sizeof(DrawCommand)), GLsizei(count), 0);
                return;
            }
            const GLint base = program.Uniform("u_baseInstance");
            for (size_t c = firstCommand; c < firstCommand + count && c < g->cpuCommands.size(); ++c) {
                const DrawCommand& d = g->cpuCommands[c];
                if (d.instanceCount == 0) continue;
                glUniform1ui(base, d.baseInstance);
                glDrawElementsInstancedBaseVertex(GL_TRIANGLES, GLsizei(d.count), GL_UNSIGNED_INT, reinterpret_cast<const void*>(size_t(d.firstIndex) * sizeof(uint32_t)),
                                                  GLsizei(d.instanceCount), d.baseVertex);
            }
        }

        // Window: the shown resolution (ids of an upscaled frame are nearest-sampled from the scene).
        bool Capture(const TargetSet& t, FrameCapture& out) {
            if (!t.rendered || t.size.x <= 0) return false;
            const bool upscaled = t.upscaledThisFrame;
            out.width = upscaled ? t.outputSize.x : t.size.x;
            out.height = upscaled ? t.outputSize.y : t.size.y;
            const size_t pixels = size_t(out.width) * size_t(out.height);
            out.rgba.resize(pixels * 4);
            (upscaled ? t.output : t.ldr).Read(GL_RGBA, GL_UNSIGNED_BYTE, out.rgba.size(), out.rgba.data());
            std::vector<uint32_t> ids(size_t(t.size.x) * size_t(t.size.y));
            t.idResolved.Read(GL_RED_INTEGER, GL_UNSIGNED_INT, ids.size() * 4, ids.data());
            if (!upscaled) { out.entityIds = std::move(ids); return true; }
            out.entityIds.resize(pixels);
            for (int y = 0; y < out.height; ++y) {
                const int sy = std::min(int((y + 0.5f) * t.size.y / out.height), t.size.y - 1);
                for (int x = 0; x < out.width; ++x) {
                    const int sx = std::min(int((x + 0.5f) * t.size.x / out.width), t.size.x - 1);
                    out.entityIds[size_t(y) * out.width + x] = ids[size_t(sy) * t.size.x + sx];
                }
            }
            return true;
        }

        // ------------------------------------------------------------------ post-processing targets (window)
        void EnsurePostTargets(TargetSet& t, int bloomLevels, glm::ivec2 aoSize) {
            GL::TextureDesc d;
            d.minFilter = d.magFilter = GL_NEAREST;
            if (t.postSize != t.size) {
                t.postSize = t.size;
                d.width = t.size.x;
                d.height = t.size.y;
                d.format = GL_DEPTH_COMPONENT32F;
                t.depthResolved.Create("Window depth (resolved)", d);
                t.depthResolveFbo.Create("Window depth resolve");
                t.depthResolveFbo.AttachDepth(t.depthResolved);
                t.depthResolveFbo.SetDrawBuffers(0);
                t.bloomMips.clear();
                t.bloomFbos.clear();
                t.bloomSizes.clear();
                t.aoSize = glm::ivec2(0);
            }
            if (aoSize.x > 0 && t.aoSize != aoSize) {
                t.aoSize = aoSize;
                d.width = aoSize.x;
                d.height = aoSize.y;
                d.format = GL_R8;
                d.minFilter = d.magFilter = GL_LINEAR;
                t.aoRaw.Create("SSAO", d);
                t.aoBlur.Create("SSAO (blurred)", d);
                t.aoRawFbo.Create("SSAO");
                t.aoRawFbo.AttachColor(0, t.aoRaw);
                t.aoRawFbo.SetDrawBuffers(1);
                t.aoBlurFbo.Create("SSAO blur");
                t.aoBlurFbo.AttachColor(0, t.aoBlur);
                t.aoBlurFbo.SetDrawBuffers(1);
            }
            if (int(t.bloomMips.size()) < bloomLevels) {
                d.format = GL_RGBA16F;
                d.minFilter = d.magFilter = GL_LINEAR;
                for (int i = int(t.bloomMips.size()); i < bloomLevels; ++i) {
                    const glm::ivec2 size = glm::max(t.size >> (i + 1), glm::ivec2(1));
                    d.width = size.x;
                    d.height = size.y;
                    GL::Texture texture;
                    texture.Create("Bloom " + std::to_string(i), d);
                    GL::Framebuffer fbo;
                    fbo.Create("Bloom " + std::to_string(i));
                    fbo.AttachColor(0, texture);
                    fbo.SetDrawBuffers(1);
                    t.bloomMips.push_back(std::move(texture));
                    t.bloomFbos.push_back(std::move(fbo));
                    t.bloomSizes.push_back(size);
                }
            }
        }

        // ------------------------------------------------------------------ hot reload
        // Rebuilds every program from the current sources (overrides first). A failed program keeps
        // its previous version. Returns the number of failures.
        uint32_t RebuildPrograms(uint32_t& rebuilt, std::string& lastError) {
            const auto resolver = [](const std::string& name) { return Shaders::Find(name); };
            const std::vector<Shaders::ProgramDef>& defs = Shaders::Programs();
            uint32_t failures = 0;
            rebuilt = 0;
            for (size_t i = 0; i < defs.size(); ++i) {
                if (g->compat && Shaders::IsCompute(int(i))) continue;
                std::vector<GL::StageSource> stages;
                ShaderPreprocessor::ResourceBindings bindings;
                bool ok = true;
                for (const Shaders::StageDef& stage : defs[i].stages) {
                    const std::string* source = Shaders::Find(stage.file);
                    if (!source) { ok = false; lastError = std::string("missing ") + stage.file; break; }
                    const ShaderPreprocessor::Result expanded = ShaderPreprocessor::Expand(*source, resolver, defs[i].defines);
                    if (!expanded.ok) { ok = false; lastError = std::string(stage.file) + ": " + expanded.error; break; }
                    const GLenum glStage = stage.stage == Shaders::Stage::Vertex ? GLenum(GL_VERTEX_SHADER)
                                        : stage.stage == Shaders::Stage::Fragment ? GLenum(GL_FRAGMENT_SHADER) : GLenum(GL_COMPUTE_SHADER);
                    stages.push_back({ glStage, g->compat ? ShaderPreprocessor::RewriteForGL41(expanded.source, bindings) : expanded.source });
                }
                const GLuint program = ok ? GL::BuildProgram(defs[i].name, stages, false) : 0;
                if (!program) {
                    ++failures;
                    if (ok) lastError = std::string("program '") + defs[i].name + "' failed to compile (previous version kept; see engine.log)";
                    continue;
                }
                if (g->compat) GL::ApplyBindings(program, bindings);
                g->programs[i].Adopt(program, defs[i].name);
                ++rebuilt;
            }
            return failures;
        }

        bool ReadText(const std::filesystem::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream s;
            s << in.rdbuf();
            out = s.str();
            return true;
        }

        // Scans the watched directory; changed files become overrides. Returns true if any changed.
        bool ScanHotReload() {
            namespace fs = std::filesystem;
            std::error_code ec;
            bool changed = false;
            uint32_t overridden = 0;
            for (const fs::directory_entry& entry : fs::directory_iterator(g->hotReloadDirectory, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".glsl") continue;
                const std::string name = entry.path().filename().string();
                // Content is compared every scan (cheap: a few hundred KB) so saves within the
                // file-system timestamp resolution are not missed.
                g->hotReloadStamps[name] = entry.last_write_time(ec);
                ++overridden;
                std::string text;
                if (!ReadText(entry.path(), text)) continue;
                const std::string* current = Shaders::Find(name);
                if (current && *current == text) continue;
                Shaders::SetOverride(name, text);
                Log::Info() << "Renderer: shader '" << name << "' changed on disk";
                changed = true;
            }
            g->reloadStatus.filesOverridden = uint32_t(Shaders::OverrideCount());
            (void)overridden;
            return changed;
        }

        bool ApplyHotReload() {
            uint32_t rebuilt = 0;
            std::string error;
            const uint32_t errorsBefore = g->glErrors;
            const uint32_t failures = RebuildPrograms(rebuilt, error);
            if (failures) g->glErrors = errorsBefore;   // some drivers also raise the compile failure as a debug "error"
            ShaderReloadStatus& s = g->reloadStatus;
            ++s.reloads;
            s.programsRebuilt += rebuilt;
            s.failures += failures;
            if (failures) s.lastError = error;
            Log::Info() << "Renderer: shader hot reload: " << rebuilt << " programs rebuilt, " << failures << " failed";
            return failures == 0;
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
        entry.vertexCount = uint32_t(data.vertices.size());
        g->vertices.insert(g->vertices.end(), data.vertices.begin(), data.vertices.end());
        g->indices.insert(g->indices.end(), data.indices.begin(), data.indices.end());
        g->meshByName[name] = uint32_t(g->meshes.size());
        g->meshes.push_back(entry);
        g->meshesDirty = true;
        return true;
    }
    bool HasMesh(const std::string& name) { return g && g->meshByName.count(name) != 0; }

    bool UpdateMeshVertices(const std::string& name, const std::vector<Vertex>& vertices) {
        if (!g) return false;
        auto it = g->meshByName.find(name);
        if (it == g->meshByName.end()) return false;
        MeshEntry& e = g->meshes[it->second];
        if (vertices.size() != e.vertexCount) return false;
        std::copy(vertices.begin(), vertices.end(), g->vertices.begin() + e.baseVertex);
        float radius = 0.0f;
        for (const Vertex& v : vertices) radius = std::max(radius, glm::length(v.position));
        e.radius = radius;
        if (!g->meshesDirty && g->vertexBuffer.Id() != 0)
            GL::BufferSubData(g->vertexBuffer.Id(), size_t(e.baseVertex) * sizeof(Vertex), vertices.size() * sizeof(Vertex), vertices.data());
        return true;
    }

    bool RegisterTexture(const std::string& name, int width, int height, const std::vector<uint8_t>& rgba) {
        if (!g || name.empty() || width <= 0 || height <= 0 || rgba.size() < size_t(width) * size_t(height) * 4) return false;
        Assets::Image image;
        image.width = width;
        image.height = height;
        image.rgba.assign(rgba.begin(), rgba.begin() + size_t(width) * height * 4);
        Assets::Image sized;
        if (width != TEXTURE_SIZE || height != TEXTURE_SIZE) Assets::ResizeImage(image, TEXTURE_SIZE, TEXTURE_SIZE, sized);
        const Assets::Image& upload = sized.rgba.empty() ? image : sized;
        int layer;
        auto it = g->textureLayer.find(name);
        if (it != g->textureLayer.end() && it->second >= 0) layer = it->second;
        else {
            EnsureAlbedoCapacity(g->albedoUsed + 1);
            layer = g->albedoUsed++;
            if (g->compat) g->albedoImages.resize(size_t(layer) + 1);
        }
        g->albedoArray.UploadLayer(layer, GL_RGBA, GL_UNSIGNED_BYTE, upload.rgba.data());
        g->albedoArray.GenerateMipmaps();
        if (g->compat) g->albedoImages[size_t(layer)] = upload.rgba;
        g->textureLayer[name] = layer;
        g->textureTickets.erase(name);
        return true;
    }

    void DrawRibbon(const std::vector<RibbonPoint>& points) {
        if (!g || points.size() < 2) return;
        const size_t n = points.size();
        auto tangent = [&](size_t i) {
            const glm::vec3 d = points[std::min(i + 1, n - 1)].position - points[i > 0 ? i - 1 : 0].position;
            const float len = glm::length(d);
            return len > 1e-6f ? d / len : glm::vec3(1.0f, 0.0f, 0.0f);
        };
        auto vertex = [&](size_t i, float side, const glm::vec3& t) {
            return RibbonVertex{ glm::vec4(points[i].position, side), glm::vec4(t, points[i].width), points[i].color };
        };
        glm::vec3 t0 = tangent(0);
        for (size_t i = 0; i + 1 < n; ++i) {
            const glm::vec3 t1 = tangent(i + 1);
            const RibbonVertex a0 = vertex(i, -1.0f, t0), a1 = vertex(i, 1.0f, t0), b0 = vertex(i + 1, -1.0f, t1), b1 = vertex(i + 1, 1.0f, t1);
            g->ribbons.insert(g->ribbons.end(), { a0, a1, b1, a0, b1, b0 });
            t0 = t1;
        }
        ++g->ribbonStrips;
    }

    void DrawDecal(const DecalDesc& decal) { if (g) g->decals.push_back(decal); }

    bool CaptureBackbuffer(FrameCapture& out) {
        if (!g || !Platform::HasWindow()) return false;
        const glm::ivec2 size = GetFramebufferSize();
        if (size.x <= 0 || size.y <= 0) return false;
        out.width = size.x;
        out.height = size.y;
        out.rgba.resize(size_t(size.x) * size.y * 4);
        out.entityIds.clear();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, out.rgba.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return true;
    }

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

    void SetSdfVisualization(const SdfVisualization& sdf) { if (g) g->sdf = sdf; }
    const SdfVisualization& GetSdfVisualization() { static SdfVisualization none; return g ? g->sdf : none; }
    void SetFieldVisualization(const FieldVisualization& field) { if (g) g->fieldViz = field; }
    uint32_t GetGLErrorCount() { return g ? g->glErrors : 0; }

    // ====================================================================== presets / info / vision / upscaling
    void ApplyQualityPreset(QualityPreset preset) {
        RenderSettings& r = Settings();
        switch (preset) {
            case QualityPreset::Low:
                r.renderScale = 0.667f; r.upscaler = Upscaler::FSR1; r.sharpness = 0.2f;
                r.msaaSamples = 1; r.shadowMapSize = 1024; r.shadowDistance = 25.0f; r.occlusionCulling = false;
                r.bloom.enabled = true; r.bloom.mipCount = 4;
                r.ssao.enabled = false;
                break;
            case QualityPreset::Medium:
                r.renderScale = 0.77f; r.upscaler = Upscaler::FSR1; r.sharpness = 0.25f;
                r.msaaSamples = 2; r.shadowMapSize = 2048; r.shadowDistance = 35.0f; r.occlusionCulling = true;
                r.bloom.enabled = true; r.bloom.mipCount = 5;
                r.ssao.enabled = true; r.ssao.halfResolution = true; r.ssao.samples = 8;
                break;
            case QualityPreset::High:
                r.renderScale = 1.0f; r.upscaler = Upscaler::FSR1; r.sharpness = 0.25f;
                r.msaaSamples = 4; r.shadowMapSize = 2048; r.shadowDistance = 40.0f; r.occlusionCulling = true;
                r.bloom.enabled = true; r.bloom.mipCount = 6;
                r.ssao.enabled = true; r.ssao.halfResolution = false; r.ssao.samples = 16;
                break;
        }
        r.shadowsEnabled = true;
        r.gpuCulling = true;
    }
    const char* ToString(QualityPreset preset) {
        switch (preset) {
            case QualityPreset::Low: return "Low";
            case QualityPreset::Medium: return "Medium";
            case QualityPreset::High: return "High";
        }
        return "?";
    }

    RendererInfo GetRendererInfo() {
        RendererInfo info;
        if (!g) return info;
        info.contextVersion = g->contextVersion;
        info.compatTier = g->compat;
        info.gpuCompute = !g->compat;
        info.renderer = g->rendererName;
        info.vendor = g->vendorName;
        return info;
    }

    void RequestVisionCapture() { if (g) { g->visionRequested = true; g->visionReady = false; } }
    bool GetVisionCapture(VisionCapture& out) {
        if (!g || !g->visionReady) return false;
        out = g->vision;
        return true;
    }

    bool UpscaleImage(const std::vector<uint8_t>& rgba, int width, int height, int outWidth, int outHeight, Upscaler upscaler, float sharpness, std::vector<uint8_t>& out) {
        if (!g || width <= 0 || height <= 0 || outWidth <= 0 || outHeight <= 0 || rgba.size() < size_t(width) * height * 4) return false;
        GL::TextureDesc d;
        d.width = width;
        d.height = height;
        d.minFilter = d.magFilter = GL_LINEAR;
        GL::Texture input;
        input.Create("Upscale input", d);
        input.Upload2D(GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        GL::Framebuffer inputFbo;
        inputFbo.Create("Upscale input");
        inputFbo.AttachColor(0, input);
        inputFbo.SetDrawBuffers(1);
        d.width = outWidth;
        d.height = outHeight;
        d.minFilter = d.magFilter = GL_NEAREST;
        GL::Texture mid, result;
        mid.Create("Upscale EASU", d);
        result.Create("Upscale RCAS", d);
        GL::Framebuffer midFbo, resultFbo;
        midFbo.Create("Upscale EASU");
        midFbo.AttachColor(0, mid);
        midFbo.SetDrawBuffers(1);
        resultFbo.Create("Upscale RCAS");
        resultFbo.AttachColor(0, result);
        resultFbo.SetDrawBuffers(1);
        RunUpscaler(input.Id(), glm::ivec2(width, height), midFbo.Id(), mid.Id(), resultFbo.Id(), glm::ivec2(outWidth, outHeight), upscaler, sharpness, inputFbo.Id());
        out.resize(size_t(outWidth) * outHeight * 4);
        result.Read(GL_RGBA, GL_UNSIGNED_BYTE, out.size(), out.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }


    // ====================================================================== shader hot reload
    bool EnableShaderHotReload(const std::string& directory) {
        if (!g) return false;
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) { Log::Error() << "Renderer: hot reload directory '" << directory << "' not found"; return false; }
        g->hotReloadDirectory = directory;
        g->hotReloadStamps.clear();
        g->reloadStatus.enabled = true;
        g->reloadStatus.directory = directory;
        Log::Info() << "Renderer: watching '" << directory << "' for shader changes";
        if (ScanHotReload()) ApplyHotReload();
        g->hotReloadNextPoll = Time::NowSeconds() + 0.25;
        return true;
    }

    void DisableShaderHotReload() {
        if (!g) return;
        const bool hadOverrides = Shaders::OverrideCount() > 0;
        g->hotReloadDirectory.clear();
        g->hotReloadStamps.clear();
        g->reloadStatus.enabled = false;
        g->reloadStatus.filesOverridden = 0;
        Shaders::ClearOverrides();
        if (hadOverrides) ApplyHotReload();   // back to the embedded sources
    }

    bool ReloadShadersNow() {
        if (!g) return false;
        if (!g->hotReloadDirectory.empty()) ScanHotReload();
        return ApplyHotReload();
    }

    ShaderReloadStatus GetShaderReloadStatus() { return g ? g->reloadStatus : ShaderReloadStatus{}; }

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
            const int version = major * 10 + minor;
            if (version < 41) {
                Log::Error() << "Renderer: OpenGL 4.1 is required (context " << major << "." << minor << ")";
                return false;
            }
            // 4.6 tier needs the full 4.6 context and the window not capped below it.
            const int requested = Platform::GetRequestedGLVersion();
            const bool compat = version < 46 || (requested > 0 && requested < 46);
            GL::SetTier(compat ? GL::Tier::GL41 : GL::Tier::GL46);

            g = std::make_unique<State>();
            g->compat = compat;
            g->contextVersion = version;
            g->rendererName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            g->vendorName = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            Log::Info() << "Renderer: OpenGL " << major << "." << minor << " - " << g->rendererName << " (" << g->vendorName << "), "
                        << (compat ? "4.1 feature tier: CPU culling, CPU compute, texture-buffer storage" : "4.6 feature tier");
            if (!compat) {
                glEnable(GL_DEBUG_OUTPUT);
                glDebugMessageCallback(OnDebugMessage, nullptr);
            }
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
            if (!compat) glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &g->ssboAlignment);
            g->ring.Create("Per-frame uploads", 1 << 20);
            g->remapBuffer.Create("Remap", 4096);
            g->sdfBuffer.Create("SDF primitives", 1024);
            g->ribbonBuffer.Create("Ribbons", sizeof(RibbonVertex) * 1024);
            {
                GL::TextureDesc d;
                d.format = GL_RGBA8;
                g->black.Create("Black", d);
                const uint8_t zero[4] = { 0, 0, 0, 255 };
                g->black.Upload2D(GL_RGBA, GL_UNSIGNED_BYTE, zero);
            }
            if (!compat) for (GLuint& b : g->cullStats) b = GL::CreateBufferObject(16, nullptr);
            Advanced::Init();
            return true;
        }

        void Shutdown() {
            if (!g) return;
            glFinish();
            Advanced::Shutdown();
            Compute::Internal::ReleaseBackends();
            for (GLsync& f : g->cullFences) if (f) glDeleteSync(f);
            if (!g->compat) glDeleteBuffers(State::CULL_SLOTS, g->cullStats);
            g->ring.Release();
            glDeleteVertexArrays(1, &g->meshVao);
            glDeleteVertexArrays(1, &g->ribbonVao);
            Shaders::ClearOverrides();
            glDeleteVertexArrays(1, &g->lineVao);
            glDeleteVertexArrays(1, &g->textVao);
            glDeleteVertexArrays(1, &g->emptyVao);
            if (!g->compat) glDebugMessageCallback(nullptr, nullptr);
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

        uint32_t ProgramHandle(int programId) {
            return g && programId >= 0 && programId < Shaders::PROGRAM_COUNT ? g->programs[programId].Id() : 0;
        }

        uint32_t BuildComputeProgram(const std::string& name, const std::string& source) {
            if (!g || g->compat) return 0;
            const auto resolver = [](const std::string& file) { return Shaders::Find(file); };
            const ShaderPreprocessor::Result expanded = ShaderPreprocessor::Expand(source, resolver);
            if (!expanded.ok) {
                Log::Error() << "Compute kernel '" << name << "': " << expanded.error;
                return 0;
            }
            return g->shaderCache.GetProgram(name, { { GL_COMPUTE_SHADER, expanded.source } });
        }

        bool ReadPresented(std::vector<uint8_t>& rgba, int& width, int& height) {
            if (!g || !g->window.rendered || g->window.present.Id() == 0) return false;
            width = g->window.size.x;
            height = g->window.size.y;
            if (g->window.upscaledThisFrame) { width = g->window.outputSize.x; height = g->window.outputSize.y; }
            rgba.resize(size_t(width) * height * 4);
            g->window.present.Read(GL_RGBA, GL_UNSIGNED_BYTE, rgba.size(), rgba.data());
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
            s.compatTier = g->compat;
            return s;
        }

        void RenderFrame() {
            if (!g) return;
            if (!g->hotReloadDirectory.empty() && Time::NowSeconds() >= g->hotReloadNextPoll) {
                g->hotReloadNextPoll = Time::NowSeconds() + 0.25;
                if (ScanHotReload()) ApplyHotReload();
            }
            const glm::ivec2 windowSize = GetFramebufferSize();
            if (windowSize.x <= 0 || windowSize.y <= 0) {   // minimized
                g->lines.clear();
                g->text.clear();
                g->ribbons.clear();
                g->decals.clear();
                g->ribbonStrips = 0;
                return;
            }
            const RenderSettings& settings = g->settings;
            RenderStats stats;
            const bool compat = g->compat;
            const glm::ivec2 sceneSize = SceneSize(windowSize);   // window views render at this size
            const bool upscale = sceneSize != windowSize;
            EnsureTargetSet(g->window, sceneSize, "Window");
            if (upscale) EnsureUpscaleTargets(g->window, windowSize);
            g->window.upscaledThisFrame = false;
            stats.internalWidth = sceneSize.x;
            stats.internalHeight = sceneSize.y;
            stats.outputWidth = windowSize.x;
            stats.outputHeight = windowSize.y;
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
                glm::ivec2 size = sceneSize;
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

            // ---------------------------------------------------------- instances, remap lists, indirect commands
            // Each object gets one instance (sorted by mesh) in the persistent ring. Draw commands index a
            // per-view remap list of instance indices: CPU culling fills it here, GPU culling (cull.comp)
            // fills it on the GPU and bumps the commands' instanceCount directly.
            std::vector<Advanced::SoftBodyDraw> softDraws;
            std::vector<Advanced::SpriteDraw> spriteDraws;
            Advanced::CollectDraws(softDraws, spriteDraws);
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mesh < b.mesh; });
            const uint32_t itemCount = uint32_t(items.size());
            std::vector<uint32_t> slotOfMesh(g->meshes.size(), UINT32_MAX), slotFirst, slotCount, slotMesh;
            for (uint32_t i = 0; i < itemCount; ++i) {
                const uint32_t m = items[i].mesh;
                if (slotOfMesh[m] == UINT32_MAX) {
                    slotOfMesh[m] = uint32_t(slotMesh.size());
                    slotMesh.push_back(m);
                    slotFirst.push_back(i);
                    slotCount.push_back(0);
                }
                ++slotCount[slotOfMesh[m]];
            }
            const uint32_t slots = uint32_t(slotMesh.size());
            const uint32_t softBase = itemCount;                       // soft-body instances follow the mesh items
            const uint32_t instanceCount = std::max(itemCount + uint32_t(softDraws.size()), 1u);
            const size_t instanceBytes = instanceCount * sizeof(InstanceGpu);
            const size_t cullBytes = std::max(itemCount, 1u) * sizeof(CullObjectGpu);
            g->ring.BeginFrame(instanceBytes + cullBytes + 4 * size_t(g->ssboAlignment) + 1024);
            const size_t instanceOffset = g->ring.Allocate(instanceBytes, size_t(g->ssboAlignment));
            const size_t cullOffset = g->ring.Allocate(cullBytes, size_t(g->ssboAlignment));
            InstanceGpu* instances = static_cast<InstanceGpu*>(g->ring.Pointer(instanceOffset));
            CullObjectGpu* cullObjects = static_cast<CullObjectGpu*>(g->ring.Pointer(cullOffset));
            for (uint32_t i = 0; i < itemCount; ++i) {
                const Item& item = items[i];
                const Object& o = *item.object;
                InstanceGpu gpu;
                gpu.model = o.world;
                gpu.normalMatrix = glm::mat4(glm::inverseTranspose(glm::mat3(o.world)));
                gpu.scaleMaterial = glm::vec4(glm::length(glm::vec3(o.world[0])), glm::length(glm::vec3(o.world[1])), glm::length(glm::vec3(o.world[2])), float(item.material));
                gpu.ids = glm::uvec4(o.entityId + 1, 0, 0, 0);
                if (compat) gpu.ids.x = FloatBits(float(o.entityId + 1));   // texel buffers hold floats
                gpu.uvScale = glm::vec4(o.renderer.uvScale, 0.0f, 0.0f);
                instances[i] = gpu;
                cullObjects[i] = { glm::vec4(item.center, item.radius), glm::uvec4(slotOfMesh[item.mesh], i, 0, 0) };
            }
            for (size_t k = 0; k < softDraws.size(); ++k) {
                uint32_t material = 0;
                auto m = g->materialByName.find(softDraws[k].material);
                if (m != g->materialByName.end()) material = m->second;
                InstanceGpu gpu;
                gpu.model = glm::mat4(1.0f);
                gpu.normalMatrix = glm::mat4(1.0f);
                gpu.scaleMaterial = glm::vec4(1.0f, 1.0f, 1.0f, float(material));
                gpu.ids = glm::uvec4(softDraws[k].entityId + 1, 0, 0, 0);
                if (compat) gpu.ids.x = FloatBits(float(softDraws[k].entityId + 1));
                gpu.uvScale = glm::vec4(1.0f);
                instances[softBase + k] = gpu;
            }
            if (itemCount + softDraws.size() == 0) instances[0] = InstanceGpu{};
            g->ring.Flush();   // 4.1: staging -> buffer

            // Remap layout: [shadow casters][view 0][view 1]..., each itemCount long with a slice per mesh.
            // Command layout: [casters: slots][view 0: slots][view 1: slots]...
            const bool gpuCulling = settings.gpuCulling && !compat && itemCount > 0;
            std::vector<uint32_t> remap(std::max<size_t>(size_t(itemCount) * (views.size() + 1), 1), 0);
            std::vector<DrawCommand> commands(size_t(slots) * (views.size() + 1));
            auto initCommands = [&](size_t block) {
                for (uint32_t k = 0; k < slots; ++k) {
                    const MeshEntry& mesh = g->meshes[slotMesh[k]];
                    commands[block * slots + k] = { mesh.indexCount, 0, mesh.firstIndex, mesh.baseVertex, uint32_t(block * itemCount + slotFirst[k]) };
                }
            };
            auto addVisible = [&](size_t block, uint32_t item) {
                DrawCommand& c = commands[block * slots + slotOfMesh[items[item].mesh]];
                remap[c.baseInstance + c.instanceCount++] = item;
            };
            initCommands(0);
            for (uint32_t i = 0; i < itemCount; ++i) if (items[i].object->renderer.castShadows) addVisible(0, i);
            const size_t casterFirst = 0, casterCount = slots;
            bool firstWindowView = true;
            for (size_t vi = 0; vi < views.size(); ++vi) {
                View& v = views[vi];
                initCommands(vi + 1);
                v.firstCommand = (vi + 1) * slots;
                v.commandCount = slots;
                if (!gpuCulling) {
                    const RenderMath::Frustum frustum = RenderMath::ExtractFrustum(v.viewProjection);
                    for (uint32_t i = 0; i < itemCount; ++i) {
                        if (frustum.IntersectsSphere(items[i].center, items[i].radius)) addVisible(vi + 1, i);
                        else ++stats.instancesCulled;
                    }
                    for (size_t c = v.firstCommand; c < v.firstCommand + v.commandCount; ++c) {
                        stats.instancesDrawn += commands[c].instanceCount;
                        stats.trianglesDrawn += commands[c].count / 3 * commands[c].instanceCount;
                    }
                }
                if (firstWindowView && v.target == &g->window) { stats.drawCommands = slots; firstWindowView = false; }
            }
            stats.views = uint32_t(views.size());
            g->remapBuffer.Upload(remap.data(), remap.size() * sizeof(uint32_t));
            if (!commands.empty() && !compat) g->indirectBuffer.Upload(commands.data(), commands.size() * sizeof(DrawCommand));
            if (compat) g->cpuCommands = commands;

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
            if (!g->ribbons.empty()) {
                g->ribbonBuffer.Upload(g->ribbons.data(), g->ribbons.size() * sizeof(RibbonVertex));
                GL::SetVertexBuffer(g->ribbonVao, g->ribbonBuffer.Id(), sizeof(RibbonVertex), {
                    { 0, 4, GL_FLOAT, offsetof(RibbonVertex, positionSide) }, { 1, 4, GL_FLOAT, offsetof(RibbonVertex, tangentWidth) },
                    { 2, 4, GL_FLOAT, offsetof(RibbonVertex, color) } });
                stats.ribbonsDrawn = g->ribbonStrips;
                stats.ribbonVertices = uint32_t(g->ribbons.size());
            }

            if (!compat) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g->lightBuffer.Id());
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, g->ring.Id(), GLintptr(instanceOffset), GLsizeiptr(instanceBytes));
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g->materialBuffer.Id());
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, g->remapBuffer.Id());
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g->indirectBuffer.Id());
            }
            else {
                g->tboLights.Bind(9, GL_RGBA32F, g->lightBuffer.Id());
                g->tboInstances.Bind(10, GL_RGBA32F, g->ring.Id());
                g->tboMaterials.Bind(11, GL_RGBA32F, g->materialBuffer.Id());
                g->tboRemap.Bind(12, GL_R32UI, g->remapBuffer.Id());
                const GLint base = GLint(instanceOffset / 16);   // RGBA32F texels
                for (int p : { Shaders::FORWARD, Shaders::SHADOW, Shaders::SHADOW_POINT, Shaders::SOFTBODY, Shaders::GBUFFER, Shaders::GBUFFER_SOFT }) {
                    g->programs[p].Use();
                    glUniform1i(g->programs[p].Uniform("u_instanceBase"), base);
                }
            }
            glBindVertexArray(g->meshVao);
            if (!views.empty()) glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), 0, sizeof(FrameDataGpu));

            // ---------------------------------------------------------- GPU culling (frustum + Hi-Z occlusion)
            const int statsSlot = int(g->cullFrame % State::CULL_SLOTS);
            stats.gpuCulling = gpuCulling;
            if (gpuCulling && !views.empty()) {
                const uint32_t zero = 0;
                (void)zero;
                GL::ClearBufferZero(g->cullStats[statsSlot], 16);
                const GL::Program& cull = g->programs[Shaders::CULL];
                cull.Use();
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 4, g->ring.Id(), GLintptr(cullOffset), GLsizeiptr(cullBytes));
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, g->indirectBuffer.Id());
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g->remapBuffer.Id());
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, g->cullStats[statsSlot]);
                glBindTextureUnit(0, g->hizValid ? g->hiz.Id() : g->fontAtlas.Id());
                glUniform1ui(cull.Uniform("u_objectCount"), itemCount);
                glUniform1i(cull.Uniform("u_hizLevels"), std::max(g->hizLevels, 1));
                size_t windowView = 0;
                for (size_t vi = 0; vi < views.size(); ++vi) {
                    const View& v = views[vi];
                    const RenderMath::Frustum frustum = RenderMath::ExtractFrustum(v.viewProjection);
                    glUniform4fv(cull.Uniform("u_planes"), 6, &frustum.planes[0][0]);
                    glUniform1ui(cull.Uniform("u_commandBase"), uint32_t(v.firstCommand));
                    // Occlusion needs last frame's pyramid for this same window view.
                    bool occlusion = false;
                    if (v.target == &g->window) {
                        occlusion = settings.occlusionCulling && g->hizValid && g->hizSize == sceneSize &&
                                    windowView < g->hizViewProjections.size() && g->hizRects[windowView] == v.rect;
                        if (occlusion) {
                            glUniformMatrix4fv(cull.Uniform("u_prevViewProjection"), 1, GL_FALSE, &g->hizViewProjections[windowView][0][0]);
                            glUniform4f(cull.Uniform("u_hizRect"), float(v.rect.x), float(v.rect.y), float(v.rect.z), float(v.rect.w));
                        }
                        ++windowView;
                    }
                    glUniform1i(cull.Uniform("u_occlusion"), occlusion ? 1 : 0);
                    stats.occlusionActive = stats.occlusionActive || occlusion;
                    glDispatchCompute((itemCount + 63) / 64, 1, 1);
                }
                glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
                stats.instancesDrawn = g->gpuVisible;
                stats.instancesCulled = g->gpuFrustumCulled + g->gpuOcclusionCulled;
                stats.occlusionCulled = g->gpuOcclusionCulled;
            }

            // ---------------------------------------------------------- shadow maps
            const float one = 1.0f;
            auto renderShadowView = [&](const GL::Program& program, const glm::mat4& viewProj, int size) {
                g->shadowFbo.SetDrawBuffers(0);
                glBindFramebuffer(GL_FRAMEBUFFER, g->shadowFbo.Id());
                glViewport(0, 0, size, size);
                GL::ClearDepth(g->shadowFbo.Id(), one);
                glBindFramebuffer(GL_FRAMEBUFFER, g->shadowFbo.Id());
                glUniformMatrix4fv(program.Uniform("u_lightViewProjection"), 1, GL_FALSE, &viewProj[0][0]);
                DrawCommands(program, casterFirst, casterCount);
                if (!softDraws.empty() && program.Id() == g->programs[Shaders::SHADOW].Id()) {
                    const GL::Program& softShadow = g->programs[Shaders::SOFTBODY_SHADOW];
                    softShadow.Use();
                    glUniformMatrix4fv(softShadow.Uniform("u_lightViewProjection"), 1, GL_FALSE, &viewProj[0][0]);
                    for (const Advanced::SoftBodyDraw& d : softDraws) {
                        BindSoftBuffers(d);
                        glBindVertexArray(d.vao);
                        glDrawElements(GL_TRIANGLES, GLsizei(d.indexCount), GL_UNSIGNED_INT, nullptr);
                    }
                    glBindVertexArray(g->meshVao);
                    program.Use();
                }
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
            if (shadows && (casterCount > 0 || !softDraws.empty()) && !views.empty()) {
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

            // ---------------------------------------------------------- visualiser inputs (once per frame)
            uint32_t fieldSamples = 0, fieldSampleCount = 0;
            if (g->fieldViz.enabled && Advanced::Available()) {
                const glm::ivec3 res = glm::max(g->fieldViz.resolution, glm::ivec3(1));
                fieldSampleCount = uint32_t(res.x * res.y * res.z);
                fieldSamples = Advanced::SampleFieldGrid(g->fieldViz.boundsMin, g->fieldViz.boundsMax, res);
                if (!compat) glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
            if (g->sdf.enabled && !g->sdf.primitives.empty()) {
                std::vector<SdfPrimitiveGpu> prims;
                for (const SdfPrimitive& pr : g->sdf.primitives) {
                    prims.push_back({ glm::vec4(pr.position, float(uint32_t(pr.shape))), glm::vec4(pr.rotation.x, pr.rotation.y, pr.rotation.z, pr.rotation.w),
                                      pr.params, glm::vec4(float(uint32_t(pr.operation)), pr.smoothness, 0.0f, 0.0f) });
                }
                g->sdfBuffer.Upload(prims.data(), prims.size() * sizeof(SdfPrimitiveGpu));
            }

            // ---------------------------------------------------------- clear targets used this frame
            const glm::vec4 clear(settings.clearColor.r, settings.clearColor.g, settings.clearColor.b, 1.0f);
            const GLuint noEntity[4] = { 0, 0, 0, 0 };
            glDisable(GL_SCISSOR_TEST);
            glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            auto clearSet = [&](TargetSet& t) {
                GL::ClearColor(t.mainFbo.Id(), 0, &clear[0]);
                GL::ClearColorUint(t.mainFbo.Id(), 1, noEntity);
                GL::ClearDepth(t.mainFbo.Id(), one);
            };
            clearSet(g->window);
            for (auto& [name, set] : g->offscreen) if (set.usedThisFrame) clearSet(set);

            // ---------------------------------------------------------- views
            bool visionDone = false;
            for (size_t i = 0; i < views.size(); ++i) {
                View& v = views[i];
                glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), GLintptr(i * stride), sizeof(FrameDataGpu));
                if (shadows && dirShadowLight && (casterCount > 0 || !softDraws.empty())) {
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
                GL::ClearDepth(fbo, one);   // views may overlap: each starts with empty depth
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                g->dirShadow.Bind(4);
                g->spotShadows.Bind(5);
                g->pointShadows.Bind(6);
                g->fontAtlas.Bind(7);
                g->albedoArray.Bind(8);
                glBindVertexArray(g->meshVao);
                g->programs[Shaders::FORWARD].Use();
                DrawCommands(g->programs[Shaders::FORWARD], v.firstCommand, v.commandCount);

                // XPBD cloth / soft bodies (two-sided), straight from the solver buffers
                if (!softDraws.empty()) {
                    glDisable(GL_CULL_FACE);
                    const GL::Program& soft = g->programs[Shaders::SOFTBODY];
                    soft.Use();
                    for (size_t k = 0; k < softDraws.size(); ++k) {
                        const Advanced::SoftBodyDraw& d = softDraws[k];
                        glUniform1ui(soft.Uniform("u_instance"), softBase + uint32_t(k));
                        BindSoftBuffers(d);
                        glBindVertexArray(d.vao);
                        glDrawElements(GL_TRIANGLES, GLsizei(d.indexCount), GL_UNSIGNED_INT, nullptr);
                    }
                    stats.softBodiesDrawn = uint32_t(softDraws.size());
                    glEnable(GL_CULL_FACE);
                }
                // GPU particles and rope nodes as shaded sphere sprites
                if (!spriteDraws.empty()) {
                    glEnable(GL_PROGRAM_POINT_SIZE);
                    const GL::Program& sprites = g->programs[Shaders::SPRITES];
                    sprites.Use();
                    const Camera& cam = v.input->camera;
                    const float pointScale = cam.projection == Projection::Orthographic ? float(v.rect.w) / std::max(cam.orthoHeight, 1e-3f)
                                                                                          : float(v.rect.w) / (2.0f * std::tan(cam.verticalFov * 0.5f));
                    glUniform1f(sprites.Uniform("u_pointScale"), pointScale);
                    glBindVertexArray(g->emptyVao);
                    uint32_t drawn = 0;
                    for (const Advanced::SpriteDraw& d : spriteDraws) {
                        if (compat) g->tboAux.Bind(13, GL_RGBA32F, d.buffer);
                        else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, d.buffer);
                        glUniform1ui(sprites.Uniform("u_stride"), d.stride);
                        glUniform1f(sprites.Uniform("u_radius"), d.radius);
                        glUniform3fv(sprites.Uniform("u_color"), 1, &d.color[0]);
                        glUniform3fv(sprites.Uniform("u_fastColor"), 1, &d.fastColor[0]);
                        glUniform1f(sprites.Uniform("u_fastSpeed"), d.fastSpeed);
                        glUniform1ui(sprites.Uniform("u_entityId"), d.entityId + 1);
                        glDrawArrays(GL_POINTS, 0, GLsizei(d.count));
                        drawn += d.count;
                    }
                    stats.particlesDrawn = drawn;
                    glDisable(GL_PROGRAM_POINT_SIZE);
                }
                // Analytical SDF / CSG visualiser (raymarched, writes depth and the entity id)
                if (g->sdf.enabled && !g->sdf.primitives.empty()) {
                    glDisable(GL_CULL_FACE);
                    const GL::Program& sdf = g->programs[Shaders::SDF];
                    sdf.Use();
                    if (compat) g->tboAux.Bind(13, GL_RGBA32F, g->sdfBuffer.Id());
                    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, g->sdfBuffer.Id());
                    glUniform1i(sdf.Uniform("u_sdfCount"), int(g->sdf.primitives.size()));
                    glUniform3fv(sdf.Uniform("u_boundsMin"), 1, &g->sdf.boundsMin[0]);
                    glUniform3fv(sdf.Uniform("u_boundsMax"), 1, &g->sdf.boundsMax[0]);
                    glUniform4f(sdf.Uniform("u_surface"), g->sdf.color.r, g->sdf.color.g, g->sdf.color.b, g->sdf.roughness);
                    glUniform1f(sdf.Uniform("u_metallic"), g->sdf.metallic);
                    glUniform1ui(sdf.Uniform("u_entityId"), g->sdf.entityId == UINT32_MAX ? 0u : g->sdf.entityId + 1);
                    glUniform1i(sdf.Uniform("u_showSlice"), g->sdf.showSlice ? 1 : 0);
                    glUniform4fv(sdf.Uniform("u_slicePlane"), 1, &g->sdf.slicePlane[0]);
                    glBindVertexArray(g->emptyVao);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glEnable(GL_CULL_FACE);
                }

                // Sky on the far plane, i.e. every pixel nothing was drawn to. It also writes entity id 0
                // there: on the Intel UHD driver the cleared integer MSAA id attachment resolved stale ids
                // from earlier frames in untouched pixels (found by the P7 SDF silhouette check), so the
                // id of every pixel is now written by a draw each frame instead of relying on the clear.
                glDepthMask(GL_FALSE);
                glDepthFunc(GL_LEQUAL);
                glDisable(GL_CULL_FACE);
                g->programs[Shaders::SKY].Use();
                glBindVertexArray(g->emptyVao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // ribbons / debug lines / arrows: colour only
                if (!g->ribbons.empty()) {   // additive VFX ribbons (depth-tested, no depth writes)
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE);
                    g->programs[Shaders::RIBBON].Use();
                    glBindVertexArray(g->ribbonVao);
                    glDrawArrays(GL_TRIANGLES, 0, GLsizei(g->ribbons.size()));
                    glDisable(GL_BLEND);
                }
                if (!g->lines.empty() && v.target == &g->window) {
                    g->lineBuffer.Upload(g->lines.data(), g->lines.size() * sizeof(LineVertex));
                    g->programs[Shaders::LINES].Use();
                    SetLineBuffer();
                    glBindVertexArray(g->lineVao);
                    glDrawArrays(GL_LINES, 0, GLsizei(g->lines.size()));
                }
                if (fieldSamples && v.target == &g->window) {
                    const GL::Program& arrows = g->programs[Shaders::ARROWS];
                    arrows.Use();
                    const FieldVisualization& fv = g->fieldViz;
                    glUniform3i(arrows.Uniform("u_resolution"), fv.resolution.x, fv.resolution.y, fv.resolution.z);
                    glUniform3fv(arrows.Uniform("u_boundsMin"), 1, &fv.boundsMin[0]);
                    glUniform3fv(arrows.Uniform("u_boundsMax"), 1, &fv.boundsMax[0]);
                    glUniform1f(arrows.Uniform("u_arrowScale"), fv.arrowScale);
                    glUniform1f(arrows.Uniform("u_maxLength"), fv.maxLength);
                    if (compat) g->tboAux.Bind(13, GL_RGBA32F, fieldSamples);
                    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, fieldSamples);
                    glBindVertexArray(g->emptyVao);
                    glDrawArrays(GL_LINES, 0, GLsizei(fieldSampleCount * 6));
                }
                glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glDepthFunc(GL_LESS);
                glDisable(GL_SCISSOR_TEST);

                // Synthetic-vision G-buffer of the first window view (export / tests only).
                if (g->visionRequested && !visionDone && v.target == &g->window) {
                    visionDone = true;
                    EnsureAuxTargets(sceneSize);
                    const float zero4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    const GLuint noId[4] = { 0, 0, 0, 0 };
                    GL::ClearColor(g->auxFbo.Id(), 0, zero4);
                    GL::ClearColor(g->auxFbo.Id(), 1, zero4);
                    GL::ClearColorUint(g->auxFbo.Id(), 2, noId);
                    GL::ClearDepth(g->auxFbo.Id(), one);
                    glBindFramebuffer(GL_FRAMEBUFFER, g->auxFbo.Id());
                    glViewport(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LESS);
                    glEnable(GL_CULL_FACE);
                    glBindVertexArray(g->meshVao);
                    const GL::Program& gbuffer = g->programs[Shaders::GBUFFER];
                    gbuffer.Use();
                    DrawCommands(gbuffer, v.firstCommand, v.commandCount);
                    if (!softDraws.empty()) {
                        glDisable(GL_CULL_FACE);
                        const GL::Program& soft = g->programs[Shaders::GBUFFER_SOFT];
                        soft.Use();
                        for (size_t k = 0; k < softDraws.size(); ++k) {
                            glUniform1ui(soft.Uniform("u_instance"), softBase + uint32_t(k));
                            BindSoftBuffers(softDraws[k]);
                            glBindVertexArray(softDraws[k].vao);
                            glDrawElements(GL_TRIANGLES, GLsizei(softDraws[k].indexCount), GL_UNSIGNED_INT, nullptr);
                        }
                    }
                    glEnable(GL_CULL_FACE);
                    g->vision.view = v.input->view;
                    g->vision.projection = v.projection;
                    g->vision.nearPlane = v.input->camera.nearPlane;
                    g->vision.farPlane = v.input->camera.farPlane;
                    g->vision.orthographic = v.input->camera.projection == Projection::Orthographic;
                }
            }

            // ---------------------------------------------------------- Hi-Z pyramid for next frame's occlusion culling
            if (settings.gpuCulling && settings.occlusionCulling && !compat && !views.empty()) {
                const glm::ivec2 windowSize = sceneSize;   // the pyramid follows the scene resolution
                if (g->hizSize != windowSize) {
                    g->hizSize = windowSize;
                    g->hizLevels = 1 + int(std::floor(std::log2(float(std::max(windowSize.x, windowSize.y)))));
                    GL::TextureDesc d;
                    d.width = windowSize.x;
                    d.height = windowSize.y;
                    d.format = GL_DEPTH_COMPONENT32F;
                    d.minFilter = d.magFilter = GL_NEAREST;
                    g->hizDepth.Create("Hi-Z depth", d);
                    g->hizDepthFbo.Create("Hi-Z depth");
                    g->hizDepthFbo.AttachDepth(g->hizDepth);
                    g->hizDepthFbo.SetDrawBuffers(0);
                    d.format = GL_R32F;
                    d.levels = g->hizLevels;
                    d.minFilter = GL_NEAREST_MIPMAP_NEAREST;
                    g->hiz.Create("Hi-Z pyramid", d);
                    g->hizValid = false;
                }
                glDisable(GL_SCISSOR_TEST);
                glBlitNamedFramebuffer(g->window.mainFbo.Id(), g->hizDepthFbo.Id(), 0, 0, windowSize.x, windowSize.y, 0, 0, windowSize.x, windowSize.y,
                                       GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                const GL::Program& init = g->programs[Shaders::HIZ_INIT];
                init.Use();
                glBindTextureUnit(0, g->hizDepth.Id());
                glBindImageTexture(1, g->hiz.Id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
                glDispatchCompute((windowSize.x + 7) / 8, (windowSize.y + 7) / 8, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                const GL::Program& down = g->programs[Shaders::HIZ_DOWNSAMPLE];
                down.Use();
                for (int level = 1; level < g->hizLevels; ++level) {
                    const int w = std::max(windowSize.x >> level, 1), h = std::max(windowSize.y >> level, 1);
                    glBindImageTexture(0, g->hiz.Id(), level - 1, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
                    glBindImageTexture(1, g->hiz.Id(), level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
                    glDispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                }
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
                g->hizViewProjections.clear();
                g->hizRects.clear();
                for (const View& v : views) {
                    if (v.target != &g->window) continue;
                    g->hizViewProjections.push_back(v.viewProjection);
                    g->hizRects.push_back(v.rect);
                }
                g->hizValid = true;
            }
            else {
                g->hizValid = false;
            }

            // ---------------------------------------------------------- resolve + tonemap every used target
            auto finish = [&](TargetSet& t) {
                const GLuint fbo = t.mainFbo.Id();
                const glm::ivec2 s = t.size;
                glDisable(GL_SCISSOR_TEST);
                GL::ReadBuffer(fbo, GL_COLOR_ATTACHMENT0);
                GL::Blit(fbo, t.resolveColorFbo.Id(), s.x, s.y, s.x, s.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                GL::ReadBuffer(fbo, GL_COLOR_ATTACHMENT1);
                GL::Blit(fbo, t.resolveIdFbo.Id(), s.x, s.y, s.x, s.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                GL::ReadBuffer(fbo, GL_COLOR_ATTACHMENT0);

                // Window post-processing: projected decals, SSAO (per window view), bloom mip chain.
                float bloomIntensity = 0.0f;
                GLuint bloomTexture = g->black.Id();
                if (&t == &g->window && !views.empty()) {
                    const BloomSettings& bloom = settings.bloom;
                    const SsaoSettings& ssao = settings.ssao;
                    const bool doDecals = settings.decals && !g->decals.empty();
                    const bool doSsao = ssao.enabled && ssao.intensity > 0.0f;
                    const bool doBloom = bloom.enabled && bloom.intensity > 0.0f;
                    if (doDecals || doSsao || doBloom) {
                        int levels = 0;
                        if (doBloom) {
                            levels = std::clamp(bloom.mipCount, 1, 8);
                            while (levels > 1 && std::min(s.x >> levels, s.y >> levels) < 4) --levels;
                        }
                        const int aoScale = ssao.halfResolution ? 2 : 1;
                        const glm::ivec2 aoSize = doSsao ? glm::max(s / aoScale, glm::ivec2(1)) : glm::ivec2(0);
                        EnsurePostTargets(t, levels, aoSize);
                        if (doDecals || doSsao) GL::Blit(fbo, t.depthResolveFbo.Id(), s.x, s.y, s.x, s.y, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                        glBindVertexArray(g->emptyVao);
                        glDisable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);
                        bool firstWindow = true;
                        for (size_t vi = 0; vi < views.size(); ++vi) {
                            const View& v = views[vi];
                            if (v.target != &g->window) continue;
                            glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), GLintptr(vi * stride), sizeof(FrameDataGpu));
                            const glm::vec4 rect(float(v.rect.x), float(v.rect.y), float(v.rect.z), float(v.rect.w));
                            if (doDecals) {
                                glBindFramebuffer(GL_FRAMEBUFFER, t.resolveColorFbo.Id());
                                glViewport(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                                glScissor(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                                glEnable(GL_SCISSOR_TEST);
                                glEnable(GL_CULL_FACE);
                                glCullFace(GL_FRONT);   // back faces: also covers the camera standing inside the box
                                glEnable(GL_BLEND);
                                const GL::Program& p = g->programs[Shaders::DECAL];
                                p.Use();
                                t.depthResolved.Bind(0);
                                glUniform4fv(p.Uniform("u_viewRect"), 1, &rect[0]);
                                glUniform2f(p.Uniform("u_targetSize"), float(s.x), float(s.y));
                                for (const DecalDesc& d : g->decals) {
                                    if (d.blend == DecalBlend::Multiply) glBlendFunc(GL_DST_COLOR, GL_ZERO);
                                    else if (d.blend == DecalBlend::Additive) glBlendFunc(GL_ONE, GL_ONE);
                                    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                                    const glm::mat4 inverse = glm::inverse(d.world);
                                    const glm::vec3 up = glm::normalize(glm::vec3(d.world[1]));
                                    glUniformMatrix4fv(p.Uniform("u_decalWorld"), 1, GL_FALSE, &d.world[0][0]);
                                    glUniformMatrix4fv(p.Uniform("u_decalInverse"), 1, GL_FALSE, &inverse[0][0]);
                                    glUniform3fv(p.Uniform("u_decalUp"), 1, &up[0]);
                                    glUniform4fv(p.Uniform("u_decalColor"), 1, &d.color[0]);
                                    glUniform4f(p.Uniform("u_decalParams"), float(uint32_t(d.pattern)), float(uint32_t(d.blend)), d.angleFade, d.seed);
                                    glDrawArrays(GL_TRIANGLES, 0, 36);
                                }
                                glDisable(GL_BLEND);
                                glCullFace(GL_BACK);
                                glDisable(GL_CULL_FACE);
                                glDisable(GL_SCISSOR_TEST);
                                if (firstWindow) stats.decalsDrawn = uint32_t(g->decals.size());
                            }
                            if (doSsao) {
                                const glm::ivec4 aoRect(v.rect.x / aoScale, v.rect.y / aoScale, std::max(v.rect.z / aoScale, 1), std::max(v.rect.w / aoScale, 1));
                                const glm::mat4 inverseProjection = glm::inverse(v.projection);
                                const glm::vec4 params(ssao.radius, ssao.intensity, ssao.bias, float(std::clamp(ssao.samples, 4, 32)));
                                auto setup = [&](const GL::Program& p) {
                                    p.Use();
                                    glUniformMatrix4fv(p.Uniform("u_inverseProjection"), 1, GL_FALSE, &inverseProjection[0][0]);
                                    glUniformMatrix4fv(p.Uniform("u_projectionMatrix"), 1, GL_FALSE, &v.projection[0][0]);
                                    glUniform4fv(p.Uniform("u_viewRect"), 1, &rect[0]);
                                    glUniform2f(p.Uniform("u_targetSize"), float(s.x), float(s.y));
                                    glUniform1f(p.Uniform("u_scale"), float(aoScale));
                                    glUniform4fv(p.Uniform("u_params"), 1, &params[0]);
                                };
                                t.depthResolved.Bind(0);
                                glBindFramebuffer(GL_FRAMEBUFFER, t.aoRawFbo.Id());
                                glViewport(aoRect.x, aoRect.y, aoRect.z, aoRect.w);
                                setup(g->programs[Shaders::SSAO]);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                glBindFramebuffer(GL_FRAMEBUFFER, t.aoBlurFbo.Id());
                                setup(g->programs[Shaders::SSAO_BLUR]);
                                t.aoRaw.Bind(1);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                glBindFramebuffer(GL_FRAMEBUFFER, t.resolveColorFbo.Id());
                                glViewport(v.rect.x, v.rect.y, v.rect.z, v.rect.w);
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_DST_COLOR, GL_ZERO);   // multiply the lit image by the AO
                                setup(g->programs[Shaders::SSAO_APPLY]);
                                t.aoBlur.Bind(1);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                glDisable(GL_BLEND);
                                stats.ssaoActive = true;
                                stats.ssaoWidth = aoSize.x;
                                stats.ssaoHeight = aoSize.y;
                            }
                            firstWindow = false;
                        }
                        if (doBloom) {
                            glDisable(GL_SCISSOR_TEST);
                            const GL::Program& pre = g->programs[Shaders::BLOOM_PREFILTER];
                            glBindFramebuffer(GL_FRAMEBUFFER, t.bloomFbos[0].Id());
                            glViewport(0, 0, t.bloomSizes[0].x, t.bloomSizes[0].y);
                            pre.Use();
                            glUniform2f(pre.Uniform("u_texel"), 1.0f / float(s.x), 1.0f / float(s.y));
                            glUniform4f(pre.Uniform("u_threshold"), bloom.threshold, bloom.knee, settings.exposure, 1.0f);
                            t.hdrResolved.Bind(0);
                            glDrawArrays(GL_TRIANGLES, 0, 3);
                            const GL::Program& down = g->programs[Shaders::BLOOM_DOWN];
                            down.Use();
                            for (int i = 1; i < levels; ++i) {
                                glBindFramebuffer(GL_FRAMEBUFFER, t.bloomFbos[i].Id());
                                glViewport(0, 0, t.bloomSizes[i].x, t.bloomSizes[i].y);
                                glUniform2f(down.Uniform("u_texel"), 1.0f / float(t.bloomSizes[i - 1].x), 1.0f / float(t.bloomSizes[i - 1].y));
                                t.bloomMips[i - 1].Bind(0);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                            }
                            const GL::Program& up = g->programs[Shaders::BLOOM_UP];
                            up.Use();
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_ONE, GL_ONE);
                            for (int i = levels - 2; i >= 0; --i) {
                                glBindFramebuffer(GL_FRAMEBUFFER, t.bloomFbos[i].Id());
                                glViewport(0, 0, t.bloomSizes[i].x, t.bloomSizes[i].y);
                                glUniform2f(up.Uniform("u_texel"), 1.0f / float(t.bloomSizes[i + 1].x), 1.0f / float(t.bloomSizes[i + 1].y));
                                glUniform4f(up.Uniform("u_threshold"), 0.0f, 0.0f, 0.0f, 1.0f);
                                t.bloomMips[i + 1].Bind(0);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                            }
                            glDisable(GL_BLEND);
                            bloomTexture = t.bloomMips[0].Id();
                            bloomIntensity = bloom.intensity;
                            stats.bloomMips = levels;
                        }
                        glDepthMask(GL_TRUE);
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, t.ldrFbo.Id());
                glViewport(0, 0, s.x, s.y);
                glDisable(GL_DEPTH_TEST);
                g->programs[Shaders::TONEMAP].Use();
                glUniform1f(g->programs[Shaders::TONEMAP].Uniform("u_bloomIntensity"), bloomIntensity);
                GL::BindTexture(1, GL_TEXTURE_2D, bloomTexture);
                t.hdrResolved.Bind(0);
                glBindVertexArray(g->emptyVao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                t.rendered = true;
            };
            if (views.empty()) glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->frameUbo.Id(), 0, sizeof(FrameDataGpu));   // exposure for tonemap
            for (auto& [name, set] : g->offscreen) if (set.usedThisFrame) finish(set);
            finish(g->window);

            // Vision readback (internal resolution, aligned with the tonemapped colour).
            if (visionDone) {
                VisionCapture& vc = g->vision;
                vc.width = sceneSize.x;
                vc.height = sceneSize.y;
                const size_t pixels = size_t(vc.width) * vc.height;
                vc.rgba.resize(pixels * 4);
                g->window.ldr.Read(GL_RGBA, GL_UNSIGNED_BYTE, vc.rgba.size(), vc.rgba.data());
                vc.linearDepth.resize(pixels);
                g->auxZ.Read(GL_RED, GL_FLOAT, pixels * 4, vc.linearDepth.data());
                vc.entityIds.resize(pixels);
                g->auxId.Read(GL_RED_INTEGER, GL_UNSIGNED_INT, pixels * 4, vc.entityIds.data());
                std::vector<glm::vec4> normals(pixels);
                g->auxNormal.Read(GL_RGBA, GL_FLOAT, pixels * 16, normals.data());
                vc.normals.resize(pixels);
                for (size_t p = 0; p < pixels; ++p) vc.normals[p] = glm::vec3(normals[p]);
                g->visionReady = true;
            }
            g->visionRequested = false;

            // Upscale the window image (EASU + RCAS or bilinear) to the window size.
            TargetSet& w = g->window;
            GLuint shown = w.ldrFbo.Id();
            if (upscale && !views.empty()) {
                RunUpscaler(w.ldr.Id(), sceneSize, w.upscaledFbo.Id(), w.upscaled.Id(), w.outputFbo.Id(), windowSize, settings.upscaler, settings.sharpness, w.ldrFbo.Id());
                shown = w.outputFbo.Id();
                w.upscaledThisFrame = true;
                stats.upscaled = true;
            }
            else if (upscale) {   // nothing rendered: show the clear colour at the window size
                GL::Blit(w.ldrFbo.Id(), w.outputFbo.Id(), sceneSize.x, sceneSize.y, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                shown = w.outputFbo.Id();
                w.upscaledThisFrame = true;
            }

            // Compose the presented image (scene + overlay text) in a renderer-owned texture, then show it.
            const GLuint present = w.presentFbo.Id();
            GL::Blit(shown, present, windowSize.x, windowSize.y, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            stats.overlayGlyphs = uint32_t(g->text.size() / 6);
            DrawTextPass(present, windowSize);
            GL::Blit(present, 0, windowSize.x, windowSize.y, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
            g->ring.EndFrame();
            if (gpuCulling && !views.empty()) {
                if (g->cullFences[statsSlot]) glDeleteSync(g->cullFences[statsSlot]);   // never read: dropped
                g->cullFences[statsSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                // Newest finished slot wins; older finished ones are retired.
                for (int age = State::CULL_SLOTS - 1; age >= 1; --age) {
                    if (g->cullFrame < uint64_t(age)) continue;
                    const int slot = int((g->cullFrame - uint64_t(age)) % State::CULL_SLOTS);
                    GLsync& fence = g->cullFences[slot];
                    if (!fence) continue;
                    const GLenum status = glClientWaitSync(fence, 0, 0);
                    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) continue;
                    uint32_t counters[4] = {};
                    GL::GetBufferSubData(g->cullStats[slot], 0, sizeof(counters), counters);
                    g->gpuVisible = counters[0];
                    g->gpuFrustumCulled = counters[1];
                    g->gpuOcclusionCulled = counters[2];
                    glDeleteSync(fence);
                    fence = nullptr;
                }
                ++g->cullFrame;
            }
            stats.ringBlockedWaits = g->ring.BlockedWaits();
            if (compat) for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {   // no debug output on 4.1
                if (g->loggedDebugMessages++ < 20) Log::Error() << "OpenGL error 0x" << std::hex << e << std::dec;
                ++g->glErrors;
            }
            g->lines.clear();
            g->text.clear();
            g->ribbons.clear();
            g->decals.clear();
            g->ribbonStrips = 0;
            g->frameStats = stats;
        }
    }
}
