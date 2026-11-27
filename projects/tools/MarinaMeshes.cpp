// projectassetgen: Project Marina engineering assets (OBJ, CC0, deterministic).
//
//   meshes/marina/*.obj   bench structure (tooling plate, feet, electronics sub-plate, splash wall,
//                         slotted wiring duct), computing unit (SBC case, board, ICs, connectors,
//                         SoC water block, 30 mm fan frame + rotor), electronics (UNO-format board,
//                         ESP32-C3 dev board, breadboard, MOSFET driver module), DC diaphragm pump,
//                         brazed-plate heat exchanger, flat-sheet membrane distillation cell, seawater
//                         reservoir, finned condenser on a stand, freshwater bottle, temperature
//                         probe, and the tubing / wiring kit (straight + 90 degree bend pieces).
//
// Real-size parts in metres: every assembly's local origin is the centre of its footprint (see
// apps/lab/src/MarinaSpec.h, shared with the scene, for placements and port positions). The tubing
// kit is unit-sized: straights span y -0.5..0.5 with diameter 1 (scale = OD, length, OD); bends are
// built for scale = OD (centreline radius Spec::BEND_RATIO, entering along +X at (-R, 0, 0) and
// leaving along +Y at (0, R, 0)). Generic models: no specific product geometry is reproduced.
#include "MeshKit.h"
#include "MarinaSpec.h"

namespace fs = std::filesystem;

namespace {
    using namespace MeshKit;
    namespace S = Marina::Spec;

    bool Write(const fs::path& dir, const char* file, const Mesh& m, const char* what) { return WriteObj(dir / file, m, what); }

    // ================================================================== tubing / wiring kit
    bool Kit(const fs::path& dir) {
        bool ok = true;
        {   Mesh m; Pipe(m, { vec3(0, -0.5f, 0), vec3(0, 0.5f, 0) }, 0.5f, 32, false);
            ok &= Write(dir, "tube_straight.obj", m, "tubing kit: open straight, diameter 1, y -0.5..0.5"); }
        {   Mesh m; Pipe(m, { vec3(0, -0.5f, 0), vec3(0, 0.5f, 0) }, 0.5f, 16, true);
            ok &= Write(dir, "rod.obj", m, "tubing kit: capped straight (fluid core / wire), diameter 1"); }
        auto arc = [] {
            std::vector<vec3> path;
            const float R = S::BEND_RATIO;
            for (int i = 0; i <= 12; ++i) {
                const float a = -0.5f * PI + 0.5f * PI * float(i) / 12.0f;
                path.push_back(vec3(-R + R * std::cos(a), R + R * std::sin(a), 0.0f));
            }
            return path;
        };
        {   Mesh m; Pipe(m, arc(), 0.5f, 32, false);
            ok &= Write(dir, "tube_bend.obj", m, "tubing kit: 90 degree bend (+X in, +Y out), diameter 1"); }
        {   Mesh m; Pipe(m, arc(), 0.5f * S::CORE_RATIO, 16, false);
            ok &= Write(dir, "tube_bend_core.obj", m, "tubing kit: 90 degree bend fluid core"); }
        return ok;
    }

    // ================================================================== bench structure
    bool Structure(const fs::path& dir) {
        bool ok = true;
        {   Mesh m;   // tooling plate, top at y = 0, 12 mm thick
            Prism(m, RoundRect(S::PLATE_HALF.x, S::PLATE_HALF.z, 0.008f, 6), -2.0f * S::PLATE_HALF.y, 0.0f, 1, vec3(0.0f));
            ok &= Write(dir, "baseplate.obj", m, "aluminium tooling plate 900 x 520 x 12 mm");
        }
        {   Mesh m;   // four feet under the plate corners
            for (float x : { -0.41f, 0.41f }) for (float z : { -0.22f, 0.22f })
                LatheAt(m, { { 0, 0 }, { 0.014f, 0 }, { 0.016f, 0.006f }, { 0.016f, S::FOOT_HEIGHT }, { 0, S::FOOT_HEIGHT } }, 24,
                        vec3(x, -2.0f * S::PLATE_HALF.y - S::FOOT_HEIGHT, z), vec3(0, 1, 0));
            ok &= Write(dir, "feet.obj", m, "rubber levelling feet");
        }
        {   Mesh m;
            Prism(m, RoundRect(S::TRAY_HALF.x, S::TRAY_HALF.z, 0.005f), 0.0f, S::TRAY_TOP, 1, vec3(0.0f));
            ok &= Write(dir, "tray.obj", m, "anodised electronics sub-plate");
        }
        {   Mesh m;   // splash wall + foot flange (towards the wet side), screws in the flange
            const float hz = S::TRAY_HALF.z;
            BoxMinMax(m, vec3(-0.0015f, 0.0f, -hz), vec3(0.0015f, S::DIVIDER_HEIGHT, hz));
            BoxMinMax(m, vec3(0.0015f, 0.0f, -hz), vec3(0.0165f, 0.003f, hz));
            ok &= Write(dir, "divider.obj", m, "splash wall between electronics and water handling");
        }
        {   Mesh m;   // slotted duct: floor, two comb walls (open top, wires visible)
            const float L = S::DUCT_HALF_LENGTH, hw = 0.5f * S::DUCT_WIDTH, H = S::DUCT_HEIGHT, t = 0.0015f;
            BoxMinMax(m, vec3(-L, 0.0f, -hw), vec3(L, t, hw));
            for (float side : { -1.0f, 1.0f }) {
                const float z0 = side < 0 ? -hw : hw - t, z1 = z0 + t;
                BoxMinMax(m, vec3(-L, t, z0), vec3(L, 0.007f, z1));                    // solid lower strip
                for (float x = -L; x + 0.004f <= L + 1e-5f; x += 0.008f)                // fingers
                    BoxMinMax(m, vec3(x, 0.007f, z0), vec3(x + 0.004f, H, z1));
            }
            ok &= Write(dir, "duct.obj", m, "slotted wiring duct 25 x 25 mm, open top");
        }
        return ok;
    }

