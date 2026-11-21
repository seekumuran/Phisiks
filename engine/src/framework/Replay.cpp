#include "Phisiks/Framework/Replay.h"
#include "Phisiks/Framework/CameraController.h"
#include "Phisiks/Framework/Input.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Scene/Scene.h"

#include <algorithm>

namespace Phisiks {
    void Replay::SetCapacity(size_t snapshots) {
        m_capacity = std::max<size_t>(snapshots, 2);
        while (m_snapshots.size() > m_capacity) m_snapshots.pop_front();
    }

    void Replay::Clear() {
        m_snapshots.clear();
        m_scrubbed = false;
        m_scrubIndex = 0;
    }

    void Replay::Record(Scene& scene, uint64_t step, double simulationTime, const InputSystem* input, const CameraController* camera) {
        if (!m_enabled) return;
        if (m_scrubbed) ResumeFromScrub();
        ReplaySnapshot s;
        s.step = step;
        s.simulationTime = simulationTime;
        if (input) {
            for (int a = 0; a < int(Action::Count) && a < 32; ++a) if (input->Down(Action(a))) s.actionsDown |= 1u << a;
        }
        if (camera) {
            s.cameraPosition = camera->GetPosition();
            s.cameraYaw = camera->GetYaw();
            s.cameraPitch = camera->GetPitch();
        }
        s.entities.reserve(scene.EntityCount());
        scene.Each<Transform>([&](Entity e, Transform& t) {
            ReplayEntityState state;
            state.entity = e.Id();
            state.position = t.position;
            state.rotation = t.rotation;
            state.scale = t.scale;
            const Physics::BodyId body = e.GetBody();
            if (body.Valid()) {
                state.hasBody = true;
                state.linearVelocity = Physics::GetLinearVelocity(body);
                state.angularVelocity = Physics::GetAngularVelocity(body);
            }
            s.entities.push_back(state);
        });
        std::sort(s.entities.begin(), s.entities.end(), [](const ReplayEntityState& a, const ReplayEntityState& b) { return a.entity < b.entity; });
        m_snapshots.push_back(std::move(s));
        while (m_snapshots.size() > m_capacity) m_snapshots.pop_front();
    }

    size_t Replay::MemoryBytes() const {
        size_t bytes = 0;
        for (const ReplaySnapshot& s : m_snapshots) bytes += sizeof(ReplaySnapshot) + s.entities.capacity() * sizeof(ReplayEntityState);
        return bytes;
    }

    bool Replay::Restore(Scene& scene, size_t index) {
        if (index >= m_snapshots.size()) return false;
        const ReplaySnapshot& s = m_snapshots[index];
        // transforms first (parents before bodies read their world pose)
        for (const ReplayEntityState& state : s.entities) {
            Entity e = scene.FromId(state.entity);
            if (!e) continue;
            Transform& t = e.LocalTransform();
            t.position = state.position;
            t.rotation = state.rotation;
            t.scale = state.scale;
        }
        for (const ReplayEntityState& state : s.entities) {
            if (!state.hasBody) continue;
            Entity e = scene.FromId(state.entity);
            if (!e) continue;
            const Physics::BodyId body = e.GetBody();
            if (body.Valid()) Physics::SetBodyState(body, e.WorldPosition(), e.WorldRotation(), state.linearVelocity, state.angularVelocity);
        }
        m_scrubbed = true;
        m_scrubIndex = index;
        return true;
    }

    void Replay::ResumeFromScrub() {
        if (!m_scrubbed) return;
        while (m_snapshots.size() > m_scrubIndex + 1) m_snapshots.pop_back();
        m_scrubbed = false;
    }
}
