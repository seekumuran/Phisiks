#pragma once
// Standalone CAD workspace framework for assemblies built on PHISIKS (the Old-McDonald rover and
// drone). One assembly on its own in a neutral studio (ground grid + three lights), built from the
// same shared builder the project scene uses, so every part stays an ordinary editable entity.
//
// The base provides the whole workspace: perspective + six orthographic principal views, eased
// explode with per-part offsets (wiring hidden while exploded), CAD edge / ground toggles,
// subsystem visibility + selection, hide-selected / show-all, the selected part's subsystem and
// material, and Save / Reopen / Rebuild on the workspace's scene file. The editor's persistence
// hooks keep the explode state consistent on every save / load path (the scene file always holds
// the assembled layout). A subclass supplies the Workspace() description and BuildAssembly(), and
// may add its own panel section.
#include "ProjectKit.h"
#include "Editor.h"

#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Projects {
    class CadWorkspace : public Phisiks::Scene, public Editor::WorkspaceExtension {
    public:
        static constexpr const char* STUDIO = "CAD Studio";

        // ---- camera presets: perspective + six orthographic principal views (assembly +X = front, +Z = right)
        enum View { Perspective, Front, Rear, Left, Right, Top, Bottom, ViewCount };
        void ApplyView(Phisiks::GameContext& ctx, int view);
        int CurrentView() const { return m_view; }

        // ---- exploded view (0 assembled .. 1 exploded), eased towards the target
        void SetExplodeTarget(float t) { m_explodeTarget = t; }
        void SetExplodeNow(float t);
        float Explode() const { return m_explode; }

        void OnEnter(Phisiks::GameContext& ctx) override;
        void OnExit(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;

        // ---- Editor::WorkspaceExtension
        const char* WorkspaceTitle() const override { return Workspace().panelTitle; }
        void DrawWorkspacePanel(Phisiks::GameContext& ctx) override;
        const char* DefaultScenePath() const override { return Workspace().projectPath; }
        void OnBeforeSave(Phisiks::GameContext& ctx) override;    // collapse to the assembled layout
        void OnAfterSave(Phisiks::GameContext& ctx) override;     // restore the explode state
        void OnAfterLoad(Phisiks::GameContext& ctx) override;     // the loaded file is assembled

        bool cadEdges = true, showGround = true;

    protected:
        explicit CadWorkspace(const char* sceneName) : Scene(sceneName) {}

        struct ExplodePart { std::string entity; glm::vec3 offset; };
        struct Description {
            const char* panelTitle;          // "ROVER CAD"
            const char* rootName;            // assembly root entity
            const char* projectPath;         // Save / Reopen JSON
            const char* screenshotPrefix;
            std::vector<ProjectKit::SubsystemRow> subsystems;
            glm::vec3 centre;                // framing target (assembled)
            float perspectiveDistance, orthoDistance;
            float perspectiveYaw = 0.80f, perspectivePitch = -0.42f;
            glm::vec3 planCentre;            // Top / Bottom target
            glm::vec3 explodeTargetShift;    // framing target shift at explode = 1
            float explodeGrow;               // framing distance growth at explode = 1
            glm::vec3 explodeRootLift;       // whole-assembly lift at explode = 1 (keeps parts above the ground)
        };
        struct Built {
            std::vector<ExplodePart> explode;
            std::vector<std::string> wiring;   // groups shown only while assembled
        };

        virtual const Description& Workspace() const = 0;
        virtual Built BuildAssembly() = 0;                       // builds the assembly root at the origin
        virtual void OnAssemblyBuilt() {}                        // e.g. arm stow pose
        virtual void DrawAssemblyPanel(Phisiks::GameContext&) {} // section between EXPLODE and DISPLAY

    private:
        void Build();
        void ApplyEnvironment();
        void ApplyExplode(float from, float to);
        void SetGroundVisible();

        Built m_built;
        float m_explode = 0.0f, m_explodeTarget = 0.0f;
        float m_savedExplode = 0.0f;           // OnBeforeSave -> OnAfterSave
        int m_view = Perspective;
        bool m_cameraNearSet = false;

        Phisiks::Render::RenderSettings m_savedSettings;
        float m_savedFov = 1.0f, m_savedMinDistance = 0.5f, m_savedMinPitch = -1.5f, m_savedMaxPitch = 1.5f;
        bool m_savedOverlay = true;
        std::string m_status;
    };
}