    // ================================================================== 1 computing unit
    bool Computing(const fs::path& dir) {
        bool ok = true;
        const float pcb0 = S::PI_PCB_Y, pcb1 = S::PI_PCB_Y + 0.0014f;
        {   Mesh m;   // open-frame case base: plate, low lip, four standoffs
            Prism(m, RoundRect(0.047f, 0.0325f, 0.004f), 0.0f, 0.004f, 1, vec3(0.0f));
            BoxMinMax(m, vec3(-0.047f, 0.004f, -0.0325f), vec3(0.047f, 0.008f, -0.0310f));
            BoxMinMax(m, vec3(-0.047f, 0.004f, 0.0310f), vec3(0.047f, 0.008f, 0.0325f));
            BoxMinMax(m, vec3(0.0455f, 0.004f, -0.031f), vec3(0.047f, 0.008f, 0.031f));
            for (float x : { -0.039f, 0.019f }) for (float z : { -0.0245f, 0.0245f })
                CylAt(m, vec3(x, 0.004f, z), vec3(0, 1, 0), 0.0028f, pcb0 - 0.004f, 16);
            ok &= Write(dir, "sbc_case.obj", m, "SBC open-frame case base");
        }
        {   Mesh m;
            Prism(m, RoundRect(0.0425f, 0.028f, 0.003f), pcb0, pcb1, 1, vec3(0.0f));
            ok &= Write(dir, "sbc_pcb.obj", m, "SBC board 85 x 56 mm (generic Raspberry Pi format)");
        }
        {   Mesh m;   // packages: SoC (under the water block), RAM, USB controller, PMIC, header base
            BoxMinMax(m, vec3(0.009f, pcb1, -0.007f), vec3(0.023f, pcb1 + 0.0012f, 0.007f));
            BoxMinMax(m, vec3(0.0305f, pcb1, -0.007f), vec3(0.0405f, pcb1 + 0.0010f, 0.007f));
            BoxMinMax(m, vec3(-0.026f, pcb1, -0.016f), vec3(-0.020f, pcb1 + 0.0009f, -0.010f));
            BoxMinMax(m, vec3(0.026f, pcb1, 0.012f), vec3(0.031f, pcb1 + 0.0008f, 0.017f));
            BoxMinMax(m, vec3(-0.0218f, pcb1, -0.0270f), vec3(0.0290f, pcb1 + 0.0025f, -0.0220f));
            ok &= Write(dir, "sbc_ics.obj", m, "SBC IC packages and header base");
        }
        {   Mesh m;   // connectors and 40 header pins
            for (float z : { 0.0f, 0.018f }) BoxMinMax(m, vec3(-0.0475f, pcb1, z - 0.00665f), vec3(-0.030f, pcb1 + 0.016f, z + 0.00665f));
            BoxMinMax(m, vec3(-0.0475f, pcb1, -0.0255f), vec3(-0.0265f, pcb1 + 0.0135f, -0.0095f));
            BoxMinMax(m, vec3(0.026f, pcb1, 0.0215f), vec3(0.035f, pcb1 + 0.0032f, 0.0290f));
            for (float x : { 0.0105f, -0.0030f }) BoxMinMax(m, vec3(x, pcb1, 0.0225f), vec3(x + 0.0075f, pcb1 + 0.0030f, 0.0290f));
            for (int k = 0; k < 20; ++k) for (float z : { -0.02577f, -0.02323f }) {
                const float x = -0.0205f + 0.00254f * float(k);
                BoxMinMax(m, vec3(x - 0.00032f, pcb1 + 0.0025f, z - 0.00032f), vec3(x + 0.00032f, 0.0199f, z + 0.00032f));
            }
            ok &= Write(dir, "sbc_connectors.obj", m, "SBC USB / Ethernet / USB-C / HDMI shells, GPIO pins");
        }
        {   Mesh m;   // SoC water block with two barbs (out at -z, in at +z)
            const vec3 c = S::PI_WATERBLOCK + vec3(0.0f, 0.005f, 0.0f);
            RBox(m, c, vec3(0.015f, 0.005f, 0.015f), 0.0015f);
            for (const vec3& tip : { S::PI_WB_OUT, S::PI_WB_IN }) Barb(m, tip - vec3(0.0f, 0.012f, 0.0f), vec3(0, 1, 0));
            ok &= Write(dir, "waterblock.obj", m, "SoC water block (generic), two 5 mm barbs");
        }
        {   Mesh m;   // 30 mm fan frame around its centre (rotor pivot), posts down to the board
            const float h = 0.0035f;
            Ring(m, vec3(0.0f), 0.0142f, 0.0150f, -h, h, 48);
            for (float sx : { -1.0f, 1.0f }) {
                BoxMinMax(m, vec3(sx > 0 ? 0.0138f : -0.0150f, -h, -0.015f), vec3(sx > 0 ? 0.0150f : -0.0138f, h, 0.015f));
                BoxMinMax(m, vec3(-0.015f, -h, sx > 0 ? 0.0138f : -0.0150f), vec3(0.015f, h, sx > 0 ? 0.0150f : -0.0138f));
            }
            for (float x : { -1.0f, 1.0f }) for (float z : { -1.0f, 1.0f }) {
                BoxMinMax(m, vec3(x > 0 ? 0.0105f : -0.0150f, -h, z > 0 ? 0.0105f : -0.0150f), vec3(x > 0 ? 0.0150f : -0.0105f, h, z > 0 ? 0.0150f : -0.0105f));
                CylAt(m, vec3(x * 0.0126f, S::PI_PCB_Y + 0.0014f - S::PI_FAN_CENTRE.y, z * 0.0126f), vec3(0, 1, 0), 0.0011f, S::PI_FAN_CENTRE.y - h - S::PI_PCB_Y - 0.0014f, 10);
            }
            CylAt(m, vec3(0.0f, -h, 0.0f), vec3(0, 1, 0), 0.0068f, 0.0015f, 24);   // motor mount
            for (int i = 0; i < 3; ++i) {
                const float a = float(i) / 3.0f * 2.0f * PI + 0.4f;
                Mesh s;
                BoxMinMax(s, vec3(0.0065f, 0.0f, -0.0006f), vec3(0.0145f, 0.0012f, 0.0006f));
                m.Append(s, Axis(vec3(0, 1, 0), a), vec3(0.0f, -h, 0.0f));
            }
            ok &= Write(dir, "fan_frame.obj", m, "30 mm axial fan frame with mounting posts");
        }
        {   Mesh m;   // rotor: hub + 7 twisted blades, spins about +Y
            Cylinder(m, 0.0065f, -0.0028f, 0.0026f, 32, 0.0008f);
            const Mesh blade = Blade(0.0058f, 0.0136f, [](float s) { return 0.0085f + 0.002f * s; }, [](float s) { return 1.0f - 0.25f * s; }, 0.08f, 0.04f, 10, 5);
            RingOf(m, blade, 7, 0.0f, Axis(vec3(0, 0, 1), -0.5f * PI));
            ok &= Write(dir, "fan_rotor.obj", m, "30 mm fan rotor, 7 blades");
        }
        return ok;
    }

