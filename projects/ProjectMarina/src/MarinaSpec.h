#pragma once
// Project Marina: dimensional specification shared by the mesh generator (tools/assetgen/
// MarinaMeshes.cpp, which builds projects/assets/meshes/marina/*.obj) and the scene (MarinaScene.cpp,
// which places the parts and routes tubing and wiring between their ports).
//
// Units are metres. World axes: +X right (process flows left -> right), +Y up, +Z towards the
// front of the bench. The top face of the baseplate is y = 0.
//
// Every assembly is unrotated. Its local origin is the centre of its footprint on the surface it
// stands on, so world point = ORIGIN + local point. Port values are the tips of hose barbs and
// fittings (tubing sleeves over them) or the tops of header pins / terminal entries (wiring).
//
// Dimensions follow the public board outlines where they exist (Raspberry Pi 85 x 56 mm,
// Arduino UNO 68.6 x 53.4 mm, full-size breadboard 165 x 55 mm). Everything else is a generic,
// plausible engineering model of the stated component class, not a specific product.
#include <glm/vec3.hpp>

namespace Marina::Spec {
    // ---------------------------------------------------------------- tubing and wiring
    inline constexpr float TUBE_OD = 0.008f;        // 5 x 8 mm silicone tubing
    inline constexpr float CORE_RATIO = 0.6f;       // fluid core diameter / tube OD (the bore)
    inline constexpr float BEND_RATIO = 1.5f;       // centreline bend radius / OD (meshes are built for this)
    inline constexpr float SLEEVE = 0.007f;         // tubing pushed over a barb
    inline constexpr float WIRE_OD = 0.0018f;       // 22 AWG jumper wire
    inline constexpr float CABLE_OD = 0.0032f;      // sheathed 3-core sensor cable

    // ---------------------------------------------------------------- bench structure
    inline const glm::vec3 PLATE_HALF{ 0.45f, 0.006f, 0.26f };    // aluminium tooling plate, top at y = 0
    inline constexpr float FOOT_HEIGHT = 0.008f;                  // rubber feet under the plate
    inline const glm::vec3 TRAY_CENTRE{ -0.3025f, 0.0f, 0.02f };  // anodised electronics sub-plate
    inline const glm::vec3 TRAY_HALF{ 0.1375f, 0.002f, 0.225f };
    inline constexpr float TRAY_TOP = 0.004f;
    inline constexpr float DIVIDER_X = -0.155f;                   // splash wall between dry and wet zones
    inline constexpr float DIVIDER_HEIGHT = 0.045f;
    inline const glm::vec3 DUCT_CENTRE{ 0.0f, 0.0f, -0.232f };    // slotted wiring duct along the back
    inline constexpr float DUCT_HALF_LENGTH = 0.43f;
    inline constexpr float DUCT_WIDTH = 0.025f, DUCT_HEIGHT = 0.025f;

    // ---------------------------------------------------------------- 1 computing unit (Raspberry Pi class SBC)
    inline const glm::vec3 PI_ORIGIN{ -0.32f, TRAY_TOP, -0.10f };
    inline constexpr float PI_PCB_Y = 0.010f;                     // underside of the 1.4 mm board
    inline const glm::vec3 PI_WATERBLOCK{ 0.016f, 0.0126f, 0.0f }; // block base centre, on the SoC
    inline const glm::vec3 PI_WB_OUT{ 0.016f, 0.0346f, -0.008f };  // barb tips, pointing +Y
    inline const glm::vec3 PI_WB_IN{ 0.016f, 0.0346f, 0.008f };
    inline const glm::vec3 PI_FAN_CENTRE{ -0.012f, 0.0315f, 0.006f }; // 30 mm fan, axis +Y (rotor pivot)
    inline const glm::vec3 PI_FAN_5V_EXIT{ -0.018f, 0.0315f, -0.009f };   // fan lead exits, pointing -Z
    inline const glm::vec3 PI_FAN_GND_EXIT{ -0.0155f, 0.0315f, -0.009f };
    inline const glm::vec3 PI_PIN_5V{ -0.018f, 0.0199f, -0.0232f };       // GPIO header pin tops
    inline const glm::vec3 PI_PIN_GND{ -0.0155f, 0.0199f, -0.0232f };

    // ---------------------------------------------------------------- 3 electronics
    inline const glm::vec3 ARDUINO_ORIGIN{ -0.35f, TRAY_TOP, 0.03f };
    inline const glm::vec3 ARD_D2{ -0.002f, 0.0161f, -0.0245f };   // digital header (back edge) pin tops
    inline const glm::vec3 ARD_GND{ 0.0055f, 0.0161f, -0.0245f };
    inline const glm::vec3 ARD_D9{ 0.0105f, 0.0161f, -0.0245f };
    inline const glm::vec3 ARD_SDA{ 0.0295f, 0.0161f, 0.0245f };   // analog header (front edge)
    inline const glm::vec3 ARD_SCL{ 0.0320f, 0.0161f, 0.0245f };

    inline const glm::vec3 BREADBOARD_ORIGIN{ -0.30f, TRAY_TOP, 0.17f };
    inline constexpr float BREADBOARD_TOP = 0.0085f;               // local
    inline const glm::vec3 ESP32_CENTRE{ -0.03f, 0.0f, 0.0f };     // on the breadboard (local), straddling the channel
    inline const glm::vec3 BB_SDA{ -0.0205f, BREADBOARD_TOP, -0.0165f };   // holes beside the ESP32-C3 pins
    inline const glm::vec3 BB_SCL{ -0.0180f, BREADBOARD_TOP, -0.0165f };
    inline const glm::vec3 BB_BUS{ 0.060f, BREADBOARD_TOP, -0.0165f };     // 1-Wire sensor bus row
    inline constexpr float BB_PROBE_X = 0.0725f;                   // probe cable landing column (local x)

