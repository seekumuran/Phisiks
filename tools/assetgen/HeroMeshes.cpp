// assetgen: procedural "hero" meshes of the P11 showcase demos (OBJ, CC0, deterministic).
//
//   meshes/hero/arm_*.obj        6-DOF industrial arm: bolted base, turret, cast upper arm and
//                                forearm housings, servo motor drums, 3-axis wrist, tool flange,
//                                parallel gripper (fingers separate), each in its joint frame
//   meshes/hero/turbine_*.obj    axial-centrifugal turbine assembly: bell-mouth and casing flanges,
//                                hex bolt rings, 3 axial rotor stages, 2 stator rings, centrifugal
//                                impeller (main vanes + splitters), volute scroll, turbine wheel,
//                                nozzle ring, shaft, bearing housing, exhaust cone, casing shells
//   meshes/hero/pool_basin.obj   open-top tiled pool basin (inner + outer walls, rim, floor)
//   meshes/hero/cyclorama.obj    studio cyclorama: floor + curved cove + back wall
//   meshes/hero/arrow.obj        coordinate-frame arrow (shaft + cone) along +Y, 1 m long
//
// Files contain positions and faces only ("v" / "f a b c"): the engine's OBJ loader rebuilds
// area-weighted smooth normals per position, and every hard edge here is modelled with separate
// vertices, so smooth groups survive. Every closed part is checked for outward orientation.
#include <glm/glm.hpp>
#include "MeshKit.h"

namespace fs = std::filesystem;

namespace {
    using namespace MeshKit;