    // ================================================================== 3 electronics
    bool Electronics(const fs::path& dir) {
        bool ok = true;
        {   // UNO-format board: PCB on 6 mm standoffs
            const float p0 = 0.006f, p1 = 0.0076f;
            {   Mesh m;
                Prism(m, RoundRect(0.0343f, 0.0267f, 0.0015f), p0, p1, 1, vec3(0.0f));
                ok &= Write(dir, "uno_pcb.obj", m, "UNO-format board 68.6 x 53.4 mm (generic)");
            }
            {   Mesh m;
                BoxMinMax(m, vec3(-0.012f, p1, -0.0258f), vec3(0.012f, 0.0161f, -0.0232f));       // digital headers
                BoxMinMax(m, vec3(0.0135f, p1, -0.0258f), vec3(0.031f, 0.0161f, -0.0232f));
                BoxMinMax(m, vec3(-0.0105f, p1, 0.0232f), vec3(0.0098f, 0.0161f, 0.0258f));       // power / analog
                BoxMinMax(m, vec3(0.0115f, p1, 0.0232f), vec3(0.0335f, 0.0161f, 0.0258f));
                BoxMinMax(m, vec3(-0.0060f, p1, 0.0020f), vec3(0.0300f, p1 + 0.0020f, 0.0101f));   // DIP socket
                BoxMinMax(m, vec3(-0.0055f, p1 + 0.0020f, 0.0023f), vec3(0.0295f, p1 + 0.0055f, 0.0098f));   // DIP-28 MCU
                BoxMinMax(m, vec3(-0.0405f, p1, 0.0080f), vec3(-0.0265f, p1 + 0.0110f, 0.0170f));  // barrel jack
                BoxMinMax(m, vec3(-0.0200f, p1, -0.0150f), vec3(-0.0150f, p1 + 0.0009f, -0.0100f)); // USB bridge chip
                BoxMinMax(m, vec3(-0.0300f, p1, -0.0240f), vec3(-0.0240f, p1 + 0.0030f, -0.0180f)); // reset button
                ok &= Write(dir, "uno_parts.obj", m, "UNO-format headers, DIP MCU, jack, chips");
            }
            {   Mesh m;
                BoxMinMax(m, vec3(-0.0405f, p1, -0.0180f), vec3(-0.0245f, p1 + 0.0110f, -0.0060f)); // USB-B shell
                BoxMinMax(m, vec3(0.0000f, p1, -0.0050f), vec3(0.0110f, p1 + 0.0035f, -0.0010f));   // crystal can
                BoxMinMax(m, vec3(-0.0300f, p1, 0.0200f), vec3(-0.0200f, p1 + 0.0045f, 0.0240f));   // regulator
                for (float x : { -0.0204f, 0.0304f }) for (float z : { -0.0230f, 0.0230f })
                    CylAt(m, vec3(x, 0.0f, z), vec3(0, 1, 0), 0.0022f, p0, 6);                       // hex standoffs
                ok &= Write(dir, "uno_metal.obj", m, "UNO-format USB-B shell, crystal, regulator, standoffs");
            }
        }
        {   // breadboard (local origin: footprint centre; top at 8.5 mm)
            const float H = S::BREADBOARD_TOP;
            {   Mesh m;
                BoxMinMax(m, vec3(-0.0825f, 0.0f, -0.0275f), vec3(0.0825f, H, -0.0015f));
                BoxMinMax(m, vec3(-0.0825f, 0.0f, 0.0015f), vec3(0.0825f, H, 0.0275f));
                BoxMinMax(m, vec3(-0.0825f, 0.0f, -0.0015f), vec3(0.0825f, H - 0.003f, 0.0015f));
                ok &= Write(dir, "breadboard.obj", m, "solderless breadboard 165 x 55 mm");
            }
            {   Mesh m; BoxMinMax(m, vec3(-0.078f, 0.0f, -0.0004f), vec3(0.078f, 0.0001f, 0.0004f));
                ok &= Write(dir, "breadboard_rail.obj", m, "breadboard power-rail marking"); }
        }
        {   // ESP32-C3 dev board (local origin: on the breadboard top surface, centred)
            {   Mesh m; BoxMinMax(m, vec3(-0.026f, 0.0025f, -0.01275f), vec3(0.026f, 0.0041f, 0.01275f));
                ok &= Write(dir, "esp32c3_pcb.obj", m, "ESP32-C3 dev board PCB (generic)"); }
            {   Mesh m;
                for (float z : { -0.0115f, 0.0115f }) BoxMinMax(m, vec3(-0.0254f, 0.0f, z - 0.00127f), vec3(0.0254f, 0.0025f, z + 0.00127f));
                for (float x : { -0.012f, -0.006f }) BoxMinMax(m, vec3(x, 0.0041f, -0.0095f), vec3(x + 0.0035f, 0.0058f, -0.0060f));
                ok &= Write(dir, "esp32c3_parts.obj", m, "ESP32-C3 headers and buttons"); }
            {   Mesh m;
                BoxMinMax(m, vec3(0.0040f, 0.0041f, -0.0066f), vec3(0.0206f, 0.0065f, 0.0066f));      // module shield
                BoxMinMax(m, vec3(-0.0285f, 0.0041f, -0.0045f), vec3(-0.0215f, 0.0073f, 0.0045f));    // USB-C
                ok &= Write(dir, "esp32c3_metal.obj", m, "ESP32-C3 module shield and USB-C"); }
        }
        {   // MOSFET pump-driver module (local origin: footprint centre, on 5 mm standoffs)
            const float p0 = 0.005f, p1 = 0.0066f;
            {   Mesh m; Prism(m, RoundRect(0.017f, 0.010f, 0.001f), p0, p1, 1, vec3(0.0f));
                ok &= Write(dir, "driver_pcb.obj", m, "MOSFET driver module PCB (generic)"); }
            {   Mesh m;
                BoxMinMax(m, vec3(-0.0153f, p1, -0.0053f), vec3(-0.0077f, p1 + 0.0025f, -0.0027f));   // 3-pin header
                BoxMinMax(m, vec3(-0.0050f, 0.0096f, -0.0060f), vec3(0.0050f, 0.0251f, -0.0015f));     // TO-220 body
                BoxMinMax(m, vec3(-0.0010f, p1, 0.0040f), vec3(0.0020f, p1 + 0.0010f, 0.0080f));      // LED + resistor
                ok &= Write(dir, "driver_parts.obj", m, "driver header base and TO-220 package"); }
            {   Mesh m; BoxMinMax(m, vec3(0.0060f, p1, 0.0005f), vec3(0.0170f, 0.0166f, 0.0095f));
                ok &= Write(dir, "driver_terminal.obj", m, "driver output screw terminal"); }
            {   Mesh m;
                for (const vec3& pin : { S::DRV_SIG, vec3(-0.0115f, S::DRV_SIG.y, S::DRV_SIG.z), S::DRV_GND })
                    BoxMinMax(m, vec3(pin.x - 0.00032f, p1 + 0.0025f, pin.z - 0.00032f), vec3(pin.x + 0.00032f, pin.y, pin.z + 0.00032f));
                BoxMinMax(m, vec3(-0.0050f, 0.0251f, -0.0040f), vec3(0.0050f, 0.0311f, -0.0027f));    // tab
                for (float x : { -0.00254f, 0.0f, 0.00254f }) BoxMinMax(m, vec3(x - 0.0004f, p1, -0.0040f), vec3(x + 0.0004f, 0.0096f, -0.0032f));
                for (float x : { -0.0145f, 0.0145f }) for (float z : { -0.0075f, 0.0075f }) CylAt(m, vec3(x, 0.0f, z), vec3(0, 1, 0), 0.0016f, p0, 6);
                for (float x : { S::DRV_OUT_NEG.x, S::DRV_OUT_POS.x }) CylAt(m, vec3(x, 0.0166f, 0.0080f), vec3(0, 1, 0), 0.0014f, 0.0004f, 12);
                ok &= Write(dir, "driver_metal.obj", m, "driver pins, tab, standoffs, terminal screws"); }
        }
        return ok;
    }

