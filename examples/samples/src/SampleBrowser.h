#pragma once
// Sample browser for the PHISIKS sample scenes (examples/samples): a side panel listing the samples
// with their captions, and a "Samples" menu. Used by the PhisiksSamples executable on top of the editor.
namespace Phisiks { struct GameContext; class SceneManager; }

namespace Samples {
    void RegisterAll(Phisiks::SceneManager& scenes);   // Sandbox, PhysicsTower, FieldLab, lessons, P9 scenes
    void RegisterAllMaterials();                        // IGame::OnStart (needs the renderer)
    void BrowserPanel(Phisiks::GameContext& ctx);       // Editor::AppConfig::sidePanel
    void SamplesMenu(Phisiks::GameContext& ctx);        // Editor::AppConfig::menus
}
