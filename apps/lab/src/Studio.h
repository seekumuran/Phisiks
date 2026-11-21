#pragma once
// PhisiksLab interactive studio (Dear ImGui, "retro deck" skin: Phisiks/UI/RetroDeck.h): lesson
// playlist, equaliser-style simulation sliders, LCD / LED telemetry, scene hierarchy playlist,
// entity inspector with viewport gizmos, JSON scene save/load and the replay transport bar.
// Tab hides / shows the panels (the engine's bitmap HUD text only appears while they are hidden).
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
    void SetCaption(const std::string& line1, const std::string& line2);   // lesson caption (classroom panel)
    const std::string& Caption();                    // last caption (both lines)
}