    // ================================================================== 4 pump assembly
    bool Pump(const fs::path& dir) {
        bool ok = true;
        const float ay = S::PUMP_AXIS_Y;
        const vec3 X(1, 0, 0);
        {   Mesh m;   // motor can (axis X) + rear bearing boss
            CylAt(m, vec3(-0.045f, ay, 0.0f), X, 0.01375f, 0.040f, 40, 0.0012f);
            CylAt(m, vec3(-0.0472f, ay, 0.0f), X, 0.0050f, 0.0024f, 24, 0.0004f);
            ok &= Write(dir, "pump_motor.obj", m, "DC pump motor can (generic 385 class)");
        }
        {   Mesh m;   // head: flange disc + chamber block + barbs on top
            CylAt(m, vec3(-0.005f, ay, 0.0f), X, 0.0190f, 0.008f, 40, 0.0010f);
            RBox(m, vec3(0.012f, ay, 0.0f), vec3(0.009f, 0.019f, 0.019f), 0.003f);
            for (const vec3& tip : { S::PUMP_IN, S::PUMP_OUT }) {
                CylAt(m, vec3(tip.x, ay + 0.019f, tip.z), vec3(0, 1, 0), 0.0050f, tip.y - 0.012f - ay - 0.019f, 20);
                Barb(m, tip - vec3(0.0f, 0.012f, 0.0f), vec3(0, 1, 0));
            }
            ok &= Write(dir, "pump_head.obj", m, "diaphragm pump head with inlet / outlet barbs");
        }
        {   Mesh m;
            for (const vec3& tip : { S::PUMP_TERM_POS, S::PUMP_TERM_NEG })
                BoxMinMax(m, vec3(tip.x, ay - 0.0015f, tip.z - 0.0012f), vec3(-0.0450f, ay + 0.0015f, tip.z + 0.0012f));
            ok &= Write(dir, "pump_terminals.obj", m, "pump motor solder tabs");
        }
        {   Mesh m;   // bracket: base plate, two saddles, two straps over the motor
            BoxMinMax(m, vec3(-0.050f, 0.0f, -0.022f), vec3(0.000f, 0.003f, 0.022f));
            const float bottom = ay - 0.01375f;
            for (float x : { -0.040f, -0.016f }) {
                BoxMinMax(m, vec3(x, 0.003f, -0.012f), vec3(x + 0.006f, bottom, 0.012f));
                Mesh strap;
                // half ring over the top of the motor: lathe axis Y -> X, the z >= 0 half -> y >= 0
                Lathe(strap, { { 0.0139f, 0.0f }, { 0.0154f, 0.0f }, { 0.0154f, 0.006f }, { 0.0139f, 0.006f } }, 24, 35.0f, 0.0f, PI);
                m.Append(strap, Axis(vec3(0, 0, 1), -0.5f * PI) * Axis(vec3(0, 1, 0), -0.5f * PI), vec3(x, ay, 0.0f));
                for (float z : { -0.01465f, 0.01465f }) BoxMinMax(m, vec3(x, 0.003f, z - 0.00075f), vec3(x + 0.006f, ay, z + 0.00075f));
            }
            ok &= Write(dir, "pump_bracket.obj", m, "pump mounting bracket with straps");
        }
        return ok;
    }

