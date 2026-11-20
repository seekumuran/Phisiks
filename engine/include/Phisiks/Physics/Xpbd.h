#pragma once
// Extended Position-Based Dynamics (XPBD): data model, builders and the CPU reference solver.
// The GPU solver (Phisiks::Physics::Advanced, compute shaders) runs the same algorithm on the same
// data: small-step XPBD (one iteration per substep, Lagrange multipliers reset each substep) with
// constraints graph-coloured so that every colour can be solved in parallel.
//
//   Distance  C = |x1 - x0| - rest            (ropes, cloth, soft-body edges; compliance alpha)
//   Volume    C = V(x0..x3) - restVolume      (tetrahedra of soft bodies)
// A distance constraint of compliance alpha holding a mass m under gravity g settles at a stretch
// of exactly alpha * m * g, which the tests use as an analytic reference.

#include "Phisiks/Physics/Fields.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace Phisiks::Physics::Xpbd {
    enum ConstraintType : uint32_t { Distance = 0, Volume = 1 };

    struct Constraint {
        uint32_t type = Distance;
        uint32_t i[4] = { 0, 0, 0, 0 };
        float rest = 0.0f;           // rest length or signed rest volume
        float compliance = 0.0f;     // inverse stiffness (m/N for distance)
    };

    enum ColliderType : uint32_t { Plane = 0, Sphere = 1, Box = 2 };
    struct Collider {
        uint32_t type = Plane;
        glm::vec3 center{ 0.0f };          // sphere/box centre, or a point on the plane
        glm::vec3 normal{ 0.0f, 1.0f, 0.0f };   // plane normal
        float radius = 0.0f;               // sphere
        glm::vec3 halfExtents{ 0.0f };     // box
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    struct PlacedField {
        ForceField field;
        glm::vec3 origin{ 0.0f };
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
    };

    struct Model {
        std::vector<glm::vec4> x;            // position, w = inverse mass (0 = pinned)
        std::vector<glm::vec3> v;
        std::vector<Constraint> constraints; // grouped by colour after Colorize()
        std::vector<uint32_t> colorStart;    // colour k = [colorStart[k], colorStart[k+1])
        std::vector<uint32_t> triangles;     // render surface (counter-clockwise outward); empty for ropes
        float particleRadius = 0.02f;
        float damping = 0.5f;                // 1/s, linear velocity damping
        float friction = 0.4f;               // collision friction (fraction of tangential motion removed)
    };

    // Builders (masses are spread evenly over the particles).
    Model MakeRope(const glm::vec3& start, const glm::vec3& end, int segments, float mass, float compliance, bool pinStart);
    // Cloth in the plane spanned by `right` (width) and `down` (height) from corner `origin`; pins the
    // two top corners if requested. Bending uses distance constraints across two particles.
    Model MakeCloth(const glm::vec3& origin, const glm::vec3& right, const glm::vec3& down, int resX, int resY,
                    float mass, float stretchCompliance, float bendCompliance, bool pinTopCorners);
    // Soft box: (res+1)^3 lattice, 5 tetrahedra per cell, distance constraints on all tet edges and a
    // volume constraint per tet.
    Model MakeSoftBox(const glm::vec3& center, const glm::vec3& size, int res, float mass, float edgeCompliance, float volumeCompliance);

    void Colorize(Model& model);                 // greedy graph colouring, constraints regrouped by colour
    bool ValidateColoring(const Model& model);   // no particle appears twice within one colour

    struct StepParams {
        float dt = 1.0f / 60.0f;
        int substeps = 10;
        glm::vec3 gravity{ 0.0f, -9.81f, 0.0f };
        const PlacedField* fields = nullptr;
        size_t fieldCount = 0;
        const Collider* colliders = nullptr;
        size_t colliderCount = 0;
    };
    void Step(Model& model, const StepParams& params);   // CPU reference

    float TetVolume(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d);   // signed
    float TotalVolume(const Model& model);               // sum of |tet volumes|
    float MaxRelativeStretch(const Model& model);        // max |L - rest| / rest over distance constraints
}
