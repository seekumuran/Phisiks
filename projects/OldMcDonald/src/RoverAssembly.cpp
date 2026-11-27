#include "RoverAssembly.h"
#include "OldMcDonaldMaterials.h"
#include "OldMcDonaldSpec.h"

#include "Phisiks/Scene/Components.h"

#include <cmath>
#include <string>

using namespace Phisiks;
namespace S = OldMcDonald::Spec;
namespace Kit = Projects::ProjectKit;

namespace Projects::RoverAssembly {
    namespace {
        constexpr float PI = Kit::PI;
        const std::string MESH = "meshes/oldmcdonald/";
        Entity Part(Scene& s, const std::string& name, Entity parent, const std::string& file, const std::string& material,
                    const glm::vec3& position = glm::vec3(0.0f), bool castShadows = true) {
            return Kit::Part(s, name, parent, MESH + file, material, position, castShadows);
        }
        glm::quat Yaw(float a) { return glm::angleAxis(a, glm::vec3(0, 1, 0)); }
        const glm::vec3 AXES[4] = { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
    }

    void RegisterMaterials() {
        OldMcDonald::RegisterHardwareMaterials();   // metals, plastics, wiring (shared with the drone)
        auto M = [](const char* n, glm::vec3 c, float r, float m = 0.0f, glm::vec3 e = glm::vec3(0.0f), float o = 1.0f) { Kit::Material(n, c, r, m, e, o); };
        M("OMD/Rubber", { 0.025f, 0.025f, 0.027f }, 0.92f);
        M("OMD/RoverYellow", { 0.85f, 0.62f, 0.05f }, 0.45f);
        M("OMD/RimBlue", { 0.30f, 0.45f, 0.62f }, 0.32f, 0.85f);        // blue metallic rims (photo)
        M("OMD/ServoBlue", { 0.03f, 0.36f, 0.72f }, 0.40f);             // blue servo cases (photo)
        M("OMD/HornOrange", { 0.85f, 0.38f, 0.06f }, 0.35f, 0.7f);      // orange anodised horns (photo)
        M("OMD/Battery", { 0.05f, 0.05f, 0.055f }, 0.50f);
        M("OMD/PicoGreen", { 0.02f, 0.26f, 0.09f }, 0.38f);
        M("OMD/Perfboard", { 0.42f, 0.30f, 0.14f }, 0.55f);
        M("OMD/ICBlack", { 0.018f, 0.018f, 0.020f }, 0.35f);
        M("OMD/Tin", { 0.80f, 0.80f, 0.82f }, 0.30f, 1.0f);
        M("OMD/DriverRed", { 0.42f, 0.04f, 0.04f }, 0.40f);
        M("OMD/TerminalBlue", { 0.06f, 0.20f, 0.55f }, 0.50f);
        M("OMD/LidarBody", { 0.03f, 0.032f, 0.036f }, 0.35f, 0.2f);
        M("OMD/LidarWindow", { 0.02f, 0.02f, 0.03f }, 0.05f, 0.0f, { 0.04f, 0.0f, 0.0f });
        M("OMD/PHGlass", { 0.85f, 0.92f, 0.95f }, 0.05f, 0.0f, glm::vec3(0.0f), 0.45f);
    }

