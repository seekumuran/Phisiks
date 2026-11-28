#pragma once
// Engine-private scene components and presentation backends.

#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Scene/Components.h"

#include <entt.hpp>
#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Phisiks::SceneInternal {

    struct Hierarchy {
        entt::entity parent = entt::null;
        std::vector<entt::entity> children;
    };

    struct PhysicsRuntime { Physics::BodyId body; };
    struct JointRuntime { Physics::JointId joint; };

    struct MeshRuntime {
        uint64_t object = 0;
        glm::mat4 world{ 1.0f };
        MeshRenderer synced;
    };

    struct LightRuntime {
        uint64_t light = 0;
        glm::vec3 position{ 0.0f };
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
        Light synced;
    };

    struct CameraView {
        glm::mat4 view;      // world -> camera
        Camera camera;
        uint32_t entityId;
    };

    // Rendering backend used by Scene::SyncPresentation.
    class IRenderBackend {
    public:
        virtual ~IRenderBackend() = default;
        virtual uint64_t CreateMesh(const MeshRenderer& renderer, const glm::mat4& world, uint32_t entityId) = 0;
        virtual void UpdateMesh(uint64_t object, const MeshRenderer& renderer, const glm::mat4& world) = 0;
        virtual void DestroyMesh(uint64_t object) = 0;
        // direction = the entity's world -Z (where directional/spot lights shine)
        virtual uint64_t CreateLight(const Light& light, const glm::vec3& position, const glm::vec3& direction) = 0;
        virtual void UpdateLight(uint64_t light, const Light& settings, const glm::vec3& position, const glm::vec3& direction) = 0;
        virtual void DestroyLight(uint64_t light) = 0;
        // The enabled cameras of the scene, sorted by Camera::order (empty: nothing to render).
        virtual void SetCameras(const std::vector<CameraView>& cameras) = 0;
        virtual RenderStats Stats() = 0;
    };

    class IAudioBackend {
    public:
        virtual ~IAudioBackend() = default;
        virtual uint64_t Play(const std::string& clip, float volume, bool loop) = 0;   // 0 = failed
        virtual uint64_t PlaySpatial(const std::string& clip, float volume, bool loop, const glm::vec3& /*position*/,
                                     float /*minDistance*/, float /*maxDistance*/, float /*dopplerFactor*/) { return Play(clip, volume, loop); }
        virtual void SetVoicePosition(uint64_t /*voice*/, const glm::vec3& /*position*/, const glm::vec3& /*velocity*/) {}
        virtual void SetListener(const glm::vec3& /*position*/, const glm::vec3& /*forward*/, const glm::vec3& /*up*/, const glm::vec3& /*velocity*/) {}
        virtual void Stop(uint64_t voice) = 0;
        virtual bool IsPlaying(uint64_t voice) = 0;
    };

    // Null by default (headless tests); the runtime installs real backends at startup.
    void SetBackends(IRenderBackend* render, IAudioBackend* audio);
    IRenderBackend* Render();
    IAudioBackend* Audio();
}