    // ================================================================== 5 heat exchanger
    bool HeatExchanger(const fs::path& dir) {
        bool ok = true;
        const float cy = S::HX_BASE + S::HX_HALF.y, hx = S::HX_HALF.x, hy = S::HX_HALF.y, hz = S::HX_HALF.z;
        {   Mesh m;   // cover plates + corrugated plate pack + four nozzles on the front plate
            RoundSlab(m, vec3(0.0f, cy, hz - 0.002f), vec3(hx, hy, 0.002f), 0.008f, 2);
            RoundSlab(m, vec3(0.0f, cy, -hz + 0.002f), vec3(hx, hy, 0.002f), 0.008f, 2);
            const int plates = 14;
            const float z0 = -hz + 0.004f, pitch = (2.0f * hz - 0.008f) / float(plates);
            for (int i = 0; i < plates; ++i) {
                const float inset = (i % 2) ? 0.0009f : 0.0f;
                RoundSlab(m, vec3(0.0f, cy, z0 + pitch * (float(i) + 0.5f)), vec3(hx - inset, hy - inset, pitch * 0.5f * 0.96f), 0.008f, 2);
            }
            for (const vec3& tip : { S::HX_COOLANT_IN, S::HX_COOLANT_OUT, S::HX_FEED_IN, S::HX_FEED_OUT }) {
                const vec3 base(tip.x, tip.y, hz);
                LatheAt(m, { { 0, 0 }, { 0.0065f, 0 }, { 0.0065f, 0.0010f }, { 0.0072f, 0.0013f }, { 0.0065f, 0.0016f }, { 0.0065f, 0.0024f },
                             { 0.0072f, 0.0027f }, { 0.0065f, 0.0030f }, { 0.0065f, 0.0040f }, { 0, 0.0040f } }, 28, base, vec3(0, 0, 1), 30.0f);
                Barb(m, vec3(tip.x, tip.y, tip.z - 0.012f), vec3(0, 0, 1));
            }
            ok &= Write(dir, "hx_body.obj", m, "brazed-plate heat exchanger (generic), 4 front nozzles");
        }
        {   Mesh m;
            BoxMinMax(m, vec3(-0.045f, 0.0f, -0.030f), vec3(0.045f, 0.004f, 0.030f));
            for (float x : { -0.028f, 0.028f }) BoxMinMax(m, vec3(x - 0.006f, 0.004f, -hz), vec3(x + 0.006f, S::HX_BASE, hz));
            ok &= Write(dir, "hx_stand.obj", m, "heat exchanger mounting stand");
        }
        return ok;
    }

