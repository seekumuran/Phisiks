// assetgen: Old-McDonald agricultural-robotics assets (OBJ, CC0, deterministic).
//
//   meshes/oldmcdonald/*.obj   greenhouse (slab, aluminium frame, wall / roof glazing), raised beds,
//                              two leafy-crop variants, crop-location rings, sampling-point disc +
//                              stake, dock bay and drone-pad markings; mapping drone (F450-class
//                              frame plates / arms / landing gear, brushless motor, 10 inch prop,
//                              ESC, Pixhawk 2.4.8-size flight-controller case on a damping mount,
//                              GPS puck on a mast, LiPo pack); soil rover styled after the prototype
//                              photo (black box chassis, black top plate, standoffs, yellow fenders +
//                              front panel, block-tread tyres, Y-spoke rims, hubs, gearmotors, arm
//                              mounting platform, Raspberry Pi Pico board, motor driver, battery
//                              pack, 2D LiDAR base / spinning head, servo-and-bracket arm: servos,
//                              horns, U brackets, aluminium-tube forearm, wrist bracket, gripper,
//                              sensor head, soil moisture / temperature / pH probes).
//
// Placements and dimensions: apps/lab/src/OldMcDonaldSpec.h (shared with the scene). Parts are
// authored in their assembly frame (or joint frame for the arm and spinning parts, as noted).
// Generic engineering models: no manufacturer CAD data or artwork is reproduced.
#include "MeshKit.h"
#include "OldMcDonaldSpec.h"

namespace fs = std::filesystem;

namespace {
    using namespace MeshKit;
    namespace S = OldMcDonald::Spec;

    bool Write(const fs::path& dir, const char* file, const Mesh& m, const char* what) { return WriteObj(dir / file, m, what); }

    // Box of half size `half` (local Y along `dir`) centred at `centre`.
    void BoxAlong(Mesh& m, const vec3& centre, const vec3& dir, const vec3& half) {
        Mesh t;
        Box(t, vec3(0.0f), half);
        m.Append(t, FromY(dir), centre);
    }
    // Straight round rod between two points.
    void Rod(Mesh& m, const vec3& a, const vec3& b, float r, int sides = 12) { Pipe(m, { a, b }, r, sides, true); }

    uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
    float Rand01(uint32_t seed) { return float(Hash(seed) & 0xFFFFFF) / float(0xFFFFFF); }

    // ================================================================== greenhouse
    bool Greenhouse(const fs::path& dir) {
        bool ok = true;
        const float hx = S::GH_HALF_X, hz = S::GH_HALF_Z, eave = S::GH_EAVE, ridge = S::GH_RIDGE, p = 0.5f * S::GH_POST;
        {   Mesh m;
            BoxMinMax(m, vec3(-hx - S::SLAB_MARGIN, -0.12f, -hz - S::SLAB_MARGIN), vec3(hx + S::SLAB_MARGIN, 0.0f, hz + S::SLAB_MARGIN));
            ok &= Write(dir, "gh_slab.obj", m, "greenhouse concrete slab"); }
        {   Mesh m;   // aluminium frame
            for (float x = -hx; x <= hx + 1e-4f; x += S::GH_BAY) {
                for (float z : { -hz, hz }) BoxMinMax(m, vec3(x - p, 0.0f, z - p), vec3(x + p, eave, z + p));   // posts
                const float slope = std::sqrt((ridge - eave) * (ridge - eave) + hz * hz);
                for (float s : { -1.0f, 1.0f }) {   // rafters
                    const vec3 a(x, eave, s * hz), b(x, ridge, 0.0f);
                    BoxAlong(m, (a + b) * 0.5f, b - a, vec3(0.022f, slope * 0.5f, 0.035f));
                }
            }
            for (float z : { -hz, hz }) {
                BoxMinMax(m, vec3(-hx - p, eave - 0.07f, z - p), vec3(hx + p, eave, z + p));        // eave beams
                BoxMinMax(m, vec3(-hx - p, 0.0f, z - p), vec3(hx + p, 0.08f, z + p));               // sill rails
                const float zm = z * 0.5f, ym = 0.5f * (eave + ridge);                               // purlins
                BoxMinMax(m, vec3(-hx, ym - 0.02f, zm - 0.02f), vec3(hx, ym + 0.02f, zm + 0.02f));
            }
            BoxMinMax(m, vec3(-hx - p, ridge - 0.05f, -0.03f), vec3(hx + p, ridge + 0.03f, 0.03f));      // ridge beam
            for (float x : { -hx, hx }) {
                BoxMinMax(m, vec3(x - p, 0.0f, -hz), vec3(x + p, 0.08f, hz));                         // end sills
                for (float z : { -1.0f, 1.0f }) BoxMinMax(m, vec3(x - p, 0.0f, z - p), vec3(x + p, eave + (ridge - eave) * 0.5f, z + p));
            }
            // west door frame (rover / drone pass in front of it), east centre stud
            for (float z : { -0.5f, 0.5f }) BoxMinMax(m, vec3(-hx - p, 0.0f, z - p), vec3(-hx + p, 2.0f, z + p));
            BoxMinMax(m, vec3(-hx - p, 2.0f, -0.5f - p), vec3(-hx + p, 2.06f, 0.5f + p));
            BoxMinMax(m, vec3(hx - p, 0.0f, -p), vec3(hx + p, ridge, p));
            ok &= Write(dir, "gh_frame.obj", m, "greenhouse aluminium frame (posts, rafters, ridge, purlins, door)");
        }
        {   Mesh m;   // wall glazing: two side walls + two gables
            for (float z : { -hz, hz }) BoxMinMax(m, vec3(-hx, 0.08f, z - 0.002f), vec3(hx, eave - 0.07f, z + 0.002f));
            const float yc = 1.4f;
            const std::vector<vec2> gable = { { 0.08f - yc, -hz }, { 0.08f - yc, hz }, { eave - yc, hz }, { ridge - yc, 0.0f }, { eave - yc, -hz } };
            for (float x : { -hx, hx }) Prism(m, gable, x - 0.002f, x + 0.002f, 0, vec3(0.0f, yc, 0.0f));
            ok &= Write(dir, "gh_glass_walls.obj", m, "greenhouse wall glazing");
        }
        {   Mesh m;   // roof glazing: two slopes
            const float slope = std::sqrt((ridge - eave) * (ridge - eave) + hz * hz);
            for (float s : { -1.0f, 1.0f }) {
                const vec3 a(0.0f, eave + 0.04f, s * hz), b(0.0f, ridge + 0.04f, 0.0f);
                BoxAlong(m, (a + b) * 0.5f, b - a, vec3(hx, slope * 0.5f, 0.002f));
            }
            ok &= Write(dir, "gh_glass_roof.obj", m, "greenhouse roof glazing");
        }
        {   Mesh m;   // dock bay: painted outline + arrow (local origin: bay centre)
            const float x = S::DOCK_HALF.x, z = S::DOCK_HALF.z, w = 0.04f, h = 0.003f;
            BoxMinMax(m, vec3(-x, 0, -z), vec3(x, h, -z + w));
            BoxMinMax(m, vec3(-x, 0, z - w), vec3(x, h, z));
            BoxMinMax(m, vec3(-x, 0, -z), vec3(-x + w, h, z));
            for (int i = 0; i < 3; ++i) {   // chevrons pointing +X (departure direction)
                const float cx = -0.12f + 0.12f * float(i);
                for (float s : { -1.0f, 1.0f }) BoxAlong(m, vec3(cx, h * 0.5f, s * 0.05f), vec3(0.0f, 0.0f, 0.0f) + vec3(1.0f, 0.0f, -s * 1.0f), vec3(0.012f, 0.075f, h * 0.5f));
            }
            ok &= Write(dir, "dock_marking.obj", m, "rover dock bay floor marking");
        }
        {   Mesh m;   // dock post with sign plate (local origin: post foot)
            BoxMinMax(m, vec3(-0.04f, 0.0f, -0.04f), vec3(0.04f, 0.008f, 0.04f));
            BoxMinMax(m, vec3(-0.012f, 0.008f, -0.012f), vec3(0.012f, 0.62f, 0.012f));
            BoxMinMax(m, vec3(-0.006f, 0.48f, -0.10f), vec3(0.006f, 0.62f, 0.10f));
            ok &= Write(dir, "dock_post.obj", m, "dock marker post and sign plate");
        }
        {   Mesh m; BoxMinMax(m, vec3(-S::PAD_HALF, 0.0f, -S::PAD_HALF), vec3(S::PAD_HALF, 0.012f, S::PAD_HALF));
            ok &= Write(dir, "pad_mat.obj", m, "drone landing pad mat"); }
        {   Mesh m;   // H and circle
            const float h = 0.0135f;
            Ring(m, vec3(0.0f, 0.012f, 0.0f), 0.30f, 0.33f, 0.0f, h - 0.012f, 64);
            for (float x : { -0.10f, 0.10f }) BoxMinMax(m, vec3(x - 0.02f, 0.012f, -0.15f), vec3(x + 0.02f, h, 0.15f));
            BoxMinMax(m, vec3(-0.10f, 0.012f, -0.02f), vec3(0.10f, h, 0.02f));
            ok &= Write(dir, "pad_marking.obj", m, "drone landing pad H marking");
        }
        return ok;
    }

