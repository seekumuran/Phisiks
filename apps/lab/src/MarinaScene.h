#pragma once
// Project Marina: an editable engineering visualisation of a waste-heat membrane-distillation
// desalination prototype, built from ordinary scene entities in PhisiksLab.
//
//   Bench           aluminium tooling plate, electronics sub-plate, splash wall, slotted wiring duct
//   1  SBC          Raspberry Pi format computer with SoC water block and 30 mm fan (the heat source)
//   2  Cooling      closed coolant loop SBC -> pump P1 -> heat exchanger HX1 -> SBC (clear tubing)
//   3  Electronics  UNO-format controller, ESP32-C3 board, breadboard, MOSFET pump driver
//   4  Pump P1      DC diaphragm pump on a strapped bracket
//   5  HX1          brazed-plate heat exchanger (coolant <-> seawater feed)
//   6  MD1          enclosed flat-sheet membrane distillation cell, hot side / cold side
//   7  TK1          seawater reservoir (feed out, brine return)
//   8  Condensation finned condenser CD1 on a stand above the freshwater bottle TK2
//   9  Instruments  temperature probes T1-T4, sensor cables in the duct, jumper wiring
//
// Every part is its own entity (selectable, movable, editable materials) grouped per subsystem in
// the hierarchy; meshes come from assets/meshes/marina (tools/assetgen) and materials are saved
// with the scene, so File > Save JSON / Load JSON round-trips the whole project. Tubes and wires
// are chains of straight + 90 degree bend pieces (each a scene entity). The optional flow
// animation follows those pieces and only shows the intended flow direction: no thermal or fluid
// simulation is run and no values are displayed.
#include "ProjectKit.h"
#include "Studio.h"

#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Phisiks { class SceneManager; }

namespace Lab {
    class MarinaScene : public Phisiks::Scene, public StudioExtension {
    public:
        MarinaScene() : Scene("ProjectMarina") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void OnExit(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        void DrawUI(Phisiks::GameContext& ctx) override;   // flow animation (ribbons, this frame)

        // StudioExtension
        void DrawStudioOverlay(Phisiks::GameContext& ctx) override;   // callouts, title block
        void DrawStudioPanel(Phisiks::GameContext& ctx) override;     // PROJECT MARINA console panel
        const char* DefaultProjectPath() const override { return "projects/project_marina.json"; }

        // A tubing / wiring run: the scene entity (group of numbered pieces) the flow animation follows.
        struct FlowRoute {
            std::string group;
            glm::vec3 color{ 1.0f };
            float od = 0.008f;
            bool fluid = true;           // tubing (animated) or wiring (not animated)
        };
        using Callout = ProjectKit::Callout;
        struct CameraPreset {
            const char* name;
            glm::vec3 target;
            float distance, yaw, pitch;
        };

        // Rebuilds the polyline a run currently follows (piece order = child name order).
        bool RoutePolyline(const std::string& group, std::vector<glm::vec3>& out);
        // Largest gap between consecutive pieces of a run in metres (0 when every piece meets the next).
        float RouteGap(const std::string& group);
        const std::vector<FlowRoute>& Routes() const { return m_routes; }
        void ApplyCamera(Phisiks::GameContext& ctx, int preset);

        bool showLabels = true;
        bool showFlow = true;
        bool spinFan = true;
        bool cadEdges = true;
        float flowSpeed = 1.0f;          // schematic dash speed multiplier

    private:
        void Build();
        void ApplyEnvironment();

        std::vector<FlowRoute> m_routes;
        std::vector<Callout> m_callouts;
        float m_flowPhase = 0.0f;
        int m_camera = 0;
        bool m_cameraNearSet = false;
        Phisiks::Render::RenderSettings m_savedSettings;
        float m_savedFov = 1.0f, m_savedMinDistance = 0.5f;
        bool m_savedOverlay = true;
        std::string m_status;
    };

    void RegisterMarinaScene(Phisiks::SceneManager& scenes);
}
