#pragma once
// Engine-private: convex collision primitives of the native rigid-body backend.
//   Convex      a "core" shape (point, segment, oriented box, point cloud, triangle) plus a radius:
//               sphere = point + r, capsule = segment + r, box / hull / triangle = polytope + 0.
//   Gjk         distance and closest points between two cores (Gilbert-Johnson-Keerthi with
//               Voronoi-region simplex reduction).
//   Epa         penetration normal/depth of two overlapping cores (expanding polytope).
//   Generate*   contact points for a shape pair (normal from A to B, depth > 0 = penetrating,
//               depth < 0 = speculative gap).
// Written from the textbook descriptions (Ericson, "Real-Time Collision Detection"; van den
// Bergen, "Collision Detection in Interactive 3D Environments").

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace Phisiks::Physics::Native {

    struct Convex {
        enum class Kind : uint8_t { Point, Segment, Box, Points, Triangle } kind = Kind::Point;
        glm::vec3 center{ 0.0f };          // Point / Box / Points: origin of the core
        glm::mat3 rotation{ 1.0f };        // Box / Points: local -> world
        glm::vec3 halfExtents{ 0.0f };     // Box
        glm::vec3 a{ 0.0f }, b{ 0.0f }, c{ 0.0f };   // Segment: a-b, Triangle: a-b-c (world)
        const std::vector<glm::vec3>* points = nullptr;   // Points (local)
        float radius = 0.0f;

        glm::vec3 Support(const glm::vec3& direction) const;   // core only (no radius)
        glm::vec3 Centroid() const;
        bool IsRound() const { return kind == Kind::Point || kind == Kind::Segment; }
    };

    struct GjkResult {
        float distance = 0.0f;             // between the cores
        glm::vec3 pointA{ 0.0f }, pointB{ 0.0f };
        bool overlap = false;
        // terminal simplex (for EPA)
        glm::vec3 simplexA[4], simplexB[4];
        int simplexCount = 0;
    };
    GjkResult Gjk(const Convex& a, const Convex& b);

    struct Penetration {
        glm::vec3 normal{ 0.0f, 1.0f, 0.0f };   // A -> B
        float depth = 0.0f;                     // cores only
        glm::vec3 pointA{ 0.0f }, pointB{ 0.0f };
    };
    bool Epa(const Convex& a, const Convex& b, const GjkResult& gjk, Penetration& out);

    struct ContactPoint {
        glm::vec3 pointA{ 0.0f };          // on A's surface (world)
        glm::vec3 pointB{ 0.0f };          // on B's surface (world)
        float depth = 0.0f;
    };
    struct ContactSet {
        glm::vec3 normal{ 0.0f, 1.0f, 0.0f };   // A -> B
        ContactPoint points[8];
        int count = 0;
        bool incremental = false;          // single-point result: accumulate into a persistent manifold
    };
    // Contacts closer than `margin` (speculative) are reported. Returns false if none.
    bool GenerateContacts(const Convex& a, const Convex& b, float margin, ContactSet& out);
    // Keeps at most 4 points (deepest + widest spread).
    void ReduceContacts(ContactSet& set);

    // Signed distance between shapes (negative = penetrating) with the closest points / normal A->B.
    bool Distance(const Convex& a, const Convex& b, float& separation, glm::vec3& normal, glm::vec3& pointA, glm::vec3& pointB);

    // Ray against a convex (with radius). `t` along the unit direction.
    bool RaycastConvex(const Convex& shape, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float& t, glm::vec3& normal);
    bool RaycastTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float maxDistance, float& t, glm::vec3& normal);
    // Sweeps `moving` along the unit `direction` against `target` (conservative advancement).
    // Initial overlap reports t = 0 and normal = -direction.
    bool Sweep(const Convex& moving, const glm::vec3& direction, float maxDistance, const Convex& target, float& t, glm::vec3& normal, glm::vec3& point);

    // Closest points between segments p1-q1 and p2-q2 (s, t parameters).
    float ClosestSegmentSegment(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2, float& s, float& t, glm::vec3& c1, glm::vec3& c2);
}