    // ================================================================== 6 membrane distillation unit
    bool Membrane(const fs::path& dir) {
        bool ok = true;
        const float cy = S::MD_CENTRE_Y;
        auto shell = [&](float side, const char* file, const char* what, std::initializer_list<vec3> ports, const vec3& probe) {
            Mesh m;
            const float xa = side * 0.010f, xb = side * 0.075f;
            Prism(m, RoundRect(0.055f, 0.045f, 0.008f), std::min(xa, xb), std::max(xa, xb), 0, vec3(0.0f, cy, 0.0f));
            Prism(m, RoundRect(0.066f, 0.056f, 0.006f), std::min(side * 0.002f, side * 0.010f), std::max(side * 0.002f, side * 0.010f), 0, vec3(0.0f, cy, 0.0f));
            for (const vec3& tip : ports) {
                const vec3 dir(side, 0.0f, 0.0f);
                CylAt(m, vec3(xb, tip.y, tip.z), dir, 0.0062f, 0.002f, 28, 0.0004f);
                Barb(m, tip - dir * 0.012f, dir);
            }
            CylAt(m, vec3(probe.x, probe.y - 0.004f, probe.z), vec3(0, 1, 0), 0.0065f, 0.004f, 24, 0.0005f);
            return Write(dir, file, m, what);
        };
        ok &= shell(-1.0f, "md_hot_shell.obj", "MD cell hot-side housing (feed chamber), 2 ports + probe boss", { S::MD_HOT_IN, S::MD_HOT_OUT }, S::MD_PROBE_HOT);
        ok &= shell(1.0f, "md_cold_shell.obj", "MD cell cold-side housing (permeate / vapour chamber), port + probe boss", { S::MD_COLD_OUT }, S::MD_PROBE_COLD);
        {   Mesh m; Prism(m, RoundRect(0.063f, 0.053f, 0.005f), -0.002f, 0.002f, 0, vec3(0.0f, cy, 0.0f));
            ok &= Write(dir, "md_membrane.obj", m, "MD membrane frame (hydrophobic flat sheet + gaskets)"); }
        {   Mesh m;   // 8 through-bolts: heads on the hot flange, nuts on the cold flange
            for (float y : { -0.061f, 0.0f, 0.061f }) for (float z : { -0.051f, 0.0f, 0.051f }) {
                if (y == 0.0f && z == 0.0f) continue;
                const vec3 p(0.0f, cy + y, z);
                const Mesh bolt = HexBolt(0.0040f);
                m.Append(bolt, FromY(vec3(-1, 0, 0)), p + vec3(-0.010f, 0.0f, 0.0f));
                HexAt(m, p + vec3(0.010f, 0.0f, 0.0f), vec3(1, 0, 0), 0.0042f, 0.0030f);
                CylAt(m, p + vec3(0.013f, 0.0f, 0.0f), vec3(1, 0, 0), 0.0018f, 0.0025f, 10, 0.0003f);
            }
            ok &= Write(dir, "md_bolts.obj", m, "MD cell flange bolts and nuts"); }
        {   Mesh m; Prism(m, RoundRect(0.0565f, 0.0465f, 0.0085f), -0.006f, 0.006f, 0, vec3(0.0f));
            ok &= Write(dir, "md_band.obj", m, "MD cell identification band (hot / cold colour)"); }
        {   Mesh m;
            for (float x : { -0.05f, 0.05f }) {
                BoxMinMax(m, vec3(x - 0.012f, 0.0f, -0.048f), vec3(x + 0.012f, 0.003f, 0.048f));
                BoxMinMax(m, vec3(x - 0.005f, 0.003f, -0.040f), vec3(x + 0.005f, cy - 0.055f, 0.040f));
            }
            ok &= Write(dir, "md_cradle.obj", m, "MD cell cradles"); }
        return ok;
    }

