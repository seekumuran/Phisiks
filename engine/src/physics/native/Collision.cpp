// Native rigid-body backend: convex collision (see Collision.h).
#include "physics/native/Collision.h"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Phisiks::Physics::Native {
    namespace {
        float Dot(const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); }
        float Length2(const glm::vec3& v) { return glm::dot(v, v); }

        glm::vec3 AnyPerpendicular(const glm::vec3& n) {
            const glm::vec3 axis = std::fabs(n.x) < 0.57f ? glm::vec3(1, 0, 0) : (std::fabs(n.y) < 0.57f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1));
            return glm::normalize(glm::cross(n, axis));
        }

        struct Vert { glm::vec3 w, a, b; };

        Vert SupportOf(const Convex& A, const Convex& B, const glm::vec3& d) {
            Vert v;
            v.a = A.Support(d);
            v.b = B.Support(-d);
            v.w = v.a - v.b;
            return v;
        }

        // Closest point to the origin on triangle abc (Ericson 5.1.5) with barycentrics; `used`
        // receives which vertices span the result (bit mask).
        glm::vec3 ClosestOnTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float bary[3], int& used) {
            const glm::vec3 ab = b - a, ac = c - a, ap = -a;
            const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) { bary[0] = 1; bary[1] = bary[2] = 0; used = 1; return a; }
            const glm::vec3 bp = -b;
            const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) { bary[1] = 1; bary[0] = bary[2] = 0; used = 2; return b; }
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
                const float v = d1 / (d1 - d3);
                bary[0] = 1 - v; bary[1] = v; bary[2] = 0; used = 3;
                return a + v * ab;
            }
            const glm::vec3 cp = -c;
            const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
            if (d6 >= 0.0f && d5 <= d6) { bary[2] = 1; bary[0] = bary[1] = 0; used = 4; return c; }
            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
                const float w = d2 / (d2 - d6);
                bary[0] = 1 - w; bary[2] = w; bary[1] = 0; used = 5;
                return a + w * ac;
            }
            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
                const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                bary[1] = 1 - w; bary[2] = w; bary[0] = 0; used = 6;
                return b + w * (c - b);
            }
            const float denom = 1.0f / (va + vb + vc);
            const float v = vb * denom, w = vc * denom;
            bary[0] = 1 - v - w; bary[1] = v; bary[2] = w; used = 7;
            return a + ab * v + ac * w;
        }

        // Reduces the simplex to the vertices supporting the closest point to the origin.
        glm::vec3 SolveSimplex(Vert* s, int& n, float* lambda, bool& inside) {
            inside = false;
            if (n == 1) { lambda[0] = 1.0f; return s[0].w; }
            if (n == 2) {
                const glm::vec3 ab = s[1].w - s[0].w;
                const float len2 = Length2(ab);
                const float t = len2 > 1e-20f ? -Dot(s[0].w, ab) / len2 : 0.0f;
                if (t <= 0.0f) { n = 1; lambda[0] = 1; return s[0].w; }
                if (t >= 1.0f) { s[0] = s[1]; n = 1; lambda[0] = 1; return s[0].w; }
                lambda[0] = 1 - t; lambda[1] = t;
                return s[0].w + t * ab;
            }
            auto reduceTriangle = [&](const Vert& a, const Vert& b, const Vert& c, float* bary, int used) {
                const Vert in[3] = { a, b, c };
                int m = 0;
                float l[3];
                for (int i = 0; i < 3; ++i) if (used & (1 << i)) { s[m] = in[i]; l[m] = bary[i]; ++m; }
                n = m;
                for (int i = 0; i < m; ++i) lambda[i] = l[i];
            };
            if (n == 3) {
                float bary[3]; int used = 0;
                const glm::vec3 p = ClosestOnTriangle(s[0].w, s[1].w, s[2].w, bary, used);
                reduceTriangle(s[0], s[1], s[2], bary, used);
                return p;
            }
            // tetrahedron: test the faces the origin lies outside of
            const int faces[4][4] = { { 0, 1, 2, 3 }, { 0, 2, 3, 1 }, { 0, 3, 1, 2 }, { 1, 3, 2, 0 } };
            float best = FLT_MAX;
            glm::vec3 bestPoint(0.0f);
            Vert bestVerts[3]; float bestBary[3] = { 1, 0, 0 }; int bestUsed = 0;
            bool anyOutside = false;
            for (const auto& f : faces) {
                const glm::vec3& a = s[f[0]].w; const glm::vec3& b = s[f[1]].w; const glm::vec3& c = s[f[2]].w; const glm::vec3& d = s[f[3]].w;
                const glm::vec3 nrm = glm::cross(b - a, c - a);
                const float signP = Dot(-a, nrm), signD = Dot(d - a, nrm);
                const bool outside = std::fabs(signD) < 1e-12f ? true : signP * signD < 0.0f;
                if (!outside) continue;
                anyOutside = true;
                float bary[3]; int used = 0;
                const glm::vec3 p = ClosestOnTriangle(a, b, c, bary, used);
                const float dist = Length2(p);
                if (dist < best) {
                    best = dist; bestPoint = p; bestUsed = used;
                    bestVerts[0] = s[f[0]]; bestVerts[1] = s[f[1]]; bestVerts[2] = s[f[2]];
                    for (int i = 0; i < 3; ++i) bestBary[i] = bary[i];
                }
            }
            if (!anyOutside) { inside = true; return glm::vec3(0.0f); }
            reduceTriangle(bestVerts[0], bestVerts[1], bestVerts[2], bestBary, bestUsed);
            return bestPoint;
        }

        glm::vec3 BarycentricOrigin(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float out[3]) {
            const glm::vec3 v0 = b - a, v1 = c - a, v2 = p - a;
            const float d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
            const float denom = d00 * d11 - d01 * d01;
            if (std::fabs(denom) < 1e-20f) { out[0] = 1; out[1] = out[2] = 0; return a; }
            const float v = (d11 * d20 - d01 * d21) / denom;
            const float w = (d00 * d21 - d01 * d20) / denom;
            out[0] = 1 - v - w; out[1] = v; out[2] = w;
            return p;
        }

        Convex Translated(Convex c, const glm::vec3& offset) {
            c.center += offset; c.a += offset; c.b += offset; c.c += offset;
            return c;
        }

        void AddPoint(ContactSet& set, const glm::vec3& pa, const glm::vec3& pb) {
            for (int i = 0; i < set.count; ++i) if (Length2(set.points[i].pointB - pb) < 1e-6f) return;
            if (set.count >= 8) return;
            ContactPoint& p = set.points[set.count++];
            p.pointA = pa; p.pointB = pb;
            p.depth = Dot(pa - pb, set.normal);
        }

        // ---------------------------------------------------------------- round vs round
        void SegmentOf(const Convex& c, glm::vec3& p, glm::vec3& q) {
            if (c.kind == Convex::Kind::Point) { p = q = c.center; }
            else { p = c.a; q = c.b; }
        }

        glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 ab = b - a;
            const float len2 = Length2(ab);
            const float t = len2 > 1e-20f ? std::clamp(Dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            return a + t * ab;
        }

        bool RoundRound(const Convex& A, const Convex& B, float margin, ContactSet& out) {
            glm::vec3 p1, q1, p2, q2;
            SegmentOf(A, p1, q1);
            SegmentOf(B, p2, q2);
            float s, t;
            glm::vec3 c1, c2;
            const float dist = std::sqrt(ClosestSegmentSegment(p1, q1, p2, q2, s, t, c1, c2));
            const float r = A.radius + B.radius;
            if (dist - r > margin) return false;
            glm::vec3 n;
            if (dist > 1e-6f) n = (c2 - c1) / dist;
            else {
                const glm::vec3 dA = q1 - p1, dB = q2 - p2;
                const glm::vec3 cr = glm::cross(dA, dB);
                n = Length2(cr) > 1e-12f ? glm::normalize(cr) : (Length2(dA) > 1e-12f ? AnyPerpendicular(glm::normalize(dA)) : glm::vec3(0, 1, 0));
                if (Dot(n, B.Centroid() - A.Centroid()) < 0.0f) n = -n;
            }
            out = ContactSet{};
            out.normal = n;
            AddPoint(out, c1 + n * A.radius, c2 - n * B.radius);
            // parallel segments: add endpoint contacts for a stable two-point manifold
            auto endpoints = [&](const glm::vec3& e0, const glm::vec3& e1, const glm::vec3& sa, const glm::vec3& sb, bool eIsA) {
                for (const glm::vec3& e : { e0, e1 }) {
                    const glm::vec3 q = ClosestOnSegment(e, sa, sb);
                    const glm::vec3 delta = eIsA ? q - e : e - q;   // A -> B
                    const float d = std::sqrt(Length2(delta));
                    if (d < 1e-6f || d - r > margin || Dot(delta / d, n) < 0.95f) continue;
                    if (eIsA) AddPoint(out, e + n * A.radius, q - n * B.radius);
                    else AddPoint(out, q + n * A.radius, e - n * B.radius);
                }
            };
            if (Length2(q1 - p1) > 1e-12f) endpoints(p1, q1, p2, q2, true);
            if (Length2(q2 - p2) > 1e-12f) endpoints(p2, q2, p1, q1, false);
            return out.count > 0;
        }

        // ---------------------------------------------------------------- round vs box
        glm::vec3 ClosestOnBox(const glm::vec3& p, const Convex& box, bool& inside) {
            const glm::vec3 local = glm::transpose(box.rotation) * (p - box.center);
            const glm::vec3 clamped = glm::clamp(local, -box.halfExtents, box.halfExtents);
            inside = clamped == local;
            return box.center + box.rotation * clamped;
        }

        bool RoundBox(const Convex& R, const Convex& box, float margin, ContactSet& out) {
            const GjkResult g = Gjk(R, box);
            out = ContactSet{};
            if (!g.overlap && g.distance > 1e-5f) {
                if (g.distance - R.radius > margin) return false;
                const glm::vec3 n = (g.pointB - g.pointA) / g.distance;
                out.normal = n;
                AddPoint(out, g.pointA + n * R.radius, g.pointB);
                if (R.kind == Convex::Kind::Segment) {
                    for (const glm::vec3& e : { R.a, R.b }) {
                        bool inside = false;
                        const glm::vec3 q = ClosestOnBox(e, box, inside);
                        const float d = std::sqrt(Length2(q - e));
                        if (inside || d < 1e-6f || d - R.radius > margin || Dot((q - e) / d, n) < 0.95f) continue;
                        AddPoint(out, e + n * R.radius, q);
                    }
                }
                return true;
            }
            // the core penetrates the box: separating-axis search over the box faces (and the
            // segment x face-axis edges)
            glm::vec3 p, q;
            SegmentOf(R, p, q);
            glm::vec3 axes[6];
            int axisCount = 0;
            for (int i = 0; i < 3; ++i) axes[axisCount++] = box.rotation[i];
            if (Length2(q - p) > 1e-12f) {
                const glm::vec3 dir = glm::normalize(q - p);
                for (int i = 0; i < 3; ++i) {
                    const glm::vec3 c = glm::cross(dir, box.rotation[i]);
                    if (Length2(c) > 1e-6f) axes[axisCount++] = glm::normalize(c);
                }
            }
            float bestPen = FLT_MAX;
            glm::vec3 bestN(0, 1, 0);
            float bestFace = 0.0f;
            for (int i = 0; i < axisCount; ++i) {
                for (float sgn : { 1.0f, -1.0f }) {
                    const glm::vec3 n = axes[i] * sgn;   // candidate normal A -> B
                    const float rb = box.halfExtents.x * std::fabs(Dot(box.rotation[0], n)) + box.halfExtents.y * std::fabs(Dot(box.rotation[1], n)) + box.halfExtents.z * std::fabs(Dot(box.rotation[2], n));
                    const float boxMin = Dot(box.center, n) - rb;
                    const float segMax = std::max(Dot(p, n), Dot(q, n));
                    const float pen = segMax - boxMin;
                    const float biased = i < 3 ? pen : pen * 1.05f + 1e-3f;
                    if (biased < bestPen) { bestPen = biased; bestN = n; bestFace = boxMin; }
                }
            }
            out.normal = bestN;
            for (const glm::vec3& e : { p, q }) {
                const float sep = Dot(e, bestN) - bestFace;   // > 0: inside past the face
                if (sep < -margin) continue;
                AddPoint(out, e + bestN * R.radius, e - bestN * sep);
            }
            if (out.count == 0) {
                const glm::vec3 e = Dot(p, bestN) > Dot(q, bestN) ? p : q;
                AddPoint(out, e + bestN * R.radius, e - bestN * (Dot(e, bestN) - bestFace));
            }
            return true;
        }

        // ---------------------------------------------------------------- box vs box
        int ClipPolygon(const glm::vec3* in, int count, const glm::vec3& n, float offset, glm::vec3* out) {
            // keeps dot(x, n) <= offset
            int m = 0;
            for (int i = 0; i < count; ++i) {
                const glm::vec3& a = in[i];
                const glm::vec3& b = in[(i + 1) % count];
                const float da = Dot(a, n) - offset, db = Dot(b, n) - offset;
                if (da <= 0.0f) out[m++] = a;
                if ((da <= 0.0f) != (db <= 0.0f)) {
                    const float t = da / (da - db);
                    out[m++] = a + t * (b - a);
                }
            }
            return m;
        }

        bool BoxBox(const Convex& A, const Convex& B, float margin, ContactSet& out) {
            const glm::vec3 d = B.center - A.center;
            const glm::vec3* uA = &A.rotation[0];
            const glm::vec3* uB = &B.rotation[0];
            const glm::vec3 hA = A.halfExtents, hB = B.halfExtents;
            auto project = [](const glm::vec3* u, const glm::vec3& h, const glm::vec3& L) {
                return h.x * std::fabs(Dot(u[0], L)) + h.y * std::fabs(Dot(u[1], L)) + h.z * std::fabs(Dot(u[2], L));
            };
            float facePenA = FLT_MAX, facePenB = FLT_MAX, edgePen = FLT_MAX;
            int faceA = -1, faceB = -1, edgeI = -1, edgeJ = -1;
            glm::vec3 edgeAxis(0.0f);
            for (int i = 0; i < 3; ++i) {
                const float pen = hA[i] + project(uB, hB, uA[i]) - std::fabs(Dot(d, uA[i]));
                if (pen < -margin) return false;
                if (pen < facePenA) { facePenA = pen; faceA = i; }
            }
            for (int i = 0; i < 3; ++i) {
                const float pen = project(uA, hA, uB[i]) + hB[i] - std::fabs(Dot(d, uB[i]));
                if (pen < -margin) return false;
                if (pen < facePenB) { facePenB = pen; faceB = i; }
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    glm::vec3 L = glm::cross(uA[i], uB[j]);
                    const float len = std::sqrt(Length2(L));
                    if (len < 1e-4f) continue;
                    L /= len;
                    const float pen = project(uA, hA, L) + project(uB, hB, L) - std::fabs(Dot(d, L));
                    if (pen < -margin) return false;
                    if (pen < edgePen) { edgePen = pen; edgeI = i; edgeJ = j; edgeAxis = L; }
                }
            }
            out = ContactSet{};
            const float facePen = std::min(facePenA, facePenB);
            if (edgeI >= 0 && edgePen < 0.95f * facePen - 0.005f) {
                glm::vec3 n = edgeAxis;
                if (Dot(d, n) < 0.0f) n = -n;
                glm::vec3 pa = A.center, pb = B.center;
                for (int k = 0; k < 3; ++k) {
                    if (k != edgeI) pa += uA[k] * (hA[k] * (Dot(uA[k], n) > 0.0f ? 1.0f : -1.0f));
                    if (k != edgeJ) pb += uB[k] * (hB[k] * (Dot(uB[k], n) > 0.0f ? -1.0f : 1.0f));
                }
                float s, t;
                glm::vec3 c1, c2;
                ClosestSegmentSegment(pa - uA[edgeI] * hA[edgeI], pa + uA[edgeI] * hA[edgeI], pb - uB[edgeJ] * hB[edgeJ], pb + uB[edgeJ] * hB[edgeJ], s, t, c1, c2);
                out.normal = n;
                AddPoint(out, c1, c2);
                return true;
            }
            // face contact: reference face on one box, incident face of the other clipped against it
            const bool refIsA = facePenA <= facePenB * 0.95f + 0.001f;
            const Convex& ref = refIsA ? A : B;
            const Convex& inc = refIsA ? B : A;
            const int refAxis = refIsA ? faceA : faceB;
            glm::vec3 nAB = refIsA ? uA[faceA] : uB[faceB];
            if (Dot(d, nAB) < 0.0f) nAB = -nAB;
            const glm::vec3 refNormal = refIsA ? nAB : -nAB;   // reference face outward normal (towards the incident box)
            const glm::vec3* uR = &ref.rotation[0];
            const glm::vec3* uI = &inc.rotation[0];
            int incAxis = 0;
            float minDot = FLT_MAX;
            for (int k = 0; k < 3; ++k) {
                const float dk = -std::fabs(Dot(uI[k], refNormal));
                if (dk < minDot) { minDot = dk; incAxis = k; }
            }
            const float incSign = Dot(uI[incAxis], refNormal) > 0.0f ? -1.0f : 1.0f;
            const glm::vec3 incCenter = inc.center + uI[incAxis] * (inc.halfExtents[incAxis] * incSign);
            const int i1 = (incAxis + 1) % 3, i2 = (incAxis + 2) % 3;
            const glm::vec3 e1 = uI[i1] * inc.halfExtents[i1], e2 = uI[i2] * inc.halfExtents[i2];
            glm::vec3 poly[16] = { incCenter + e1 + e2, incCenter - e1 + e2, incCenter - e1 - e2, incCenter + e1 - e2 };
            glm::vec3 tmp[16];
            int count = 4;
            const int r1 = (refAxis + 1) % 3, r2 = (refAxis + 2) % 3;
            for (int side : { r1, r2 }) {
                const float c = Dot(ref.center, uR[side]);
                count = ClipPolygon(poly, count, uR[side], c + ref.halfExtents[side], tmp);
                if (count == 0) break;
                count = ClipPolygon(tmp, count, -uR[side], -c + ref.halfExtents[side], poly);
                if (count == 0) break;
            }
            if (count == 0) return false;
            const float refOffset = Dot(ref.center, refNormal) + ref.halfExtents[refAxis];
            out.normal = nAB;
            for (int k = 0; k < count && k < 16; ++k) {
                const float sep = Dot(poly[k], refNormal) - refOffset;   // < 0: incident point below the reference face
                if (sep > margin) continue;
                const glm::vec3 onRef = poly[k] - refNormal * sep;
                if (refIsA) AddPoint(out, onRef, poly[k]);
                else AddPoint(out, poly[k], onRef);
            }
            ReduceContacts(out);
            return out.count > 0;
        }

        bool Generic(const Convex& A, const Convex& B, float margin, ContactSet& out) {
            const GjkResult g = Gjk(A, B);
            out = ContactSet{};
            out.incremental = true;
            if (!g.overlap && g.distance > 1e-6f) {
                if (g.distance - A.radius - B.radius > margin) return false;
                const glm::vec3 n = (g.pointB - g.pointA) / g.distance;
                out.normal = n;
                AddPoint(out, g.pointA + n * A.radius, g.pointB - n * B.radius);
                return true;
            }
            Penetration pen;
            if (!Epa(A, B, g, pen)) return false;
            out.normal = pen.normal;
            AddPoint(out, pen.pointA + pen.normal * A.radius, pen.pointB - pen.normal * B.radius);
            return true;
        }

        void Flip(ContactSet& set) {
            set.normal = -set.normal;
            for (int i = 0; i < set.count; ++i) std::swap(set.points[i].pointA, set.points[i].pointB);
        }
    }

    // ==================================================================== Convex
    glm::vec3 Convex::Support(const glm::vec3& d) const {
        switch (kind) {
            case Kind::Point: return center;
            case Kind::Segment: return Dot(a, d) >= Dot(b, d) ? a : b;
            case Kind::Box: {
                const glm::vec3 local = glm::transpose(rotation) * d;
                const glm::vec3 s(local.x >= 0.0f ? halfExtents.x : -halfExtents.x, local.y >= 0.0f ? halfExtents.y : -halfExtents.y, local.z >= 0.0f ? halfExtents.z : -halfExtents.z);
                return center + rotation * s;
            }
            case Kind::Points: {
                const glm::vec3 local = glm::transpose(rotation) * d;
                float best = -FLT_MAX;
                glm::vec3 bestPoint(0.0f);
                for (const glm::vec3& p : *points) {
                    const float v = Dot(p, local);
                    if (v > best) { best = v; bestPoint = p; }
                }
                return center + rotation * bestPoint;
            }
            case Kind::Triangle: {
                const float da = Dot(a, d), db = Dot(b, d), dc = Dot(c, d);
                return da >= db ? (da >= dc ? a : c) : (db >= dc ? b : c);
            }
        }
        return center;
    }

    glm::vec3 Convex::Centroid() const {
        switch (kind) {
            case Kind::Segment: return 0.5f * (a + b);
            case Kind::Triangle: return (a + b + c) / 3.0f;
            case Kind::Points: {
                if (!points || points->empty()) return center;
                glm::vec3 sum(0.0f);
                for (const glm::vec3& p : *points) sum += p;
                return center + rotation * (sum / float(points->size()));
            }
            default: return center;
        }
    }

    // ==================================================================== GJK
    GjkResult Gjk(const Convex& A, const Convex& B) {
        GjkResult result;
        Vert s[4];
        float lambda[4] = { 1, 0, 0, 0 };
        int n = 0;
        glm::vec3 v = A.Centroid() - B.Centroid();
        if (Length2(v) < 1e-12f) v = glm::vec3(1.0f, 0.0f, 0.0f);
        bool inside = false;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const Vert p = SupportOf(A, B, -v);
            const float vv = Length2(v);
            if (n > 0 && vv - Dot(v, p.w) <= 1e-6f * vv + 1e-12f) break;   // no further progress
            bool duplicate = false;
            for (int i = 0; i < n; ++i) if (Length2(s[i].w - p.w) < 1e-14f) duplicate = true;
            if (duplicate) break;
            s[n++] = p;
            const glm::vec3 next = SolveSimplex(s, n, lambda, inside);
            if (inside || Length2(next) < 1e-14f) { inside = true; v = next; break; }
            if (n > 0 && Length2(next) >= vv && iteration > 0) { v = next; break; }
            v = next;
        }
        result.simplexCount = n;
        for (int i = 0; i < n; ++i) { result.simplexA[i] = s[i].a; result.simplexB[i] = s[i].b; }
        if (inside) {
            result.overlap = true;
            result.distance = 0.0f;
            return result;
        }
        glm::vec3 pa(0.0f), pb(0.0f);
        for (int i = 0; i < n; ++i) { pa += lambda[i] * s[i].a; pb += lambda[i] * s[i].b; }
        result.pointA = pa;
        result.pointB = pb;
        result.distance = std::sqrt(Length2(v));
        if (result.distance < 1e-7f) result.overlap = true;
        return result;
    }

    // ==================================================================== EPA
    bool Epa(const Convex& A, const Convex& B, const GjkResult& gjk, Penetration& out) {
        std::vector<Vert> verts;
        verts.reserve(64);
        for (int i = 0; i < gjk.simplexCount; ++i) verts.push_back({ gjk.simplexA[i] - gjk.simplexB[i], gjk.simplexA[i], gjk.simplexB[i] });
        const glm::vec3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        auto addIfNew = [&](const glm::vec3& dir) {
            const Vert p = SupportOf(A, B, dir);
            for (const Vert& q : verts) if (Length2(q.w - p.w) < 1e-12f) return false;
            verts.push_back(p);
            return true;
        };
        if (verts.empty()) addIfNew(axes[0]);
        for (int k = 0; verts.size() < 2 && k < 6; ++k) addIfNew(axes[k]);
        if (verts.size() == 2) {
            const glm::vec3 d = glm::normalize(verts[1].w - verts[0].w);
            const glm::vec3 p0 = AnyPerpendicular(d);
            for (int k = 0; k < 6 && verts.size() < 3; ++k) {
                const float angle = float(k) * 1.0471976f;
                const glm::vec3 dir = p0 * std::cos(angle) + glm::cross(d, p0) * std::sin(angle);
                const Vert p = SupportOf(A, B, dir);
                if (Length2(glm::cross(p.w - verts[0].w, d)) > 1e-10f) verts.push_back(p);
            }
        }
        if (verts.size() == 3) {
            const glm::vec3 nrm = glm::cross(verts[1].w - verts[0].w, verts[2].w - verts[0].w);
            if (Length2(nrm) < 1e-14f) return false;
            const glm::vec3 nn = glm::normalize(nrm);
            Vert p = SupportOf(A, B, nn);
            if (std::fabs(Dot(p.w - verts[0].w, nn)) < 1e-7f) p = SupportOf(A, B, -nn);
            if (std::fabs(Dot(p.w - verts[0].w, nn)) < 1e-7f) return false;
            verts.push_back(p);
        }
        if (verts.size() < 4) return false;
        verts.resize(4);
        const glm::vec3 interior = 0.25f * (verts[0].w + verts[1].w + verts[2].w + verts[3].w);
        if (std::fabs(Dot(glm::cross(verts[1].w - verts[0].w, verts[2].w - verts[0].w), verts[3].w - verts[0].w)) < 1e-12f) return false;

        struct Face { int i, j, k; glm::vec3 n; float d; bool valid; };
        std::vector<Face> faces;
        faces.reserve(128);
        auto makeFace = [&](int i, int j, int k) {
            Face f{ i, j, k, glm::vec3(0.0f), FLT_MAX, false };
            glm::vec3 nrm = glm::cross(verts[j].w - verts[i].w, verts[k].w - verts[i].w);
            const float len = std::sqrt(Length2(nrm));
            if (len < 1e-12f) return f;
            nrm /= len;
            if (Dot(nrm, verts[i].w - interior) < 0.0f) { nrm = -nrm; std::swap(f.j, f.k); }
            f.n = nrm;
            f.d = Dot(nrm, verts[i].w);
            f.valid = true;
            return f;
        };
        faces.push_back(makeFace(0, 1, 2));
        faces.push_back(makeFace(0, 3, 1));
        faces.push_back(makeFace(0, 2, 3));
        faces.push_back(makeFace(1, 3, 2));
        for (const Face& f : faces) if (!f.valid || f.d < -1e-5f) return false;   // origin must be inside

        int bestFace = 0;
        for (int iteration = 0; iteration < 64; ++iteration) {
            bestFace = -1;
            float bestD = FLT_MAX;
            for (int i = 0; i < int(faces.size()); ++i) if (faces[i].valid && faces[i].d < bestD) { bestD = faces[i].d; bestFace = i; }
            if (bestFace < 0) return false;
            const Face f = faces[bestFace];
            const Vert p = SupportOf(A, B, f.n);
            if (Dot(p.w, f.n) - f.d < 1e-4f) break;
            bool duplicate = false;
            for (const Vert& q : verts) if (Length2(q.w - p.w) < 1e-12f) duplicate = true;
            if (duplicate) break;
            const int pi = int(verts.size());
            verts.push_back(p);
            std::vector<std::pair<int, int>> horizon;
            for (Face& g : faces) {
                if (!g.valid || Dot(g.n, p.w - verts[g.i].w) <= 1e-9f) continue;
                g.valid = false;
                const std::pair<int, int> edges[3] = { { g.i, g.j }, { g.j, g.k }, { g.k, g.i } };
                for (const auto& e : edges) {
                    auto reverse = std::find(horizon.begin(), horizon.end(), std::make_pair(e.second, e.first));
                    if (reverse != horizon.end()) horizon.erase(reverse);
                    else horizon.push_back(e);
                }
            }
            std::vector<Face> next;
            next.reserve(faces.size() + horizon.size());
            for (const Face& g : faces) if (g.valid) next.push_back(g);
            for (const auto& e : horizon) {
                Face nf = makeFace(e.first, e.second, pi);
                if (nf.valid) next.push_back(nf);
            }
            faces.swap(next);
            if (faces.size() > 512) break;
        }
        bestFace = -1;
        float bestD = FLT_MAX;
        for (int i = 0; i < int(faces.size()); ++i) if (faces[i].valid && faces[i].d < bestD) { bestD = faces[i].d; bestFace = i; }
        if (bestFace < 0) return false;
        const Face& f = faces[bestFace];
        float bary[3];
        BarycentricOrigin(f.n * f.d, verts[f.i].w, verts[f.j].w, verts[f.k].w, bary);
        out.normal = f.n;
        out.depth = std::max(f.d, 0.0f);
        out.pointA = bary[0] * verts[f.i].a + bary[1] * verts[f.j].a + bary[2] * verts[f.k].a;
        out.pointB = bary[0] * verts[f.i].b + bary[1] * verts[f.j].b + bary[2] * verts[f.k].b;
        return true;
    }

    // ==================================================================== contacts
    bool GenerateContacts(const Convex& a, const Convex& b, float margin, ContactSet& out) {
        using K = Convex::Kind;
        out = ContactSet{};
        if (a.IsRound() && b.IsRound()) return RoundRound(a, b, margin, out);
        if (a.IsRound() && b.kind == K::Box) return RoundBox(a, b, margin, out);
        if (a.kind == K::Box && b.IsRound()) {
            if (!RoundBox(b, a, margin, out)) return false;
            Flip(out);
            return true;
        }
        if (a.kind == K::Box && b.kind == K::Box) return BoxBox(a, b, margin, out);
        return Generic(a, b, margin, out);
    }

    void ReduceContacts(ContactSet& set) {
        if (set.count <= 4) return;
        ContactPoint in[8];
        const int n = set.count;
        for (int i = 0; i < n; ++i) in[i] = set.points[i];
        bool used[8] = {};
        int pick[4];
        pick[0] = 0;
        for (int i = 1; i < n; ++i) if (in[i].depth > in[pick[0]].depth) pick[0] = i;
        used[pick[0]] = true;
        auto farthest = [&](auto score) {
            int best = -1; float bestScore = -1.0f;
            for (int i = 0; i < n; ++i) if (!used[i]) { const float s = score(in[i].pointB); if (s > bestScore) { bestScore = s; best = i; } }
            return best;
        };
        pick[1] = farthest([&](const glm::vec3& p) { return Length2(p - in[pick[0]].pointB); });
        used[pick[1]] = true;
        pick[2] = farthest([&](const glm::vec3& p) { return Length2(glm::cross(in[pick[1]].pointB - in[pick[0]].pointB, p - in[pick[0]].pointB)); });
        used[pick[2]] = true;
        pick[3] = farthest([&](const glm::vec3& p) {
            const glm::vec3 a = in[pick[0]].pointB, b = in[pick[1]].pointB, c = in[pick[2]].pointB;
            const glm::vec3 nrm = glm::cross(b - a, c - a);
            // area added outside the triangle (negative barycentric side)
            float best = 0.0f;
            const glm::vec3 tri[3] = { a, b, c };
            for (int e = 0; e < 3; ++e) {
                const glm::vec3 area = glm::cross(tri[(e + 1) % 3] - tri[e], p - tri[e]);
                if (Dot(area, nrm) < 0.0f) best = std::max(best, Length2(area));
            }
            return best;
        });
        set.count = 0;
        for (int k = 0; k < 4; ++k) if (pick[k] >= 0) set.points[set.count++] = in[pick[k]];
    }

    bool Distance(const Convex& a, const Convex& b, float& separation, glm::vec3& normal, glm::vec3& pointA, glm::vec3& pointB) {
        const GjkResult g = Gjk(a, b);
        if (!g.overlap && g.distance > 1e-6f) {
            normal = (g.pointB - g.pointA) / g.distance;
            separation = g.distance - a.radius - b.radius;
            pointA = g.pointA + normal * a.radius;
            pointB = g.pointB - normal * b.radius;
            return true;
        }
        Penetration pen;
        if (!Epa(a, b, g, pen)) {
            normal = glm::vec3(0, 1, 0);
            separation = -(a.radius + b.radius) - 1e-4f;
            pointA = pointB = 0.5f * (a.Centroid() + b.Centroid());
            return false;
        }
        normal = pen.normal;
        separation = -(pen.depth + a.radius + b.radius);
        pointA = pen.pointA + normal * a.radius;
        pointB = pen.pointB - normal * b.radius;
        return true;
    }

    // ==================================================================== rays and sweeps
    bool RaycastTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float maxDistance, float& t, glm::vec3& normal) {
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(d, e2);
        const float det = Dot(e1, p);
        if (std::fabs(det) < 1e-12f) return false;
        const float inv = 1.0f / det;
        const glm::vec3 s = o - a;
        const float u = Dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = Dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        const float hit = Dot(e2, q) * inv;
        if (hit < 0.0f || hit > maxDistance) return false;
        t = hit;
        normal = glm::normalize(glm::cross(e1, e2));
        if (Dot(normal, d) > 0.0f) normal = -normal;
        return true;
    }

    bool Sweep(const Convex& moving, const glm::vec3& dir, float maxDistance, const Convex& target, float& t, glm::vec3& normal, glm::vec3& point) {
        t = 0.0f;
        glm::vec3 lastNormal = -dir;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const Convex m = Translated(moving, dir * t);
            const GjkResult g = Gjk(m, target);
            if (g.overlap || g.distance < 1e-7f) {
                if (iteration == 0) { normal = -dir; point = moving.Centroid(); return true; }
                normal = lastNormal;
                point = g.pointB;
                return t <= maxDistance;
            }
            const glm::vec3 n = (g.pointA - g.pointB) / g.distance;   // target -> mover
            const float sep = g.distance - m.radius - target.radius;
            if (iteration == 0 && sep < 1e-4f) {
                // touching or shallow overlap: only blocks if the motion goes into the target
                if (-Dot(dir, n) <= 1e-6f && sep > -1e-2f) return false;
                if (sep < -1e-2f) { normal = -dir; point = moving.Centroid(); return true; }
            }
            if (sep < 1e-4f) {
                normal = n;
                point = g.pointB + n * target.radius;
                return t <= maxDistance;
            }
            const float closing = -Dot(dir, n);
            if (closing <= 1e-7f) return false;
            lastNormal = n;
            t += sep / closing;
            if (t > maxDistance) return false;
        }
        return false;
    }

    bool RaycastConvex(const Convex& shape, const glm::vec3& o, const glm::vec3& d, float maxDistance, float& t, glm::vec3& normal) {
        if (shape.kind == Convex::Kind::Point) {
            const glm::vec3 m = o - shape.center;
            const float b = Dot(m, d), c = Length2(m) - shape.radius * shape.radius;
            if (c <= 0.0f) { t = 0.0f; normal = -d; return true; }
            if (b > 0.0f) return false;
            const float disc = b * b - c;
            if (disc < 0.0f) return false;
            t = -b - std::sqrt(disc);
            if (t > maxDistance) return false;
            normal = glm::normalize(o + d * t - shape.center);
            return true;
        }
        if (shape.kind == Convex::Kind::Box && shape.radius == 0.0f) {
            const glm::mat3 rt = glm::transpose(shape.rotation);
            const glm::vec3 lo = rt * (o - shape.center), ld = rt * d;
            float tmin = -FLT_MAX, tmax = FLT_MAX;
            int axis = -1; float axisSign = 1.0f;
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(ld[i]) < 1e-12f) {
                    if (lo[i] < -shape.halfExtents[i] || lo[i] > shape.halfExtents[i]) return false;
                    continue;
                }
                float t1 = (-shape.halfExtents[i] - lo[i]) / ld[i], t2 = (shape.halfExtents[i] - lo[i]) / ld[i];
                float sgn = -1.0f;
                if (t1 > t2) { std::swap(t1, t2); sgn = 1.0f; }
                if (t1 > tmin) { tmin = t1; axis = i; axisSign = sgn; }
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }
            if (tmax < 0.0f) return false;
            if (tmin < 0.0f) { t = 0.0f; normal = -d; return true; }
            if (tmin > maxDistance || axis < 0) return false;
            t = tmin;
            glm::vec3 ln(0.0f);
            ln[axis] = axisSign;
            normal = shape.rotation * ln;
            return true;
        }
        Convex ray;
        ray.kind = Convex::Kind::Point;
        ray.center = o;
        glm::vec3 point;
        return Sweep(ray, d, maxDistance, shape, t, normal, point);
    }

    // ==================================================================== segments
    float ClosestSegmentSegment(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2, float& s, float& t, glm::vec3& c1, glm::vec3& c2) {
        // Ericson 5.1.9
        const glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
        const float a = Length2(d1), e = Length2(d2), f = Dot(d2, r);
        const float eps = 1e-12f;
        if (a <= eps && e <= eps) { s = t = 0.0f; c1 = p1; c2 = p2; return Length2(c1 - c2); }
        if (a <= eps) { s = 0.0f; t = std::clamp(f / e, 0.0f, 1.0f); }
        else {
            const float c = Dot(d1, r);
            if (e <= eps) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else {
                const float b = Dot(d1, d2), denom = a * e - b * b;
                s = denom > eps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
                else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
            }
        }
        c1 = p1 + d1 * s;
        c2 = p2 + d2 * t;
        return Length2(c1 - c2);
    }
}