    Result Build(Scene& s, Entity parent, const std::string& name, const glm::vec3& position, const glm::quat& rotation,
                 const glm::quat& bodyRotation) {
        Result out;
        auto& ex = out.explode;
        auto wire = [](float od, const char* material) { return Kit::RunStyle{ od, material, "", 0.0f }; };

        Entity rover = Kit::Group(s, name, parent, position);
        rover.Get<Transform>().rotation = rotation;
        Entity body = Kit::Group(s, BODY, rover);
        body.Get<Transform>().rotation = bodyRotation;
        out.root = rover;
        out.body = body;

        // ---- chassis: black box, standoffs, black top plate, raised arm platform (photo)
        Entity chassis = Kit::Group(s, "Chassis", body);
        Part(s, "Chassis box", chassis, "rover_chassis.obj", "OMD/Anodised");
        Part(s, "Bumpers", chassis, "rover_bumpers.obj", "OMD/Anodised");
        Part(s, "Deck standoffs", chassis, "rover_standoffs.obj", "OMD/Aluminium");
        Part(s, "Equipment deck", chassis, "rover_deck.obj", "OMD/Anodised");
        Part(s, "Deck fasteners", chassis, "rover_fasteners.obj", "OMD/Stainless");
        Part(s, "Arm mounting platform", chassis, "rover_arm_platform.obj", "OMD/Anodised");
        ex.push_back({ "Equipment deck", { 0.0f, 0.10f, 0.0f } });
        ex.push_back({ "Deck fasteners", { 0.0f, 0.13f, 0.0f } });
        ex.push_back({ "Arm mounting platform", { 0.0f, 0.17f, 0.0f } });

        // ---- bodywork: yellow fenders and front panel (photo)
        Entity bodywork = Kit::Group(s, "Bodywork", body);
        Entity fenderL = Part(s, "Fender L", bodywork, "rover_fender.obj", "OMD/RoverYellow");
        fenderL.Get<Transform>().scale = glm::vec3(1.0f, 1.0f, -1.0f);    // mirrored (the part is built for the right side, +Z)
        Part(s, "Fender R", bodywork, "rover_fender.obj", "OMD/RoverYellow");
        Part(s, "Fender brackets", bodywork, "rover_fender_brackets.obj", "OMD/Anodised");
        Part(s, "Front panel", bodywork, "rover_front_panel.obj", "OMD/RoverYellow");
        ex.push_back({ "Fender L", { 0.0f, 0.06f, -0.20f } });
        ex.push_back({ "Fender R", { 0.0f, 0.06f, 0.20f } });
        ex.push_back({ "Front panel", { 0.16f, 0.0f, 0.0f } });

        // ---- drive: four gearmotor-driven wheels (block-tread tyre, Y-spoke rim, hub)
        Entity drive = Kit::Group(s, "Drive system", body);
        Part(s, "Gearmotors", drive, "rover_motors.obj", "OMD/MotorBlack");
        const glm::vec3 wheelPos[4] = { { S::RV_WHEEL_X, S::RV_WHEEL_R, -S::RV_WHEEL_Z }, { S::RV_WHEEL_X, S::RV_WHEEL_R, S::RV_WHEEL_Z },
                                       { -S::RV_WHEEL_X, S::RV_WHEEL_R, -S::RV_WHEEL_Z }, { -S::RV_WHEEL_X, S::RV_WHEEL_R, S::RV_WHEEL_Z } };
        for (int w = 0; w < 4; ++w) {
            Entity wheel = Kit::Group(s, WHEELS[w], drive, wheelPos[w]);
            if (wheelPos[w].z > 0.0f) wheel.Get<Transform>().rotation = Yaw(PI);   // rim face outwards
            Part(s, "Tyre", wheel, "rover_tyre.obj", "OMD/Rubber");
            Part(s, "Rim", wheel, "rover_rim.obj", "OMD/RimBlue");
            Part(s, "Hub", wheel, "rover_hub.obj", "OMD/Anodised");
            ex.push_back({ WHEELS[w], { 0.0f, 0.0f, wheelPos[w].z > 0.0f ? 0.20f : -0.20f } });
        }

        // ---- electronics
        Entity elec = Kit::Group(s, "Electronics", body);
        Entity pico = Kit::Group(s, "Raspberry Pi Pico", elec, S::RV_PICO);
        Part(s, "Carrier board", pico, "pico_carrier.obj", "OMD/Perfboard");
        Part(s, "Pico board", pico, "pico_pcb.obj", "OMD/PicoGreen");
        Part(s, "RP2040 and parts", pico, "pico_parts.obj", "OMD/ICBlack");
        Part(s, "USB and crystal", pico, "pico_metal.obj", "OMD/Tin");
        Entity drv = Kit::Group(s, "Motor driver", elec, S::RV_DRIVER);
        Part(s, "Driver board", drv, "driver_board.obj", "OMD/DriverRed");
        Part(s, "Heatsink", drv, "driver_heatsink.obj", "OMD/Anodised");
        Part(s, "Screw terminals", drv, "driver_terminals.obj", "OMD/TerminalBlue");
        Entity header = Kit::Part(s, "Logic header", drv, "Cube", "OMD/ICBlack", { 0.0038f, 0.0076f + 0.00425f, -0.019f });
        header.Get<Transform>().scale = glm::vec3(0.0115f, 0.0085f, 0.0025f);
        Part(s, "Battery pack", elec, "rover_battery.obj", "OMD/Battery");
        ex.push_back({ "Raspberry Pi Pico", { 0.0f, 0.20f, -0.04f } });
        ex.push_back({ "Motor driver", { 0.0f, 0.20f, 0.04f } });
        ex.push_back({ "Battery pack", { 0.0f, 0.04f, 0.0f } });

        // ---- LiDAR
        Entity lidar = Kit::Group(s, LIDAR, body, S::RV_LIDAR);
        Part(s, "LiDAR mast", lidar, "lidar_mast.obj", "OMD/Aluminium");
        Part(s, "LiDAR base", lidar, "lidar_base.obj", "OMD/LidarBody", { 0.0f, S::RV_LIDAR_MAST, 0.0f });
        Entity head = Kit::Group(s, "LiDAR head", lidar, { 0.0f, S::RV_LIDAR_MAST + 0.030f, 0.0f });
        Part(s, "Rotor housing", head, "lidar_head.obj", "OMD/LidarBody");
        Part(s, "Optical window", head, "lidar_window.obj", "OMD/LidarWindow");
        ex.push_back({ LIDAR, { 0.06f, 0.30f, 0.0f } });

        // ---- arm: J1 yaw -> J2 shoulder -> J3 elbow -> J4 wrist -> end effector + sensor head (joint
        // frames, links along +X). Servo-and-bracket construction as in the photo; each servo body sits in
        // the link before the joint it drives, its horn in the driven link. The IK chain is J1-J4; the
        // wrist-rotate and gripper servos are modelled but not articulated.
        Entity arm = Kit::Group(s, ARM, body, S::RV_ARM_BASE);
        const float shaft = S::RV_BASE_SERVO_TOP - 0.002f - S::SERVO_HZ;   // base servo axis origin (shaft up)
        Part(s, "Base servo mount", arm, "arm_base.obj", "OMD/Anodised");
        Entity baseServo = Part(s, "Base servo", arm, "arm_servo.obj", "OMD/ServoBlue", { 0.0f, shaft, 0.0f });
        baseServo.Get<Transform>().rotation = glm::angleAxis(-0.5f * PI, glm::vec3(1, 0, 0));   // shaft +Z -> up
        Entity j1 = Kit::Group(s, JOINTS[0], arm);
        Part(s, "Base turntable", j1, "arm_turntable.obj", "OMD/HornOrange");
        Part(s, "Shoulder cradle", j1, "arm_cradle.obj", "OMD/Anodised");
        Entity shoulderServo = Part(s, "Shoulder actuator", j1, "arm_servo.obj", "OMD/ServoBlue", { 0.0f, S::RV_SHOULDER_H, 0.0f });
        shoulderServo.Get<Transform>().rotation = glm::angleAxis(0.5f * PI, glm::vec3(0, 0, 1));   // body hangs down into the cradle
        Entity j2 = Kit::Group(s, JOINTS[1], j1, { 0.0f, S::RV_SHOULDER_H, 0.0f });
        Part(s, "Shoulder horn", j2, "arm_horn.obj", "OMD/HornOrange");
        Part(s, "Upper link", j2, "arm_upper.obj", "OMD/Anodised");
        Part(s, "Elbow actuator", j2, "arm_servo.obj", "OMD/ServoBlue", { S::RV_L2, 0.0f, 0.0f });
        Entity j3 = Kit::Group(s, JOINTS[2], j2, { S::RV_L2, 0.0f, 0.0f });
        Part(s, "Elbow horn", j3, "arm_horn.obj", "OMD/HornOrange");
        Part(s, "Forearm link", j3, "arm_fore.obj", "OMD/Anodised");
        Part(s, "Forearm tube", j3, "arm_tube.obj", "OMD/Aluminium");
        Part(s, "Wrist actuator", j3, "arm_servo.obj", "OMD/ServoBlue", { S::RV_L3, 0.0f, 0.0f });
        Entity j4 = Kit::Group(s, JOINTS[3], j3, { S::RV_L3, 0.0f, 0.0f });
        Part(s, "Wrist horn", j4, "arm_horn.obj", "OMD/HornOrange");
        Part(s, "Wrist bracket", j4, "arm_wrist.obj", "OMD/Anodised");
        Entity effector = Kit::Group(s, "End effector", j4);
        Entity rotServo = Part(s, "Wrist rotate servo", effector, "arm_servo.obj", "OMD/ServoBlue", { 0.0f, -0.053f, 0.0f });
        rotServo.Get<Transform>().rotation = glm::angleAxis(0.5f * PI, glm::vec3(1, 0, 0));      // shaft down
        Entity rotHorn = Part(s, "Rotate horn", effector, "arm_horn_disc.obj", "OMD/HornOrange", { 0.0f, -0.053f, 0.0f });
        rotHorn.Get<Transform>().rotation = rotServo.Get<Transform>().rotation;
        Part(s, "Gripper", effector, "arm_gripper.obj", "OMD/Anodised");
        Entity gripServo = Part(s, "Gripper servo", effector, "arm_servo.obj", "OMD/ServoBlue", { -0.005f, -0.066f, 0.040f });
        gripServo.Get<Transform>().rotation = Yaw(PI);
        Entity headGroup = Kit::Group(s, SENSORS, j4);
        Part(s, "Head bracket", headGroup, "sensor_head.obj", "OMD/Aluminium");
        const float plate = -S::RV_HEAD_PLATE;
        Entity moist = Kit::Group(s, "Soil moisture sensor", headGroup, { -S::PROBE_SPACING, plate, 0.0f });
        Part(s, "Moisture probe housing", moist, "probe_moisture_body.obj", "OMD/DarkPlastic");
        Part(s, "Moisture probe blade", moist, "probe_moisture_blade.obj", "OMD/PCBDark");
        Entity temp = Kit::Group(s, "Soil temperature sensor", headGroup, { 0.0f, plate, 0.0f });
        Part(s, "Temperature probe", temp, "probe_temperature.obj", "OMD/Stainless");
        Entity ph = Kit::Group(s, "Soil pH sensor", headGroup, { S::PROBE_SPACING, plate, 0.0f });
        Part(s, "pH electrode body", ph, "probe_ph_body.obj", "OMD/DarkPlastic");
        Part(s, "pH glass tip", ph, "probe_ph_tip.obj", "OMD/PHGlass", glm::vec3(0.0f), false);
        ex.push_back({ ARM, { -0.06f, 0.26f, 0.0f } });

        // ---- wiring (rover frame): Pico -> driver logic, LiDAR UART, arm bus, battery feed
        Entity rw = Kit::Group(s, WIRING, body);
        const float pinTop = S::RV_DECK_TOP + 0.0076f + 0.0085f + 0.001f + 0.002f;
        const float hdrTop = S::RV_DECK_TOP + 0.0076f + 0.0085f;
        const char* logic[4] = { "OMD/Wire/Yellow", "OMD/Wire/Green", "OMD/Wire/Blue", "OMD/Wire/White" };
        for (int k = 0; k < 4; ++k) {
            const float x = S::RV_PICO.x + 0.00254f * float(k);
            const float z0 = S::RV_PICO.z + 0.0089f, z1 = S::RV_DRIVER.z - 0.019f;
            Kit::BuildRun(s, rw, "Pico GP to driver IN" + std::to_string(k + 1), { { x, pinTop, z0 }, { x, 0.198f, z0 }, { x, 0.198f, z1 }, { x, hdrTop, z1 } }, wire(0.0015f, logic[k]));
        }
        const float rowOut = S::RV_PICO.z - 0.0089f;
        Kit::BuildRun(s, rw, "LiDAR UART cable", { { S::RV_LIDAR.x - 0.036f, S::RV_LIDAR.y + S::RV_LIDAR_MAST + 0.015f, 0.0f }, { 0.125f, S::RV_LIDAR.y + S::RV_LIDAR_MAST + 0.015f, 0.0f },
                                                 { 0.125f, S::RV_DECK_TOP + 0.007f, 0.0f }, { 0.125f, S::RV_DECK_TOP + 0.007f, rowOut }, { 0.078f, S::RV_DECK_TOP + 0.007f, rowOut },
                                                 { 0.078f, pinTop, rowOut } }, wire(0.0025f, "OMD/Cable"));
        const glm::vec3 ab = S::RV_ARM_BASE;   // servo bus leaves the base servo's end face, drops off the platform
        Kit::BuildRun(s, rw, "Arm actuator bus", { { ab.x + S::SERVO_FRONT + 0.0005f, ab.y + 0.012f, 0.0f }, { ab.x + 0.060f, ab.y + 0.012f, 0.0f },
                                                 { ab.x + 0.060f, S::RV_DECK_TOP + 0.004f, 0.0f }, { 0.004f, S::RV_DECK_TOP + 0.004f, 0.0f },
                                                 { 0.004f, S::RV_DECK_TOP + 0.004f, rowOut }, { 0.0215f, S::RV_DECK_TOP + 0.004f, rowOut }, { 0.0215f, pinTop, rowOut } },
                      wire(0.003f, "OMD/Cable"));
        for (int k = 0; k < 2; ++k) {
            const float z = S::RV_DRIVER.z + (k ? 0.004f : -0.004f), x = S::RV_DRIVER.x + 0.017f;
            Kit::BuildRun(s, rw, k ? "Battery feed -" : "Battery feed +", { { x, S::RV_DECK_TOP + 0.0176f, z }, { x, 0.198f, z }, { 0.105f, 0.198f, z }, { 0.105f, S::RV_DECK_TOP + 0.0015f, z } },
                          wire(0.003f, k ? "OMD/Wire/Black" : "OMD/Wire/Red"));
        }

        return out;
    }

    ArmPose StowPose() {
        const float d = PI / 180.0f;
        return ArmPose{ { 0.0f, 100.0f * d, -135.0f * d, 35.0f * d } };   // folded, probe tips clear of the deck wiring
    }
    void SetArm(Scene& s, const ArmPose& pose) {
        for (int i = 0; i < 4; ++i) if (Entity j = s.FindByName(JOINTS[i])) j.LocalTransform().rotation = glm::angleAxis(pose.q[i], AXES[i]);
    }
    ArmPose GetArm(Scene& s) {
        ArmPose p{};
        for (int i = 0; i < 4; ++i) {
            if (Entity j = s.FindByName(JOINTS[i])) {
                const glm::quat q = j.LocalTransform().rotation;
                const float angle = 2.0f * std::atan2(glm::length(glm::vec3(q.x, q.y, q.z)), q.w);
                const float sign = glm::dot(glm::vec3(q.x, q.y, q.z), AXES[i]) < 0.0f ? -1.0f : 1.0f;
                p.q[i] = std::remainder(angle * sign, 2.0f * PI);
            }
        }
        return p;
    }
}
