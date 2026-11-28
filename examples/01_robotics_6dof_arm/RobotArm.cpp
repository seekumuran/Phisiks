// Showcase 01 (robotics): 6-DOF industrial robot cell on the 1990s CAD studio cyclorama.
//
//   * The arm (tools/assetgen meshes: bolted base, turret, cast orange upper arm / forearm housings,
//     black servo drums, 3-axis wrist, tool flange, parallel gripper) is driven by joint servos
//     (velocity-limited) through dual-quaternion forward kinematics (ArmKinematics.h).
//   * A robot program in the classic PTP / LIN form picks a workpiece (a dynamic physics box) from
//     one stand and places it on the other, back and forth: PTP moves interpolate joint space with
//     an S-curve, LIN moves follow a straight TCP line with damped-least-squares IK every tick.
//   * Coordinate frames (X red, Y green, Z blue; Z along each joint axis) at the base, J1-J6 and
//     the tool, with labels; the TCP leaves a glowing trajectory trail.
//   * Console control groups: Program, Joint Jog (J1-J6 sliders), Cartesian Jog (X / Y / Z with
//     IK, Euler Z-Y-X and quaternion readouts), and a joint-space Timeline (|<  <  >  ||  >  LIVE).
#include "ArmKinematics.h"
#include "Showcase.h"
#include "ShowcaseTest.h"

