#include "DroneAssembly.h"
#include "OldMcDonaldMaterials.h"
#include "OldMcDonaldSpec.h"

#include "Phisiks/Scene/Components.h"

#include <cmath>
#include <string>

using namespace Phisiks;
namespace S = OldMcDonald::Spec;
namespace Kit = Lab::ProjectKit;

namespace Lab::DroneAssembly {
    namespace {
        constexpr float PI = Kit::PI;
        const std::string MESH = "meshes/oldmcdonald/";
        Entity Part(Scene& s, const std::string& name, Entity parent, const std::string& file, const std::string& material,
                    const glm::vec3& position = glm::vec3(0.0f), bool castShadows = true) {
            return Kit::Part(s, name, parent, MESH + file, material, position, castShadows);
        }
        glm::quat Yaw(float a) { return glm::angleAxis(a, glm::vec3(0, 1, 0)); }   // +X -> (cos a, 0, -sin a)
    }

    void RegisterMaterials() {
        OldMcDonald::RegisterHardwareMaterials();   // metals, plastics, wiring (shared with the rover)
        auto M = [](const char* n, glm::vec3 c, float r, float m = 0.0f) { Kit::Material(n, c, r, m); };
        M("OMD/Carbon", { 0.035f, 0.036f, 0.040f }, 0.35f, 0.3f);         // top plate, GPS mast (black, photo)
        M("OMD/FrameBlack", { 0.030f, 0.031f, 0.034f }, 0.60f);           // lattice arms (matte black, photo)
        M("OMD/MotorRed", { 0.70f, 0.06f, 0.05f }, 0.35f, 0.6f);          // motor bell band (red, photo)
        M("OMD/Copper", { 0.93f, 0.56f, 0.40f }, 0.30f, 1.0f);
        M("OMD/Prop", { 0.05f, 0.05f, 0.055f }, 0.40f);                   // dark grey props (photo)
        M("OMD/ESC", { 0.85f, 0.65f, 0.05f }, 0.45f);                     // yellow blocks under the arms (photo)
        M("OMD/FCCase", { 0.05f, 0.05f, 0.055f }, 0.50f);                 // black Pixhawk case (photo)
        M("OMD/GPS", { 0.06f, 0.06f, 0.065f }, 0.40f);
        M("OMD/Strap", { 0.02f, 0.02f, 0.025f }, 0.90f);
        M("OMD/LipoBlue", { 0.10f, 0.25f, 0.65f }, 0.50f);                // blue-wrapped pack (photo)
        M("OMD/CableTie", { 0.86f, 0.86f, 0.83f }, 0.60f);                // white nylon ties (photo)
    }