    // ================================================================== beds and crops
    // A leaf: a curved, tapered shell (top and bottom surfaces) from the rosette centre outwards.
    void Leaf(Mesh& m, float yaw, float length, float width, float lift, float droop, float y0) {
        const int nu = 9, nv = 7;
        const float thick = 0.0015f;
        for (int side = 0; side < 2; ++side) {
            std::vector<vec3> pts;
            for (int i = 0; i < nu; ++i) {
                const float u = float(i) / float(nu - 1);
                const float w = width * std::pow(std::max(0.0f, std::sin(PI * std::min(u * 1.1f, 1.0f))), 0.8f) * (1.0f - 0.25f * u);
                const float r = length * u;
                const float y = y0 + lift * std::sin(u * PI * 0.55f) - droop * u * u;
                for (int j = 0; j < nv; ++j) {
                    const float v = -1.0f + 2.0f * float(j) / float(nv - 1);
                    const float cup = 0.35f * w * v * v;   // leaves cup upwards at the edges
                    vec3 q(r, y + cup + (side ? -thick : thick), v * w);
                    pts.push_back(Axis(vec3(0, 1, 0), yaw) * q);
                }
            }
            const size_t before = m.idx.size();
            Grid(m, pts, nu, nv, false);
            const float s = side ? -1.0f : 1.0f;
            OrientEach(m, before, [&](const vec3&) { return vec3(0.0f, s, 0.0f); });
        }
    }
    Mesh Plant(int variant) {
        Mesh m;
        CylAt(m, vec3(0.0f), vec3(0, 1, 0), 0.012f, 0.03f, 10);   // stem / core
        const int layers = variant ? 2 : 3;
        int seed = 17 + variant * 101;
        for (int l = 0; l < layers; ++l) {
            const int count = 6 - l;
            for (int k = 0; k < count; ++k) {
                const float yaw = (float(k) + 0.5f * float(l)) / float(count) * 2.0f * PI + 0.3f * Rand01(uint32_t(seed++));
                const float size = (variant ? 0.10f : 0.13f) * (1.0f - 0.22f * float(l)) * (0.9f + 0.2f * Rand01(uint32_t(seed++)));
                Leaf(m, yaw, size, size * 0.42f, 0.05f + 0.035f * float(l), 0.03f * (1.0f - float(l) / float(layers)), 0.01f + 0.012f * float(l));
            }
        }
        return m;
    }

    bool Beds(const fs::path& dir) {
        bool ok = true;
        const float hx = S::BED_HALF_X, hz = S::BED_HALF_Z, h = S::BED_HEIGHT, t = S::BED_BOARD;
        {   Mesh m;   // timber boards (open box) + corner posts
            BoxMinMax(m, vec3(-hx, 0.0f, -hz), vec3(hx, h, -hz + t));
            BoxMinMax(m, vec3(-hx, 0.0f, hz - t), vec3(hx, h, hz));
            BoxMinMax(m, vec3(-hx, 0.0f, -hz + t), vec3(-hx + t, h, hz - t));
            BoxMinMax(m, vec3(hx - t, 0.0f, -hz + t), vec3(hx, h, hz - t));
            ok &= Write(dir, "bed_frame.obj", m, "raised bed timber frame 4.2 x 0.7 m");
        }
        {   Mesh m; BoxMinMax(m, vec3(-hx + t, 0.0f, -hz + t), vec3(hx - t, S::SOIL_TOP, hz - t));
            ok &= Write(dir, "bed_soil.obj", m, "raised bed soil fill"); }
        ok &= Write(dir, "plant_a.obj", Plant(0), "leafy crop (lettuce-type rosette), large");
        ok &= Write(dir, "plant_b.obj", Plant(1), "leafy crop (lettuce-type rosette), small");
        {   Mesh m; Ring(m, vec3(0.0f), 0.105f, 0.118f, 0.0f, 0.005f, 40);
            ok &= Write(dir, "crop_ring.obj", m, "crop-location ring (mapped crop position)"); }
        {   Mesh m; Cylinder(m, 0.035f, 0.0f, 0.006f, 32, 0.0015f);
            ok &= Write(dir, "sample_disc.obj", m, "sampling-point disc"); }
        {   Mesh m;
            BoxMinMax(m, vec3(-0.004f, 0.0f, -0.004f), vec3(0.004f, 0.17f, 0.004f));
            BoxMinMax(m, vec3(-0.001f, 0.13f, 0.004f), vec3(0.001f, 0.17f, 0.054f));
            ok &= Write(dir, "sample_stake.obj", m, "sampling-point stake with flag"); }
        return ok;
    }

