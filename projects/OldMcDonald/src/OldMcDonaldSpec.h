#pragma once
// Old-McDonald: dimensional specification shared by the mesh generator (tools/assetgen/
// OldMcDonaldMeshes.cpp -> projects/assets/meshes/oldmcdonald/*.obj) and the PHISIKS scene
// (OldMcDonaldScene.cpp). Units are metres; +X runs along the greenhouse (east), +Y up, +Z south.
// The greenhouse floor slab top is y = 0.
//
// Assemblies are authored in their own local frame, origin at the centre of their ground contact
// (the drone: under the frame centre; the rover: between the four wheel contacts, +X forward).
// Hardware with public outline dimensions uses them (Pixhawk 2.4.8 case 81.5 x 50 x 15.5 mm,
// Raspberry Pi Pico board 51 x 21 mm, F450-class 450 mm frame, 10 inch propellers); everything else
// is a generic model of the stated component class.
#include <glm/vec3.hpp>

namespace OldMcDonald::Spec {
    // ---------------------------------------------------------------- greenhouse (gable roof, ridge along X)
    inline constexpr float GH_HALF_X = 3.0f, GH_HALF_Z = 2.0f;
    inline constexpr float GH_EAVE = 2.2f, GH_RIDGE = 3.0f, GH_BAY = 1.0f;
    inline constexpr float GH_POST = 0.06f;                         // square aluminium extrusion
    inline constexpr float SLAB_MARGIN = 0.2f;                      // slab beyond the walls

    // ---------------------------------------------------------------- growing beds (raised, timber)
    inline constexpr int BEDS = 3;
    inline constexpr float BED_Z[BEDS] = { -1.4f, 0.0f, 1.4f };
    inline constexpr float BED_CX = 0.1f, BED_HALF_X = 2.1f, BED_HALF_Z = 0.35f;
    inline constexpr float BED_HEIGHT = 0.22f, SOIL_TOP = 0.20f, BED_BOARD = 0.04f;
    inline constexpr int PLANTS_PER_ROW = 8;
    inline constexpr float PLANT_X0 = -1.65f, PLANT_DX = 0.5f, PLANT_ROW_DZ = 0.17f;   // two rows per bed

    // ---------------------------------------------------------------- aisles, dock, drone pad
    inline constexpr float AISLE_Z[2] = { -0.7f, 0.7f };           // between the beds (0.7 m wide)
    inline const glm::vec3 DOCK{ -2.5f, 0.0f, -0.7f };              // rover start / docking bay (rover faces +X)
    inline const glm::vec3 DOCK_HALF{ 0.36f, 0.0f, 0.30f };
    inline const glm::vec3 PAD{ -2.55f, 0.0f, 1.45f };              // drone landing pad (outside the rover lanes)
    inline constexpr float PAD_HALF = 0.40f;

    // ---------------------------------------------------------------- fixed soil-sampling grid (rows A-D x columns 1-4)
    inline constexpr int GRID_ROWS = 4, GRID_COLS = 4;
    inline constexpr float SAMPLE_X[GRID_COLS] = { -1.4f, -0.4f, 0.6f, 1.6f };       // between the plants
    inline constexpr float SAMPLE_Z[GRID_ROWS] = { -1.12f, -0.28f, 0.28f, 1.12f };   // 0.07 m inside the aisle-side bed edge
    inline constexpr char ROW_NAME[GRID_ROWS] = { 'A', 'B', 'C', 'D' };
    inline constexpr int ROW_AISLE[GRID_ROWS] = { 0, 0, 1, 1 };    // aisle the rover samples each row from

    // ---------------------------------------------------------------- drone survey (scripted path)
    inline constexpr float SURVEY_HEIGHT = 1.7f;
    inline constexpr float FOOTPRINT_HALF_X = 0.60f, FOOTPRINT_HALF_Z = 0.45f;   // illustrative coverage area