    // ================================================================== 7 seawater reservoir
    bool Reservoir(const fs::path& dir) {
        bool ok = true;
        const float R = S::TANK_RADIUS, H = S::TANK_HEIGHT;
        {   Mesh m; Lathe(m, { { 0, 0 }, { R - 0.004f, 0 }, { R, 0.004f }, { R, H } }, 64, 30.0f);
            ok &= Write(dir, "tank_shell.obj", m, "seawater reservoir vessel (clear, open top under the lid)"); }
        {   Mesh m; Cylinder(m, R - 0.0015f, 0.0015f, S::TANK_FILL, 64);
            ok &= Write(dir, "tank_water.obj", m, "seawater volume (visual fill level)"); }
        {   Mesh m;
            Lathe(m, { { 0, H - 0.002f }, { R + 0.0025f, H - 0.002f }, { R + 0.0025f, S::TANK_LID_TOP - 0.001f }, { R + 0.0015f, S::TANK_LID_TOP }, { 0, S::TANK_LID_TOP } }, 64, 30.0f);
            ok &= Write(dir, "tank_lid.obj", m, "reservoir screw lid"); }
        {   Mesh m;
            Barb(m, S::TANK_RETURN - vec3(0.0f, 0.012f, 0.0f), vec3(0, 1, 0));
            HexAt(m, vec3(R - 0.0005f, S::TANK_OUTLET.y, 0.0f), vec3(1, 0, 0), 0.0085f, 0.0035f);
            Barb(m, S::TANK_OUTLET - vec3(0.012f, 0.0f, 0.0f), vec3(1, 0, 0));
            ok &= Write(dir, "tank_fittings.obj", m, "reservoir bulkhead outlet and return barb"); }
        return ok;
    }