    // ================================================================== mapping drone
    bool Drone(const fs::path& dir) {
        bool ok = true;
        const float T = S::DR_PLATE_T;
        {   Mesh m;   // bottom plate = power distribution board (frame-integrated)
            Prism(m, RoundRect(0.072f, 0.072f, 0.014f), S::DR_GEAR, S::DR_GEAR + T, 1, vec3(0.0f));
            for (int i = 0; i < 4; ++i) {   // arm tabs
                Mesh tab;
                BoxMinMax(tab, vec3(0.05f, S::DR_GEAR, -0.022f), vec3(0.085f, S::DR_GEAR + T, 0.022f));
                m.Append(tab, Axis(vec3(0, 1, 0), PI * 0.25f + PI * 0.5f * float(i)));
            }
            ok &= Write(dir, "drone_pdb.obj", m, "frame bottom plate with integrated power distribution");
        }
        {   Mesh m;   // PDB solder pads + battery lead terminals
            for (int i = 0; i < 4; ++i) {
                const quat r = Axis(vec3(0, 1, 0), PI * 0.25f + PI * 0.5f * float(i));
                Mesh pad;
                for (float z : { -0.007f, 0.007f }) BoxMinMax(pad, vec3(0.040f, S::DR_GEAR + T, z - 0.004f), vec3(0.050f, S::DR_GEAR + T + 0.0006f, z + 0.004f));
                m.Append(pad, r);
            }
            ok &= Write(dir, "drone_pdb_pads.obj", m, "power distribution solder pads");
        }
        {   Mesh m;   // slotted top plate (photo: black plate with long slots), boxes around the slot openings
            const float hx = S::DR_TOP_HALF_X, hz = S::DR_TOP_HALF_Z, y0 = S::DR_TOP, y1 = S::DR_TOP + T;
            struct Slot { float x0, x1, z0, z1; };
            const std::vector<Slot> slots = { { -0.060f, 0.060f, 0.043f, 0.051f }, { -0.060f, 0.060f, -0.051f, -0.043f },
                                              { 0.082f, 0.088f, -0.030f, 0.030f }, { -0.088f, -0.082f, -0.030f, 0.030f } };
            std::vector<float> xs = { -hx, hx }, zs = { -hz, hz };
            for (const Slot& s : slots) { xs.insert(xs.end(), { s.x0, s.x1 }); zs.insert(zs.end(), { s.z0, s.z1 }); }
            std::sort(xs.begin(), xs.end());
            std::sort(zs.begin(), zs.end());
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                for (size_t k = 0; k + 1 < zs.size(); ++k) {
                    if (xs[i + 1] - xs[i] < 1e-5f || zs[k + 1] - zs[k] < 1e-5f) continue;
                    const float cx = 0.5f * (xs[i] + xs[i + 1]), cz = 0.5f * (zs[k] + zs[k + 1]);
                    bool open = false;
                    for (const Slot& s : slots) open |= cx > s.x0 && cx < s.x1 && cz > s.z0 && cz < s.z1;
                    if (!open) BoxMinMax(m, vec3(xs[i], y0, zs[k]), vec3(xs[i + 1], y1, zs[k + 1]));
                }
            }
            ok &= Write(dir, "drone_top_plate.obj", m, "frame top plate, slotted (photo)");
        }
        {   Mesh m;   // button-head screws clamping the four arm roots through the top plate
            for (int i = 0; i < 4; ++i) {
                Mesh s;
                for (float x : { 0.055f, 0.080f }) for (float z : { -0.009f, 0.009f })
                    LatheAt(s, { { 0, 0 }, { 0.0028f, 0 }, { 0.0024f, 0.0010f }, { 0.0012f, 0.0015f }, { 0, 0.0015f } }, 12, vec3(x, S::DR_TOP + T, z), vec3(0, 1, 0));
                m.Append(s, Axis(vec3(0, 1, 0), PI * 0.25f + PI * 0.5f * float(i)));
            }
            ok &= Write(dir, "drone_frame_screws.obj", m, "arm clamp screws (top plate)");
        }
        {   Mesh m;   // one arm along +X (frame centre origin): root clamp block, open lattice beam, motor pad
            const float y0 = S::DR_ARM_Y0, h = S::DR_ARM_H, t = 0.0022f;
            BoxMinMax(m, vec3(0.045f, y0, -0.016f), vec3(0.090f, y0 + h, 0.016f));
            auto halfW = [](float x) { return 0.014f - (x - 0.088f) / (0.205f - 0.088f) * 0.004f; };
            const int bays = 5;
            const float x0 = 0.088f, x1 = 0.207f;
            auto bar = [&](float ya, float yb, vec2 a, vec2 b, float w) {   // flat bar in the XZ plane (x, z) from a to b
                const vec2 d = b - a;
                Mesh s;
                Box(s, vec3(0.0f), vec3(0.5f * glm::length(d) + 0.5f * w, 0.5f * (yb - ya), 0.5f * w));
                const vec2 c = 0.5f * (a + b);
                m.Append(s, Axis(vec3(0, 1, 0), std::atan2(-d.y, d.x)), vec3(c.x, 0.5f * (ya + yb), c.y));
            };
            for (float yb : { y0, y0 + h - t }) {   // bottom and top lattice faces: edge rails + zigzag braces
                for (float s : { -1.0f, 1.0f }) bar(yb, yb + t, vec2(x0, s * (halfW(x0) - 0.0015f)), vec2(x1, s * (halfW(x1) - 0.0015f)), 0.003f);
                for (int i = 0; i < bays; ++i) {
                    const float xa = x0 + (x1 - x0) * float(i) / float(bays), xb = x0 + (x1 - x0) * float(i + 1) / float(bays);
                    const float sa = (i % 2) ? 1.0f : -1.0f;
                    bar(yb, yb + t, vec2(xa, sa * (halfW(xa) - 0.0015f)), vec2(xb, -sa * (halfW(xb) - 0.0015f)), 0.0028f);
                }
            }
            for (int i = 0; i <= bays; ++i) {   // vertical posts at the lattice nodes (open side walls)
                const float x = x0 + (x1 - x0) * float(i) / float(bays);
                for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(x - 0.0015f, y0, s * (halfW(x) - 0.0015f) - 0.0012f), vec3(x + 0.0015f, y0 + h, s * (halfW(x) - 0.0015f) + 0.0012f));
            }
            CylAt(m, vec3(S::DR_MOTOR_R, y0 + h * 0.15f, 0.0f), vec3(0, 1, 0), 0.020f, h * 0.85f, 32, 0.002f);
            ok &= Write(dir, "drone_arm.obj", m, "frame arm, open lattice beam (photo; F450-class length)");
        }
        {   Mesh m;   // X motor mount plate on the arm-end pad + four screw heads (origin at the pad top)
            for (float a : { 0.25f * PI, -0.25f * PI }) {
                Mesh b;
                BoxMinMax(b, vec3(-0.019f, 0.0f, -0.0035f), vec3(0.019f, S::DR_MOUNT_T, 0.0035f));
                m.Append(b, Axis(vec3(0, 1, 0), a));
            }
            for (int i = 0; i < 4; ++i) {
                const float a = 0.25f * PI + 0.5f * PI * float(i);
                CylAt(m, vec3(std::cos(a) * 0.0165f, S::DR_MOUNT_T, std::sin(a) * 0.0165f), vec3(0, 1, 0), 0.0022f, 0.0012f, 10);
            }
            ok &= Write(dir, "drone_motor_mount.obj", m, "X motor mount plate with screws");
        }
        {   Mesh m;   // outrunner motor: stator base + black bell top + shaft, origin at its mounting face
            CylAt(m, vec3(0.0f), vec3(0, 1, 0), 0.0135f, 0.006f, 32, 0.001f);
            Lathe(m, { { 0, 0.016f }, { 0.0140f, 0.016f }, { 0.0140f, 0.024f }, { 0.0110f, 0.027f }, { 0.0035f, 0.027f }, { 0.0035f, 0.032f }, { 0, 0.032f } }, 40, 30.0f);
            ok &= Write(dir, "drone_motor.obj", m, "brushless outrunner motor (2212 class): base, bell top, shaft");
            Mesh b;   // lower bell band (red in the photo)
            Lathe(b, { { 0.0060f, 0.006f }, { 0.0141f, 0.006f }, { 0.0141f, 0.016f }, { 0.0060f, 0.016f }, { 0.0060f, 0.006f } }, 40, 30.0f);
            ok &= Write(dir, "drone_motor_band.obj", b, "motor bell band (red, photo)");
        }
        {   Mesh m;   // bullet prop nut (origin at the propeller hub centre)
            Lathe(m, { { 0, 0.004f }, { 0.0055f, 0.004f }, { 0.0055f, 0.008f }, { 0.0045f, 0.012f }, { 0.0020f, 0.0145f }, { 0, 0.015f } }, 24, 30.0f);
            ok &= Write(dir, "drone_prop_nut.obj", m, "propeller bullet nut");
        }
        {   Mesh m;   // nylon cable tie around the arm beam (arm frame)
            const float y0 = S::DR_ARM_Y0, h = S::DR_ARM_H, x = 0.170f, w = 0.0136f, e = 0.0012f, b = 0.0022f;
            BoxMinMax(m, vec3(x - b, y0 + h, -w), vec3(x + b, y0 + h + e, w));
            BoxMinMax(m, vec3(x - b, y0 - e, -w), vec3(x + b, y0, w));
            for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(x - b, y0 - e, std::min(s * w, s * (w - e))), vec3(x + b, y0 + h + e, std::max(s * w, s * (w - e))));
            BoxMinMax(m, vec3(x - 0.003f, y0 + h + e, -0.003f), vec3(x + 0.003f, y0 + h + e + 0.0025f, 0.003f));   // locking head
            ok &= Write(dir, "drone_cable_tie.obj", m, "nylon cable tie on the arm (photo)");
        }
        {   Mesh m;   // 10 inch two-blade propeller, origin at the hub centre, spins about +Y
            Cylinder(m, 0.0095f, -0.004f, 0.004f, 24, 0.0012f);
            const Mesh blade = Blade(0.008f, S::DR_PROP_R, [](float s) { return 0.024f * (1.0f - 0.55f * s) + 0.006f * std::sin(PI * s); },
                                     [](float s) { return 1.25f - 0.75f * s; }, 0.09f, 0.05f, 12, 9);
            RingOf(m, blade, 2, 0.0f, Axis(vec3(0, 0, 1), -0.5f * PI));
            ok &= Write(dir, "drone_prop.obj", m, "10 x 4.5 inch propeller (generic)");
        }
        {   Mesh m; RBox(m, vec3(0.0f), vec3(0.022f, 0.0045f, 0.011f), 0.003f);
            ok &= Write(dir, "drone_esc.obj", m, "electronic speed controller (heat-shrink body)"); }
        {   Mesh m;   // landing gear: four struts + two skids (frame centre origin)
            for (float sz : { -1.0f, 1.0f }) {
                Pipe(m, { vec3(-0.10f, 0.006f, sz * 0.095f), vec3(0.10f, 0.006f, sz * 0.095f) }, 0.006f, 14, true);
                for (float sx : { -1.0f, 1.0f }) Rod(m, vec3(sx * 0.035f, S::DR_GEAR, sz * 0.035f), vec3(sx * 0.07f, 0.010f, sz * 0.095f), 0.005f);
            }
            ok &= Write(dir, "drone_gear.obj", m, "landing gear struts and skids");
        }
        {   Mesh m;   // FC damping plate + four dampers on the top plate
            const float y = S::DR_TOP + T;
            for (float x : { -0.03f, 0.03f }) for (float z : { -0.02f, 0.02f })
                LatheAt(m, { { 0, 0 }, { 0.003f, 0 }, { 0.0045f, 0.002f }, { 0.0045f, 0.0045f }, { 0.003f, 0.0065f }, { 0, 0.0065f } }, 16, vec3(x, y, z), vec3(0, 1, 0));
            Prism(m, RoundRect(0.042f, 0.027f, 0.004f), y + 0.0065f, S::DR_FC.y, 1, vec3(0.0f));
            ok &= Write(dir, "drone_fc_mount.obj", m, "flight-controller vibration damping mount");
        }
        {   Mesh m; RBox(m, S::DR_FC + vec3(0.0f, S::DR_FC_HALF.y, 0.0f), S::DR_FC_HALF, 0.004f);
            ok &= Write(dir, "drone_fc.obj", m, "Pixhawk 2.4.8 flight controller case (81.5 x 50 x 15.5 mm)"); }
        {   Mesh m;   // connector rows on the FC ends + orientation arrow on the lid
            const vec3 c = S::DR_FC + vec3(0.0f, S::DR_FC_HALF.y, 0.0f);
            for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(c.x + s * S::DR_FC_HALF.x - (s > 0 ? 0.0f : 0.003f), c.y - 0.004f, -0.020f),
                                                         vec3(c.x + s * S::DR_FC_HALF.x + (s > 0 ? 0.003f : 0.0f), c.y + 0.004f, 0.020f));
            const float top = S::DR_FC.y + 2.0f * S::DR_FC_HALF.y;
            Prism(m, { { 0.018f, 0.0f }, { 0.004f, 0.009f }, { 0.004f, -0.009f } }, top, top + 0.0005f, 1, vec3(0.0f));
            BoxMinMax(m, vec3(-0.016f, top, -0.003f), vec3(0.004f, top + 0.0005f, 0.003f));
            ok &= Write(dir, "drone_fc_ports.obj", m, "flight-controller connectors and arrow");
        }
        {   Mesh m;   // GPS folding mast (origin at its foot on the top plate)
            BoxMinMax(m, vec3(-0.012f, 0.0f, -0.012f), vec3(0.012f, 0.008f, 0.012f));
            CylAt(m, vec3(0.0f, 0.008f, 0.0f), vec3(0, 1, 0), 0.0035f, S::DR_GPS_MAST_H - 0.012f, 12);
            CylAt(m, vec3(0.0f, S::DR_GPS_MAST_H - 0.004f, 0.0f), vec3(0, 1, 0), 0.012f, 0.004f, 24);
            ok &= Write(dir, "drone_gps_mast.obj", m, "GPS folding mast");
        }
        {   Mesh m;   // GPS puck (origin at the mast top)
            Lathe(m, { { 0, 0 }, { S::DR_GPS_R, 0 }, { S::DR_GPS_R, S::DR_GPS_H * 0.55f }, { S::DR_GPS_R * 0.8f, S::DR_GPS_H * 0.9f }, { 0, S::DR_GPS_H } }, 48, 30.0f);
            ok &= Write(dir, "drone_gps.obj", m, "GPS module puck (ReadyToSky M8N class)");
        }
        {   Mesh m; RBox(m, vec3(0.0f, S::DR_BATTERY_Y, 0.0f), S::DR_BATTERY_HALF, 0.004f);
            ok &= Write(dir, "drone_battery.obj", m, "LiPo flight battery (generic 3S / 4S pack)"); }
        {   Mesh m;   // two hook-and-loop straps around the battery and bottom plate
            const vec3 b = S::DR_BATTERY_HALF;
            for (float x : { -0.035f, 0.035f }) {
                BoxMinMax(m, vec3(x - 0.010f, S::DR_BATTERY_Y - b.y - 0.0012f, -b.z - 0.0012f), vec3(x + 0.010f, S::DR_BATTERY_Y - b.y, b.z + 0.0012f));
                for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(x - 0.010f, S::DR_BATTERY_Y - b.y - 0.0012f, std::min(s * b.z, s * (b.z + 0.0012f))),
                                                             vec3(x + 0.010f, S::DR_GEAR, std::max(s * b.z, s * (b.z + 0.0012f))));
            }
            ok &= Write(dir, "drone_strap.obj", m, "battery straps");
        }
        return ok;
    }

    // ================================================================== soil rover
    bool Rover(const fs::path& dir) {
        bool ok = true;
        // ---- body (follows the prototype photo: black box chassis, black top plate, yellow panels)
        const float nose = S::RV_DECK_NOSE, tail = S::RV_DECK_TAIL, dz = S::RV_DECK_HALF_Z;
        const float chassisTop = S::RV_CHASSIS_Y + S::RV_CHASSIS_HALF.y;
        {   Mesh m;   // box chassis
            RBox(m, vec3(0.0f, S::RV_CHASSIS_Y, 0.0f), S::RV_CHASSIS_HALF, 0.006f, 3);
            ok &= Write(dir, "rover_chassis.obj", m, "rover box chassis (black, sheet construction)");
        }
        {   Mesh m;   // front / rear bumper blocks, low on the chassis ends
            const float hx = S::RV_CHASSIS_HALF.x;
            BoxMinMax(m, vec3(hx, 0.040f, -0.12f), vec3(hx + 0.026f, 0.075f, 0.12f));
            BoxMinMax(m, vec3(-hx - 0.022f, 0.040f, -0.12f), vec3(-hx, 0.090f, 0.12f));
            ok &= Write(dir, "rover_bumpers.obj", m, "rover bumper blocks");
        }
        {   Mesh m;   // top plate: full length, chamfered nose
            Prism(m, { { nose, -0.11f }, { nose, 0.11f }, { nose - 0.05f, dz }, { tail + 0.012f, dz }, { tail, dz - 0.012f },
                       { tail, -dz + 0.012f }, { tail + 0.012f, -dz }, { nose - 0.05f, -dz } }, S::RV_DECK, S::RV_DECK_TOP, 1, vec3(0.0f));
            ok &= Write(dir, "rover_deck.obj", m, "rover top plate (black, chamfered nose)");
        }
        {   Mesh m;   // hex standoffs between the chassis top and the top plate
            for (float x : { -0.18f, 0.0f, 0.18f }) for (float z : { -0.14f, 0.14f }) HexAt(m, vec3(x, chassisTop, z), vec3(0, 1, 0), 0.0035f, S::RV_DECK - chassisTop);
            ok &= Write(dir, "rover_standoffs.obj", m, "top-plate hex standoffs");
        }
        {   Mesh m;   // fender for the +Z side (the -Z fender is the same part mirrored)
            const float y0 = S::RV_FENDER_Y, t = S::RV_FENDER_T, z0 = S::RV_FENDER_Z0, z1 = S::RV_FENDER_Z1, hx = 0.215f;
            BoxMinMax(m, vec3(-hx, y0, z0), vec3(hx, y0 + t, z1));
            for (float s : { -1.0f, 1.0f }) {   // ends bent down over the tyre fronts / backs
                const vec3 a(s * (hx - 0.002f), y0 + 0.5f * t, 0.0f), b(s * 0.275f, y0 - 0.045f, 0.0f);
                const vec3 d = b - a;
                Mesh e;
                Box(e, vec3(0.0f), vec3(0.5f * glm::length(d), 0.5f * t, 0.5f * (z1 - z0)));
                m.Append(e, Axis(vec3(0, 0, 1), std::atan2(d.y, d.x)), vec3(0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (z0 + z1)));
            }
            // angular skirt between the wheels on the outer edge
            Prism(m, { { -0.095f, y0 + t }, { -0.065f, 0.100f }, { 0.065f, 0.100f }, { 0.095f, y0 + t } }, z1 - t, z1, 2, vec3(0.0f));
            ok &= Write(dir, "rover_fender.obj", m, "yellow wheel fender (one side)");
        }
        {   Mesh m;   // fender brackets: L brackets from the chassis sides to the fender undersides
            for (float sz : { -1.0f, 1.0f }) for (float x : { -0.04f, 0.04f }) {
                const float zi = sz * S::RV_CHASSIS_HALF.z;
                BoxMinMax(m, vec3(x - 0.012f, 0.100f, std::min(zi, zi + sz * 0.003f)), vec3(x + 0.012f, S::RV_FENDER_Y, std::max(zi, zi + sz * 0.003f)));
                BoxMinMax(m, vec3(x - 0.012f, S::RV_FENDER_Y - 0.003f, std::min(zi, zi + sz * 0.035f)), vec3(x + 0.012f, S::RV_FENDER_Y, std::max(zi, zi + sz * 0.035f)));
            }
            ok &= Write(dir, "rover_fender_brackets.obj", m, "fender mounting brackets");
        }
        {   Mesh m;   // sloped front panel under the top-plate nose
            const vec3 a(nose - 0.008f, S::RV_DECK - 0.002f, 0.0f), b(S::RV_CHASSIS_HALF.x + 0.012f, 0.078f, 0.0f);
            const vec3 d = b - a;
            Mesh p;
            Box(p, vec3(0.0f), vec3(0.5f * glm::length(d), 0.0015f, 0.15f));
            m.Append(p, Axis(vec3(0, 0, 1), std::atan2(d.y, d.x)), 0.5f * (a + b));
            ok &= Write(dir, "rover_front_panel.obj", m, "yellow sloped front panel");
        }
        {   // raised arm mounting platform (rover frame)
            const vec3 c = S::RV_ARM_BASE;
            const float y0 = S::RV_DECK_TOP, y1 = y0 + S::RV_PLATFORM_H;
            Mesh m;
            Prism(m, RoundRect(0.045f, 0.050f, 0.006f), y1, y1 + S::RV_PLATFORM_T, 1, vec3(c.x, 0.0f, c.z));
            for (float x : { -0.035f, 0.035f }) for (float z : { -0.040f, 0.040f }) HexAt(m, vec3(c.x + x, y0, c.z + z), vec3(0, 1, 0), 0.004f, S::RV_PLATFORM_H);
            ok &= Write(dir, "rover_arm_platform.obj", m, "raised arm mounting platform on standoffs");
        }
        {   Mesh m;   // button-head screws: top plate over the standoffs, front panel, arm platform
            auto head = [&](const vec3& p, const vec3& n) { LatheAt(m, { { 0, 0 }, { 0.0035f, 0 }, { 0.0030f, 0.0012f }, { 0.0015f, 0.0018f }, { 0, 0.0018f } }, 12, p, n); };
            for (float x : { -0.18f, 0.0f, 0.18f }) for (float z : { -0.14f, 0.14f }) head(vec3(x, S::RV_DECK_TOP, z), vec3(0, 1, 0));
            const vec3 a(nose - 0.008f, S::RV_DECK - 0.002f, 0.0f), b(S::RV_CHASSIS_HALF.x + 0.012f, 0.078f, 0.0f);
            const vec3 n = glm::normalize(vec3(-(b - a).y, (b - a).x, 0.0f));   // panel leans forward: normal points forward-down
            for (float u : { 0.3f, 0.7f }) for (float z : { -0.11f, 0.11f }) { vec3 p = glm::mix(a, b, u) + n * 0.0015f; p.z = z; head(p, n); }
            const vec3 c = S::RV_ARM_BASE;
            for (float x : { -0.035f, 0.035f }) for (float z : { -0.040f, 0.040f }) head(vec3(c.x + x, c.y, c.z + z), vec3(0, 1, 0));
            ok &= Write(dir, "rover_fasteners.obj", m, "button-head screws (top plate, front panel, arm platform)");
        }
        {   Mesh m;   // gearmotors at the four wheel axles (axis Z)
            for (float x : { -S::RV_WHEEL_X, S::RV_WHEEL_X }) for (float s : { -1.0f, 1.0f }) {
                CylAt(m, vec3(x, S::RV_WHEEL_R, s * 0.115f), vec3(0, 0, s), 0.018f, 0.05f, 24, 0.002f);
                CylAt(m, vec3(x, S::RV_WHEEL_R, s * 0.165f), vec3(0, 0, s), 0.004f, 0.025f, 12);
            }
            ok &= Write(dir, "rover_motors.obj", m, "drive gearmotors (generic)");
        }
        // ---- wheels (built about +Y, outer face +Y, then turned so the axle is Z and the outer face is -Z)
        const quat toAxleZ = Axis(vec3(1, 0, 0), -0.5f * PI);
        {   Mesh t;   // off-road tyre: rounded carcass + staggered block tread wrapping onto the shoulders
            const float R = S::RV_WHEEL_R, hw = 0.5f * S::RV_WHEEL_W, rc = R - 0.007f, rb = S::RV_RIM_R;
            Lathe(t, { { rb, -hw + 0.002f }, { rc - 0.012f, -hw }, { rc - 0.004f, -hw + 0.004f }, { rc, -hw + 0.011f }, { rc, hw - 0.011f },
                       { rc - 0.004f, hw - 0.004f }, { rc - 0.012f, hw }, { rb, hw - 0.002f }, { rb, -hw + 0.002f } }, 56, 30.0f);
            const int pitches = 18;
            for (int i = 0; i < pitches; ++i) {
                const float a = float(i) / float(pitches) * 2.0f * PI, half = PI / float(pitches);
                for (int row = 0; row < 2; ++row) {   // two staggered centre rows
                    const float s = row ? 1.0f : -1.0f;
                    Mesh b;
                    BoxMinMax(b, vec3(rc - 0.002f, s * 0.002f, -0.0075f), vec3(R, s * 0.002f + s * 0.0125f, 0.0075f));
                    t.Append(b, Axis(vec3(0, 1, 0), a + (row ? half : 0.0f)));
                }
                for (float s : { -1.0f, 1.0f }) {   // shoulder blocks, alternating sides, wrapping down the sidewall
                    Mesh b;
                    BoxMinMax(b, vec3(rc - 0.012f, s * (hw - 0.0105f), -0.0065f), vec3(R - 0.003f, s * (hw - 0.0105f) + s * 0.0105f, 0.0065f));
                    t.Append(b, Axis(vec3(0, 1, 0), a + (s > 0.0f ? half : 0.0f) + 0.5f * half));
                }
            }
            Mesh m;
            m.Append(t, toAxleZ);
            ok &= Write(dir, "rover_tyre.obj", m, "off-road tyre with block tread (origin at the wheel centre, axle Z)");
        }
        {   Mesh t;   // rim: barrel with flanges, hub ring and five Y spokes on the outer face
            Lathe(t, { { 0.041f, -0.027f }, { 0.047f, -0.027f }, { 0.047f, -0.024f }, { S::RV_RIM_R, -0.022f }, { S::RV_RIM_R, 0.022f },
                       { 0.047f, 0.024f }, { 0.047f, 0.027f }, { 0.041f, 0.027f }, { 0.041f, -0.027f } }, 56, 30.0f);
            Lathe(t, { { 0.0125f, 0.004f }, { 0.017f, 0.004f }, { 0.017f, 0.018f }, { 0.0125f, 0.018f }, { 0.0125f, 0.004f } }, 32, 30.0f);
            auto spoke = [&](const vec2& p0, const vec2& p1, float width, float y0, float y1) {
                const vec2 d = p1 - p0;
                Mesh s;
                Box(s, vec3(0.0f), vec3(0.5f * glm::length(d) + 0.001f, 0.5f * (y1 - y0), 0.5f * width));
                const vec2 c = 0.5f * (p0 + p1);
                t.Append(s, Axis(vec3(0, 1, 0), std::atan2(-d.y, d.x)), vec3(c.x, 0.5f * (y0 + y1), c.y));
            };
            for (int i = 0; i < 5; ++i) {
                const float a = float(i) / 5.0f * 2.0f * PI;
                const vec2 dirA(std::cos(a), std::sin(a));
                const vec2 fork = dirA * 0.026f;
                spoke(dirA * 0.016f, fork, 0.0065f, 0.008f, 0.019f);
                for (float s : { -1.0f, 1.0f }) {
                    const float b = a + s * 0.30f;
                    spoke(fork, vec2(std::cos(b), std::sin(b)) * 0.0425f, 0.0042f, 0.010f, 0.019f);
                }
            }
            Mesh m;
            m.Append(t, toAxleZ);
            ok &= Write(dir, "rover_rim.obj", m, "Y-spoke wheel rim (origin at the wheel centre, axle Z, face -Z)");
        }
        {   Mesh t;   // hub centre and axle nut
            Lathe(t, { { 0, -0.020f }, { 0.0125f, -0.020f }, { 0.0125f, 0.016f }, { 0.010f, 0.020f }, { 0, 0.020f } }, 32, 30.0f);
            HexAt(t, vec3(0.0f, 0.020f, 0.0f), vec3(0, 1, 0), 0.0065f, 0.005f);
            Mesh m;
            m.Append(t, toAxleZ);
            ok &= Write(dir, "rover_hub.obj", m, "wheel hub centre and axle nut (origin at the wheel centre, axle Z, face -Z)");
        }
        {   // electronics: Raspberry Pi Pico on a carrier board, motor driver, battery pack
            const vec3 pc = S::RV_PICO;
            {   Mesh m;   // carrier (perfboard) on standoffs, origin at the Pico position on the deck
                Prism(m, RoundRect(0.036f, 0.018f, 0.002f), 0.006f, 0.0076f, 1, vec3(0.0f));
                for (float x : { -0.031f, 0.031f }) for (float z : { -0.013f, 0.013f }) CylAt(m, vec3(x, 0.0f, z), vec3(0, 1, 0), 0.0022f, 0.006f, 6);
                ok &= Write(dir, "pico_carrier.obj", m, "Pico carrier board on standoffs");
            }
            const float b0 = 0.0076f + 0.0085f, b1 = b0 + 0.001f;   // Pico PCB on 8.5 mm headers
            {   Mesh m; Prism(m, RoundRect(0.0255f, 0.0105f, 0.0012f), b0, b1, 1, vec3(0.0f));
                ok &= Write(dir, "pico_pcb.obj", m, "Raspberry Pi Pico board 51 x 21 mm (RP2040, no wireless module)"); }
            {   Mesh m;
                BoxMinMax(m, vec3(-0.0035f, b1, -0.0035f), vec3(0.0035f, b1 + 0.0009f, 0.0035f));        // RP2040 7 x 7 mm
                BoxMinMax(m, vec3(0.0060f, b1, -0.0030f), vec3(0.0110f, b1 + 0.0010f, 0.0030f));         // QSPI flash
                BoxMinMax(m, vec3(-0.0120f, b1, 0.0040f), vec3(-0.0085f, b1 + 0.0007f, 0.0065f));        // regulator
                for (float z : { -0.0089f, 0.0089f }) BoxMinMax(m, vec3(-0.0254f, 0.0076f, z - 0.00127f), vec3(0.0254f, b0, z + 0.00127f));   // header bodies
                ok &= Write(dir, "pico_parts.obj", m, "Pico RP2040, flash, regulator, header bodies");
            }
            {   Mesh m;
                BoxMinMax(m, vec3(-0.0275f, b1, -0.0040f), vec3(-0.0220f, b1 + 0.0026f, 0.0040f));       // micro-USB
                BoxMinMax(m, vec3(-0.0170f, b1, -0.0060f), vec3(-0.0135f, b1 + 0.0012f, -0.0025f));      // BOOTSEL button frame
                BoxMinMax(m, vec3(0.0150f, b1, -0.0020f), vec3(0.0200f, b1 + 0.0010f, 0.0020f));         // crystal
                ok &= Write(dir, "pico_metal.obj", m, "Pico micro-USB, button frame, crystal");
            }
            (void)pc;
            {   Mesh m;   // motor driver board, origin at its deck position
                Prism(m, RoundRect(0.0215f, 0.0215f, 0.0015f), 0.006f, 0.0076f, 1, vec3(0.0f));
                for (float x : { -0.018f, 0.018f }) for (float z : { -0.018f, 0.018f }) CylAt(m, vec3(x, 0.0f, z), vec3(0, 1, 0), 0.0022f, 0.006f, 6);
                ok &= Write(dir, "driver_board.obj", m, "dual H-bridge motor driver board (generic)");
            }
            {   Mesh m;   // heatsink fins
                for (int i = 0; i < 7; ++i) BoxMinMax(m, vec3(-0.011f + 0.0035f * float(i), 0.0076f, -0.010f), vec3(-0.0095f + 0.0035f * float(i), 0.0256f, 0.010f));
                BoxMinMax(m, vec3(-0.012f, 0.0076f, -0.011f), vec3(0.013f, 0.0106f, 0.011f));
                ok &= Write(dir, "driver_heatsink.obj", m, "motor driver heatsink");
            }
            {   Mesh m;   // screw terminals (motor outputs both sides, power in front)
                for (float z : { -0.017f, 0.017f }) BoxMinMax(m, vec3(-0.010f, 0.0076f, z - 0.004f), vec3(0.010f, 0.0176f, z + 0.004f));
                BoxMinMax(m, vec3(0.013f, 0.0076f, -0.008f), vec3(0.021f, 0.0176f, 0.008f));
                ok &= Write(dir, "driver_terminals.obj", m, "motor driver screw terminals");
            }
            {   Mesh m; RBox(m, S::RV_BATTERY, vec3(0.06f, 0.0175f, 0.035f), 0.004f);
                ok &= Write(dir, "rover_battery.obj", m, "rover battery pack (generic Li-ion, inside the tub)"); }
        }
        {   // LiDAR (origin at the mast foot on the deck); head spins about +Y
            {   Mesh m;
                CylAt(m, vec3(0.0f), vec3(0, 1, 0), 0.030f, 0.004f, 32, 0.001f);
                CylAt(m, vec3(0.0f, 0.004f, 0.0f), vec3(0, 1, 0), 0.010f, S::RV_LIDAR_MAST - 0.004f, 20);
                ok &= Write(dir, "lidar_mast.obj", m, "LiDAR mast");
            }
            {   Mesh m;
                Lathe(m, { { 0, 0 }, { 0.036f, 0 }, { 0.036f, 0.026f }, { 0.033f, 0.030f }, { 0, 0.030f } }, 48, 30.0f);
                ok &= Write(dir, "lidar_base.obj", m, "2D scanning LiDAR base (generic, RPLIDAR class)");
            }
            {   Mesh m;   // rotating head (origin at its rotation axis on the base top)
                Lathe(m, { { 0, 0 }, { 0.034f, 0 }, { 0.034f, 0.020f }, { 0.030f, 0.026f }, { 0, 0.026f } }, 48, 30.0f);
                ok &= Write(dir, "lidar_head.obj", m, "LiDAR rotating head");
            }
            {   Mesh m;   // optical window (a band on the +X side of the head)
                Mesh w;
                Lathe(w, { { 0.0345f, 0.005f }, { 0.0350f, 0.005f }, { 0.0350f, 0.017f }, { 0.0345f, 0.017f } }, 16, 30.0f, -0.45f, 0.45f);
                m.Append(w);
                ok &= Write(dir, "lidar_window.obj", m, "LiDAR optical window");
            }
        }
        {   // sensor-positioning arm, each part in its joint frame (links along +X, joints about +Z)
            // Construction follows the prototype photo: standard-size hobby servos (blue) in black sheet
            // brackets, a black U-bracket upper arm, an aluminium-tube forearm, a wrist-rotate servo and a
            // parallel-jaw gripper. Each servo body sits in the PARENT link at the joint it drives.
            const float hs = S::RV_SHOULDER_H, L2 = S::RV_L2, L3 = S::RV_L3, T = 0.002f, H = 0.014f;
            const float sf = S::SERVO_FRONT, sb = S::SERVO_BACK, sw = S::SERVO_HW, sz = S::SERVO_HZ;
            // side plates (pair at z = +-[zi, zi + T]) from x0 to x1, half height H, solid spans + edge rails
            // around lightening slots, optional round end on the joint axis at x = 0
            auto plates = [&](Mesh& m, float zi, float x0, float x1, std::vector<vec2> solid, bool roundEnd) {
                for (float s : { -1.0f, 1.0f }) {
                    const float za = s > 0.0f ? zi : -zi - T, zb = za + T;
                    BoxMinMax(m, vec3(x0, H - 0.005f, za), vec3(x1, H, zb));
                    BoxMinMax(m, vec3(x0, -H, za), vec3(x1, -H + 0.005f, zb));
                    for (const vec2& span : solid) BoxMinMax(m, vec3(span.x, -H + 0.005f, za), vec3(span.y, H - 0.005f, zb));
                    if (roundEnd) CylAt(m, vec3(0.0f, 0.0f, za), vec3(0, 0, 1), H, T, 28);
                }
            };
            auto web = [&](Mesh& m, float x0, float x1, float zHalf) { BoxMinMax(m, vec3(x0, -H, -zHalf), vec3(x1, H, zHalf)); };
            {   Mesh m;   // hobby servo: output shaft on local +Z through the origin, body along -X
                BoxMinMax(m, vec3(-sb, -sw, -sz), vec3(sf, sw, sz));
                BoxMinMax(m, vec3(-sb - 0.007f, -sw, 0.006f), vec3(sf + 0.007f, sw, 0.0085f));   // mounting tabs
                CylAt(m, vec3(0.0f, 0.0f, sz), vec3(0, 0, 1), 0.0065f, 0.002f, 20);           // output boss
                CylAt(m, vec3(-0.011f, 0.0f, sz), vec3(0, 0, 1), 0.005f, 0.0012f, 16);         // gear-train bump
                ok &= Write(dir, "arm_servo.obj", m, "standard-size hobby servo, 40 x 20 x 36 mm body (generic)");
            }
            {   Mesh m;   // round servo horn + spacer boss out to the driven bracket (on the joint axis, +Z side)
                CylAt(m, vec3(0.0f, 0.0f, sz + 0.002f), vec3(0, 0, 1), 0.012f, 0.003f, 28, 0.0006f);
                CylAt(m, vec3(0.0f, 0.0f, sz + 0.005f), vec3(0, 0, 1), 0.005f, 0.0055f, 16);
                CylAt(m, vec3(0.0f, 0.0f, -sz - 0.0105f), vec3(0, 0, 1), 0.005f, 0.0055f, 16);   // idler bearing boss (-Z side)
                ok &= Write(dir, "arm_horn.obj", m, "servo horn and idler boss (joint axis Z)");
            }
            {   Mesh m;   // base: servo mount on the platform (arm frame; the base servo stands shaft-up at the J1 axis)
                BoxMinMax(m, vec3(-0.045f, 0.0f, -0.022f), vec3(0.025f, 0.004f, 0.022f));
                const float tab0 = S::RV_BASE_SERVO_TOP - 0.002f - sz + 0.006f;   // tab underside (servo shaft-up)
                for (const vec2& xs : { vec2(-sb - 0.0068f, -sb - 0.0012f), vec2(sf + 0.0012f, sf + 0.0068f) }) {
                    for (float s : { -1.0f, 1.0f }) BoxMinMax(m, vec3(xs.x, 0.004f, std::min(s * 0.012f, s * 0.020f)), vec3(xs.y, tab0, std::max(s * 0.012f, s * 0.020f)));
                    BoxMinMax(m, vec3(xs.x, tab0 + 0.0025f, -0.020f), vec3(xs.y, tab0 + 0.005f, 0.020f));   // clamp bar over the tab
                }
                ok &= Write(dir, "arm_base.obj", m, "arm base servo mount (posts and clamp bars)");
            }
            {   Mesh m;   // J1: turntable horn on the base servo, cradle holding the shoulder servo (body down)
                CylAt(m, vec3(0.0f, S::RV_BASE_SERVO_TOP, 0.0f), vec3(0, 1, 0), 0.019f, 0.004f, 36, 0.0008f);
                ok &= Write(dir, "arm_turntable.obj", m, "base turntable horn (J1 yaw)");
                Mesh c;
                const float y0 = S::RV_BASE_SERVO_TOP + 0.004f;
                BoxMinMax(c, vec3(-0.022f, y0, -0.021f), vec3(0.022f, y0 + 0.003f, 0.021f));
                for (float s : { -1.0f, 1.0f }) BoxMinMax(c, vec3(std::min(s * 0.0105f, s * 0.0125f), y0 + 0.003f, -0.021f), vec3(std::max(s * 0.0105f, s * 0.0125f), hs - 0.014f, 0.021f));
                ok &= Write(dir, "arm_cradle.obj", c, "shoulder servo cradle bracket (J1)");
            }
            {   Mesh m;   // J2 upper arm: back-to-back U brackets with lightening slots, elbow servo bracket at the far end
                plates(m, 0.024f, 0.0f, L2 - 0.016f, { { 0.0f, 0.034f }, { 0.5f * L2 - 0.008f, 0.5f * L2 + 0.008f }, { L2 - 0.046f, L2 - 0.016f } }, true);
                web(m, 0.031f, 0.034f, 0.026f);                       // U bottom behind the shoulder horn
                web(m, 0.5f * L2 - 0.0015f, 0.5f * L2 + 0.0015f, 0.026f);   // back-to-back joint
                web(m, L2 - sb - 0.0105f, L2 - sb - 0.0075f, 0.026f);  // elbow servo mounting web (behind its tabs)
                ok &= Write(dir, "arm_upper.obj", m, "arm upper link: black U-bracket pair (J2 -> J3)");
            }
            const float tube0 = 0.050f, tube1 = L3 - 0.065f;
            {   Mesh m;   // J3 forearm brackets: U bracket on the elbow horn, tube clamps, wrist servo bracket
                plates(m, 0.028f, 0.0f, 0.045f, { { 0.0f, 0.045f } }, true);
                BoxMinMax(m, vec3(0.040f, -H, -0.030f), vec3(0.045f, H, 0.030f));
                CylAt(m, vec3(0.045f, 0.0f, 0.0f), vec3(1, 0, 0), 0.013f, 0.010f, 24, 0.001f);           // proximal tube clamp
                CylAt(m, vec3(tube1, 0.0f, 0.0f), vec3(1, 0, 0), 0.013f, L3 - sb - 0.0105f - tube1, 24, 0.001f);   // distal clamp
                web(m, L3 - sb - 0.0105f, L3 - sb - 0.0075f, 0.026f);
                plates(m, 0.024f, L3 - sb - 0.0105f, L3 - 0.016f, { { L3 - sb - 0.0105f, L3 - 0.016f } }, false);
                ok &= Write(dir, "arm_fore.obj", m, "arm forearm brackets and tube clamps (J3 -> J4)");
                Mesh t;
                CylAt(t, vec3(tube0, 0.0f, 0.0f), vec3(1, 0, 0), 0.009f, tube1 + 0.004f - tube0, 28);
                ok &= Write(dir, "arm_tube.obj", t, "forearm aluminium tube");
            }
            {   Mesh m;   // J4 wrist: U bracket hanging from the wrist horn to the wrist-rotate servo
                for (float s : { -1.0f, 1.0f }) {
                    const float za = s > 0.0f ? 0.028f : -0.028f - T;
                    BoxMinMax(m, vec3(-H, -0.032f, za), vec3(H, 0.0f, za + T));
                    CylAt(m, vec3(0.0f, 0.0f, za), vec3(0, 0, 1), H, T, 28);
                }
                BoxMinMax(m, vec3(-H, -0.035f, -0.030f), vec3(H, -0.032f, 0.030f));
                ok &= Write(dir, "arm_wrist.obj", m, "wrist U bracket (J4)");
            }
            {   Mesh m;   // rotate-servo horn (disc only: the gripper palm is screwed straight to it)
                CylAt(m, vec3(0.0f, 0.0f, sz + 0.002f), vec3(0, 0, 1), 0.012f, 0.003f, 28, 0.0006f);
                ok &= Write(dir, "arm_horn_disc.obj", m, "servo horn disc (axis Z)");
            }
            {   Mesh m;   // gripper: palm plate (extended to carry the gripper servo), two parallel jaws clamping the sensor-head tab
                BoxMinMax(m, vec3(-0.032f, -0.080f, -0.022f), vec3(0.032f, -0.076f, 0.060f));
                for (float s : { -1.0f, 1.0f }) {
                    BoxMinMax(m, vec3(std::min(s * 0.0145f, s * 0.018f), -0.0945f, -0.011f), vec3(std::max(s * 0.0145f, s * 0.018f), -0.080f, 0.011f));
                    BoxMinMax(m, vec3(std::min(s * 0.018f, s * 0.028f), -0.0825f, -0.004f), vec3(std::max(s * 0.018f, s * 0.028f), -0.080f, 0.004f));   // jaw link
                }
                ok &= Write(dir, "arm_gripper.obj", m, "parallel-jaw gripper (palm, gears, jaws)");
            }
            {   Mesh m;   // sensor head (J4 frame): mounting plate + tab held in the gripper jaws; probes hang below
                const float p0 = -S::RV_HEAD_PLATE;
                BoxMinMax(m, vec3(-0.050f, p0, -0.016f), vec3(0.050f, p0 + 0.005f, 0.016f));
                BoxMinMax(m, vec3(-0.0145f, p0 + 0.005f, -0.006f), vec3(0.0145f, -0.080f, 0.006f));
                ok &= Write(dir, "sensor_head.obj", m, "soil sensor head (mounting plate + gripper tab)");
            }
            const float drop = S::RV_HEAD - S::RV_HEAD_PLATE;   // plate underside -> probe tips
            {   Mesh m;   // capacitive soil-moisture probe: electronics box + fork blade (origin at plate underside)
                BoxMinMax(m, vec3(-0.012f, -0.022f, -0.006f), vec3(0.012f, 0.0f, 0.006f));
                ok &= Write(dir, "probe_moisture_body.obj", m, "soil moisture probe housing");
                Mesh b;
                BoxMinMax(b, vec3(-0.011f, -0.050f, -0.0008f), vec3(0.011f, -0.022f, 0.0008f));
                for (float s : { -1.0f, 1.0f }) Prism(b, { { s * 0.004f, 0.0f }, { s * 0.011f, 0.0f }, { s * 0.009f, -(drop - 0.050f) }, { s * 0.006f, -(drop - 0.050f) } },
                                                      -0.0008f, 0.0008f, 2, vec3(0.0f, -0.050f, 0.0f));
                ok &= Write(dir, "probe_moisture_blade.obj", b, "soil moisture probe fork blade (capacitive, generic)");
            }
            {   Mesh m;   // temperature probe: gland + stainless spike
                CylAt(m, vec3(0.0f, -0.016f, 0.0f), vec3(0, 1, 0), 0.006f, 0.016f, 20, 0.001f);
                LatheAt(m, { { 0, 0 }, { 0.003f, 0.004f }, { 0.003f, drop - 0.016f }, { 0, drop - 0.016f } }, 16, vec3(0.0f, -drop, 0.0f), vec3(0, 1, 0));
                ok &= Write(dir, "probe_temperature.obj", m, "soil temperature probe (stainless sheath, generic)");
            }
            {   Mesh m;   // pH electrode body (dark) + glass bulb tip
                CylAt(m, vec3(0.0f, -(drop - 0.020f), 0.0f), vec3(0, 1, 0), 0.006f, drop - 0.020f, 24, 0.001f);
                ok &= Write(dir, "probe_ph_body.obj", m, "soil pH electrode body (generic)");
                Mesh t;
                Lathe(t, { { 0, -drop }, { 0.004f, -drop + 0.003f }, { 0.0045f, -drop + 0.010f }, { 0.0055f, -(drop - 0.020f) }, { 0, -(drop - 0.020f) } }, 24, 30.0f);
                ok &= Write(dir, "probe_ph_tip.obj", t, "soil pH electrode glass tip");
            }
        }
        return ok;
    }
}

bool OldMcDonaldMeshes(const fs::path& out) {
    const fs::path dir = out / "meshes/oldmcdonald";
    bool ok = Greenhouse(dir);
    ok &= Beds(dir);
    ok &= Drone(dir);
    ok &= Rover(dir);
    return ok;
}