    // ---------------------------------------------------------------- mapping drone (F450-class quad, X configuration)
    inline constexpr float DR_GEAR = 0.100f;                        // bottom plate underside above ground
    inline constexpr float DR_PLATE_T = 0.0016f;
    inline constexpr float DR_ARM_Y0 = DR_GEAR + DR_PLATE_T;        // arms between the plates
    inline constexpr float DR_ARM_H = 0.026f;
    inline constexpr float DR_TOP = DR_ARM_Y0 + DR_ARM_H;           // top plate underside
    inline constexpr float DR_MOTOR_R = 0.225f;                     // 450 mm motor-to-motor diagonal
    inline constexpr float DR_MOTOR_H = 0.027f;
    inline constexpr float DR_MOUNT_T = 0.002f;                     // X motor mount plate on the arm-end pad
    inline constexpr float DR_MOTOR_Y = DR_ARM_Y0 + DR_ARM_H + DR_MOUNT_T;   // motor base
    inline constexpr float DR_PROP_Y = DR_MOTOR_Y + DR_MOTOR_H + 0.006f;
    // Styling follows the prototype photo (docs/reference/old mcdonald/drone-reference.jpeg): black
    // lattice arms, slotted black top plate, red-banded motors, black Pixhawk case, black GPS puck on a
    // mast, white cable ties, blue-wrapped battery. Not visible in the photo: landing gear (kept generic).
    inline constexpr float DR_TOP_HALF_X = 0.095f, DR_TOP_HALF_Z = 0.068f;   // slotted top plate
    inline constexpr float DR_PROP_R = 0.127f;                      // 10 inch
    inline const glm::vec3 DR_FC{ 0.0f, DR_TOP + DR_PLATE_T + 0.008f, 0.0f };   // Pixhawk case base (on damping mount)
    inline const glm::vec3 DR_FC_HALF{ 0.04075f, 0.00775f, 0.025f };
    inline const glm::vec3 DR_GPS_MAST{ -0.065f, DR_TOP + DR_PLATE_T, 0.0f };
    inline constexpr float DR_GPS_MAST_H = 0.10f, DR_GPS_R = 0.029f, DR_GPS_H = 0.016f;
    inline const glm::vec3 DR_BATTERY_HALF{ 0.0675f, 0.0175f, 0.0215f };     // 3S / 4S LiPo class, under the frame
    inline constexpr float DR_BATTERY_Y = 0.068f;                   // centre height
    inline constexpr float DR_ESC_R = 0.13f;                        // ESC along each arm (underside)

    // ---------------------------------------------------------------- soil rover (skid steer, +X forward)
    // Body styling follows the prototype photograph (docs/reference/old mcdonald/rover-reference.jpeg):
    // black box chassis, black top plate with a chamfered nose, yellow sloped front panel, yellow
    // fenders over each wheel pair, large block-tread tyres on blue Y-spoke rims, servo-and-bracket
    // arm. No dimensions were measured from the prototype: wheel size and track / wheelbase are kept
    // from the original layout, the rest is proportioned from the photo.
    inline constexpr float RV_WHEEL_R = 0.07f, RV_WHEEL_W = 0.06f;  // tyre block tops at R
    inline constexpr float RV_WHEEL_X = 0.17f, RV_WHEEL_Z = 0.215f;
    inline constexpr float RV_RIM_R = 0.045f;                       // bead seat
    inline const glm::vec3 RV_CHASSIS_HALF{ 0.20f, 0.055f, 0.165f };  // black box chassis, y 0.035 .. 0.145
    inline constexpr float RV_CHASSIS_Y = 0.090f;
    inline constexpr float RV_DECK = 0.160f, RV_DECK_T = 0.008f, RV_DECK_TOP = RV_DECK + RV_DECK_T;
    inline constexpr float RV_DECK_NOSE = 0.27f, RV_DECK_TAIL = -0.25f, RV_DECK_HALF_Z = 0.17f;   // top plate outline
    inline constexpr float RV_FENDER_Y = 0.152f, RV_FENDER_T = 0.004f;                 // fender plate underside / thickness
    inline constexpr float RV_FENDER_Z0 = 0.150f, RV_FENDER_Z1 = 0.258f;              // inner / outer edge (per side)
    inline constexpr float RV_PLATFORM_H = 0.025f, RV_PLATFORM_T = 0.005f;            // raised arm mounting platform
    inline const glm::vec3 RV_PICO{ 0.045f, RV_DECK_TOP, -0.095f };   // Pico on a carrier board (long axis X)
    inline const glm::vec3 RV_DRIVER{ 0.045f, RV_DECK_TOP, 0.085f };  // dual motor driver board
    inline const glm::vec3 RV_BATTERY{ -0.02f, 0.085f, 0.0f };        // pack inside the tub (centre)
    inline const glm::vec3 RV_LIDAR{ 0.185f, RV_DECK_TOP, 0.0f };     // mast base; scan centre ~0.1 m above
    inline constexpr float RV_LIDAR_MAST = 0.05f;
    inline const glm::vec3 RV_ARM_BASE{ -0.11f, RV_DECK_TOP + RV_PLATFORM_H + RV_PLATFORM_T, 0.0f };  // J1 (yaw) on the platform
    inline constexpr float RV_BASE_SERVO_TOP = 0.042f;               // base (yaw) servo output face above the platform
    inline constexpr float RV_SHOULDER_H = 0.088f;                   // J2 above the platform
    inline constexpr float RV_L2 = 0.30f, RV_L3 = 0.26f;             // shoulder -> elbow -> wrist (kept: sampling reach)
    // Standard-size hobby servo envelope (40 x 20 x 36 mm body, output shaft 10 mm from one end).
    inline constexpr float SERVO_FRONT = 0.0103f, SERVO_BACK = 0.0304f, SERVO_HW = 0.00985f, SERVO_HZ = 0.018f;
    inline constexpr float RV_HEAD_PLATE = 0.100f;                   // wrist axis -> sensor plate underside
    inline constexpr float RV_HEAD = 0.194f;                         // wrist -> probe tips (head kept vertical)
    inline constexpr float PROBE_DEPTH = 0.04f;                      // insertion below the soil surface
    inline constexpr float PROBE_SPACING = 0.03f;                    // moisture | temperature | pH along the head
}