#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/UI/RetroDeck.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Showcase {
    namespace {
        using Arm::Angles;
        using Arm::DOF;
        using Arm::DEG;

        std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
            char t[256];
            std::snprintf(t, sizeof(t), f, a, b, c, d);
            return t;
        }
        float SmoothStep(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

        const glm::vec3 STAND[2] = { { 0.92f, 0.0f, 0.52f }, { 0.28f, 0.0f, -0.96f } };
        constexpr float STAND_HEIGHT = 0.58f, BOX = 0.08f, APPROACH = 0.25f;
        constexpr float GRIP_OPEN = 0.115f, GRIP_CLOSED = 0.079f;
        constexpr size_t HISTORY = 3600;   // 60 s at 60 Hz
        const Angles HOME = { 0.0f, 0.15f, -0.35f, 0.0f, 1.25f, 0.0f };

        // Joint-frame orientation (Z along the joint axis) in the zero pose.
        glm::quat FrameAlign(const glm::vec3& axis) {
            if (axis.y > 0.5f) return glm::angleAxis(-1.5707963f, glm::vec3(1, 0, 0));     // Z -> +Y
            if (axis.x > 0.5f) return glm::angleAxis(1.5707963f, glm::vec3(0, 1, 0));      // Z -> +X
            if (axis.z < -0.5f) return glm::angleAxis(3.1415926f, glm::vec3(0, 1, 0));     // Z -> -Z
            return glm::quat(1, 0, 0, 0);
        }
    }

    class RobotArmScene : public ShowcaseScene {
    public:
        RobotArmScene() : ShowcaseScene("RoboticArm") {}

        enum class Mode { Program, Manual, Replay };
        struct Move {
            enum Type { Ptp, Lin, Grip, Wait } type = Wait;
            glm::vec3 position{ 0.0f };
            glm::quat rotation{ 1, 0, 0, 0 };
            float grip = GRIP_OPEN, duration = 0.5f;
            const char* label = "";
            // filled when the move starts
            Angles qFrom{}, qTo{};
            glm::vec3 pFrom{ 0.0f };
            glm::quat rFrom{ 1, 0, 0, 0 };
            float gFrom = GRIP_OPEN;
        };
        struct Sample { Angles q; float grip; glm::vec3 boxPosition; glm::quat boxRotation; bool holding; };
        struct Part { Entity entity; int link; glm::vec3 offset; glm::quat rotation; };

        // state
        Angles q = HOME, cmd = HOME;
        float grip = GRIP_OPEN;
        Mode mode = Mode::Program;
        std::vector<Move> program;
        size_t moveIndex = 0;
        float moveTime = 0.0f;
        bool moveStarted = false;
        int from = 0, deliveries = 0, cycles = 0;
        bool holding = false;
        glm::vec3 holdOffset{ 0.0f };
        glm::quat holdRotation{ 1, 0, 0, 0 };
        Entity box;
        float maxLinIkError = 0.0f, lastIkError = 0.0f;
        glm::vec3 cartTarget = Arm::TCP_ZERO;
        bool cartActive = false;
        // visuals
        std::vector<Part> parts;
        Entity fingers[2];
        Entity arrows[DOF + 2][3];
        std::deque<glm::vec3> trail;
        bool showFrames = true, showTrail = true;
        // timeline
        std::deque<Sample> history;
        int scrub = 0;
        bool replayPlaying = false;

        // ---------------------------------------------------------------- world
        void Build(GameContext& ctx) override {
            Material("Arm/Orange", { 0.92f, 0.34f, 0.035f }, 0.38f, 0.05f);
            Material("Arm/Black", { 0.025f, 0.026f, 0.03f }, 0.32f, 0.55f);
            Material("Arm/Steel", { 0.66f, 0.67f, 0.70f }, 0.25f, 1.0f);
            Material("Arm/Base", { 0.18f, 0.19f, 0.21f }, 0.45f, 0.5f);
            Material("Arm/Pad", { 0.06f, 0.06f, 0.07f }, 0.8f);
            Material("Arm/Part", { 0.08f, 0.30f, 0.78f }, 0.35f, 0.2f, {}, Render::Pattern::Grid, { 0.75f, 0.85f, 1.0f }, 12.5f, 0.05f);
            Material("Arm/Stand", { 0.42f, 0.44f, 0.47f }, 0.4f, 0.7f);
            Material("Arm/StandTop", { 0.9f, 0.72f, 0.06f }, 0.5f, 0.0f, {}, Render::Pattern::Tiles, { 0.05f, 0.05f, 0.05f }, 10.0f, 0.08f);
            Material("Arm/Conduit", { 0.05f, 0.05f, 0.06f }, 0.6f);
            Material("Frame/X", { 0.6f, 0.02f, 0.02f }, 0.5f, 0.0f, { 1.6f, 0.06f, 0.04f });
            Material("Frame/Y", { 0.02f, 0.5f, 0.04f }, 0.5f, 0.0f, { 0.05f, 1.3f, 0.10f });
            Material("Frame/Z", { 0.02f, 0.06f, 0.6f }, 0.5f, 0.0f, { 0.08f, 0.25f, 1.9f });
            CadStudio(*this, 0.6f, glm::vec3(0.2f, 0.0f, -1.0f));
            Sun(glm::vec3(-0.45f, -1.0f, -0.55f), glm::vec3(1.0f, 0.98f, 0.95f), 2.4f, true);

            const char* meshes[] = { "arm_base", "arm_turret", "arm_motor_large", "arm_upper", "arm_forearm", "arm_motor_small",
                                     "arm_wrist_roll", "arm_wrist_pitch", "arm_tool", "arm_finger", "arm_conduit", "arrow" };
            for (const char* m : meshes) HeroMesh(m);
            auto part = [&](const char* name, const char* mesh, const char* material, int link, const glm::vec3& offset, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                            const glm::vec3& scale = glm::vec3(1.0f)) {
                Entity e = Visual(name, HeroMeshName(mesh).c_str(), material, glm::vec3(0.0f), scale);
                parts.push_back({ e, link, offset, rotation });
                return e;
            };
            const glm::quat yToZ = glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0));
            const glm::quat yToMinusX = glm::angleAxis(1.5707963f, glm::vec3(0, 0, 1));
            part("ArmBase", "arm_base", "Arm/Base", -1, glm::vec3(0.0f));
            part("ArmTurret", "arm_turret", "Arm/Orange", 0, glm::vec3(0.0f));
            part("ArmMotorJ2", "arm_motor_large", "Arm/Black", 0, glm::vec3(0.15f, 0.35f, 0.19f));
            part("ArmMotorJ1", "arm_motor_small", "Arm/Black", -1, glm::vec3(-0.17f, 0.20f, -0.17f));
            part("ArmUpper", "arm_upper", "Arm/Orange", 1, glm::vec3(0.0f));
            part("ArmMotorJ3", "arm_motor_small", "Arm/Black", 1, glm::vec3(0.0f, 0.70f, 0.11f), yToZ);
            part("ArmConduitUpper", "arm_conduit", "Arm/Conduit", 1, glm::vec3(-0.135f, 0.02f, 0.06f), glm::quat(1, 0, 0, 0), glm::vec3(0.045f, 0.62f, 0.045f));
            part("ArmForearm", "arm_forearm", "Arm/Orange", 2, glm::vec3(0.0f));
            part("ArmMotorJ4", "arm_motor_small", "Arm/Black", 2, glm::vec3(-0.19f, 0.12f, 0.0f), yToMinusX);
            part("ArmConduitFore", "arm_conduit", "Arm/Conduit", 2, glm::vec3(0.0f, 0.245f, 0.0f), glm::angleAxis(-1.5707963f, glm::vec3(0, 0, 1)), glm::vec3(0.035f, 0.30f, 0.035f));
            part("ArmWristRoll", "arm_wrist_roll", "Arm/Orange", 3, glm::vec3(0.0f));
            part("ArmMotorJ5", "arm_motor_small", "Arm/Black", 3, glm::vec3(0.20f, 0.0f, 0.075f), yToZ, glm::vec3(0.8f));
            part("ArmWristPitch", "arm_wrist_pitch", "Arm/Steel", 4, glm::vec3(0.0f));
            part("ArmTool", "arm_tool", "Arm/Steel", 5, glm::vec3(0.0f));
            for (int k = 0; k < 2; ++k) {
                fingers[k] = Visual(k == 0 ? "ArmFingerA" : "ArmFingerB", HeroMeshName("arm_finger").c_str(), "Arm/Pad", glm::vec3(0.0f), glm::vec3(1.0f));
            }
            // stands + workpiece
            for (int k = 0; k < 2; ++k) {
                const std::string n = k == 0 ? "StandA" : "StandB";
                Entity s = Box(n, STAND[k] + glm::vec3(0, STAND_HEIGHT * 0.5f - 0.01f, 0), glm::vec3(0.22f, STAND_HEIGHT - 0.02f, 0.22f), "Arm/Stand");
                (void)s;
                Box(n + "Top", STAND[k] + glm::vec3(0, STAND_HEIGHT - 0.01f, 0), glm::vec3(0.26f, 0.02f, 0.26f), "Arm/StandTop");
            }
            box = Box("Workpiece", STAND[0] + glm::vec3(0, STAND_HEIGHT + BOX * 0.5f + 0.002f, 0), glm::vec3(BOX), "Arm/Part", 1.5f);
            // frame arrows: base frame, J1..J6, tool
            const char* mats[3] = { "Frame/X", "Frame/Y", "Frame/Z" };
            for (int f = 0; f < DOF + 2; ++f) for (int a = 0; a < 3; ++a) {
                arrows[f][a] = Visual("Frame" + std::to_string(f) + "XYZ"[a], HeroMeshName("arrow").c_str(), mats[a], glm::vec3(0.0f), glm::vec3(0.12f));
                arrows[f][a].Get<MeshRenderer>().castShadows = false;
            }
            BuildProgram(0, false);
            UpdateVisuals();
            ctx.camera.SetOrbit(glm::vec3(0.45f, 0.85f, -0.1f), 4.3f, 0.75f, -0.24f);
        }

        // ---------------------------------------------------------------- program
        static glm::quat ToolDown(const glm::vec3& p) { return Arm::DownwardTool(std::atan2(-p.z, p.x)); }

        void BuildProgram(int source, bool startHolding) {
            const glm::vec3 s = STAND[source] + glm::vec3(0, STAND_HEIGHT + BOX * 0.5f + 0.002f, 0);
            const glm::vec3 d = STAND[1 - source] + glm::vec3(0, STAND_HEIGHT + BOX * 0.5f + 0.006f, 0);
            const glm::vec3 up(0, APPROACH, 0);
            program.clear();
            auto add = [&](Move::Type type, const glm::vec3& p, const glm::quat& r, float duration, const char* label, float g = GRIP_OPEN) {
                Move m;
                m.type = type; m.position = p; m.rotation = r; m.duration = duration; m.label = label; m.grip = g;
                program.push_back(m);
            };
            if (!startHolding) {
                add(Move::Ptp, s + up, ToolDown(s), 0.0f, "PTP  APPROACH PICK");
                add(Move::Grip, {}, {}, 0.3f, "GRIP OPEN", GRIP_OPEN);
                add(Move::Lin, s, ToolDown(s), 1.0f, "LIN  DOWN TO PART");
                add(Move::Grip, {}, {}, 0.45f, "GRIP CLOSE", GRIP_CLOSED);
            }
            add(Move::Lin, s + up, ToolDown(s), 0.8f, "LIN  LIFT");
            add(Move::Ptp, d + up, ToolDown(d), 0.0f, "PTP  TRANSFER");
            add(Move::Lin, d, ToolDown(d), 1.0f, "LIN  DOWN TO STAND");
            add(Move::Grip, {}, {}, 0.4f, "GRIP OPEN (RELEASE)", GRIP_OPEN);
            add(Move::Lin, d + up, ToolDown(d), 0.7f, "LIN  RETRACT");
            Move home;
            home.type = Move::Ptp; home.label = "PTP  HOME"; home.qTo = HOME; home.duration = -1.0f;   // joint target
            program.push_back(home);
            add(Move::Wait, {}, {}, 0.4f, "WAIT");
            from = source;
            moveIndex = 0;
            moveStarted = false;
        }

        const char* Status() const {
            if (mode == Mode::Replay) return replayPlaying ? "REPLAY  PLAY" : "REPLAY  PAUSED";
            if (mode == Mode::Manual) return cartActive ? "MANUAL  CARTESIAN JOG" : "MANUAL  JOINT JOG";
            return moveIndex < program.size() ? program[moveIndex].label : "-";
        }

        void StartMove(Move& m) {
            m.qFrom = cmd;
            m.pFrom = Arm::Forward(cmd).TcpPosition();
            m.rFrom = Arm::Forward(cmd).TcpRotation();
            m.gFrom = grip;
            if (m.type == Move::Ptp) {
                if (m.duration >= 0.0f) {   // Cartesian target -> joint target
                    m.qTo = Arm::SolveIk(cmd, m.position, m.rotation).q;
                }
                float maxDelta = 0.0f;
                for (int i = 0; i < DOF; ++i) maxDelta = std::max(maxDelta, std::abs(m.qTo[size_t(i)] - m.qFrom[size_t(i)]));
                m.duration = std::max(0.7f, maxDelta / (70.0f * DEG) * 1.5f);   // S-curve peak 1.5x the mean speed
            }
        }

        void StepProgram(float dt) {
            if (program.empty()) return;
            Move& m = program[moveIndex];
            if (!moveStarted) { StartMove(m); moveStarted = true; moveTime = 0.0f; }
            moveTime += dt;
            const float u = SmoothStep(moveTime / std::max(m.duration, 1e-3f));
            switch (m.type) {
                case Move::Ptp:
                    for (int i = 0; i < DOF; ++i) cmd[size_t(i)] = m.qFrom[size_t(i)] + (m.qTo[size_t(i)] - m.qFrom[size_t(i)]) * u;
                    break;
                case Move::Lin: {
                    const glm::vec3 p = glm::mix(m.pFrom, m.position, u);
                    const glm::quat r = glm::slerp(m.rFrom, m.rotation, u);
                    const Arm::IkResult ik = Arm::SolveIk(cmd, p, r, true, 12);
                    cmd = ik.q;
                    lastIkError = ik.positionError;
                    maxLinIkError = std::max(maxLinIkError, ik.positionError);
                    break;
                }
                case Move::Grip: grip = m.gFrom + (m.grip - m.gFrom) * u; break;
                case Move::Wait: break;
            }
            if (moveTime < m.duration) return;
            // move finished
            if (m.type == Move::Grip && m.grip < m.gFrom) Grab();
            if (m.type == Move::Grip && m.grip > m.gFrom && holding) Release();
            moveStarted = false;
            if (++moveIndex >= program.size()) {
                ++cycles;
                BuildProgram(1 - from, false);
            }
        }

        void Grab() {
            const Arm::Pose pose = Arm::Forward(q);
            const glm::quat inv = glm::conjugate(pose.TcpRotation());
            holdOffset = inv * (box.WorldPosition() - pose.TcpPosition());
            holdRotation = inv * box.WorldRotation();
            holding = glm::length(holdOffset) < 0.03f;   // the part is between the pads
        }

        void Release() {
            holding = false;
            ++deliveries;
        }

        // ---------------------------------------------------------------- simulation
        void FixedTick(GameContext&, float dt) override {
            if (mode == Mode::Replay) {
                if (history.empty()) return;
                if (replayPlaying) scrub = std::min(scrub + 1, int(history.size()) - 1);
                scrub = std::clamp(scrub, 0, int(history.size()) - 1);
                ApplySample(history[size_t(scrub)]);   // every tick: the physics part stays on the recorded pose
                if (replayPlaying && scrub >= int(history.size()) - 1) ExitReplay();
                return;
            }
            if (mode == Mode::Program) StepProgram(dt);
            else if (cartActive) {
                const Arm::IkResult ik = Arm::SolveIk(cmd, cartTarget, Arm::Forward(cmd).TcpRotation(), true, 20);
                cmd = ik.q;
                lastIkError = ik.positionError;
            }
            // velocity-limited joint servos
            const float vmax = 150.0f * DEG * dt;
            for (int i = 0; i < DOF; ++i) q[size_t(i)] += std::clamp(cmd[size_t(i)] - q[size_t(i)], -vmax, vmax);
            if (holding) {
                const Arm::Pose pose = Arm::Forward(q);
                box.SetWorldPose(pose.TcpPosition() + pose.TcpRotation() * holdOffset, pose.TcpRotation() * holdRotation);
            }
            history.push_back({ q, grip, box.WorldPosition(), box.WorldRotation(), holding });
            if (history.size() > HISTORY) history.pop_front();
            const glm::vec3 tcp = Arm::Forward(q).TcpPosition();
            if (trail.empty() || glm::length(trail.back() - tcp) > 0.004f) trail.push_back(tcp);
            if (trail.size() > 320) trail.pop_front();
        }

        void ApplySample(const Sample& s) {
            q = cmd = s.q;
            grip = s.grip;
            box.SetWorldPose(s.boxPosition, s.boxRotation);
        }

        void EnterReplay() {
            if (history.empty()) return;
            if (mode != Mode::Replay) scrub = int(history.size()) - 1;
            mode = Mode::Replay;
        }
        void ExitReplay() {
            if (mode != Mode::Replay) return;
            if (!history.empty()) ApplySample(history.back());   // back to the live state (the program was frozen)
            mode = Mode::Program;
            replayPlaying = false;
        }

        void Jog() {
            if (mode == Mode::Replay) ExitReplay();
            mode = Mode::Manual;
        }
        void RunProgram() {
            if (mode == Mode::Replay) ExitReplay();
            cartActive = false;
            if (mode == Mode::Manual) BuildProgram(from, holding);
            mode = Mode::Program;
        }

        // ---------------------------------------------------------------- visuals
        void UpdateVisuals() {
            const Arm::Pose pose = Arm::Forward(q);
            for (Part& p : parts) {
                if (p.link < 0) { p.entity.SetWorldPose(p.offset, p.rotation); continue; }
                const Math::DualQuat& f = pose.frames[p.link];
                p.entity.SetWorldPose(f.TransformPoint(p.offset), f.Rotation() * p.rotation);
            }
            const Math::DualQuat& tool = pose.frames[5];
            const float g = grip * 0.5f + 0.009f;
            fingers[0].SetWorldPose(tool.TransformPoint(glm::vec3(0.13f, g, 0.0f)), tool.Rotation());
            fingers[1].SetWorldPose(tool.TransformPoint(glm::vec3(0.13f, -g, 0.0f)), tool.Rotation() * glm::angleAxis(3.1415926f, glm::vec3(1, 0, 0)));
            const glm::quat toArrow[3] = { glm::angleAxis(-1.5707963f, glm::vec3(0, 0, 1)), glm::quat(1, 0, 0, 0), glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0)) };
            for (int f = 0; f < DOF + 2; ++f) {
                glm::vec3 origin;
                glm::quat frame;
                if (f == 0) { origin = glm::vec3(0.0f, 0.004f, 0.0f); frame = glm::quat(1, 0, 0, 0); }
                else if (f <= DOF) { origin = pose.Position(f - 1); frame = pose.frames[f - 1].Rotation() * FrameAlign(Arm::JOINTS[f - 1].axis); }
                else { origin = pose.TcpPosition(); frame = pose.TcpRotation() * FrameAlign(glm::vec3(1, 0, 0)); }
                const float size = f == 0 ? 0.32f : (f >= DOF ? 0.14f : 0.22f);
                for (int a = 0; a < 3; ++a) {
                    Entity e = arrows[f][a];
                    e.Get<MeshRenderer>().visible = showFrames && GetViewMode() != ViewMode::Particles;
                    e.Get<Transform>().scale = glm::vec3(size);
                    e.SetWorldPose(origin, frame * toArrow[a]);
                }
            }
        }

        bool Project(const glm::vec3& p, glm::vec2& out) {
            Entity cam = FindByName("MainCamera");
            if (!cam.Valid() || !cam.Has<Camera>()) return false;
            const glm::vec2 size = glm::vec2(Render::GetFramebufferSize());
            const Camera& c = cam.Get<Camera>();
            const glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.0f), cam.WorldPosition()) * glm::mat4_cast(cam.WorldRotation()));
            const glm::vec4 clip = glm::perspective(c.verticalFov, size.x / std::max(size.y, 1.0f), c.nearPlane, c.farPlane) * view * glm::vec4(p, 1.0f);
            if (clip.w <= 0.01f) return false;
            out = glm::vec2((clip.x / clip.w * 0.5f + 0.5f) * size.x, (0.5f - clip.y / clip.w * 0.5f) * size.y);
            return true;
        }

        void Tick(GameContext&, float) override {
            if (mode == Mode::Replay && !history.empty()) ApplySample(history[size_t(std::clamp(scrub, 0, int(history.size()) - 1))]);   // after physics too
            UpdateVisuals();
            if (showTrail && trail.size() > 2 && GetViewMode() != ViewMode::Particles) {
                std::vector<Render::RibbonPoint> pts;
                const size_t n = trail.size();
                for (size_t i = 0; i < n; ++i) {
                    const float age = float(i) / float(n - 1);
                    pts.push_back({ trail[i], 0.010f + 0.006f * age, glm::vec4(0.15f, 1.1f, 2.6f, 1.0f) * (0.15f + 0.85f * age) });
                }
                Render::DrawRibbon(pts);
            }
            if (showFrames && GetViewMode() != ViewMode::Cad && GetViewMode() != ViewMode::Particles) {
                const Arm::Pose pose = Arm::Forward(q);
                const char* names[DOF + 2] = { "BASE", "J1", "J2", "J3", "J4", "J5", "J6", "TCP" };
                for (int f = 0; f < DOF + 2; ++f) {
                    const glm::vec3 p = f == 0 ? glm::vec3(0.0f) : (f <= DOF ? pose.Position(f - 1) : pose.TcpPosition());
                    glm::vec2 s;
                    if (Project(p + glm::vec3(0.0f, 0.02f, 0.0f), s)) Render::DrawScreenText(names[f], s + glm::vec2(10.0f, -18.0f), glm::vec4(0.05f, 0.08f, 0.16f, 1.0f), 2.0f);
                }
            }
        }

        // ---------------------------------------------------------------- framework hooks
        std::vector<CameraShot> Shots() override {
            std::vector<CameraShot> shots;
            CameraShot arc;
            arc.name = "Cell orbit";
            arc.duration = 22.0f;
            arc.closed = true;
            for (int i = 0; i < 8; ++i) {
                const float a = -0.2f + float(i) / 8.0f * 6.2831853f;
                arc.points.push_back(glm::vec3(0.45f, 0.0f, -0.1f) + glm::vec3(std::cos(a) * 3.9f, 1.7f + 0.4f * std::sin(a * 2.0f), std::sin(a) * 3.9f));
                arc.lookAt.push_back(glm::vec3(0.45f, 0.8f, -0.1f));
            }
            shots.push_back(arc);
            shots.push_back(DollyShot("Wrist close-up", { 2.0f, 1.45f, 1.35f }, { 1.45f, 1.05f, 0.95f }, { 0.75f, 0.85f, 0.15f }, 10.0f));
            shots.push_back(DollyShot("Top view", { 0.45f, 5.6f, 0.05f }, { 0.65f, 5.0f, 0.55f }, { 0.45f, 0.4f, -0.1f }, 12.0f));
            shots.push_back(DollyShot("Low hero angle", { 2.7f, 0.35f, 2.0f }, { 2.4f, 0.45f, -1.9f }, { 0.4f, 1.0f, -0.1f }, 14.0f));
            shots.push_back(DollyShot("Side profile", { 0.4f, 1.0f, 3.9f }, { 0.9f, 1.25f, 3.4f }, { 0.5f, 0.9f, -0.1f }, 12.0f));
            return shots;
        }

        glm::vec4 CadBounds() const override { return glm::vec4(0.45f, 0.75f, -0.2f, 1.7f); }

        void BuildSdf(Render::SdfVisualization& sdf) override {
            // the arm's collision proxy as an SDF union of capsules (link envelopes) at the current pose
            const Arm::Pose pose = Arm::Forward(q);
            auto capsule = [&](const glm::vec3& a, const glm::vec3& b, float r) {
                Render::SdfPrimitive p;
                p.shape = Render::SdfShape::Capsule;
                p.position = (a + b) * 0.5f;
                const glm::vec3 d = b - a;
                const float len = glm::length(d);
                const glm::vec3 dir = len > 1e-5f ? d / len : glm::vec3(0, 1, 0);
                const float c = glm::dot(glm::vec3(0, 1, 0), dir);
                p.rotation = c > 0.9999f ? glm::quat(1, 0, 0, 0) : (c < -0.9999f ? glm::angleAxis(3.1415926f, glm::vec3(1, 0, 0)) : glm::angleAxis(std::acos(c), glm::normalize(glm::cross(glm::vec3(0, 1, 0), dir))));
                p.params = glm::vec4(len * 0.5f, r, 0.0f, 0.0f);
                p.smoothness = 0.06f;
                sdf.primitives.push_back(p);
            };
            capsule({ 0.0f, 0.02f, 0.0f }, { 0.0f, 0.42f, 0.0f }, 0.27f);
            capsule(pose.Position(1), pose.Position(2), 0.15f);
            capsule(pose.Position(2), pose.Position(4), 0.12f);
            capsule(pose.Position(4), pose.TcpPosition(), 0.07f);
            sdf.boundsMin = glm::vec3(-0.6f, 0.0f, -1.6f);
            sdf.boundsMax = glm::vec3(1.8f, 2.2f, 1.6f);
            sdf.color = glm::vec3(0.95f, 0.45f, 0.1f);
            sdf.showSlice = true;
            sdf.slicePlane = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        }

        const char* Controls() const override { return "Joint / Cartesian jog sliders switch to manual mode; RUN PROGRAM resumes the cycle."; }

        void DrawMenu(GameContext&) override {
            if (ImGui::MenuItem("Run program", nullptr, mode == Mode::Program)) RunProgram();
            if (ImGui::MenuItem("Home position")) { Jog(); cartActive = false; cmd = HOME; }
            if (ImGui::MenuItem("Joint frames + labels", nullptr, showFrames)) showFrames = !showFrames;
            if (ImGui::MenuItem("TCP trajectory trail", nullptr, showTrail)) showTrail = !showTrail;
        }

        void DrawPanel(GameContext&) override {
            const Arm::Pose pose = Arm::Forward(q);
            Retro::BeginGroup("Program");
            if (Retro::Button("RUN PROGRAM", 132.0f, mode == Mode::Program ? 1 : 0)) RunProgram();
            ImGui::SameLine();
            if (Retro::Button("HOME", 70.0f)) { Jog(); cartActive = false; cmd = HOME; }
            ImGui::SameLine();
            if (Retro::Button(grip > (GRIP_OPEN + GRIP_CLOSED) * 0.5f ? "CLOSE" : "OPEN", 0.0f, holding ? 2 : 0)) {
                Jog();
                const bool close = grip > (GRIP_OPEN + GRIP_CLOSED) * 0.5f;
                grip = close ? GRIP_CLOSED : GRIP_OPEN;
                if (close) Grab(); else if (holding) Release();
            }
            Retro::LcdField("STATUS", Status(), mode != Mode::Program);
            Retro::LcdField("CYCLES / PARTS", Fmt("%.0f / %.0f", cycles, deliveries).c_str());
            Retro::EndGroup();

            Retro::BeginGroup("Joint Jog  (deg)");
            for (int i = 0; i < DOF; ++i) {
                float deg = cmd[size_t(i)] / DEG;
                char label[32];
                std::snprintf(label, sizeof(label), "J%d %s", i + 1, Arm::JOINTS[i].name + 3);
                if (Retro::EqSlider(label, &deg, Arm::JOINTS[i].minDeg, Arm::JOINTS[i].maxDeg, "%.1f")) {
                    Jog();
                    cartActive = false;
                    cmd[size_t(i)] = deg * DEG;
                }
            }
            Retro::EndGroup();

            Retro::BeginGroup("Cartesian Jog  (TCP, mm)");
            glm::vec3 mm = (cartActive ? cartTarget : pose.TcpPosition()) * 1000.0f;
            const char* axes[3] = { "X", "Y", "Z" };
            const float lo[3] = { -1300.0f, 0.0f, -1300.0f }, hi[3] = { 1500.0f, 2000.0f, 1300.0f };
            for (int a = 0; a < 3; ++a) {
                if (Retro::EqSlider(axes[a], &mm[a], lo[a], hi[a], "%.0f")) {
                    Jog();
                    if (!cartActive) cartTarget = pose.TcpPosition();
                    cartActive = true;
                    cartTarget[a] = mm[a] / 1000.0f;
                }
            }
            const glm::vec3 p = pose.TcpPosition() * 1000.0f;
            const glm::vec3 e = Arm::EulerZyxDegrees(pose.TcpRotation());
            const glm::quat r = pose.TcpRotation();
            Retro::LcdField("XYZ", Fmt("%.1f %.1f %.1f", p.x, p.y, p.z).c_str());
            Retro::LcdField("EULER ZYX", Fmt("%.1f %.1f %.1f", e.x, e.y, e.z).c_str(), true);
            Retro::LcdField("QUAT WXYZ", Fmt("%.3f %.3f %.3f %.3f", r.w, r.x, r.y, r.z).c_str(), true);
            Retro::LcdField("IK RESIDUAL", Fmt("%.2f mm", lastIkError * 1000.0f).c_str());
            Retro::EndGroup();

            Retro::BeginGroup("Timeline Replay");
            const int last = int(history.size()) - 1;
            auto go = [&](int i) { EnterReplay(); replayPlaying = false; scrub = std::clamp(i, 0, std::max(last, 0)); if (last >= 0) ApplySample(history[size_t(scrub)]); };
            if (Retro::TransportButton("##first", Retro::Glyph::StepBack, false, "|<  first recorded step")) go(0);
            ImGui::SameLine();
            if (Retro::TransportButton("##back", Retro::Glyph::Eject, false, "step back 0.25 s")) go((mode == Mode::Replay ? scrub : last) - 15);
            ImGui::SameLine();
            if (Retro::TransportButton("##play", Retro::Glyph::Play, mode == Mode::Replay && replayPlaying, "|>  play the recording")) { EnterReplay(); replayPlaying = true; }
            ImGui::SameLine();
            if (Retro::TransportButton("##pause", Retro::Glyph::Pause, mode == Mode::Replay && !replayPlaying, "||  pause")) { EnterReplay(); replayPlaying = false; }
            ImGui::SameLine();
            if (Retro::Button("LIVE", 0.0f, mode != Mode::Replay ? 1 : 0)) ExitReplay();
            int index = mode == Mode::Replay ? scrub : last;
            if (last >= 0) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##scrub", &index, 0, last, "step %d")) go(index);
            }
            Retro::LcdField("RECORDED", Fmt("%.1f s of %.0f s", double(history.size()) / 60.0, double(HISTORY) / 60.0).c_str());
            Retro::EndGroup();

            Retro::BeginGroup("Display");
            Retro::LedToggle("FRAMES", &showFrames);
            ImGui::SameLine();
            Retro::LedToggle("TRAIL", &showTrail);
            ImGui::SameLine();
            bool ink = CadInkEnabled();
            if (Retro::LedToggle("CAD INK", &ink)) SetCadInk(ink);
            Retro::EndGroup();
        }

        // ---------------------------------------------------------------- autotest
        void AddTests(ShowcaseTest& test) override {
            test.AddStep("hero meshes and kinematic chain", [this](GameContext&, ShowcaseTest& t) {
                const RenderStats r = GetRenderStats();
                bool meshes = true;
                for (const char* m : { "arm_base", "arm_turret", "arm_upper", "arm_forearm", "arm_wrist_roll", "arm_tool", "arm_finger" }) meshes &= Render::HasMesh(HeroMeshName(m));
                t.Expect("assetgen arm meshes loaded (base, turret, housings, wrist, tool, fingers)", meshes, F("%.0f parts + 2 fingers, %.0f triangles drawn", double(parts.size()), r.trianglesDrawn));
                const Arm::Pose zero = Arm::Forward(Angles{});
                t.Expect("dual-quaternion FK: zero pose TCP at (1.240, 1.470, 0)", glm::length(zero.TcpPosition() - Arm::TCP_ZERO) < 1e-4f,
                         F("TCP (%.4f, %.4f, %.4f)", zero.TcpPosition().x, zero.TcpPosition().y, zero.TcpPosition().z));
                // FK against a hand-composed chain: J1 = 90 deg turns the reach to -Z
                Angles a{}; a[0] = 90.0f * DEG;
                const glm::vec3 p = Arm::Forward(a).TcpPosition();
                t.Expect("J1 +90 deg swings the TCP to (0, 1.47, -1.24)", glm::length(p - glm::vec3(0.0f, 1.47f, -1.24f)) < 1e-3f, F("TCP (%.4f, %.4f, %.4f)", p.x, p.y, p.z));
                // IK round trip on random reachable poses
                std::mt19937 rng(7);
                std::uniform_real_distribution<float> u(-1.0f, 1.0f);
                int ok = 0;
                float worst = 0.0f, worstAngle = 0.0f;
                for (int k = 0; k < 40; ++k) {
                    Angles g{};
                    for (int i = 0; i < DOF; ++i) g[size_t(i)] = HOME[size_t(i)] + u(rng) * 0.6f;
                    g = Arm::ClampToLimits(g);
                    const Arm::Pose target = Arm::Forward(g);
                    const Arm::IkResult ik = Arm::SolveIk(HOME, target.TcpPosition(), target.TcpRotation(), true, 120);
                    worst = std::max(worst, ik.positionError);
                    worstAngle = std::max(worstAngle, ik.angleError);
                    ok += ik.positionError < 1e-3f && ik.angleError < 2e-3f ? 1 : 0;
                }
                t.Expect("DLS IK reaches 40 random poses (position < 1 mm, orientation < 0.11 deg)", ok >= 38,
                         F("%.0f / 40 converged, worst %.3f mm, %.4f deg", ok, worst * 1000.0f, worstAngle / DEG));
                return true;
            });
            test.AddStep("pick-and-place program (PTP / LIN, gripper, physics part)", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Phase() == 0) { t.Value("start") = deliveries; t.Value("t0") = t.StepSeconds(c); t.Phase() = 1; maxLinIkError = 0.0f; return false; }
                const bool delivered = deliveries > int(t.Value("start"));
                if (delivered && t.Value("doneAt") == 0.0) t.Value("doneAt") = t.StepSeconds(c);
                if (!delivered || t.StepSeconds(c) - t.Value("doneAt") < 1.0) return t.StepSeconds(c) > 40.0;
                // the part rests on one of the stands (the one it was carried to)
                const glm::vec3 b = box.WorldPosition();
                float bestDist = 1e9f;
                int stand = -1;
                for (int k = 0; k < 2; ++k) {
                    const float d = glm::length(glm::vec2(b.x - STAND[k].x, b.z - STAND[k].z));
                    if (d < bestDist) { bestDist = d; stand = k; }
                }
                const glm::vec3 up = box.WorldRotation() * glm::vec3(0, 1, 0);
                t.Expect("part carried to the other stand and released", stand == 1 && bestDist < 0.03f && std::abs(b.y - (STAND_HEIGHT + BOX * 0.5f)) < 0.01f,
                         F("%.0f deliveries, part %.1f mm from stand B centre, height %.4f m (rest %.4f)", deliveries, bestDist * 1000.0f, b.y, STAND_HEIGHT + BOX * 0.5f));
                t.Expect("part upright after the drop", std::abs(up.y) > 0.995f, F("up . Y = %.4f", up.y));
                t.Expect("LIN moves track a straight TCP line (IK residual < 1 mm)", maxLinIkError < 1e-3f, F("worst residual %.3f mm", maxLinIkError * 1000.0f));
                t.Expect("cycle time", true, F("%.1f s from the step start to the release", t.Value("doneAt") - t.Value("t0")));
                t.Screenshot("arm_delivered");
                return true;
            }, 60.0);
            test.AddStep("joint and Cartesian jog", [this](GameContext&, ShowcaseTest& t) {
                switch (t.Phase()) {
                    case 0: Jog(); cartActive = false; cmd = HOME; cmd[0] = 35.0f * DEG; t.Phase() = 1; return false;
                    case 1: {
                        if (t.Frame() < 60) return false;
                        t.Expect("joint jog: J1 servo reaches 35 deg", std::abs(q[0] - 35.0f * DEG) < 1e-4f, F("J1 %.3f deg", q[0] / DEG));
                        t.Value("x") = Arm::Forward(q).TcpPosition().x;
                        t.Value("y") = Arm::Forward(q).TcpPosition().y;
                        t.Value("z") = Arm::Forward(q).TcpPosition().z;
                        cartTarget = Arm::Forward(q).TcpPosition() + glm::vec3(0.0f, -0.10f, 0.0f);
                        cartActive = true;
                        t.Value("r") = 0.0;
                        t.Phase() = 2;
                        return false;
                    }
                    default: {
                        if (t.Frame() < 120) return false;
                        const glm::vec3 p = Arm::Forward(q).TcpPosition();
                        const glm::vec3 d = p - glm::vec3(float(t.Value("x")), float(t.Value("y")), float(t.Value("z")));
                        t.Expect("Cartesian jog: TCP moves -100 mm in Y (+-1 mm, others < 1 mm)", std::abs(d.y + 0.1f) < 1e-3f && std::abs(d.x) < 1e-3f && std::abs(d.z) < 1e-3f,
                                 F("dX %.2f dY %.2f dZ %.2f mm", d.x * 1000.0f, d.y * 1000.0f, d.z * 1000.0f));
                        t.Screenshot("arm_jog");
                        RunProgram();
                        return true;
                    }
                }
            }, 30.0);
            test.AddStep("joint frames, labels and TCP trail", [this](GameContext& c, ShowcaseTest& t) {
                if (t.Frame() == 0) { StopShot(c); c.camera.SetOrbit(glm::vec3(0.6f, 1.0f, 0.0f), 2.6f, 0.75f, -0.2f); }
                if (t.Frame() == 8) t.RequestCapture();
                if (t.Frame() < 9 || !t.CaptureReady()) return false;
                const Render::FrameCapture& cap = t.Capture();
                size_t red = 0, green = 0, blue = 0;
                for (size_t i = 0; i + 3 < cap.rgba.size(); i += 4) {
                    const int r = cap.rgba[i], g = cap.rgba[i + 1], b = cap.rgba[i + 2];
                    red += r > g + 90 && r > b + 90;
                    green += g > r + 60 && g > b + 60;
                    blue += b > r + 70 && b > g + 40;
                }
                int visible = 0;
                for (auto& f : arrows) for (Entity e : f) visible += e.Get<MeshRenderer>().visible ? 1 : 0;
                const RenderStats r = GetRenderStats();
                t.Expect("24 frame arrows (base, J1-J6, tool) drawn in X red / Y green / Z blue", visible == 24 && red > 40 && green > 40 && blue > 40,
                         F("%.0f arrows, red %.0f green %.0f blue %.0f px", visible, double(red), double(green)) + F(" / %.0f", double(blue)));
                t.Expect("glowing TCP trajectory ribbon", r.ribbonsDrawn >= 1 && trail.size() > 50, F("%.0f ribbons, %.0f trail points", r.ribbonsDrawn, double(trail.size())));
                return true;
            });
            test.AddStep("timeline replay scrubbing", [this](GameContext&, ShowcaseTest& t) {
                switch (t.Phase()) {
                    case 0: {
                        if (history.size() < 400) return false;
                        EnterReplay();
                        replayPlaying = false;
                        scrub = int(history.size()) - 300;
                        ApplySample(history[size_t(scrub)]);
                        t.Value("scrub") = scrub;
                        t.Phase() = 1;
                        return false;
                    }
                    case 1: {
                        if (t.Frame() < 10) return false;
                        const Sample& s = history[size_t(t.Value("scrub"))];
                        float err = 0.0f;
                        for (int i = 0; i < DOF; ++i) err = std::max(err, std::abs(q[size_t(i)] - s.q[size_t(i)]));
                        t.Expect("scrubbed pose equals the recorded joint state (paused replay; the physics part is re-pinned every tick)", mode == Mode::Replay && err < 1e-6f && glm::length(box.WorldPosition() - s.boxPosition) < 3e-3f,
                                 F("max joint error %.2e rad, part %.2e m, %.0f recorded steps", err, glm::length(box.WorldPosition() - s.boxPosition), double(history.size())));
                        replayPlaying = true;
                        t.Phase() = 2;
                        return false;
                    }
                    default:
                        if (mode == Mode::Replay) return false;
                        t.Expect("play runs the recording to the end and returns to live", mode == Mode::Program, std::string("status ") + Status());
                        return true;
                }
            }, 30.0);
        }
    };

    void RegisterRoboticArm(SceneManager& scenes) { scenes.Register("RoboticArm", [] { return std::make_unique<RobotArmScene>(); }); }
}
