#pragma once
// Shared building blocks of the example projects built on PHISIKS (Project Marina, Old-McDonald):
// entities made of real scene components, wire / tube runs from the tubing kit, subsystem visibility
// and the project-file buttons. Everything works on ordinary entities, so the
// editor hierarchy, inspector, gizmo and JSON save / load apply unchanged.
#include "Phisiks/Scene/Scene.h"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Phisiks { struct GameContext; }

namespace Projects::ProjectKit {
    inline constexpr float PI = 3.14159265358979f;

    // Tubing / wiring kit (tools/assetgen, CC0): unit straights and 90 degree bends built for a
    // centreline bend radius of BEND_RATIO x OD (projects/assets/meshes/marina, where it was introduced).
    inline constexpr const char* KIT = "meshes/marina/";
    inline constexpr float BEND_RATIO = 1.5f;
    inline constexpr float CORE_RATIO = 0.6f;

    // Plain PBR material (Render::RegisterMaterial; saved with the scene).
    void Material(const std::string& name, glm::vec3 base, float roughness, float metallic = 0.0f, glm::vec3 emissive = glm::vec3(0.0f),
                  float opacity = 1.0f, int pattern = 0, glm::vec3 patternColor = glm::vec3(0.0f), float patternScale = 1.0f, float lineWidth = 0.04f);

    Phisiks::Entity Group(Phisiks::Scene& scene, const std::string& name, Phisiks::Entity parent, const glm::vec3& position = glm::vec3(0.0f));
    // Entity with a MeshRenderer (`mesh` is a full mesh name, e.g. "meshes/marina/rod.obj" or "Cube").
    Phisiks::Entity Part(Phisiks::Scene& scene, const std::string& name, Phisiks::Entity parent, const std::string& mesh, const std::string& material,
                         const glm::vec3& position = glm::vec3(0.0f), bool castShadows = true);
    glm::quat FromY(const glm::vec3& dir);   // rotation taking +Y onto dir

    // A run through axis-aligned corner points: numbered straight pieces + 90 degree bends grouped
    // under one entity placed at the first point. Tubing (core set) gets a clear shell with a fluid
    // core and is pushed `sleeve` metres over the fittings at both ends.
    struct RunStyle {
        float od = 0.002f;
        std::string shell;        // outer material
        std::string core;         // fluid core material (tubing) or empty (wire / cable)
        float sleeve = 0.0f;
    };
    Phisiks::Entity BuildRun(Phisiks::Scene& scene, Phisiks::Entity parent, const std::string& name, std::vector<glm::vec3> pts, const RunStyle& style);
    bool RunPolyline(Phisiks::Scene& scene, const std::string& group, std::vector<glm::vec3>& out);   // visible pieces, in order
    float RunGap(Phisiks::Scene& scene, const std::string& group);   // worst gap between consecutive pieces (m)

    // Visibility of every MeshRenderer below `e`.
    void SetVisible(Phisiks::Entity e, bool visible);
    int CountVisible(Phisiks::Entity e, int& total);
    bool AnyVisible(Phisiks::Entity e);
    std::vector<Phisiks::Entity> SortedChildren(Phisiks::Entity e);   // by name

    // Workspace panel blocks.
    // Rows = `rows` (entity names, label); checkbox toggles visibility, click selects in the inspector.
    struct SubsystemRow { std::string entity; std::string label; };
    void SubsystemList(Phisiks::Scene& scene, const char* id, const std::vector<SubsystemRow>& rows, float lines = 9.0f);
    void SelectedAssembly(Phisiks::Scene& scene, const std::string& rootName);   // "Selected: ..." + select-parent button
    // Save / Reopen / Rebuild on the workspace's scene file (Editor::SaveScene / LoadScene, so the
    // workspace's persistence hooks run). Reopen and Rebuild ask about unsaved changes first.
    void ProjectFileButtons(Phisiks::GameContext& ctx, Phisiks::Scene& scene, std::string& status);
    // PNG of the last presented 3D frame (the editor panels are not included) to
    // screenshots/<prefix>_<date>_<time>.png (Editor::ResolveScenePath: next to the executable or the
    // working directory, else the per-user folder); sets `status`.
    void ScreenshotButton(const std::string& prefix, std::string& status);
}
