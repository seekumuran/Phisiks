#pragma once
// assetgen mesh kit: procedural mesh building blocks shared by the asset generators
// (HeroMeshes.cpp, MarinaMeshes.cpp). Positions + triangle indices; normals are rebuilt by the
// engine's OBJ loader, so hard edges are modelled with separate vertices.
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

namespace MeshKit {
    namespace fs = std::filesystem;
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

    inline quat Axis(const vec3& axis, float angle) { return glm::angleAxis(angle, glm::normalize(axis)); }
    // Rotation taking +Y onto `dir`.
    inline quat FromY(const vec3& dir) {
        const vec3 d = glm::normalize(dir);
        const float c = glm::dot(vec3(0, 1, 0), d);
        if (c > 0.9999f) return quat(1, 0, 0, 0);
        if (c < -0.9999f) return Axis(vec3(1, 0, 0), PI);
        return glm::angleAxis(std::acos(c), glm::normalize(glm::cross(vec3(0, 1, 0), d)));
    }

    // Grid surface rows x cols (row-major points); wrapCols closes each row into a ring.
    inline void Grid(Mesh& m, const std::vector<vec3>& pts, int rows, int cols, bool wrapCols) {
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
    inline void Cap(Mesh& m, uint32_t first, int n, const vec3& centre) {
        const uint32_t c = m.Add(centre);
        for (int i = 0; i < n; ++i) m.Tri(c, first + uint32_t(i), first + uint32_t((i + 1) % n));
    }
    // Duplicates a ring (for a hard edge) and caps it.
    inline void CapRing(Mesh& m, const std::vector<vec3>& ring, const vec3& centre) {
        const uint32_t first = uint32_t(m.p.size());
        for (const vec3& v : ring) m.Add(v);
        Cap(m, first, int(ring.size()), centre);
    }

    // Surface of revolution about +Y. `profile` holds (radius, y) points ordered bottom -> outside
    // -> top. Consecutive segments turning by more than `hardDeg` get a hard edge (separate rings).
    // Optional angular range [a0, a1] (radians) for partial (cutaway) shells, then not wrapped.
    inline void Lathe(Mesh& m, const std::vector<vec2>& profile, int segments, float hardDeg = 35.0f, float a0 = 0.0f, float a1 = 2.0f * PI) {
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
    inline void Cylinder(Mesh& m, float r, float y0, float y1, int segments, float chamfer = 0.0f) {
        if (chamfer > 0.0f) Lathe(m, { { 0, y0 }, { r - chamfer, y0 }, { r, y0 + chamfer }, { r, y1 - chamfer }, { r - chamfer, y1 }, { 0, y1 } }, segments, 30.0f);
        else Lathe(m, { { 0, y0 }, { r, y0 }, { r, y1 }, { 0, y1 } }, segments, 30.0f);
    }

    // Rounded box (fillet radius `b`, `arc` segments per quarter fillet) centred at the origin.
    inline void RoundedBox(Mesh& m, const vec3& half, float b, int arc = 3, int inner = 1) {
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
    inline void Loft(Mesh& m, const std::vector<Section>& sections, const vec3& up, float e = 4.0f, int segments = 32) {
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
    inline void Sweep(Mesh& m, const std::vector<vec3>& path, const std::function<float(float)>& radius, int sides, bool caps = true) {
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
    inline Mesh HexBolt(float size) {
        Mesh m;
        const float w = size, hh = size * 0.62f;
        Cylinder(m, w * 1.05f, 0.0f, size * 0.12f, 20, size * 0.03f);   // washer
        // hex head: lathe with 6 segments, chamfered top
        Lathe(m, { { 0, size * 0.12f }, { w, size * 0.12f }, { w, size * 0.12f + hh * 0.8f }, { w * 0.82f, size * 0.12f + hh }, { 0, size * 0.12f + hh } }, 6, 25.0f, PI / 6.0f, PI / 6.0f + 2.0f * PI);
        Cylinder(m, w * 0.45f, size * 0.12f + hh, size * 0.12f + hh + size * 0.35f, 12, size * 0.06f);   // thread stub
        return m;
    }

    // Ring of `count` copies of `part` around +Y at `radius` (part's local +Y kept along `up`).
    inline void RingOf(Mesh& m, const Mesh& part, int count, float radius, const quat& orient, float phase = 0.0f) {
        for (int i = 0; i < count; ++i) {
            const float a = phase + float(i) / float(count) * 2.0f * PI;
            const quat r = Axis(vec3(0, 1, 0), -a);
            m.Append(part, r * orient, r * vec3(radius, 0, 0));
        }
    }

    // Cambered airfoil blade: span along +Y from r0 to r1, chord along +X (leading edge -X),
    // thickness along Z; chord c(s) and twist(s) over the span s in [0, 1].
    inline Mesh Blade(float r0, float r1, const std::function<float(float)>& chord, const std::function<float(float)>& twist,
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
    inline bool WriteObj(const fs::path& path, const Mesh& m, const char* what) {
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

    // ------------------------------------------------------------------ primitives with explicit orientation
    // (introduced for MarinaMeshes.cpp; shared by the engineering-project generators)
    // Flips every triangle since `first` whose normal faces away from `outward(centroid)`.
    inline void OrientEach(Mesh& m, size_t first, const std::function<vec3(const vec3&)>& outward) {
        for (size_t i = first; i + 2 < m.idx.size(); i += 3) {
            const vec3 a = m.p[m.idx[i]], b = m.p[m.idx[i + 1]], c = m.p[m.idx[i + 2]];
            const vec3 n = glm::cross(b - a, c - a);
            if (glm::dot(n, outward((a + b + c) / 3.0f)) < 0.0f) std::swap(m.idx[i + 1], m.idx[i + 2]);
        }
    }
    // Convex part: every face points away from its centre.
    inline void OrientConvex(Mesh& m, size_t first, const vec3& centre) {
        OrientEach(m, first, [&](const vec3& c) { return c - centre; });
    }

    // Axis-aligned box with hard edges (separate vertices per face).
    inline void Box(Mesh& m, const vec3& centre, const vec3& half) {
        const size_t first = m.idx.size();
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign = -1; sign <= 1; sign += 2) {
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                vec3 q[4];
                const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
                for (int k = 0; k < 4; ++k) {
                    q[k] = centre;
                    q[k][axis] += float(sign) * half[axis];
                    q[k][u] += su[k] * half[u];
                    q[k][v] += sv[k] * half[v];
                }
                const uint32_t a = m.Add(q[0]), b = m.Add(q[1]), c = m.Add(q[2]), d = m.Add(q[3]);
                m.Quad(a, b, c, d);
            }
        }
        OrientConvex(m, first, centre);
    }
    inline void BoxMinMax(Mesh& m, const vec3& lo, const vec3& hi) { Box(m, (lo + hi) * 0.5f, (hi - lo) * 0.5f); }

    inline void RBox(Mesh& m, const vec3& centre, const vec3& half, float r, int arc = 3) {
        Mesh t;
        RoundedBox(t, half, r, arc);
        m.Append(t, quat(1, 0, 0, 0), centre);
    }

    // Rounded rectangle outline (counter-clockwise), `seg` points per corner arc.
    inline std::vector<vec2> RoundRect(float hx, float hy, float r, int seg = 5) {
        std::vector<vec2> pts;
        r = std::min({ r, hx * 0.999f, hy * 0.999f });
        const vec2 corners[4] = { { hx - r, hy - r }, { -hx + r, hy - r }, { -hx + r, -hy + r }, { hx - r, -hy + r } };
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k <= seg; ++k) {
                const float a = (float(c) + float(k) / float(seg)) * 0.5f * PI;
                pts.push_back(corners[c] + vec2(std::cos(a), std::sin(a)) * r);
            }
        }
        return pts;
    }

    // Convex outline (in its own xy plane) extruded from z0 to z1, then rotated / moved. `axis`
    // selects the extrusion direction: 0 = X (outline in YZ), 1 = Y (outline in XZ), 2 = Z (XY).
    inline void Prism(Mesh& m, const std::vector<vec2>& poly, float z0, float z1, int axis, const vec3& at) {
        Mesh t;
        const int n = int(poly.size());
        auto P = [&](const vec2& q, float z) {
            if (axis == 0) return vec3(z, q.x, q.y);
            if (axis == 1) return vec3(q.x, z, q.y);
            return vec3(q.x, q.y, z);
        };
        std::vector<vec3> side;
        for (float z : { z0, z1 }) for (const vec2& q : poly) side.push_back(P(q, z));
        Grid(t, side, 2, n, true);
        std::vector<vec3> bottom, top;
        for (const vec2& q : poly) { bottom.push_back(P(q, z0)); top.push_back(P(q, z1)); }
        vec2 centroid(0.0f);   // caps fan from the centroid (convex outline: always inside)
        for (const vec2& q : poly) centroid += q / float(n);
        CapRing(t, bottom, P(centroid, z0));
        CapRing(t, top, P(centroid, z1));
        OrientConvex(t, 0, P(centroid, 0.5f * (z0 + z1)));
        m.Append(t, quat(1, 0, 0, 0), at);
    }
    inline void RoundSlab(Mesh& m, const vec3& centre, const vec3& half, float r, int axis, int seg = 5) {
        // `axis` is the thin direction; the rounded outline lies in the other two.
        float hx, hy;
        if (axis == 0) { hx = half.y; hy = half.z; }
        else if (axis == 1) { hx = half.x; hy = half.z; }
        else { hx = half.x; hy = half.y; }
        Prism(m, RoundRect(hx, hy, r, seg), -half[axis], half[axis], axis, centre);
    }

    // Lathe / cylinder about an arbitrary axis: built about +Y then rotated onto `dir` at `base`.
    inline void LatheAt(Mesh& m, const std::vector<vec2>& profile, int segments, const vec3& base, const vec3& dir, float hardDeg = 35.0f) {
        Mesh t;
        Lathe(t, profile, segments, hardDeg);
        m.Append(t, FromY(dir), base);
    }
    inline void CylAt(Mesh& m, const vec3& base, const vec3& dir, float r, float length, int segments = 24, float chamfer = 0.0f) {
        Mesh t;
        Cylinder(t, r, 0.0f, length, segments, chamfer);
        m.Append(t, FromY(dir), base);
    }
    // Hex nut / hex boss along `dir`.
    inline void HexAt(Mesh& m, const vec3& base, const vec3& dir, float r, float h) {
        Mesh t;
        Lathe(t, { { 0, 0 }, { r, 0 }, { r, h }, { 0, h } }, 6, 25.0f);
        m.Append(t, FromY(dir), base);
    }
    // Hose barb (collar, shank, two ridges), 12 mm long from `base` along `dir`, for 5 mm bore tubing.
    inline void Barb(Mesh& m, const vec3& base, const vec3& dir) {
        LatheAt(m, { { 0, 0 }, { 0.0050f, 0 }, { 0.0050f, 0.0030f }, { 0.0028f, 0.0030f }, { 0.0028f, 0.0045f }, { 0.0041f, 0.0045f },
                     { 0.0030f, 0.0080f }, { 0.0030f, 0.0085f }, { 0.0039f, 0.0085f }, { 0.0026f, 0.0120f }, { 0, 0.0120f } },
                20, base, dir, 30.0f);
    }

    // Pipe of radius r along a polyline (parallel-transport frames). Side faces point away from the
    // centreline, end caps (optional) along the end tangents.
    inline void Pipe(Mesh& m, const std::vector<vec3>& path, float r, int sides, bool caps) {
        const size_t first = m.idx.size();
        std::vector<vec3> pts;
        const vec3 t0 = glm::normalize(path[1] - path[0]);
        vec3 n = std::abs(t0.z) < 0.9f ? glm::normalize(glm::cross(t0, vec3(0, 0, 1))) : glm::normalize(glm::cross(t0, vec3(1, 0, 0)));
        for (size_t i = 0; i < path.size(); ++i) {
            const vec3 t = glm::normalize(path[std::min(i + 1, path.size() - 1)] - path[i > 0 ? i - 1 : 0]);
            n = glm::normalize(n - t * glm::dot(n, t));
            const vec3 b = glm::cross(t, n);
            for (int k = 0; k < sides; ++k) {
                const float a = float(k) / float(sides) * 2.0f * PI;
                pts.push_back(path[i] + (n * std::cos(a) + b * std::sin(a)) * r);
            }
        }
        Grid(m, pts, int(path.size()), sides, true);
        // side faces: away from the nearest centreline point
        OrientEach(m, first, [&](const vec3& c) {
            size_t best = 0;
            float d = 1e30f;
            for (size_t i = 0; i < path.size(); ++i) {
                const float e = glm::length(c - path[i]);
                if (e < d) { d = e; best = i; }
            }
            return c - path[best];
        });
        if (caps) {
            const size_t c0 = m.idx.size();
            CapRing(m, std::vector<vec3>(pts.begin(), pts.begin() + sides), path.front());
            OrientEach(m, c0, [&](const vec3&) { return path[0] - path[1]; });
            const size_t c1 = m.idx.size();
            CapRing(m, std::vector<vec3>(pts.end() - sides, pts.end()), path.back());
            OrientEach(m, c1, [&](const vec3&) { return path.back() - path[path.size() - 2]; });
        }
    }

    // Annulus wall (fan duct, rings): inner + outer cylinder walls and the two end faces, about +Y.
    inline void Ring(Mesh& m, const vec3& centre, float rIn, float rOut, float y0, float y1, int seg) {
        const size_t first = m.idx.size();
        auto ring = [&](float r, float y) {
            std::vector<vec3> pts;
            for (int i = 0; i < seg; ++i) {
                const float a = float(i) / float(seg) * 2.0f * PI;
                pts.push_back(centre + vec3(std::cos(a) * r, y, std::sin(a) * r));
            }
            return pts;
        };
        auto wall = [&](float r) {
            std::vector<vec3> pts = ring(r, y0), top = ring(r, y1);
            pts.insert(pts.end(), top.begin(), top.end());
            const size_t before = m.idx.size();
            Grid(m, pts, 2, seg, true);
            const float sign = r == rOut ? 1.0f : -1.0f;
            OrientEach(m, before, [&](const vec3& c) { return vec3(c.x - centre.x, 0.0f, c.z - centre.z) * sign; });
        };
        wall(rOut);
        wall(rIn);
        for (float y : { y0, y1 }) {
            std::vector<vec3> pts = ring(rIn, y), outer = ring(rOut, y);
            pts.insert(pts.end(), outer.begin(), outer.end());
            const size_t before = m.idx.size();
            Grid(m, pts, 2, seg, true);
            const float dir = y == y1 ? 1.0f : -1.0f;
            OrientEach(m, before, [&](const vec3&) { return vec3(0.0f, dir, 0.0f); });
        }
        (void)first;
    }

}
