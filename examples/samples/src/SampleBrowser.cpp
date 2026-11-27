#include "SampleBrowser.h"
#include "Editor.h"
#include "LessonScenes.h"
#include "P9Scenes.h"
#include "SandboxScenes.h"

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

namespace Retro = Phisiks::UI::Retro;

namespace Samples {
    namespace {
        struct Sample { const char* title; const char* scene; const char* tag; const char* about; };
        const Sample SAMPLES[] = {
            { "Transforms & Rotors", "Rotors", "MATH", "Euler angles, quaternions and geometric-algebra rotors give the same rotation." },
            { "RK4 vs Euler Orbits", "Orbits", "MATH", "Integrator accuracy: explicit Euler gains energy, RK4 keeps the orbit closed." },
            { "Friction & Raycasts", "Sandbox", "PHYS", "Coulomb friction, raycast picking and the character controller (F2: physics debug)." },
            { "Rigid-body Stacking", "PhysicsTower", "PHYS", "A 16-box tower: solver sub-steps and sleeping." },
            { "PBR Lighting", "PBR", "GFX", "Roughness x metallic grid: GGX specular, hemisphere ambient, rim light." },
            { "GPU Culling, Particles, XPBD", "FieldLab", "GPU", "Frustum + Hi-Z culling, spatial-hash particles, XPBD cloth / rope / soft box, force fields, SDF visualiser." },
            { "Emitters & Coupling", "Fountain", "GPU", "A particle emitter whose jet pushes dynamic crates (two-way coupling)." },
            { "glTF Skinning & Camera Paths", "Animation", "ANIM", "Dual-quaternion vs linear blend skinning of an imported glTF; Catmull-Rom fly-through." },
            { "Bloom, SSAO, Trails & Decals", "VFX", "VFX", "HDR bloom mip chain, SSAO, ribbon trails, orbit paths, a raycast beam and projected decals." },
            { "Terrain, Foliage & NavMesh", "Terrain", "AI", "Splatmapped height-field terrain, instanced foliage, A* crowd, Doppler spatial audio." },
            { "Joints & Lua Scripting", "Joints", "LUA", "Hinge / ball / prismatic / spring joints on both physics backends, driven by Lua scripts." },
        };
    }

    void RegisterAll(Phisiks::SceneManager& scenes) {
        RegisterScenes(scenes);
        RegisterLessonScenes(scenes);
        RegisterP9Scenes(scenes);
    }

    void RegisterAllMaterials() {
        RegisterMaterials();
        RegisterLessonMaterials();
        RegisterP9Materials();
    }

    void BrowserPanel(Phisiks::GameContext& ctx) {
        Phisiks::Scene* scene = ctx.CurrentScene();
        const ImVec4 amber(Retro::AMBER[0], Retro::AMBER[1], Retro::AMBER[2], 1.0f);
        if (Retro::BeginPlaylist("##samples", (ImGui::GetTextLineHeight() + 4.0f) * 7.5f)) {
            int n = 1;
            for (const Sample& s : SAMPLES) {
                const bool current = scene && scene->Name() == s.scene;
                if (Retro::PlaylistRow(n++, s.title, s.tag, current)) { ClearCaption(); Editor::RequestScene(ctx, s.scene); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.about);
            }
        }
        Retro::EndPlaylist();
        if (!CaptionLine1().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, amber);
            ImGui::TextWrapped("%s", CaptionLine1().c_str());
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", CaptionLine2().c_str());
        }
        ImGui::Spacing();
        Editor::DrawSettingsPanel(ctx);
    }

    void SamplesMenu(Phisiks::GameContext& ctx) {
        if (!ImGui::BeginMenu("Samples")) return;
        for (const Sample& s : SAMPLES) if (ImGui::MenuItem(s.title, s.tag)) { ClearCaption(); Editor::RequestScene(ctx, s.scene); }
        ImGui::EndMenu();
    }
}