    // ================================================================== 6-DOF industrial arm
    // Zero pose (metres, X forward, Y up, Z left): J1 (Y) at (0, 0.30, 0), J2 (Z) at (0.15, 0.65, 0),
    // J3 (Z) at (0.15, 1.35, 0), J4 (X) at (0.47, 1.47, 0), J5 (Z) at the wrist centre (0.95, 1.47, 0),
    // J6 (X) at (1.05, 1.47, 0); the TCP is 0.20 m beyond J6. Every link mesh is expressed in its
    // joint frame (origin at the joint, axes as the world axes in the zero pose).
    bool Arm(const fs::path& dir) {
        bool ok = true;
        const Mesh bolt = HexBolt(0.022f);
        {   // base: chamfered plate, pedestal, flange ring, 8 bolts (static, world frame)
            Mesh m;
            Lathe(m, { { 0, 0 }, { 0.42f, 0 }, { 0.42f, 0.035f }, { 0.40f, 0.055f }, { 0.27f, 0.055f }, { 0.25f, 0.075f }, { 0.25f, 0.24f },
                       { 0.29f, 0.25f }, { 0.29f, 0.29f }, { 0.27f, 0.30f }, { 0, 0.30f } }, 72, 28.0f);
            Mesh bolts;
            RingOf(bolts, bolt, 8, 0.355f, quat(1, 0, 0, 0), PI / 8.0f);
            bolts.Transform(quat(1, 0, 0, 0), vec3(0, 0.055f, 0));
            m.Append(bolts);
            // cable inlet box at the back
            Mesh box;
            RoundedBox(box, { 0.07f, 0.06f, 0.09f }, 0.02f);
            m.Append(box, quat(1, 0, 0, 0), vec3(-0.30f, 0.12f, 0));
            ok &= WriteObj(dir / "arm_base.obj", m, "arm base");
        }
        {   // J1 turret: rotating drum + shoulder housing (orange)
            Mesh m;
            Lathe(m, { { 0, 0 }, { 0.255f, 0 }, { 0.255f, 0.10f }, { 0.235f, 0.12f }, { 0, 0.12f } }, 72, 30.0f);
            Loft(m, { { { 0.02f, 0.10f, 0 }, { 0.20f, 0.20f } }, { { 0.08f, 0.24f, 0 }, { 0.19f, 0.19f } }, { { 0.12f, 0.36f, 0 }, { 0.16f, 0.17f } },
                      { { 0.15f, 0.42f, 0 }, { 0.13f, 0.16f } } }, vec3(1, 0, 0), 3.2f, 48);
            ok &= WriteObj(dir / "arm_turret.obj", m, "arm J1 turret");
        }
        {   // J2 motor drum (black): along Z on the turret's left side, cooling ribs
            Mesh m;
            std::vector<vec2> prof = { { 0, 0 }, { 0.10f, 0 } };
            for (int r = 0; r < 6; ++r) {
                const float y = 0.02f + r * 0.022f;
                prof.push_back({ 0.10f, y }); prof.push_back({ 0.112f, y + 0.004f }); prof.push_back({ 0.112f, y + 0.012f }); prof.push_back({ 0.10f, y + 0.016f });
            }
            prof.push_back({ 0.10f, 0.16f }); prof.push_back({ 0.085f, 0.175f }); prof.push_back({ 0.04f, 0.175f }); prof.push_back({ 0.03f, 0.19f }); prof.push_back({ 0, 0.19f });
            Lathe(m, prof, 48, 30.0f);
            m.Transform(Axis(vec3(1, 0, 0), PI * 0.5f) * quat(1, 0, 0, 0), vec3(0, 0, 0));   // +Y -> +Z
            ok &= WriteObj(dir / "arm_motor_large.obj", m, "arm servo motor (large)");
        }
        {   // upper arm: cast housing from J2 (origin) up to J3 (0, 0.70, 0), bosses at both ends
            Mesh m;
            Loft(m, { { { 0, -0.02f, 0 }, { 0.15f, 0.13f } }, { { 0, 0.12f, 0 }, { 0.135f, 0.12f } }, { { 0.01f, 0.35f, 0 }, { 0.115f, 0.105f } },
                      { { 0, 0.58f, 0 }, { 0.10f, 0.10f } }, { { 0, 0.72f, 0 }, { 0.10f, 0.095f } } }, vec3(1, 0, 0), 3.0f, 48);
            Mesh boss;
            Cylinder(boss, 0.16f, -0.13f, 0.13f, 56, 0.015f);
            m.Append(boss, Axis(vec3(1, 0, 0), PI * 0.5f));
            Mesh boss2;
            Cylinder(boss2, 0.125f, -0.115f, 0.115f, 56, 0.012f);
            m.Append(boss2, Axis(vec3(1, 0, 0), PI * 0.5f), vec3(0, 0.70f, 0));
            // stiffening rib on the front face
            Mesh rib;
            RoundedBox(rib, { 0.02f, 0.25f, 0.05f }, 0.012f);
            m.Append(rib, quat(1, 0, 0, 0), vec3(0.11f, 0.35f, 0));
            ok &= WriteObj(dir / "arm_upper.obj", m, "arm upper arm housing");
        }
        {   // forearm: elbow boss + housing along +X at y = 0.12 to the J4 joint (0.32, 0.12, 0)
            Mesh m;
            Mesh boss;
            Cylinder(boss, 0.13f, -0.105f, 0.105f, 56, 0.012f);
            m.Append(boss, Axis(vec3(1, 0, 0), PI * 0.5f));
            Loft(m, { { { -0.20f, 0.10f, 0 }, { 0.10f, 0.11f } }, { { -0.06f, 0.11f, 0 }, { 0.125f, 0.13f } }, { { 0.10f, 0.12f, 0 }, { 0.105f, 0.11f } },
                      { { 0.33f, 0.12f, 0 }, { 0.095f, 0.095f } } }, vec3(0, 1, 0), 3.0f, 48);
            ok &= WriteObj(dir / "arm_forearm.obj", m, "arm forearm housing");
        }
        {   // small motor drum (J3 / J4 / J5 servo), along +Y, 0.12 long
            Mesh m;
            std::vector<vec2> prof = { { 0, 0 }, { 0.075f, 0 } };
            for (int r = 0; r < 4; ++r) {
                const float y = 0.015f + r * 0.022f;
                prof.push_back({ 0.075f, y }); prof.push_back({ 0.084f, y + 0.003f }); prof.push_back({ 0.084f, y + 0.011f }); prof.push_back({ 0.075f, y + 0.014f });
            }
            prof.push_back({ 0.075f, 0.11f }); prof.push_back({ 0.06f, 0.125f }); prof.push_back({ 0, 0.125f });
            Lathe(m, prof, 40, 30.0f);
            ok &= WriteObj(dir / "arm_motor_small.obj", m, "arm servo motor (small)");
        }
        {   // J4 wrist roll housing along +X: tapered drum, then the J5 yoke cheeks around the wrist centre (0.48, 0, 0)
            Mesh m;
            Mesh drum;
            Lathe(drum, { { 0, 0 }, { 0.095f, 0 }, { 0.095f, 0.03f }, { 0.088f, 0.04f }, { 0.08f, 0.33f }, { 0.07f, 0.36f }, { 0, 0.36f } }, 56, 25.0f);
            m.Append(drum, Axis(vec3(0, 0, 1), -PI * 0.5f));   // +Y -> +X
            for (int side = -1; side <= 1; side += 2) {
                Mesh cheek;
                Loft(cheek, { { { 0.33f, 0, side * 0.083f }, { 0.068f, 0.012f } }, { { 0.46f, 0, side * 0.083f }, { 0.062f, 0.012f } }, { { 0.53f, 0, side * 0.083f }, { 0.045f, 0.012f } } },
                     vec3(0, 0, 1), 2.6f, 32);
                m.Append(cheek);
            }
            ok &= WriteObj(dir / "arm_wrist_roll.obj", m, "arm J4 wrist roll + J5 yoke");
        }
        {   // J5 wrist pitch body: hub along Z between the cheeks + nose along +X to the flange (0.10, 0, 0)
            Mesh m;
            Mesh hub;
            Cylinder(hub, 0.055f, -0.068f, 0.068f, 40, 0.008f);
            m.Append(hub, Axis(vec3(1, 0, 0), PI * 0.5f));
            Mesh nose;
            Lathe(nose, { { 0, 0 }, { 0.05f, 0 }, { 0.05f, 0.07f }, { 0.042f, 0.085f }, { 0.042f, 0.10f }, { 0, 0.10f } }, 40, 25.0f);
            m.Append(nose, Axis(vec3(0, 0, 1), -PI * 0.5f));
            ok &= WriteObj(dir / "arm_wrist_pitch.obj", m, "arm J5 wrist pitch");
        }
        {   // J6 tool flange + gripper body along +X (flange face at x = 0)
            Mesh m;
            Mesh flange;
            Lathe(flange, { { 0, 0 }, { 0.063f, 0 }, { 0.063f, 0.018f }, { 0.058f, 0.024f }, { 0, 0.024f } }, 48, 25.0f);
            Mesh bolts;
            RingOf(bolts, HexBolt(0.007f), 6, 0.048f, quat(1, 0, 0, 0));
            bolts.Transform(quat(1, 0, 0, 0), vec3(0, 0.024f, 0));
            flange.Append(bolts);
            m.Append(flange, Axis(vec3(0, 0, 1), -PI * 0.5f));
            Mesh body;
            RoundedBox(body, { 0.04f, 0.075f, 0.038f }, 0.012f);
            m.Append(body, quat(1, 0, 0, 0), vec3(0.07f, 0, 0));
            Mesh rail;
            RoundedBox(rail, { 0.012f, 0.085f, 0.022f }, 0.005f);
            m.Append(rail, quat(1, 0, 0, 0), vec3(0.118f, 0, 0));
            ok &= WriteObj(dir / "arm_tool.obj", m, "arm J6 flange + gripper body");
        }
        {   // gripper finger (one; the other is mirrored at runtime): along +X from the rail
            Mesh m;
            RoundedBox(m, { 0.045f, 0.009f, 0.02f }, 0.005f);
            m.Transform(quat(1, 0, 0, 0), vec3(0.045f, 0, 0));
            Mesh pad;
            RoundedBox(pad, { 0.03f, 0.004f, 0.017f }, 0.003f);
            m.Append(pad, quat(1, 0, 0, 0), vec3(0.06f, -0.012f, 0));
            ok &= WriteObj(dir / "arm_finger.obj", m, "arm gripper finger");
        }
        {   // cable harness segment (generic flexible conduit along +Y, 1 m, ribbed)
            Mesh m;
            std::vector<vec3> path;
            for (int i = 0; i <= 40; ++i) path.push_back(vec3(0, float(i) / 40.0f, 0));
            Sweep(m, path, [](float s) { return 0.5f + 0.06f * std::cos(s * 40.0f * PI); }, 12);
            ok &= WriteObj(dir / "arm_conduit.obj", m, "arm cable conduit (unit length, unit radius 0.5)");
        }
        return ok;
    }

