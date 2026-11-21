#pragma once
// PhisiksLab interactive studio (Dear ImGui): scene hierarchy, entity inspector with viewport gizmos,
// classroom lesson switcher with live simulation / rendering controls, CPU/GPU/RAM telemetry, JSON
// scene save/load and the replay timeline scrubber.
#include <cstdint>
#include <string>

namespace Phisiks { struct GameContext; }

namespace Lab::Studio {
    void OnSceneLoaded(Phisiks::GameContext& ctx);   // resets the selection
    void Draw(Phisiks::GameContext& ctx);            // inside IGame::OnGui
    void HandleViewportClick(Phisiks::GameContext& ctx);   // picking (entity id buffer) when the UI does not own the mouse
    uint32_t SelectedEntity();                       // Entity::Id() + 1, 0 = none
    void Select(uint32_t entityIdPlusOne);
    std::string& ScenePath();                        // JSON path used by Save / Load
}