    inline const glm::vec3 DRIVER_ORIGIN{ -0.255f, TRAY_TOP, 0.03f };      // generic MOSFET pump driver module
    inline const glm::vec3 DRV_SIG{ -0.014f, 0.0151f, -0.004f };   // 3-pin header tops
    inline const glm::vec3 DRV_GND{ -0.009f, 0.0151f, -0.004f };
    inline const glm::vec3 DRV_OUT_POS{ 0.014f, 0.0166f, 0.005f }; // output screw terminal entries
    inline const glm::vec3 DRV_OUT_NEG{ 0.009f, 0.0166f, 0.005f };

    // ---------------------------------------------------------------- 4 pump assembly (generic 12 V DC diaphragm pump)
    inline const glm::vec3 PUMP_ORIGIN{ -0.085f, 0.0f, -0.10f };
    inline constexpr float PUMP_AXIS_Y = 0.024f;                   // motor axis along X
    inline const glm::vec3 PUMP_IN{ 0.012f, 0.056f, -0.008f };     // head barbs, pointing +Y
    inline const glm::vec3 PUMP_OUT{ 0.012f, 0.056f, 0.008f };
    inline const glm::vec3 PUMP_TERM_POS{ -0.052f, PUMP_AXIS_Y, 0.0045f };   // motor tabs, pointing -X
    inline const glm::vec3 PUMP_TERM_NEG{ -0.052f, PUMP_AXIS_Y, -0.0045f };

    // ---------------------------------------------------------------- 5 heat exchanger (generic brazed-plate)
    inline const glm::vec3 HX_ORIGIN{ 0.03f, 0.0f, -0.11f };
    inline const glm::vec3 HX_HALF{ 0.0375f, 0.065f, 0.0225f };     // plate pack (centre y = 0.077)
    inline constexpr float HX_BASE = 0.012f;                       // stand height
    inline const glm::vec3 HX_COOLANT_IN{ -0.022f, 0.120f, 0.0385f };  // front-face nozzles, pointing +Z
    inline const glm::vec3 HX_COOLANT_OUT{ -0.022f, 0.034f, 0.0385f };
    inline const glm::vec3 HX_FEED_IN{ 0.022f, 0.034f, 0.0385f };
    inline const glm::vec3 HX_FEED_OUT{ 0.022f, 0.120f, 0.0385f };

    // ---------------------------------------------------------------- 6 membrane distillation unit (flat-sheet cell)
    inline const glm::vec3 MD_ORIGIN{ 0.19f, 0.0f, 0.02f };
    inline constexpr float MD_CENTRE_Y = 0.071f;                   // housing: x +-0.075, y +-0.055, z +-0.045
    inline const glm::vec3 MD_HOT_IN{ -0.089f, 0.036f, 0.0f };     // hot side (x < 0), pointing -X
    inline const glm::vec3 MD_HOT_OUT{ -0.089f, 0.106f, 0.0f };
    inline const glm::vec3 MD_COLD_OUT{ 0.089f, 0.106f, 0.0f };    // cold side (x > 0), pointing +X
    inline const glm::vec3 MD_PROBE_HOT{ -0.040f, 0.130f, 0.0f };  // probe bosses on top
    inline const glm::vec3 MD_PROBE_COLD{ 0.040f, 0.130f, 0.0f };

    // ---------------------------------------------------------------- 7 seawater reservoir
    inline const glm::vec3 TANK_ORIGIN{ -0.07f, 0.0f, 0.12f };
    inline constexpr float TANK_RADIUS = 0.055f, TANK_HEIGHT = 0.150f, TANK_LID_TOP = 0.162f;
    inline constexpr float TANK_FILL = 0.110f;                     // water level (visual)
    inline const glm::vec3 TANK_OUTLET{ 0.067f, 0.034f, 0.0f };    // side bulkhead, pointing +X
    inline const glm::vec3 TANK_RETURN{ 0.020f, 0.174f, 0.0f };    // lid barb, pointing +Y
    inline const glm::vec3 TANK_PROBE{ -0.020f, TANK_LID_TOP, 0.0f };

    // ---------------------------------------------------------------- 8 condensation system
    inline const glm::vec3 COND_ORIGIN{ 0.375f, 0.0f, 0.02f };     // stand + condenser above the collection bottle
    inline const glm::vec3 COND_HALF{ 0.035f, 0.040f, 0.0225f };    // fin block, centre y = 0.20
    inline constexpr float COND_CENTRE_Y = 0.20f;
    inline constexpr float COND_POST_X = 0.055f;                   // uprights at +-x
    inline const glm::vec3 COND_IN{ -0.020f, 0.252f, 0.0f };       // pointing +Y
    inline const glm::vec3 COND_OUT{ 0.0f, 0.148f, 0.0f };         // pointing -Y
    inline constexpr float BOTTLE_RADIUS = 0.040f, BOTTLE_HEIGHT = 0.100f, BOTTLE_LID_TOP = 0.110f;
    inline constexpr float BOTTLE_FILL = 0.045f;
    inline const glm::vec3 BOTTLE_IN{ 0.0f, 0.122f, 0.0f };        // pointing +Y
    inline const glm::vec3 BOTTLE_PROBE{ 0.022f, BOTTLE_LID_TOP, -0.012f };

    // ---------------------------------------------------------------- 9 instrumentation
    inline constexpr float PROBE_HEIGHT = 0.030f;                  // mount face -> cable exit
}