    // ================================================================== turbine assembly (axis +X)
    // Every part is centred on the axis; x positions are the assembled layout (see the demo).
    bool Turbine(const fs::path& dir) {
        bool ok = true;
        const quat yToX = Axis(vec3(0, 0, 1), -PI * 0.5f);
        auto latheX = [&](Mesh& m, const std::vector<vec2>& profile, int segs, float hard = 30.0f) {   // profile (r, x)
            Mesh l;
            Lathe(l, profile, segs, hard);
            m.Append(l, yToX);
        };
        auto shellX = [&](Mesh& m, const std::vector<vec2>& outer, float thickness, int segs) {   // open shell (r, x) with wall thickness
            std::vector<vec2> prof;
            for (const vec2& p : outer) prof.push_back(p);
            for (auto it = outer.rbegin(); it != outer.rend(); ++it) prof.push_back({ it->x - thickness, it->y });
            prof.push_back(outer.front());
            Mesh l;
            Lathe(l, prof, segs, 40.0f);
            m.Append(l, yToX);
        };
        const Mesh bolt = HexBolt(0.028f);
        auto boltRingX = [&](Mesh& m, int count, float radius, float x, float dir) {   // bolt heads facing +dir X
            Mesh ring;
            RingOf(ring, bolt, count, radius, quat(1, 0, 0, 0), PI / float(count));
            m.Append(ring, dir > 0 ? yToX : Axis(vec3(0, 0, 1), PI * 0.5f), vec3(x, 0, 0));
        };
        {   // intake bell-mouth with front flange (steel) at x = -1.30
            Mesh m;
            shellX(m, { { 0.62f, -1.36f }, { 0.62f, -1.30f }, { 0.52f, -1.30f }, { 0.47f, -1.24f }, { 0.45f, -1.12f }, { 0.445f, -1.05f } }, 0.03f, 96);
            ok &= WriteObj(dir / "turbine_intake.obj", m, "turbine intake bell-mouth + flange");
            Mesh bolts;
            boltRingX(bolts, 20, 0.575f, -1.30f, 1.0f);
            ok &= WriteObj(dir / "turbine_bolts_intake.obj", bolts, "turbine intake flange bolts");
        }
        {   // compressor casing shell (ghosted) with flanges at both ends, x -1.05 .. -0.22
            Mesh m;
            shellX(m, { { 0.56f, -1.08f }, { 0.56f, -1.03f }, { 0.47f, -1.03f }, { 0.465f, -0.70f }, { 0.455f, -0.30f }, { 0.60f, -0.28f }, { 0.60f, -0.22f } }, 0.02f, 96);
            ok &= WriteObj(dir / "turbine_casing_front.obj", m, "turbine compressor casing (ghosted shell)");
            Mesh bolts;
            boltRingX(bolts, 20, 0.52f, -1.03f, -1.0f);
            boltRingX(bolts, 24, 0.56f, -0.22f, 1.0f);
            ok &= WriteObj(dir / "turbine_bolts_casing.obj", bolts, "turbine casing flange bolts");
        }
        {   // turbine casing shell (ghosted), x 0.40 .. 1.10
            Mesh m;
            shellX(m, { { 0.58f, 0.40f }, { 0.58f, 0.46f }, { 0.46f, 0.47f }, { 0.43f, 0.70f }, { 0.40f, 1.00f }, { 0.50f, 1.02f }, { 0.50f, 1.08f } }, 0.02f, 96);
            ok &= WriteObj(dir / "turbine_casing_rear.obj", m, "turbine casing (ghosted shell)");
            Mesh bolts;
            boltRingX(bolts, 24, 0.54f, 0.46f, -1.0f);
            boltRingX(bolts, 18, 0.465f, 1.02f, 1.0f);
            ok &= WriteObj(dir / "turbine_bolts_rear.obj", bolts, "turbine rear flange bolts");
        }
        {   // shaft + front spinner (steel), x -1.20 .. 1.05
            Mesh m;
            latheX(m, { { 0, -1.22f }, { 0.04f, -1.21f }, { 0.08f, -1.16f }, { 0.12f, -1.07f }, { 0.135f, -1.0f }, { 0.07f, -0.98f }, { 0.07f, 0.95f }, { 0.05f, 1.05f }, { 0, 1.05f } }, 48, 30.0f);
            ok &= WriteObj(dir / "turbine_shaft.obj", m, "turbine shaft + spinner");
        }
        // axial compressor rotor stages: disk + blades (stage s at x_s, blades tip radius decreasing)
        const float stageX[3] = { -0.90f, -0.66f, -0.44f };
        const float stageTip[3] = { 0.44f, 0.425f, 0.41f };
        const float stageHub[3] = { 0.20f, 0.23f, 0.26f };
        const int stageBlades[3] = { 24, 28, 32 };
        for (int st = 0; st < 3; ++st) {
            Mesh m;
            const float hub = stageHub[st];
            latheX(m, { { 0, -0.05f }, { hub, -0.05f }, { hub, 0.05f }, { 0, 0.05f } }, 96, 30.0f);
            const Mesh blade = Blade(hub - 0.01f, stageTip[st], [&](float s) { return 0.085f - 0.025f * s; }, [](float s) { return 0.35f + 0.55f * s; }, 0.10f, 0.06f);
            Mesh ring;
            for (int b = 0; b < stageBlades[st]; ++b) {
                const float a = float(b) / float(stageBlades[st]) * 2.0f * PI;
                ring.Append(blade, Axis(vec3(1, 0, 0), a));   // chord along the axis X (twist = stagger)
            }
            m.Append(ring);
            m.Transform(quat(1, 0, 0, 0), vec3(stageX[st], 0, 0));
            char name[48];
            std::snprintf(name, sizeof(name), "turbine_rotor_%d.obj", st + 1);
            ok &= WriteObj(dir / name, m, "turbine axial compressor rotor stage");
        }
        // stator rings between the rotor stages: outer band + inner band + vanes (fixed)
        const float statorX[2] = { -0.78f, -0.55f };
        for (int st = 0; st < 2; ++st) {
            Mesh m;
            const float x = statorX[st];
            const float inner = 0.22f + 0.03f * st, outer = 0.452f;
            latheX(m, { { outer, x - 0.04f }, { outer + 0.022f, x - 0.04f }, { outer + 0.022f, x + 0.04f }, { outer, x + 0.04f }, { outer, x - 0.04f } }, 96, 30.0f);
            latheX(m, { { inner - 0.02f, x - 0.035f }, { inner, x - 0.035f }, { inner, x + 0.035f }, { inner - 0.02f, x + 0.035f }, { inner - 0.02f, x - 0.035f } }, 96, 30.0f);
            ok &= WriteObj(dir / (st == 0 ? "turbine_stator_1_rings.obj" : "turbine_stator_2_rings.obj"), m, "turbine stator rings");
            Mesh vanes;
            const Mesh vane = Blade(inner - 0.005f, outer + 0.005f, [](float) { return 0.06f; }, [](float s) { return -0.45f - 0.15f * s; }, 0.08f, 0.04f, 10, 5);
            const int count = 30 + 6 * st;
            for (int b = 0; b < count; ++b) vanes.Append(vane, Axis(vec3(1, 0, 0), (float(b) + 0.5f) / float(count) * 2.0f * PI));
            vanes.Transform(quat(1, 0, 0, 0), vec3(x, 0, 0));
            ok &= WriteObj(dir / (st == 0 ? "turbine_stator_1_vanes.obj" : "turbine_stator_2_vanes.obj"), vanes, "turbine stator vanes");
        }
        {   // centrifugal impeller (gold): hub lathe x -0.22 .. 0.12 + 12 main vanes + 12 splitters with backsweep
            Mesh m;
            latheX(m, { { 0, -0.24f }, { 0.12f, -0.24f }, { 0.14f, -0.20f }, { 0.17f, -0.10f }, { 0.24f, 0.00f }, { 0.36f, 0.06f }, { 0.39f, 0.08f }, { 0.39f, 0.10f }, { 0.16f, 0.12f }, { 0, 0.12f } }, 96, 35.0f);
            auto vaneMesh = [&](float sStart, float span0) {
                // meridional path on the hub: inlet (axial) -> outlet (radial)
                Mesh v;
                const int n = 20;
                std::vector<vec3> pts;
                const float thick = 0.006f;
                for (int i = 0; i < n; ++i) {
                    const float s = sStart + (1.0f - sStart) * float(i) / float(n - 1);
                    const float xh = -0.22f + 0.30f * s;                      // hub x
                    const float rh = 0.13f + 0.25f * s * s;                   // hub radius
                    const float height = span0 * (1.0f - 0.72f * s);          // blade height (normal to the hub)
                    const float wrap = 0.55f * s * s;                         // backsweep (radians)
                    // hub normal in the (x, r) plane
                    const float dx = 0.30f, dr = 0.50f * s * 0.25f / 0.25f;
                    vec2 nrm = glm::normalize(vec2(-dr, dx));
                    for (int k = 0; k < 4; ++k) {
                        const float h = (k == 0 || k == 3) ? 0.0f : height;
                        const float side = (k < 2) ? -1.0f : 1.0f;
                        const float x = xh + nrm.x * h, r = rh + nrm.y * h;
                        const float a = wrap + side * thick / std::max(r, 0.05f);
                        pts.push_back(vec3(x, std::cos(a) * r, std::sin(a) * r));
                    }
                }
                // rows = n, cols = 4 (closed loop: base-, tip-, tip+, base+)
                Grid(v, pts, n, 4, true);
                CapRing(v, std::vector<vec3>(pts.begin(), pts.begin() + 4), (pts[0] + pts[2]) * 0.5f);
                CapRing(v, std::vector<vec3>(pts.end() - 4, pts.end()), (pts[pts.size() - 4] + pts[pts.size() - 2]) * 0.5f);
                v.Orient(0);
                return v;
            };
            const Mesh mainVane = vaneMesh(0.0f, 0.15f), splitter = vaneMesh(0.4f, 0.15f);
            for (int b = 0; b < 12; ++b) {
                const float a = float(b) / 12.0f * 2.0f * PI;
                m.Append(mainVane, Axis(vec3(1, 0, 0), a));
                m.Append(splitter, Axis(vec3(1, 0, 0), a + PI / 12.0f));
            }
            ok &= WriteObj(dir / "turbine_impeller.obj", m, "turbine centrifugal impeller");
        }
        {   // diffuser / volute scroll around the impeller (x ~ 0.0), spiral tube growing, tangential outlet
            Mesh m;
            std::vector<vec3> path;
            const int n = 96;
            for (int i = 0; i <= n; ++i) {
                const float t = float(i) / float(n);
                const float a = t * 2.0f * PI * 0.97f;
                const float r = 0.50f + 0.09f * t;
                path.push_back(vec3(0.02f, std::cos(a) * r, std::sin(a) * r));
            }
            // tangential outlet pipe
            const vec3 end = path.back();
            const vec3 tangent = glm::normalize(path.back() - path[path.size() - 2]);
            for (int i = 1; i <= 10; ++i) path.push_back(end + tangent * (0.05f * float(i)));
            const float total = float(path.size() - 1);
            Sweep(m, path, [&](float s) { const float t = std::min(s * total / float(n), 1.0f); return 0.06f + 0.07f * t; }, 28);
            // outlet flange
            Mesh fl;
            Lathe(fl, { { 0.13f, 0 }, { 0.19f, 0 }, { 0.19f, 0.03f }, { 0.13f, 0.03f }, { 0.13f, 0 } }, 48, 30.0f);
            m.Append(fl, FromY(tangent), path.back() - tangent * 0.03f);
            // diffuser plate joining scroll and casing
            latheX(m, { { 0.40f, -0.02f }, { 0.50f, -0.02f }, { 0.50f, 0.06f }, { 0.40f, 0.06f }, { 0.40f, -0.02f } }, 96, 30.0f);
            ok &= WriteObj(dir / "turbine_volute.obj", m, "turbine volute scroll + diffuser");
        }
        {   // bearing housing between compressor and turbine (steel), x 0.14 .. 0.40
            Mesh m;
            latheX(m, { { 0.075f, 0.13f }, { 0.30f, 0.13f }, { 0.30f, 0.16f }, { 0.22f, 0.19f }, { 0.20f, 0.36f }, { 0.30f, 0.38f }, { 0.30f, 0.41f }, { 0.075f, 0.41f }, { 0.075f, 0.13f } }, 72, 35.0f);
            Mesh ribs;
            Mesh rib;
            RoundedBox(rib, { 0.08f, 0.012f, 0.012f }, 0.004f);
            for (int i = 0; i < 8; ++i) ribs.Append(rib, Axis(vec3(1, 0, 0), float(i) / 8.0f * 2.0f * PI), Axis(vec3(1, 0, 0), float(i) / 8.0f * 2.0f * PI) * vec3(0.275f, 0.22f, 0));
            m.Append(ribs);
            ok &= WriteObj(dir / "turbine_bearing.obj", m, "turbine bearing housing");
        }
        {   // nozzle guide vane ring (fixed) at x 0.52 + turbine wheel (rotating) at x 0.66
            Mesh m;
            latheX(m, { { 0.40f, 0.49f }, { 0.43f, 0.49f }, { 0.43f, 0.55f }, { 0.40f, 0.55f }, { 0.40f, 0.49f } }, 96, 30.0f);
            latheX(m, { { 0.17f, 0.495f }, { 0.20f, 0.495f }, { 0.20f, 0.545f }, { 0.17f, 0.545f }, { 0.17f, 0.495f } }, 72, 30.0f);
            const Mesh vane = Blade(0.195f, 0.405f, [](float) { return 0.07f; }, [](float s) { return 0.9f + 0.1f * s; }, 0.12f, 0.12f, 10, 5);
            for (int b = 0; b < 26; ++b) m.Append(vane, Axis(vec3(1, 0, 0), float(b) / 26.0f * 2.0f * PI), vec3(0.52f, 0, 0));
            ok &= WriteObj(dir / "turbine_nozzle.obj", m, "turbine nozzle guide vanes");
            Mesh w;
            latheX(w, { { 0, 0.60f }, { 0.22f, 0.60f }, { 0.22f, 0.72f }, { 0, 0.72f } }, 96, 30.0f);
            const Mesh blade = Blade(0.21f, 0.415f, [](float s) { return 0.09f - 0.02f * s; }, [](float s) { return -0.9f - 0.3f * s; }, 0.13f, 0.14f);
            for (int b = 0; b < 34; ++b) w.Append(blade, Axis(vec3(1, 0, 0), float(b) / 34.0f * 2.0f * PI), vec3(0.66f, 0, 0));
            ok &= WriteObj(dir / "turbine_wheel.obj", w, "turbine wheel");
        }
        {   // exhaust cone (steel) x 0.74 .. 1.15
            Mesh m;
            latheX(m, { { 0, 0.74f }, { 0.21f, 0.74f }, { 0.20f, 0.85f }, { 0.15f, 1.00f }, { 0.07f, 1.12f }, { 0, 1.15f } }, 72, 30.0f);
            ok &= WriteObj(dir / "turbine_exhaust_cone.obj", m, "turbine exhaust cone");
        }
        {   // support cradle (static, dark): two saddles + base rail
            Mesh m;
            for (float x : { -0.85f, 0.75f }) {
                Mesh saddle;
                Loft(saddle, { { { x, -0.62f, 0 }, { 0.05f, 0.42f } }, { { x, -0.50f, 0 }, { 0.05f, 0.40f } } }, vec3(1, 0, 0), 6.0f, 32);
                m.Append(saddle);
            }
            Mesh rail;
            RoundedBox(rail, { 1.1f, 0.04f, 0.40f }, 0.02f);
            m.Append(rail, quat(1, 0, 0, 0), vec3(-0.05f, -0.66f, 0));
            ok &= WriteObj(dir / "turbine_cradle.obj", m, "turbine support cradle");
        }
        return ok;
    }