    Result Build(Scene& s, Entity parent, const std::string& name, const glm::vec3& position, const glm::quat& rotation, int group) {
        Result out;
        auto& ex = out.explode;
        auto callout = [&](const std::string& entity, glm::vec3 anchor, const std::string& tag, const std::string& title, const std::string& note) {
            out.callouts.push_back({ entity, anchor, tag, title, note, group });
        };
        auto wire = [](float od, const char* material) { return Kit::RunStyle{ od, material, "", 0.0f }; };

        Entity drone = Kit::Group(s, name, parent, position);
        drone.Get<Transform>().rotation = rotation;
        out.root = drone;

        // ---- frame: slotted top plate, power distribution bottom plate, four lattice arms (X)
        Entity frame = Kit::Group(s, FRAME, drone);
        Part(s, "Top plate", frame, "drone_top_plate.obj", "OMD/Carbon");
        Part(s, "Frame screws", frame, "drone_frame_screws.obj", "OMD/Stainless");
        Part(s, "Power distribution board", frame, "drone_pdb.obj", "OMD/PCBDark");
        Part(s, "PDB pads", frame, "drone_pdb_pads.obj", "OMD/Copper");
        ex.push_back({ "Top plate", { 0.0f, 0.06f, 0.0f } });
        ex.push_back({ "Frame screws", { 0.0f, 0.08f, 0.0f } });
        ex.push_back({ "Power distribution board", { 0.0f, -0.05f, 0.0f } });
        ex.push_back({ "PDB pads", { 0.0f, -0.05f, 0.0f } });

        Entity propulsion = Kit::Group(s, PROPULSION, drone);
        Entity props = Kit::Group(s, PROPELLERS, drone);
        Entity elec = Kit::Group(s, ELECTRONICS, drone);
        Entity dw = Kit::Group(s, WIRING, drone);
        out.wiring.push_back(WIRING);

        for (int i = 0; i < 4; ++i) {
            const std::string n = std::to_string(i + 1);
            const float a = PI * 0.25f + PI * 0.5f * float(i);   // 1 front-left, 2 rear-left, 3 rear-right, 4 front-right (X configuration)
            const glm::quat r = Yaw(a);
            const glm::vec3 dir(std::cos(a), 0.0f, -std::sin(a));
            auto at = [&](const glm::vec3& armLocal) { return r * armLocal; };   // arm frame -> drone frame

            Entity arm = Part(s, "Arm beam " + n, frame, "drone_arm.obj", "OMD/FrameBlack");
            arm.Get<Transform>().rotation = r;
            Entity mount = Part(s, "Motor mount " + n, frame, "drone_motor_mount.obj", "OMD/Anodised", at({ S::DR_MOTOR_R, S::DR_ARM_Y0 + S::DR_ARM_H, 0.0f }));
            mount.Get<Transform>().rotation = r;
            Entity tie = Part(s, "Cable tie " + n, frame, "drone_cable_tie.obj", "OMD/CableTie");
            tie.Get<Transform>().rotation = r;
            ex.push_back({ "Arm beam " + n, dir * 0.11f });
            ex.push_back({ "Cable tie " + n, dir * 0.11f });
            ex.push_back({ "Motor mount " + n, dir * 0.11f + glm::vec3(0.0f, 0.025f, 0.0f) });

            Entity motor = Kit::Group(s, "Motor " + n, propulsion, at({ S::DR_MOTOR_R, S::DR_MOTOR_Y, 0.0f }));
            motor.Get<Transform>().rotation = r;
            Part(s, "Motor body " + n, motor, "drone_motor.obj", "OMD/MotorBlack");
            Part(s, "Motor band " + n, motor, "drone_motor_band.obj", "OMD/MotorRed");
            ex.push_back({ "Motor " + n, dir * 0.11f + glm::vec3(0.0f, 0.05f, 0.0f) });

            Entity prop = Part(s, "Propeller " + n, props, "drone_prop.obj", "OMD/Prop", at({ S::DR_MOTOR_R, S::DR_PROP_Y, 0.0f }));
            prop.Get<Transform>().rotation = r;
            if (i % 2) prop.Get<Transform>().scale = glm::vec3(1.0f, 1.0f, -1.0f);   // counter-rotating pitch
            Part(s, "Prop nut " + n, prop, "drone_prop_nut.obj", "OMD/Stainless");
            ex.push_back({ "Propeller " + n, dir * 0.11f + glm::vec3(0.0f, 0.11f, 0.0f) });

            Entity esc = Part(s, "ESC " + n, elec, "drone_esc.obj", "OMD/ESC", at({ S::DR_ESC_R, S::DR_ARM_Y0 - 0.0045f, 0.0f }));
            esc.Get<Transform>().rotation = r;
            ex.push_back({ "ESC " + n, dir * 0.11f + glm::vec3(0.0f, -0.05f, 0.0f) });

            // ESC leads (arm frame): power to the PDB pads, three phases up to the motor
            Entity leads = Kit::Group(s, "ESC leads " + n, dw);
            leads.Get<Transform>().rotation = r;
            out.wiring.push_back("ESC leads " + n);
            const float yE = S::DR_ARM_Y0 - 0.0045f, yP = S::DR_GEAR + S::DR_PLATE_T + 0.0008f;
            for (int k = 0; k < 2; ++k) {
                const float z = k ? 0.007f : -0.007f;
                Kit::BuildRun(s, leads, std::string(k ? "ESC power - " : "ESC power + ") + n,
                              { { S::DR_ESC_R - 0.022f, yE, z }, { 0.068f, yE, z }, { 0.068f, yP, z }, { 0.045f, yP, z } },
                              wire(0.0016f, k ? "OMD/Wire/Black" : "OMD/Wire/Red"));
            }
            for (int k = 0; k < 3; ++k) {
                const float z = -0.004f + 0.004f * float(k);
                Kit::BuildRun(s, leads, "Motor phase " + n + "-" + std::to_string(k + 1),
                              { { S::DR_ESC_R + 0.022f, yE, z }, { 0.198f, yE, z }, { 0.198f, S::DR_MOTOR_Y + 0.004f, z }, { 0.2105f, S::DR_MOTOR_Y + 0.004f, z } },
                              wire(0.0012f, "OMD/Wire/Black"));
            }
        }

        // ---- flight controller (documented: Pixhawk 2.4.8) on its damping mount
        Entity fc = Kit::Group(s, FC, elec);
        Part(s, "Damping mount", fc, "drone_fc_mount.obj", "OMD/DarkPlastic");
        Part(s, "Pixhawk 2.4.8 case", fc, "drone_fc.obj", "OMD/FCCase");
        Part(s, "FC connectors", fc, "drone_fc_ports.obj", "OMD/DarkPlastic");
        ex.push_back({ FC, { 0.0f, 0.13f, 0.0f } });

        // ---- GPS (documented: ReadyToSky) on its mast
        Entity gps = Kit::Group(s, GPS, drone, S::DR_GPS_MAST);
        Part(s, "GPS mast", gps, "drone_gps_mast.obj", "OMD/Carbon");
        Part(s, "GPS puck", gps, "drone_gps.obj", "OMD/GPS", { 0.0f, S::DR_GPS_MAST_H, 0.0f });
        ex.push_back({ GPS, { -0.06f, 0.22f, 0.0f } });

        // ---- battery under the frame, landing gear
        Entity bat = Kit::Group(s, BATTERY, drone);
        Part(s, "LiPo pack", bat, "drone_battery.obj", "OMD/LipoBlue");
        Part(s, "Battery straps", bat, "drone_strap.obj", "OMD/Strap");
        ex.push_back({ BATTERY, { 0.0f, -0.20f, 0.0f } });
        Part(s, GEAR, drone, "drone_gear.obj", "OMD/DarkPlastic");
        ex.push_back({ GEAR, { 0.0f, -0.13f, 0.0f } });

        // ---- drone wiring (frame frame): GPS cable down the mast to the FC, battery lead to the PDB
        const float fcPortY = S::DR_FC.y + S::DR_FC_HALF.y, plateTop = S::DR_TOP + S::DR_PLATE_T;
        Kit::BuildRun(s, dw, "GPS cable", { { S::DR_GPS_MAST.x + 0.006f, plateTop + S::DR_GPS_MAST_H - 0.006f, 0.0f }, { S::DR_GPS_MAST.x + 0.006f, fcPortY, 0.0f },
                                           { -S::DR_FC_HALF.x - 0.003f, fcPortY, 0.0f } }, wire(0.0015f, "OMD/Cable"));
        for (int k = 0; k < 2; ++k) {
            const float z = k ? 0.004f : -0.004f, yb = S::DR_BATTERY_Y, yp = S::DR_GEAR + S::DR_PLATE_T + 0.0016f;
            Kit::BuildRun(s, dw, k ? "Battery lead -" : "Battery lead +",
                          { { S::DR_BATTERY_HALF.x, yb, z }, { 0.088f, yb, z }, { 0.088f, yp, z }, { 0.058f, yp, z } }, wire(0.003f, k ? "OMD/Wire/Black" : "OMD/Wire/Red"));
        }

        callout("Pixhawk 2.4.8 case", S::DR_FC + glm::vec3(0.0f, 2.0f * S::DR_FC_HALF.y, 0.0f), "FC", "Pixhawk 2.4.8", "flight controller");
        callout("GPS puck", { 0.0f, S::DR_GPS_H, 0.0f }, "GPS", "ReadyToSky GPS", "M8N-class module on mast");
        callout("Motor 1", { 0.0f, S::DR_MOTOR_H, 0.0f }, "M1", "Brushless motor", "red bell, X mount");
        callout("Propeller 4", { 0.1f, 0.0f, 0.0f }, "", "10 in propeller", "");
        callout("Arm beam 3", { 0.15f, S::DR_ARM_Y0 + S::DR_ARM_H, 0.0f }, "", "Lattice arm", "F450-class length");
        callout("ESC 2", { 0.0f, 0.0f, 0.0f }, "ESC", "Speed controller", "1 per motor");
        callout("Power distribution board", { 0.06f, S::DR_GEAR, 0.0f }, "PDB", "Power distribution", "frame bottom plate");
        callout("LiPo pack", { 0.0f, S::DR_BATTERY_Y - S::DR_BATTERY_HALF.y, 0.0f }, "BAT", "LiPo battery", "generic pack");
        callout(GEAR, { 0.10f, 0.006f, 0.095f }, "", "Landing gear", "generic (not in the photo)");
        return out;
    }
}
