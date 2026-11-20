#include "runtime/Presentation.h"
#include "Phisiks/Audio/Audio.h"
#include "render/RendererInternal.h"
#include "scene/SceneInternal.h"

namespace Phisiks::Presentation {
    namespace {
        using namespace SceneInternal;

        class RenderBackend final : public IRenderBackend {
        public:
            uint64_t CreateMesh(const MeshRenderer& renderer, const glm::mat4& world, uint32_t entityId) override {
                return Render::Internal::CreateObject(renderer, world, entityId);
            }
            void UpdateMesh(uint64_t object, const MeshRenderer& renderer, const glm::mat4& world) override {
                Render::Internal::UpdateObject(object, renderer, world);
            }
            void DestroyMesh(uint64_t object) override { Render::Internal::DestroyObject(object); }
            uint64_t CreateLight(const Light& light, const glm::vec3& position, const glm::vec3& direction) override {
                return Render::Internal::CreateLight(light, position, direction);
            }
            void UpdateLight(uint64_t id, const Light& light, const glm::vec3& position, const glm::vec3& direction) override {
                Render::Internal::UpdateLight(id, light, position, direction);
            }
            void DestroyLight(uint64_t id) override { Render::Internal::DestroyLight(id); }
            void SetCameras(const std::vector<CameraView>& cameras) override {
                std::vector<Render::Internal::CameraInput> inputs;
                inputs.reserve(cameras.size());
                for (const CameraView& c : cameras) inputs.push_back({ c.view, c.camera });
                Render::Internal::SetCameras(inputs);
            }
            RenderStats Stats() override { return Render::Internal::Stats(); }
        };

        class AudioBackend final : public IAudioBackend {
        public:
            uint64_t Play(const std::string& clip, float volume, bool loop) override { return Audio::Play(clip, volume, loop); }
            void Stop(uint64_t voice) override { Audio::Stop(voice); }
            bool IsPlaying(uint64_t voice) override { return Audio::IsPlaying(voice); }
        };

        RenderBackend g_render;
        AudioBackend g_audio;
    }

    void Install(bool withRenderer) { SetBackends(withRenderer ? &g_render : nullptr, &g_audio); }
    void Uninstall() { SetBackends(nullptr, nullptr); }
}