    // ================================================================== pool basin + studio
    bool Studio(const fs::path& dir) {
        bool ok = true;
        {   // pool basin: inner size 4 x 4 m, 1.2 m deep, walls 0.2 m, rim at y = 0 (open top)
            Mesh m;
            const float hx = 2.0f, hz = 2.0f, depth = 1.2f, wall = 0.2f;
            // inner walls + floor as an inward-facing box (normals into the pool), outer walls outward, rim on top
            // quad a-b-c-d subdivided; triangles are flipped to face `normal`
            vec3 normal(0.0f);
            auto quad = [&](vec3 a, vec3 b, vec3 c, vec3 d, int divU, int divV) {
                std::vector<vec3> pts;
                for (int j = 0; j <= divV; ++j) for (int i = 0; i <= divU; ++i) {
                    const float u = float(i) / divU, v = float(j) / divV;
                    pts.push_back(glm::mix(glm::mix(a, b, u), glm::mix(d, c, u), v));
                }
                const size_t before = m.idx.size();
                Grid(m, pts, divV + 1, divU + 1, false);
                const vec3 n = glm::cross(m.p[m.idx[before + 1]] - m.p[m.idx[before]], m.p[m.idx[before + 2]] - m.p[m.idx[before]]);
                if (glm::dot(n, normal) < 0.0f) m.FlipFrom(before);
            };
            // floor (faces up): order so the CCW normal is +Y
            normal = { 0, 1, 0 };
            quad({ -hx, -depth, hz }, { hx, -depth, hz }, { hx, -depth, -hz }, { -hx, -depth, -hz }, 8, 8);
            // inner walls (face the pool centre)
            normal = { 0, 0, 1 };  quad({ -hx, -depth, -hz }, { hx, -depth, -hz }, { hx, 0, -hz }, { -hx, 0, -hz }, 8, 3);    // back wall
            normal = { 0, 0, -1 }; quad({ hx, -depth, hz }, { -hx, -depth, hz }, { -hx, 0, hz }, { hx, 0, hz }, 8, 3);        // front wall
            normal = { 1, 0, 0 };  quad({ -hx, -depth, hz }, { -hx, -depth, -hz }, { -hx, 0, -hz }, { -hx, 0, hz }, 8, 3);    // left wall
            normal = { -1, 0, 0 }; quad({ hx, -depth, -hz }, { hx, -depth, hz }, { hx, 0, hz }, { hx, 0, -hz }, 8, 3);        // right wall
            // rim (top, faces +Y): four strips
            const float ox = hx + wall, oz = hz + wall;
            normal = { 0, 1, 0 };
            quad({ -ox, 0, -oz }, { -ox, 0, oz }, { -hx, 0, hz }, { -hx, 0, -hz }, 1, 1);
            quad({ hx, 0, -hz }, { hx, 0, hz }, { ox, 0, oz }, { ox, 0, -oz }, 1, 1);
            quad({ -hx, 0, -hz }, { hx, 0, -hz }, { ox, 0, -oz }, { -ox, 0, -oz }, 1, 1);
            quad({ -ox, 0, oz }, { ox, 0, oz }, { hx, 0, hz }, { -hx, 0, hz }, 1, 1);
            // outer walls (face outwards), down to the studio floor at y = -depth - 0.25 (plinth)
            const float bottom = -depth - 0.25f;
            normal = { 0, 0, 1 };  quad({ -ox, bottom, oz }, { ox, bottom, oz }, { ox, 0, oz }, { -ox, 0, oz }, 8, 3);
            normal = { 0, 0, -1 }; quad({ ox, bottom, -oz }, { -ox, bottom, -oz }, { -ox, 0, -oz }, { ox, 0, -oz }, 8, 3);
            normal = { 1, 0, 0 };  quad({ ox, bottom, oz }, { ox, bottom, -oz }, { ox, 0, -oz }, { ox, 0, oz }, 8, 3);
            normal = { -1, 0, 0 }; quad({ -ox, bottom, -oz }, { -ox, bottom, oz }, { -ox, 0, oz }, { -ox, 0, -oz }, 8, 3);
            ok &= WriteObj(dir / "pool_basin.obj", m, "tiled pool basin (open top)");
        }
        {   // cyclorama: 40 m wide floor (z from +20 to -8), quarter-circle cove (r 4 m) into a 10 m back wall
            Mesh m;
            const int cols = 2;
            std::vector<vec3> pts;
            std::vector<vec2> prof;   // (z, y) along the cyc from the front edge
            prof.push_back({ 20.0f, 0.0f });
            prof.push_back({ 6.0f, 0.0f });
            prof.push_back({ -4.0f, 0.0f });
            const float R = 4.0f;
            for (int i = 1; i <= 16; ++i) {
                const float a = float(i) / 16.0f * PI * 0.5f;
                prof.push_back({ -4.0f - std::sin(a) * R, R - std::cos(a) * R });
            }
            prof.push_back({ -8.0f, 12.0f });
            for (const vec2& z : prof) for (int c = 0; c < cols; ++c) pts.push_back(vec3(c == 0 ? -20.0f : 20.0f, z.y, z.x));
            Grid(m, pts, int(prof.size()), cols, false);
            // the floor must face +Y: check the first triangle
            const vec3 n = glm::cross(m.p[m.idx[1]] - m.p[m.idx[0]], m.p[m.idx[2]] - m.p[m.idx[0]]);
            if (n.y < 0.0f) m.FlipFrom(0);
            ok &= WriteObj(dir / "cyclorama.obj", m, "studio cyclorama (floor + cove + back wall)");
        }
        {   // coordinate-frame arrow along +Y: shaft r 0.025 to 0.78, cone r 0.07 to 1.0
            Mesh m;
            Lathe(m, { { 0, 0 }, { 0.025f, 0 }, { 0.025f, 0.78f }, { 0.07f, 0.78f }, { 0, 1.0f } }, 20, 25.0f);
            ok &= WriteObj(dir / "arrow.obj", m, "coordinate-frame arrow");
        }
        return ok;
    }
}

bool HeroMeshes(const fs::path& out) {
    const fs::path dir = out / "meshes/hero";
    bool ok = Arm(dir);
    ok &= Turbine(dir);
    ok &= Studio(dir);
    return ok;
}
