#pragma once
// Old-McDonald: an editable engineering visualisation of an autonomous agricultural monitoring
// system in a greenhouse, built from ordinary scene entities on the PHISIKS engine and editor.
//
//   1 Greenhouse      slab, aluminium frame, glazing, dock bay, drone pad (+ static colliders)
//   2 Growing beds    three raised beds, 48 crops with crop-location rings
//   3 Mapping drone   F450-class quad: Pixhawk 2.4.8, ReadyToSky GPS, motors, ESCs, props, PDB, LiPo
//   4 Soil rover      chassis, drive, Raspberry Pi Pico, motor driver, 2D LiDAR, sensor-positioning
//                     arm carrying the soil moisture / temperature / pH probes
//   5 Sampling grid   16 fixed sampling points SP-A1 .. SP-D4 (rows A-D x columns 1-4)
//   6 Mission route   rover route (engine A* on the navigation grid) and the drone survey path
//
// Mission playback runs the operational sequence in order (drone and rover never at the same
// time). What is real engine work: the rover's route is planned by Phisiks A* on a NavGrid built
// from the scene's static colliders and followed by NavAgent steering; the arm pose at every
// sampling point comes from inverse kinematics. What is scripted: the drone's flight (a spline,
// no flight dynamics or control), crop "mapping" (geometric coverage under an assumed footprint),
// data transfer / processing (a timed step), and measurements (a timed step; no sensor values).
#include "ProjectKit.h"
#include "Editor.h"

#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace Phisiks { class SceneManager; }

namespace Projects {
    class OldMcDonaldScene : public Phisiks::Scene, public Editor::WorkspaceExtension {
    public:
        OldMcDonaldScene() : Scene("OldMcDonald") {}
        void OnEnter(Phisiks::GameContext& ctx) override;
        void OnExit(Phisiks::GameContext& ctx) override;
        void Update(Phisiks::GameContext& ctx, float dt) override;
        bool RunsWhilePaused() const override { return true; }   // workspace tools and view animation in edit mode
        void DrawUI(Phisiks::GameContext& ctx) override;   // survey footprint lines (this frame)

        const char* WorkspaceTitle() const override { return "OLD-McDONALD"; }
        void OnBeforeSave(Phisiks::GameContext& ctx) override;
        void OnAfterSave(Phisiks::GameContext& ctx) override;
        void OnAfterLoad(Phisiks::GameContext& ctx) override;
        // The navigation surface and the rover's agent are re-attached by EnsureRuntime after every load.
        std::vector<std::string> ComponentsRestoredAfterLoad() const override { return { "NavigationSurface", "NavAgent" }; }
        void DrawWorkspacePanel(Phisiks::GameContext& ctx) override;
        const char* DefaultScenePath() const override { return "projects/OldMcDonald/scenes/old_mcdonald.scene.json"; }

        // ---- camera presets
        enum Preset { Presentation, DroneClose, RoverClose, Overview, TopPlan, DroneExploded, RoverExploded, MissionView, PresetCount };
        void ApplyCamera(Phisiks::GameContext& ctx, int preset);
        int CurrentPreset() const { return m_camera; }

        // ---- exploded assemblies (0 = assembled, 1 = fully exploded); animated towards the target
        enum Assembly { DroneAssembly, RoverAssembly };
        void SetExplodeTarget(int assembly, float target) { m_explodeTarget[assembly] = target; }
        void SetExplodeNow(int assembly, float value);
        float Explode(int assembly) const { return m_explode[assembly]; }

        // ---- mission playback
        enum class Phase { Idle, Takeoff, Survey, Land, Transfer, Grid, Depart, Drive, Deploy, Measure, Stow, Return, Docked };
        void MissionPlay();
        void MissionPause() { m_playing = false; }
        void MissionReset();
        bool MissionPlaying() const { return m_playing; }
        Phase GetPhase() const { return m_phase; }
        int Stage() const;                         // 1..9 (0 idle, 10 complete)
        int StagesSeen() const { return m_stagesSeen; }   // bit (1 << stage) for every stage shown
        int SamplesTaken() const { return m_samplesTaken; }
        int CropsMapped() const { return m_cropsMapped; }
        int PathFailures() const { return m_pathFailures; }
        bool DroneLandedBeforeRover() const { return m_droneLandedBeforeRover; }
        bool RouteBuilt() const;
        float missionSpeed = 2.0f;

        bool cadEdges = true, spin = true, follow = false;

        // Re-attaches the runtime-only components (NavAgent, NavigationSurface) that the JSON scene
        // format does not store; called every frame, cheap when nothing is missing.
        void EnsureRuntime();

    private:
        struct Stop { glm::vec3 position; std::vector<int> samples; };
        struct ExplodePart { std::string entity; glm::vec3 offset; };
        struct ArmPose { float q[4]; };

        static ArmPose StowPose();
        void Build();
        void BuildRoverRoute();
        void ApplyEnvironment();
        void ApplyExplode(int assembly, float value);
        void SetArm(const ArmPose& pose);
        ArmPose CurrentArm();
        bool SolveArm(const glm::vec3& tipWorld, ArmPose& out);
        void MissionStep(Phisiks::GameContext& ctx, float dt);
        void SetPhase(Phase phase);
        void SetSampleState(int index, int state);   // 0 pending, 1 active, 2 sampled
        void SetCropMapped(size_t index, bool mapped);
        glm::vec3 SamplePoint(int index) const;
        Phisiks::Entity Rover();
        Phisiks::Entity Drone();

        std::vector<ExplodePart> m_explodeParts[2];
        std::vector<std::string> m_crops;       // crop-location ring entities
        std::vector<Stop> m_stops;
        std::vector<glm::vec3> m_surveyPoints;
        float m_surveyMapStart = 0.0f;           // survey u where mapping passes begin (stage 2)
        float m_explode[2] = { 0.0f, 0.0f }, m_explodeTarget[2] = { 0.0f, 0.0f }, m_savedExplode[2] = { 0.0f, 0.0f };

        Phase m_phase = Phase::Idle;
        bool m_playing = false;
        float m_phaseTime = 0.0f, m_surveyU = 0.0f;
        size_t m_stop = 0, m_stopSample = 0;
        ArmPose m_armFrom{}, m_armTo{};
        int m_samplesTaken = 0, m_cropsMapped = 0, m_pathFailures = 0, m_gridRevealed = 0, m_stagesSeen = 0;
        bool m_droneLandedBeforeRover = false;
        float m_lastTravelled = 0.0f;

        int m_camera = 0;
        bool m_cameraNearSet = false;
        Phisiks::Render::RenderSettings m_savedSettings;
        float m_savedFov = 1.0f, m_savedMinDistance = 0.5f;
        bool m_savedOverlay = true;
        std::string m_status;
    };

    void RegisterOldMcDonaldScene(Phisiks::SceneManager& scenes);
}