    // ================================================================== 8 condensation system
    bool Condensation(const fs::path& dir) {
        bool ok = true;
        const float cy = S::COND_CENTRE_Y, hx = S::COND_HALF.x, hy = S::COND_HALF.y, hz = S::COND_HALF.z, px = S::COND_POST_X;
        {   Mesh m;   // stand: two 15 mm uprights with feet and side brackets
            for (float s : { -1.0f, 1.0f }) {
                BoxMinMax(m, vec3(s * px - 0.0075f, 0.004f, -0.0075f), vec3(s * px + 0.0075f, 0.25f, 0.0075f));
                BoxMinMax(m, vec3(s * px - 0.015f, 0.0f, -0.030f), vec3(s * px + 0.015f, 0.004f, 0.030f));
                const float inner = s * (px - 0.0075f), edge = s * hx;
                BoxMinMax(m, vec3(std::min(inner, edge), cy - 0.030f, -0.015f), vec3(std::max(inner, edge), cy + 0.030f, 0.015f));
            }
            ok &= Write(dir, "cond_stand.obj", m, "condenser stand"); }
        {   Mesh m;   // fin stack (YZ plates along X) + end plates
            for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(std::min(s * hx, s * (hx - 0.002f)), cy - hy, -hz), vec3(std::max(s * hx, s * (hx - 0.002f)), cy + hy, hz));
            const int fins = 26;
            for (int i = 0; i < fins; ++i) {
                const float x = -hx + 0.004f + (2.0f * hx - 0.008f) * float(i) / float(fins - 1);
                BoxMinMax(m, vec3(x - 0.0002f, cy - hy + 0.001f, -hz + 0.001f), vec3(x + 0.0002f, cy + hy - 0.001f, hz - 0.001f));
            }
            ok &= Write(dir, "cond_fins.obj", m, "air-cooled finned condenser block (generic)"); }
        {   Mesh m;   // copper serpentine: return bends outside the end plates, inlet / outlet stubs
            const float rt = 0.0030f, rows[4] = { cy + 0.030f, cy + 0.010f, cy - 0.010f, cy - 0.030f };
            auto uBend = [&](float side, float yA, float yB) {
                std::vector<vec3> path;
                const float mid = 0.5f * (yA + yB), r = 0.5f * std::abs(yA - yB);
                path.push_back(vec3(side * (hx - 0.002f), yA, 0.0f));
                for (int i = 0; i <= 12; ++i) {
                    const float a = PI * float(i) / 12.0f;
                    path.push_back(vec3(side * (hx + 0.002f + r * std::sin(a)), mid + r * std::cos(a) * (yA > yB ? 1.0f : -1.0f), 0.0f));
                }
                path.push_back(vec3(side * (hx - 0.002f), yB, 0.0f));
                Pipe(m, path, rt, 16, true);
            };
            uBend(1.0f, rows[0], rows[1]);
            uBend(-1.0f, rows[1], rows[2]);
            uBend(1.0f, rows[2], rows[3]);
            CylAt(m, vec3(S::COND_IN.x, cy + hy - 0.004f, 0.0f), vec3(0, 1, 0), rt, 0.004f, 16);
            Barb(m, S::COND_IN - vec3(0.0f, 0.012f, 0.0f), vec3(0, 1, 0));
            Barb(m, S::COND_OUT + vec3(0.0f, 0.012f, 0.0f), vec3(0, -1, 0));
            ok &= Write(dir, "cond_coil.obj", m, "condenser copper coil, inlet and condensate outlet"); }
        {   Mesh m;   // wide-mouth collection bottle (clear), open neck under the lid
            const float R = S::BOTTLE_RADIUS;
            Lathe(m, { { 0, 0 }, { R - 0.003f, 0 }, { R, 0.003f }, { R, 0.084f }, { 0.032f, 0.092f }, { 0.032f, 0.100f } }, 64, 30.0f);
            ok &= Write(dir, "bottle_shell.obj", m, "freshwater collection bottle (clear)"); }
        {   Mesh m; Cylinder(m, S::BOTTLE_RADIUS - 0.0015f, 0.0015f, S::BOTTLE_FILL, 64);
            ok &= Write(dir, "bottle_water.obj", m, "collected freshwater (visual fill level)"); }
        {   Mesh m;
            Lathe(m, { { 0, 0.098f }, { 0.034f, 0.098f }, { 0.034f, S::BOTTLE_LID_TOP - 0.001f }, { 0.033f, S::BOTTLE_LID_TOP }, { 0, S::BOTTLE_LID_TOP } }, 48, 30.0f);
            Barb(m, S::BOTTLE_IN - vec3(0.0f, 0.012f, 0.0f), vec3(0, 1, 0));
            ok &= Write(dir, "bottle_lid.obj", m, "collection bottle lid with inlet barb"); }
        return ok;
    }

    // ================================================================== 9 instrumentation
    bool Probe(const fs::path& dir) {
        bool ok = true;
        {   Mesh m;   // compression fitting, body, sheath below the mount face (visible in clear vessels)
            HexAt(m, vec3(0.0f), vec3(0, 1, 0), 0.0055f, 0.006f);
            CylAt(m, vec3(0.0f, 0.006f, 0.0f), vec3(0, 1, 0), 0.0035f, 0.014f, 24, 0.0005f);
            CylAt(m, vec3(0.0f, -0.035f, 0.0f), vec3(0, 1, 0), 0.0020f, 0.035f, 16, 0.0006f);
            ok &= Write(dir, "probe.obj", m, "temperature probe: stainless sheath + compression fitting (generic)"); }
        {   Mesh m;
            LatheAt(m, { { 0, 0 }, { 0.0042f, 0 }, { 0.0042f, 0.008f }, { 0.0020f, 0.010f }, { 0, 0.010f } }, 24, vec3(0.0f, 0.020f, 0.0f), vec3(0, 1, 0), 30.0f);
            ok &= Write(dir, "probe_gland.obj", m, "temperature probe cable gland"); }
        return ok;
    }
}

bool MarinaMeshes(const fs::path& out) {
    const fs::path dir = out / "meshes/marina";
    bool ok = Kit(dir);
    ok &= Structure(dir);
    ok &= Computing(dir);
    ok &= Electronics(dir);
    ok &= Pump(dir);
    ok &= HeatExchanger(dir);
    ok &= Membrane(dir);
    ok &= Reservoir(dir);
    ok &= Condensation(dir);
    ok &= Probe(dir);
    return ok;
}
