#include "Phisiks/Physics/Xpbd.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace Phisiks::Physics::Xpbd {
    namespace {
        void AddDistance(Model& m, uint32_t a, uint32_t b, float compliance) {
            Constraint c;
            c.type = Distance;
            c.i[0] = a;
            c.i[1] = b;
            c.rest = glm::length(glm::vec3(m.x[b]) - glm::vec3(m.x[a]));
            c.compliance = compliance;
            m.constraints.push_back(c);
        }

        void SpreadMass(Model& m, float mass) {
            const float inv = float(m.x.size()) / std::max(mass, 1e-6f);
            for (glm::vec4& p : m.x) p.w = inv;
            m.v.assign(m.x.size(), glm::vec3(0.0f));
        }

        glm::vec3 P(const Model& m, uint32_t i) { return glm::vec3(m.x[i]); }

        // Push a particle of radius r out of a collider; friction removes part of the tangential
        // motion of this substep (prev -> x).
        bool Collide(glm::vec3& x, const glm::vec3& prev, float r, float friction, const Collider& c) {
            glm::vec3 n(0.0f);
            float penetration = 0.0f;
            if (c.type == Plane) {
                n = glm::normalize(c.normal);
                penetration = r - glm::dot(x - c.center, n);
            }
            else if (c.type == Sphere) {
                const glm::vec3 d = x - c.center;
                const float dist = glm::length(d);
                if (dist < 1e-9f) return false;
                n = d / dist;
                penetration = c.radius + r - dist;
            }
            else {
                const glm::quat inv = glm::inverse(c.rotation);
                const glm::vec3 local = inv * (x - c.center);
                const glm::vec3 q = glm::abs(local) - c.halfExtents;
                const glm::vec3 outside = glm::max(q, glm::vec3(0.0f));
                const float outsideDist = glm::length(outside);
                glm::vec3 localNormal;
                if (outsideDist > 0.0f) {
                    localNormal = (local - glm::clamp(local, -c.halfExtents, c.halfExtents)) / outsideDist;
                    penetration = r - outsideDist;
                }
                else {   // inside: leave through the nearest face
                    int axis = 0;
                    if (q.y > q[axis]) axis = 1;
                    if (q.z > q[axis]) axis = 2;
                    localNormal = glm::vec3(0.0f);
                    localNormal[axis] = local[axis] >= 0.0f ? 1.0f : -1.0f;
                    penetration = r - q[axis];
                }
                n = c.rotation * localNormal;
            }
            if (penetration <= 0.0f) return false;
            x += n * penetration;
            const glm::vec3 motion = x - prev;
            const glm::vec3 tangential = motion - n * glm::dot(motion, n);
            x -= tangential * friction;
            return true;
        }
    }

    bool CollideParticle(glm::vec3& x, const glm::vec3& prev, float r, float friction, const Collider& c) { return Collide(x, prev, r, friction, c); }

    float TetVolume(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d) {
        return glm::dot(glm::cross(b - a, c - a), d - a) / 6.0f;
    }

    Model MakeRope(const glm::vec3& start, const glm::vec3& end, int segments, float mass, float compliance, bool pinStart) {
        Model m;
        segments = std::max(segments, 1);
        for (int i = 0; i <= segments; ++i) m.x.push_back(glm::vec4(start + (end - start) * (float(i) / segments), 0.0f));
        SpreadMass(m, mass);
        if (pinStart) m.x[0].w = 0.0f;
        for (int i = 0; i < segments; ++i) AddDistance(m, uint32_t(i), uint32_t(i + 1), compliance);
        m.particleRadius = 0.5f * glm::length(end - start) / segments;
        Colorize(m);
        return m;
    }

    Model MakeCloth(const glm::vec3& origin, const glm::vec3& right, const glm::vec3& down, int resX, int resY,
                    float mass, float stretchCompliance, float bendCompliance, bool pinTopCorners) {
        Model m;
        resX = std::max(resX, 2);
        resY = std::max(resY, 2);
        auto index = [&](int x, int y) { return uint32_t(y * resX + x); };
        for (int y = 0; y < resY; ++y)
            for (int x = 0; x < resX; ++x)
                m.x.push_back(glm::vec4(origin + right * (float(x) / (resX - 1)) + down * (float(y) / (resY - 1)), 0.0f));
        SpreadMass(m, mass);
        if (pinTopCorners) {
            m.x[index(0, 0)].w = 0.0f;
            m.x[index(resX - 1, 0)].w = 0.0f;
        }
        for (int y = 0; y < resY; ++y)
            for (int x = 0; x < resX; ++x) {
                if (x + 1 < resX) AddDistance(m, index(x, y), index(x + 1, y), stretchCompliance);
                if (y + 1 < resY) AddDistance(m, index(x, y), index(x, y + 1), stretchCompliance);
                if (x + 1 < resX && y + 1 < resY) {
                    AddDistance(m, index(x, y), index(x + 1, y + 1), stretchCompliance);   // shear
                    AddDistance(m, index(x + 1, y), index(x, y + 1), stretchCompliance);
                }
                if (x + 2 < resX) AddDistance(m, index(x, y), index(x + 2, y), bendCompliance);   // bending
                if (y + 2 < resY) AddDistance(m, index(x, y), index(x, y + 2), bendCompliance);
            }
        // Both sides are rendered; winding faces the -normal of (right x down).
        for (int y = 0; y + 1 < resY; ++y)
            for (int x = 0; x + 1 < resX; ++x) {
                const uint32_t a = index(x, y), b = index(x + 1, y), c = index(x, y + 1), d = index(x + 1, y + 1);
                m.triangles.insert(m.triangles.end(), { a, c, b, b, c, d });
            }
        m.particleRadius = 0.5f * glm::length(right) / (resX - 1);
        Colorize(m);
        return m;
    }

    Model MakeSoftBox(const glm::vec3& center, const glm::vec3& size, int res, float mass, float edgeCompliance, float volumeCompliance) {
        Model m;
        res = std::max(res, 1);
        const int n = res + 1;
        auto index = [&](int x, int y, int z) { return uint32_t((z * n + y) * n + x); };
        const glm::vec3 lo = center - size * 0.5f;
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    m.x.push_back(glm::vec4(lo + size * glm::vec3(x, y, z) / float(res), 0.0f));
        SpreadMass(m, mass);

        // 5 tetrahedra per cell, mirrored on alternating cells so faces between cells match.
        static const int EVEN[5][4] = { { 0, 1, 2, 4 }, { 3, 1, 7, 2 }, { 5, 1, 4, 7 }, { 6, 2, 7, 4 }, { 1, 2, 4, 7 } };
        static const int ODD[5][4] = { { 1, 0, 3, 5 }, { 2, 0, 6, 3 }, { 4, 0, 5, 6 }, { 7, 3, 6, 5 }, { 0, 3, 5, 6 } };
        std::set<std::pair<uint32_t, uint32_t>> edges;
        std::vector<Constraint> volumes;
        for (int z = 0; z < res; ++z)
            for (int y = 0; y < res; ++y)
                for (int x = 0; x < res; ++x) {
                    uint32_t corner[8];
                    for (int c = 0; c < 8; ++c) corner[c] = index(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
                    const auto& tets = ((x + y + z) % 2 == 0) ? EVEN : ODD;
                    for (const auto& t : tets) {
                        Constraint c;
                        c.type = Volume;
                        for (int k = 0; k < 4; ++k) c.i[k] = corner[t[k]];
                        c.rest = TetVolume(P(m, c.i[0]), P(m, c.i[1]), P(m, c.i[2]), P(m, c.i[3]));
                        if (c.rest < 0.0f) { std::swap(c.i[2], c.i[3]); c.rest = -c.rest; }   // mirrored cells: keep every tet positive
                        c.compliance = volumeCompliance;
                        volumes.push_back(c);
                        for (int a = 0; a < 4; ++a)
                            for (int b = a + 1; b < 4; ++b)
                                edges.insert(std::minmax(c.i[a], c.i[b]));
                    }
                }
        for (const auto& [a, b] : edges) AddDistance(m, a, b, edgeCompliance);
        m.constraints.insert(m.constraints.end(), volumes.begin(), volumes.end());

        // Surface: the 6 lattice faces, outward counter-clockwise.
        auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) { m.triangles.insert(m.triangles.end(), { a, b, c, a, c, d }); };
        for (int i = 0; i < res; ++i)
            for (int j = 0; j < res; ++j) {
                quad(index(0, i, j), index(0, i, j + 1), index(0, i + 1, j + 1), index(0, i + 1, j));             // -X
                quad(index(res, i, j), index(res, i + 1, j), index(res, i + 1, j + 1), index(res, i, j + 1));     // +X
                quad(index(i, 0, j), index(i + 1, 0, j), index(i + 1, 0, j + 1), index(i, 0, j + 1));             // -Y
                quad(index(i, res, j), index(i, res, j + 1), index(i + 1, res, j + 1), index(i + 1, res, j));     // +Y
                quad(index(i, j, 0), index(i, j + 1, 0), index(i + 1, j + 1, 0), index(i + 1, j, 0));             // -Z
                quad(index(i, j, res), index(i + 1, j, res), index(i + 1, j + 1, res), index(i, j + 1, res));     // +Z
            }
        m.particleRadius = 0.25f * std::min({ size.x, size.y, size.z }) / res;
        Colorize(m);
        return m;
    }

    void Colorize(Model& model) {
        std::vector<uint64_t> used(model.x.size(), 0);   // colours already taken at each particle (up to 64)
        std::vector<uint32_t> color(model.constraints.size(), 0);
        uint32_t colors = 0;
        for (size_t c = 0; c < model.constraints.size(); ++c) {
            const Constraint& k = model.constraints[c];
            const int count = k.type == Distance ? 2 : 4;
            uint64_t taken = 0;
            for (int j = 0; j < count; ++j) taken |= used[k.i[j]];
            uint32_t col = 0;
            while (col < 63 && (taken & (1ull << col))) ++col;
            color[c] = col;
            for (int j = 0; j < count; ++j) used[k.i[j]] |= 1ull << col;
            colors = std::max(colors, col + 1);
        }
        std::vector<size_t> order(model.constraints.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return color[a] < color[b]; });
        std::vector<Constraint> sorted;
        sorted.reserve(order.size());
        model.colorStart.assign(colors + 1, 0);
        for (size_t i : order) {
            sorted.push_back(model.constraints[i]);
            ++model.colorStart[color[i] + 1];
        }
        for (uint32_t k = 0; k < colors; ++k) model.colorStart[k + 1] += model.colorStart[k];
        model.constraints = std::move(sorted);
    }

    bool ValidateColoring(const Model& model) {
        for (size_t k = 0; k + 1 < model.colorStart.size(); ++k) {
            std::set<uint32_t> seen;
            for (uint32_t c = model.colorStart[k]; c < model.colorStart[k + 1]; ++c) {
                const Constraint& con = model.constraints[c];
                const int count = con.type == Distance ? 2 : 4;
                for (int j = 0; j < count; ++j) if (!seen.insert(con.i[j]).second) return false;
            }
        }
        return model.colorStart.empty() ? model.constraints.empty() : model.colorStart.back() == model.constraints.size();
    }

    void Step(Model& m, const StepParams& params) {
        const int substeps = std::max(params.substeps, 1);
        const float h = params.dt / substeps;
        std::vector<glm::vec3> prev(m.x.size());
        for (int s = 0; s < substeps; ++s) {
            // predict
            for (size_t i = 0; i < m.x.size(); ++i) {
                prev[i] = glm::vec3(m.x[i]);
                if (m.x[i].w == 0.0f) continue;
                glm::vec3 a = params.gravity;
                for (size_t f = 0; f < params.fieldCount; ++f) {
                    const PlacedField& pf = params.fields[f];
                    if (pf.field.affectsSoftBodies) a += FieldMath::Acceleration(pf.field, pf.origin, pf.axis, prev[i]);
                }
                m.v[i] += a * h;
                m.x[i] = glm::vec4(prev[i] + m.v[i] * h, m.x[i].w);
            }
            // solve, colour by colour (each colour is one parallel dispatch on the GPU)
            const float h2 = h * h;
            for (const Constraint& c : m.constraints) {
                if (c.type == Distance) {
                    const glm::vec3 p0 = P(m, c.i[0]), p1 = P(m, c.i[1]);
                    const float w0 = m.x[c.i[0]].w, w1 = m.x[c.i[1]].w;
                    const glm::vec3 d = p1 - p0;
                    const float len = glm::length(d);
                    const float wsum = w0 + w1 + c.compliance / h2;
                    if (len < 1e-9f || wsum <= 0.0f) continue;
                    const glm::vec3 n = d / len;
                    const float dl = -(len - c.rest) / wsum;
                    m.x[c.i[0]] -= glm::vec4(n * (w0 * dl), 0.0f);
                    m.x[c.i[1]] += glm::vec4(n * (w1 * dl), 0.0f);
                }
                else {
                    const glm::vec3 x0 = P(m, c.i[0]), x1 = P(m, c.i[1]), x2 = P(m, c.i[2]), x3 = P(m, c.i[3]);
                    const glm::vec3 g1 = glm::cross(x2 - x0, x3 - x0) / 6.0f;
                    const glm::vec3 g2 = glm::cross(x3 - x0, x1 - x0) / 6.0f;
                    const glm::vec3 g3 = glm::cross(x1 - x0, x2 - x0) / 6.0f;
                    const glm::vec3 g0 = -(g1 + g2 + g3);
                    const glm::vec3 g[4] = { g0, g1, g2, g3 };
                    float wsum = c.compliance / h2;
                    for (int k = 0; k < 4; ++k) wsum += m.x[c.i[k]].w * glm::dot(g[k], g[k]);
                    if (wsum <= 1e-12f) continue;
                    const float dl = -(TetVolume(x0, x1, x2, x3) - c.rest) / wsum;
                    for (int k = 0; k < 4; ++k) m.x[c.i[k]] += glm::vec4(g[k] * (m.x[c.i[k]].w * dl), 0.0f);
                }
            }
            // collide + update velocities
            for (size_t i = 0; i < m.x.size(); ++i) {
                if (m.x[i].w == 0.0f) continue;
                glm::vec3 x = glm::vec3(m.x[i]);
                for (size_t k = 0; k < params.colliderCount; ++k) {
                    const glm::vec3 before = x;
                    if (!Collide(x, prev[i], m.particleRadius, m.friction, params.colliders[k]) || !params.colliderImpulses) continue;
                    const glm::vec3 J = -(x - before) / (m.x[i].w * h);
                    params.colliderImpulses[k * 2] += J;
                    params.colliderImpulses[k * 2 + 1] += glm::cross(x - params.colliders[k].center, J);
                }
                m.x[i] = glm::vec4(x, m.x[i].w);
                m.v[i] = (x - prev[i]) / h * std::max(0.0f, 1.0f - m.damping * h);
            }
        }
    }

    float TotalVolume(const Model& m) {
        float v = 0.0f;
        for (const Constraint& c : m.constraints) {
            if (c.type == Volume) v += std::fabs(TetVolume(P(m, c.i[0]), P(m, c.i[1]), P(m, c.i[2]), P(m, c.i[3])));
        }
        return v;
    }

    float MaxRelativeStretch(const Model& m) {
        float worst = 0.0f;
        for (const Constraint& c : m.constraints) {
            if (c.type != Distance || c.rest <= 0.0f) continue;
            worst = std::max(worst, std::fabs(glm::length(P(m, c.i[1]) - P(m, c.i[0])) - c.rest) / c.rest);
        }
        return worst;
    }
}
