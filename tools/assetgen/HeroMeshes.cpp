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
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    constexpr float PI = 3.14159265358979f;
    using glm::vec2;
    using glm::vec3;
    using glm::quat;

    struct Mesh {
        std::vector<vec3> p;
        std::vector<uint32_t> idx;
        uint32_t Add(const vec3& v) { p.push_back(v); return uint32_t(p.size() - 1); }
        void Tri(uint32_t a, uint32_t b, uint32_t c) { if (a != b && b != c && a != c) idx.insert(idx.end(), { a, b, c }); }
        void Quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { Tri(a, b, c); Tri(a, c, d); }
        // Signed volume (positive when triangles wind counter-clockwise seen from outside).
        float Volume(size_t firstIndex = 0) const {
            double v = 0.0;
            for (size_t i = firstIndex; i + 2 < idx.size(); i += 3) v += double(glm::dot(p[idx[i]], glm::cross(p[idx[i + 1]], p[idx[i + 2]])));
            return float(v / 6.0);
        }
        void FlipFrom(size_t firstIndex) { for (size_t i = firstIndex; i + 2 < idx.size(); i += 3) std::swap(idx[i + 1], idx[i + 2]); }
        // Makes the part generated since `firstIndex` (a closed surface) face outwards.
        void Orient(size_t firstIndex) { if (Volume(firstIndex) < 0.0f) FlipFrom(firstIndex); }
        void Append(const Mesh& m, const quat& r = quat(1, 0, 0, 0), const vec3& t = vec3(0.0f), const vec3& s = vec3(1.0f)) {
            const uint32_t base = uint32_t(p.size());
            for (const vec3& v : m.p) p.push_back(r * (v * s) + t);
            const bool mirror = s.x * s.y * s.z < 0.0f;
            for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
                if (mirror) idx.insert(idx.end(), { base + m.idx[i], base + m.idx[i + 2], base + m.idx[i + 1] });
                else idx.insert(idx.end(), { base + m.idx[i], base + m.idx[i + 1], base + m.idx[i + 2] });
            }
        }
        void Transform(const quat& r, const vec3& t) { for (vec3& v : p) v = r * v + t; }
    };

    quat Axis(const vec3& axis, float angle) { return glm::angleAxis(angle, glm::normalize(axis)); }
    // Rotation taking +Y onto `dir`.
    quat FromY(const vec3& dir) {
        const vec3 d = glm::normalize(dir);
        const float c = glm::dot(vec3(0, 1, 0), d);
        if (c > 0.9999f) return quat(1, 0, 0, 0);
        if (c < -0.9999f) return Axis(vec3(1, 0, 0), PI);
        return glm::angleAxis(std::acos(c), glm::normalize(glm::cross(vec3(0, 1, 0), d)));
    }

    // Grid surface rows x cols (row-major points); wrapCols closes each row into a ring.
    void Grid(Mesh& m, const std::vector<vec3>& pts, int rows, int cols, bool wrapCols) {
        const uint32_t base = uint32_t(m.p.size());
        for (const vec3& v : pts) m.Add(v);
        const int cmax = wrapCols ? cols : cols - 1;
        for (int r = 0; r + 1 < rows; ++r) {
            for (int c = 0; c < cmax; ++c) {
                const int c1 = (c + 1) % cols;
                m.Quad(base + r * cols + c, base + r * cols + c1, base + (r + 1) * cols + c1, base + (r + 1) * cols + c);
            }
        }
    }
    // Fan cap over a ring of `n` existing vertices starting at `first` (centre added).
    void Cap(Mesh& m, uint32_t first, int n, const vec3& centre) {
        const uint32_t c = m.Add(centre);
        for (int i = 0; i < n; ++i) m.Tri(c, first + uint32_t(i), first + uint32_t((i + 1) % n));
    }
    // Duplicates a ring (for a hard edge) and caps it.
    void CapRing(Mesh& m, const std::vector<vec3>& ring, const vec3& centre) {
        const uint32_t first = uint32_t(m.p.size());
        for (const vec3& v : ring) m.Add(v);
        Cap(m, first, int(ring.size()), centre);
    }

    // Surface of revolution about +Y. `profile` holds (radius, y) points ordered bottom -> outside
    // -> top. Consecutive segments turning by more than `hardDeg` get a hard edge (separate rings).
    // Optional angular range [a0, a1] (radians) for partial (cutaway) shells, then not wrapped.
    void Lathe(Mesh& m, const std::vector<vec2>& profile, int segments, float hardDeg = 35.0f, float a0 = 0.0f, float a1 = 2.0f * PI) {
        const size_t first = m.idx.size();
        const bool full = a1 - a0 >= 2.0f * PI - 1e-4f;
        const int cols = full ? segments : segments + 1;
        auto ring = [&](const vec2& rp) {
            std::vector<vec3> pts;
            for (int i = 0; i < cols; ++i) {
                const float a = a0 + (a1 - a0) * float(i) / float(segments);
                pts.push_back(vec3(std::cos(a) * rp.x, rp.y, std::sin(a) * rp.x));
            }
            return pts;
        };
        // split the profile into smooth runs
        std::vector<std::vector<vec2>> runs(1, { profile[0] });
        for (size_t k = 1; k < profile.size(); ++k) {
            runs.back().push_back(profile[k]);
            if (k + 1 < profile.size()) {
                const vec2 d0 = glm::normalize(profile[k] - profile[k - 1]), d1 = glm::normalize(profile[k + 1] - profile[k]);
                if (std::acos(std::clamp(glm::dot(d0, d1), -1.0f, 1.0f)) > hardDeg * PI / 180.0f) runs.push_back({ profile[k] });
            }
        }
        for (const auto& run : runs) {
            std::vector<vec3> pts;
            int rows = 0;
            for (const vec2& rp : run) {
                if (rp.x <= 1e-5f) continue;   // the axis point is handled as a cap centre
                for (const vec3& v : ring(rp)) pts.push_back(v);
                ++rows;
            }
            const uint32_t base = uint32_t(m.p.size());
            if (rows >= 2) Grid(m, pts, rows, cols, full);
            else for (const vec3& v : pts) m.Add(v);
            // axis endpoints: fan to the centre
            if (run.front().x <= 1e-5f && rows >= 1 && full) Cap(m, base, cols, vec3(0, run.front().y, 0));
            if (run.back().x <= 1e-5f && rows >= 1 && full) Cap(m, base + uint32_t((rows - 1) * cols), cols, vec3(0, run.back().y, 0));
        }
        // Orientation: the profile runs counter-clockwise in the (r, y) plane, so each segment's
        // outward normal is (dy, -dr). Every triangle is matched to the segment its vertices lie on.
        for (size_t i = first; i + 2 < m.idx.size(); i += 3) {
            vec2 c(0.0f);
            vec3 centre3(0.0f);
            for (int k = 0; k < 3; ++k) {
                const vec3& v = m.p[m.idx[i + k]];
                c += vec2(std::sqrt(v.x * v.x + v.z * v.z), v.y) / 3.0f;
                centre3 += v / 3.0f;
            }
            float best = 1e30f;
            vec2 n2(0.0f, 1.0f);
            for (size_t k = 0; k + 1 < profile.size(); ++k) {
                const vec2 a = profile[k], b = profile[k + 1], d = b - a;
                const float len2 = glm::dot(d, d);
                if (len2 < 1e-12f) continue;
                const float t = std::clamp(glm::dot(c - a, d) / len2, 0.0f, 1.0f);
                const float dist = glm::length(c - (a + d * t));
                if (dist < best) { best = dist; n2 = glm::normalize(vec2(d.y, -d.x)); }
            }
            const float rr = std::sqrt(centre3.x * centre3.x + centre3.z * centre3.z);
            const vec3 radial = rr > 1e-6f ? vec3(centre3.x / rr, 0.0f, centre3.z / rr) : vec3(0.0f);
            const vec3 outward = radial * n2.x + vec3(0.0f, n2.y, 0.0f);
            const vec3 n = glm::cross(m.p[m.idx[i + 1]] - m.p[m.idx[i]], m.p[m.idx[i + 2]] - m.p[m.idx[i]]);
            if (glm::dot(n, outward) < 0.0f) std::swap(m.idx[i + 1], m.idx[i + 2]);
        }
    }

    // Closed tube (ring of radius r) from y0 to y1 around +Y with flat caps.
    void Cylinder(Mesh& m, float r, float y0, float y1, int segments, float chamfer = 0.0f) {
        if (chamfer > 0.0f) Lathe(m, { { 0, y0 }, { r - chamfer, y0 }, { r, y0 + chamfer }, { r, y1 - chamfer }, { r - chamfer, y1 }, { 0, y1 } }, segments, 30.0f);
        else Lathe(m, { { 0, y0 }, { r, y0 }, { r, y1 }, { 0, y1 } }, segments, 30.0f);
    }

    // Rounded box (fillet radius `b`, `arc` segments per quarter fillet) centred at the origin.
    void RoundedBox(Mesh& m, const vec3& half, float b, int arc = 3, int inner = 1) {
        const size_t first = m.idx.size();
        b = std::min({ b, half.x * 0.99f, half.y * 0.99f, half.z * 0.99f });
        auto samples = [&](float h) {
            std::vector<float> s;
            const float in = h - b;
            for (int k = arc; k >= 1; --k) s.push_back(-(in + b * std::tan(float(k) / float(arc) * PI * 0.25f)));
            for (int k = 0; k <= inner; ++k) s.push_back(-in + 2.0f * in * float(k) / float(inner));
            for (int k = 1; k <= arc; ++k) s.push_back(in + b * std::tan(float(k) / float(arc) * PI * 0.25f));
            return s;
        };
        const vec3 in = half - vec3(b);
        auto project = [&](vec3 q) {
            const vec3 c = glm::clamp(q, -in, in);
            const vec3 d = q - c;
            const float l = glm::length(d);
            return l > 1e-7f ? c + d / l * b : q;
        };
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign = -1; sign <= 1; sign += 2) {
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                const std::vector<float> su = samples(half[u]), sv = samples(half[v]);
                std::vector<vec3> pts;
                for (float b2 : sv) for (float a2 : su) {
                    vec3 q(0.0f);
                    q[axis] = float(sign) * half[axis];
                    q[u] = a2;
                    q[v] = b2;
                    // the cube face point beyond 45 degrees belongs to the neighbour face
                    q[u] = std::clamp(q[u], -half[u], half[u]);
                    q[v] = std::clamp(q[v], -half[v], half[v]);
                    pts.push_back(project(q));
                }
                const size_t before = m.idx.size();
                Grid(m, pts, int(sv.size()), int(su.size()), false);
                // orient this face outwards
                double s = 0.0;
                for (size_t i = before; i + 2 < m.idx.size(); i += 3) {
                    const vec3 n = glm::cross(m.p[m.idx[i + 1]] - m.p[m.idx[i]], m.p[m.idx[i + 2]] - m.p[m.idx[i]]);
                    s += double(n[axis]) * sign;
                }
                if (s < 0.0) m.FlipFrom(before);
            }
        }
        (void)first;
    }

    // Loft of superellipse cross-sections (exponent `e`, half sizes) along a path of centres
    // (sections perpendicular to the local path direction, `up` fixes the roll), with flat caps.
    struct Section { vec3 centre; vec2 half; };
    void Loft(Mesh& m, const std::vector<Section>& sections, const vec3& up, float e = 4.0f, int segments = 32) {
        const size_t first = m.idx.size();
        std::vector<vec3> pts;
        auto frame = [&](size_t i, vec3& t, vec3& u, vec3& w) {
            const vec3 a = sections[i > 0 ? i - 1 : i].centre, b = sections[i + 1 < sections.size() ? i + 1 : i].centre;
            t = glm::normalize(b - a);
            w = glm::normalize(glm::cross(t, up));
            u = glm::cross(w, t);
        };
        for (size_t i = 0; i < sections.size(); ++i) {
            vec3 t, u, w;
            frame(i, t, u, w);
            for (int k = 0; k < segments; ++k) {
                const float a = float(k) / float(segments) * 2.0f * PI;
                const float c = std::cos(a), s = std::sin(a);
                const float x = std::copysign(std::pow(std::abs(c), 2.0f / e), c) * sections[i].half.x;
                const float y = std::copysign(std::pow(std::abs(s), 2.0f / e), s) * sections[i].half.y;
                pts.push_back(sections[i].centre + w * x + u * y);
            }
        }
        Grid(m, pts, int(sections.size()), segments, true);
        CapRing(m, std::vector<vec3>(pts.begin(), pts.begin() + segments), sections.front().centre);
        CapRing(m, std::vector<vec3>(pts.end() - segments, pts.end()), sections.back().centre);
        m.Orient(first);
    }

    // Tube of varying radius swept along a polyline (parallel-transport frames), capped.
    void Sweep(Mesh& m, const std::vector<vec3>& path, const std::function<float(float)>& radius, int sides, bool caps = true) {
        const size_t first = m.idx.size();
        std::vector<vec3> pts;
        vec3 t0 = glm::normalize(path[1] - path[0]);
        vec3 n = std::abs(t0.y) < 0.9f ? glm::normalize(glm::cross(t0, vec3(0, 1, 0))) : glm::normalize(glm::cross(t0, vec3(1, 0, 0)));
        for (size_t i = 0; i < path.size(); ++i) {
            const vec3 t = glm::normalize(path[std::min(i + 1, path.size() - 1)] - path[i > 0 ? i - 1 : 0]);
            n = glm::normalize(n - t * glm::dot(n, t));
            const vec3 b = glm::cross(t, n);
            const float r = radius(float(i) / float(path.size() - 1));
            for (int k = 0; k < sides; ++k) {
                const float a = float(k) / float(sides) * 2.0f * PI;
                pts.push_back(path[i] + (n * std::cos(a) + b * std::sin(a)) * r);
            }
        }
        Grid(m, pts, int(path.size()), sides, true);
        if (caps) {
            CapRing(m, std::vector<vec3>(pts.begin(), pts.begin() + sides), path.front());
            CapRing(m, std::vector<vec3>(pts.end() - sides, pts.end()), path.back());
            m.Orient(first);
        }
    }

    // Hex bolt along +Y: washer, chamfered hex head, short threaded stub. Base at y = 0.
    Mesh HexBolt(float size) {
        Mesh m;
        const float w = size, hh = size * 0.62f;
        Cylinder(m, w * 1.05f, 0.0f, size * 0.12f, 20, size * 0.03f);   // washer
        // hex head: lathe with 6 segments, chamfered top
        Lathe(m, { { 0, size * 0.12f }, { w, size * 0.12f }, { w, size * 0.12f + hh * 0.8f }, { w * 0.82f, size * 0.12f + hh }, { 0, size * 0.12f + hh } }, 6, 25.0f, PI / 6.0f, PI / 6.0f + 2.0f * PI);
        Cylinder(m, w * 0.45f, size * 0.12f + hh, size * 0.12f + hh + size * 0.35f, 12, size * 0.06f);   // thread stub
        return m;
    }

    // Ring of `count` copies of `part` around +Y at `radius` (part's local +Y kept along `up`).
    void RingOf(Mesh& m, const Mesh& part, int count, float radius, const quat& orient, float phase = 0.0f) {
        for (int i = 0; i < count; ++i) {
            const float a = phase + float(i) / float(count) * 2.0f * PI;
            const quat r = Axis(vec3(0, 1, 0), -a);
            m.Append(part, r * orient, r * vec3(radius, 0, 0));
        }
    }

    // Cambered airfoil blade: span along +Y from r0 to r1, chord along +X (leading edge -X),
    // thickness along Z; chord c(s) and twist(s) over the span s in [0, 1].
    Mesh Blade(float r0, float r1, const std::function<float(float)>& chord, const std::function<float(float)>& twist,
               float thickness, float camber, int chordPts = 14, int spanPts = 8) {
        Mesh m;
        std::vector<vec3> pts;
        const int loop = chordPts * 2;
        for (int j = 0; j < spanPts; ++j) {
            const float s = float(j) / float(spanPts - 1);
            const float c = chord(s), tw = twist(s);
            for (int k = 0; k < loop; ++k) {
                // upper surface leading -> trailing, then lower trailing -> leading
                const bool upper = k < chordPts;
                const int kk = upper ? k : loop - 1 - k;
                const float x = 0.5f - 0.5f * std::cos(float(kk) / float(chordPts - 1) * PI);
                const float yt = 5.0f * thickness * (0.2969f * std::sqrt(x) - 0.1260f * x - 0.3516f * x * x + 0.2843f * x * x * x - 0.1036f * x * x * x * x);
                const float yc = camber * 4.0f * x * (1.0f - x);
                const float z = (upper ? yc + yt : yc - yt) * c;
                vec3 q((x - 0.5f) * c, 0.0f, z);
                q = Axis(vec3(0, 1, 0), tw) * q;
                q.y = r0 + (r1 - r0) * s;
                pts.push_back(q);
            }
        }
        Grid(m, pts, spanPts, loop, true);
        CapRing(m, std::vector<vec3>(pts.begin(), pts.begin() + loop), vec3(0, r0, 0));
        CapRing(m, std::vector<vec3>(pts.end() - loop, pts.end()), vec3(0, r1, 0));
        m.Orient(0);
        return m;
    }

    // ------------------------------------------------------------------ writer
    bool WriteObj(const fs::path& path, const Mesh& m, const char* what) {
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        f << "# " << what << " - generated by tools/assetgen (CC0). Positions + faces; normals are rebuilt on load.\n";
        char line[96];
        auto num = [](char* out, float v) {
            if (std::abs(v) < 5e-5f) v = 0.0f;
            int n = std::snprintf(out, 24, "%.4f", double(v));
            while (n > 1 && out[n - 1] == '0') --n;
            if (n > 1 && out[n - 1] == '.') --n;
            out[n] = 0;
            return n;
        };
        for (const vec3& v : m.p) {
            char a[24], b[24], c[24];
            num(a, v.x); num(b, v.y); num(c, v.z);
            std::snprintf(line, sizeof(line), "v %s %s %s\n", a, b, c);
            f << line;
        }
        for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
            std::snprintf(line, sizeof(line), "f %u %u %u\n", m.idx[i] + 1, m.idx[i + 1] + 1, m.idx[i + 2] + 1);
            f << line;
        }
        std::printf("  %-34s %6zu vertices %6zu triangles\n", path.filename().string().c_str(), m.p.size(), m.idx.size() / 3);
        return bool(f);
    }

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
