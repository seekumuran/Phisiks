#pragma once
// Deterministic replay ring buffer + timeline scrubbing.
//
// The Application records one snapshot per fixed physics step while the game runs (when enabled):
// the local Transform of every entity, the velocities of every physics body, the camera pose and
// the input actions held during that step. While paused, Restore(scene, i) puts the scene back to
// snapshot i exactly (transforms, body poses and velocities); resuming from there discards the
// newer snapshots, so the simulation continues from the scrubbed frame.
// Scope: rigid bodies and transforms. GPU/CPU particles and XPBD soft bodies are not snapshotted
// (they keep their current state when scrubbing).

#include "Phisiks/Physics/PhysicsTypes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Phisiks {
    class Scene;
    class InputSystem;
    class CameraController;

    struct ReplayEntityState {
        uint32_t entity = 0;                 // Entity::Id()
        glm::vec3 position{ 0.0f };          // local transform
        glm::quat rotation{ 1, 0, 0, 0 };
        glm::vec3 scale{ 1.0f };
        bool hasBody = false;
        glm::vec3 linearVelocity{ 0.0f }, angularVelocity{ 0.0f };
    };

    struct ReplaySnapshot {
        uint64_t step = 0;                   // FrameStats::totalFixedSteps after the step
        double simulationTime = 0.0;
        uint32_t actionsDown = 0;            // bit per Phisiks::Action held during the step
        glm::vec3 cameraPosition{ 0.0f };
        float cameraYaw = 0.0f, cameraPitch = 0.0f;
        std::vector<ReplayEntityState> entities;
    };

    class Replay {
    public:
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool Enabled() const { return m_enabled; }
        void SetCapacity(size_t snapshots);          // ring size (default 1800 = 30 s at 60 Hz)
        size_t Capacity() const { return m_capacity; }

        void Record(Scene& scene, uint64_t step, double simulationTime, const InputSystem* input = nullptr, const CameraController* camera = nullptr);
        void Clear();

        size_t Size() const { return m_snapshots.size(); }
        const ReplaySnapshot& At(size_t index) const { return m_snapshots[index]; }
        size_t MemoryBytes() const;

        // Restores snapshot `index` (0 = oldest). Returns false if out of range.
        bool Restore(Scene& scene, size_t index);
        bool Scrubbed() const { return m_scrubbed; }
        size_t ScrubIndex() const { return m_scrubIndex; }
        // Before simulating again after a Restore: drops the snapshots newer than the restored one.
        void ResumeFromScrub();

    private:
        std::deque<ReplaySnapshot> m_snapshots;
        size_t m_capacity = 1800;
        bool m_enabled = false;
        bool m_scrubbed = false;
        size_t m_scrubIndex = 0;
    };
}
