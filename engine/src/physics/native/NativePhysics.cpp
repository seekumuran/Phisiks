// Phisiks::Physics implementation: native backend (no third-party physics). Used where PhysX is not
// available (macOS, Linux without a PhysX package) and selectable everywhere with
// -DPHISIKS_PHYSICS_BACKEND=Native.
//
//   bodies     boxes, spheres, Y-up capsules, convex point clouds (support-mapped), static/kinematic
//              triangle meshes; mass and inertia from collider volumes
//   step       sub-stepped: speculative contacts -> sequential impulses with warm starting,
//              Coulomb friction (static/dynamic), restitution pass, split-impulse position
//              correction, per-body sleeping
//   events     collision enter/exit per body pair, trigger enter/exit (characters included)
//   queries    raycasts, sphere/box/capsule sweeps (conservative advancement), overlaps
//   characters capsule controller: up / side / down passes, step offset, slope limit, pushing
//
// Deterministic: single-threaded, fixed iteration order (bodies by index, manifolds by key).
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Core/Log.h"
#include "physics/native/Collision.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

namespace Phisiks::Physics {
    namespace {
        using Native::Convex;
        using Native::ContactSet;

        // ------------------------------------------------------------------ ids (same layout as the PhysX backend)
        constexpr uint64_t TAG_BODY = 1, TAG_CHARACTER = 2;
        uint64_t MakeId(uint64_t tag, uint32_t generation, uint32_t index) {
            return (tag << 62) | (uint64_t(generation & 0x3FFFFFFFu) << 32) | uint64_t(index + 1);
        }
        uint64_t TagOf(uint64_t value) { return value >> 62; }
        uint32_t IndexOf(uint64_t value) { return uint32_t(value & 0xFFFFFFFFu) - 1u; }
        uint32_t GenerationOf(uint64_t value) { return uint32_t((value >> 32) & 0x3FFFFFFFu); }

        // ------------------------------------------------------------------ tuning
        constexpr int VELOCITY_ITERATIONS = 10;
        constexpr int POSITION_ITERATIONS = 4;
        constexpr float LINEAR_SLOP = 0.003f;
        constexpr float BAUMGARTE = 0.25f;
        constexpr float MAX_CORRECTION_SPEED = 3.0f;    // m/s of position correction
        constexpr float BOUNCE_THRESHOLD = 1.0f;        // m/s approach speed for restitution
        constexpr float STICK_SPEED = 0.05f;            // below: static friction coefficient
        constexpr float SLEEP_LINEAR = 0.05f, SLEEP_ANGULAR = 0.08f, SLEEP_TIME = 0.5f;
        constexpr float MAX_ANGULAR_SPEED = 100.0f;
        constexpr float CONTACT_MARGIN = 0.02f;         // base speculative distance
        constexpr float CHARACTER_SKIN = 0.01f;

        struct Aabb {
            glm::vec3 lo{ FLT_MAX }, hi{ -FLT_MAX };
            bool Overlaps(const Aabb& o) const { return lo.x <= o.hi.x && hi.x >= o.lo.x && lo.y <= o.hi.y && hi.y >= o.lo.y && lo.z <= o.hi.z && hi.z >= o.lo.z; }
            Aabb Expanded(float m) const { return { lo - glm::vec3(m), hi + glm::vec3(m) }; }
            void Add(const glm::vec3& p) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            void Add(const Aabb& o) { lo = glm::min(lo, o.lo); hi = glm::max(hi, o.hi); }
        };

        Aabb TransformedBox(const glm::vec3& center, const glm::mat3& rotation, const glm::vec3& localLo, const glm::vec3& localHi) {
            const glm::vec3 c = 0.5f * (localLo + localHi), e = 0.5f * (localHi - localLo);
            const glm::vec3 wc = center + rotation * c;
            glm::vec3 we(0.0f);
            for (int i = 0; i < 3; ++i) we += glm::abs(rotation[i]) * e[i];
            return { wc - we, wc + we };
        }

        struct Shape {
            ShapeType type = ShapeType::Box;
            glm::vec3 halfExtents{ 0.5f };
            float radius = 0.5f, halfHeight = 0.5f;
            std::vector<glm::vec3> points;       // convex: local points
            std::vector<glm::vec3> vertices;     // triangle mesh (local)
            std::vector<uint32_t> indices;
            glm::vec3 localLo{ 0.0f }, localHi{ 0.0f };   // points / mesh local bounds
            glm::vec3 localPosition{ 0.0f };
            glm::quat localRotation{ 1, 0, 0, 0 };
            PhysicsMaterial material;
            bool trigger = false;
            // world (updated per sub-step)
            glm::vec3 position{ 0.0f };
            glm::mat3 rotation{ 1.0f };
            Aabb box;
        };

        struct Body {
            BodyType type = BodyType::Static;
            Layer layer = LAYER_DEFAULT;
            uint64_t userData = 0;
            uint32_t generation = 1;
            bool alive = false;
            int character = -1;                  // >= 0: proxy of that character controller
            std::vector<Shape> shapes;
            glm::vec3 position{ 0.0f };          // actor frame
            glm::quat rotation{ 1, 0, 0, 0 };
            glm::vec3 com{ 0.0f };               // local centre of mass
            glm::vec3 v{ 0.0f }, w{ 0.0f };      // velocity of the centre of mass
            float mass = 0.0f, invMass = 0.0f;
            glm::mat3 invInertiaLocal{ 0.0f }, invInertia{ 0.0f };
            float linearDamping = 0.0f, angularDamping = 0.05f;
            bool gravity = true;
            bool sleeping = false;
            float sleepTime = 0.0f;
            bool hasTarget = false;
            glm::vec3 targetPosition{ 0.0f };
            glm::quat targetRotation{ 1, 0, 0, 0 };
            glm::vec3 force{ 0.0f }, torque{ 0.0f };
            glm::vec3 pv{ 0.0f }, pw{ 0.0f };    // split-impulse pseudo velocities
            Aabb box;

            bool Dynamic() const { return type == BodyType::Dynamic; }
            bool Awake() const { return type == BodyType::Dynamic && !sleeping; }
            glm::vec3 WorldCom() const { return position + rotation * com; }
            float InvMass() const { return Awake() ? invMass : 0.0f; }
            glm::mat3 InvInertia() const { return Awake() ? invInertia : glm::mat3(0.0f); }
        };

        struct Character {
            uint32_t body = 0;                   // proxy body index
            float radius = 0.3f, height = 1.1f, stepOffset = 0.3f, slopeCos = 0.64f, pushForce = 150.0f;
            Layer layer = LAYER_DEFAULT;
            uint64_t userData = 0;
            uint32_t generation = 1;
            bool alive = false;
            glm::vec3 foot{ 0.0f };
        };

        struct ManifoldPoint {
            glm::vec3 localA{ 0.0f }, localB{ 0.0f };   // actor frames
            glm::vec3 pointA{ 0.0f }, pointB{ 0.0f };
            float depth = 0.0f;
            float jn = 0.0f, jp = 0.0f;
            glm::vec3 jt{ 0.0f };                        // accumulated friction impulse (world)
            // solver
            glm::vec3 rA{ 0.0f }, rB{ 0.0f };
            float massN = 0.0f, massT1 = 0.0f, massT2 = 0.0f;
            float relVn = 0.0f, jt1 = 0.0f, jt2 = 0.0f;
        };

        struct ManifoldKey {
            uint32_t a, b;
            uint32_t shapeA, shapeB;
            uint32_t triangle;
            bool operator<(const ManifoldKey& o) const { return std::tie(a, b, shapeA, shapeB, triangle) < std::tie(o.a, o.b, o.shapeA, o.shapeB, o.triangle); }
        };

        struct Manifold {
            uint32_t a = 0, b = 0;
            glm::vec3 normal{ 0, 1, 0 };
            glm::vec3 t1{ 1, 0, 0 }, t2{ 0, 0, 1 };
            ManifoldPoint points[4];
            int count = 0;
            float staticFriction = 0.6f, dynamicFriction = 0.5f, restitution = 0.0f, friction = 0.5f;
            bool touched = false;
        };

        struct PairInfo {
            glm::vec3 point{ 0.0f }, normal{ 0.0f };
            float impactSpeed = 0.0f, impulse = 0.0f;
        };

        struct State {
            bool initialized = false;
            Settings settings;
            std::vector<Body> bodies;
            std::vector<uint32_t> freeBodies;
            std::vector<Character> characters;
            std::vector<uint32_t> freeCharacters;
            std::array<LayerMask, 32> collisionMatrix{};
            std::map<ManifoldKey, Manifold> manifolds;
            std::map<std::pair<uint32_t, uint32_t>, PairInfo> touching, touchingThisStep;
            std::set<std::pair<uint32_t, uint32_t>> triggerPairs;   // (trigger body, other body)
            std::vector<CollisionEvent> collisionEvents;
            std::vector<TriggerEvent> triggerEvents;
            uint64_t steps = 0;
        };
        State g;

        // ------------------------------------------------------------------ helpers
        Body* Resolve(BodyId id) {
            if (!g.initialized || TagOf(id.value) != TAG_BODY) return nullptr;
            const uint32_t index = IndexOf(id.value);
            if (index >= g.bodies.size()) return nullptr;
            Body& b = g.bodies[index];
            return (b.alive && b.character < 0 && b.generation == GenerationOf(id.value)) ? &b : nullptr;
        }
        Character* Resolve(CharacterId id) {
            if (!g.initialized || TagOf(id.value) != TAG_CHARACTER) return nullptr;
            const uint32_t index = IndexOf(id.value);
            if (index >= g.characters.size()) return nullptr;
            Character& c = g.characters[index];
            return (c.alive && c.generation == GenerationOf(id.value)) ? &c : nullptr;
        }
        BodyId IdOf(uint32_t index) { return BodyId{ MakeId(TAG_BODY, g.bodies[index].generation, index) }; }

        void Wake(Body& b) {
            if (b.type != BodyType::Dynamic) return;
            b.sleeping = false;
            b.sleepTime = 0.0f;
        }

        void UpdateWorld(Body& b) {
            const glm::mat3 r = glm::mat3_cast(b.rotation);
            b.box = Aabb{};
            for (Shape& s : b.shapes) {
                s.rotation = r * glm::mat3_cast(s.localRotation);
                s.position = b.position + r * s.localPosition;
                switch (s.type) {
                    case ShapeType::Box: s.box = TransformedBox(s.position, s.rotation, -s.halfExtents, s.halfExtents); break;
                    case ShapeType::Sphere: s.box = { s.position - glm::vec3(s.radius), s.position + glm::vec3(s.radius) }; break;
                    case ShapeType::Capsule: {
                        const glm::vec3 axis = glm::abs(s.rotation[1]) * s.halfHeight + glm::vec3(s.radius);
                        s.box = { s.position - axis, s.position + axis };
                        break;
                    }
                    default: s.box = TransformedBox(s.position, s.rotation, s.localLo, s.localHi); break;
                }
                b.box.Add(s.box);
            }
            if (b.shapes.empty()) b.box = { b.position, b.position };
            b.invInertia = r * b.invInertiaLocal * glm::transpose(r);
        }

        Convex ConvexOf(const Shape& s) {
            Convex c;
            switch (s.type) {
                case ShapeType::Box: c.kind = Convex::Kind::Box; c.center = s.position; c.rotation = s.rotation; c.halfExtents = s.halfExtents; break;
                case ShapeType::Sphere: c.kind = Convex::Kind::Point; c.center = s.position; c.radius = s.radius; break;
                case ShapeType::Capsule: {
                    c.kind = Convex::Kind::Segment;
                    const glm::vec3 axis = s.rotation[1] * s.halfHeight;
                    c.a = s.position + axis; c.b = s.position - axis; c.center = s.position; c.radius = s.radius;
                    break;
                }
                case ShapeType::ConvexMesh: c.kind = Convex::Kind::Points; c.center = s.position; c.rotation = s.rotation; c.points = &s.points; break;
                case ShapeType::TriangleMesh: break;
            }
            return c;
        }

        Convex TriangleOf(const Shape& s, size_t t) {
            Convex c;
            c.kind = Convex::Kind::Triangle;
            c.a = s.position + s.rotation * s.vertices[s.indices[3 * t + 0]];
            c.b = s.position + s.rotation * s.vertices[s.indices[3 * t + 1]];
            c.c = s.position + s.rotation * s.vertices[s.indices[3 * t + 2]];
            return c;
        }
        Aabb BoundsOf(const Convex& c) {
            Aabb box;
            box.Add(c.a); box.Add(c.b); box.Add(c.c);
            return box;
        }

        // Volume and inertia (about the shape centre, shape frame) for mass distribution.
        void ShapeMass(const Shape& s, float& volume, glm::vec3& diagonalPerMass) {
            switch (s.type) {
                case ShapeType::Box: {
                    const glm::vec3 e = 2.0f * s.halfExtents;
                    volume = e.x * e.y * e.z;
                    diagonalPerMass = glm::vec3(e.y * e.y + e.z * e.z, e.x * e.x + e.z * e.z, e.x * e.x + e.y * e.y) / 12.0f;
                    break;
                }
                case ShapeType::Sphere:
                    volume = 4.18879f * s.radius * s.radius * s.radius;
                    diagonalPerMass = glm::vec3(0.4f * s.radius * s.radius);
                    break;
                case ShapeType::Capsule: {
                    const float r = s.radius, h = 2.0f * s.halfHeight;
                    const float vc = 3.14159265f * r * r * h, vs = 4.18879f * r * r * r;
                    volume = vc + vs;
                    const float mc = vc / volume, ms = vs / volume;   // per unit mass
                    const float iy = mc * 0.5f * r * r + ms * 0.4f * r * r;
                    const float ix = mc * (r * r / 4.0f + h * h / 12.0f) + ms * (0.4f * r * r + h * h / 4.0f + 3.0f * h * r / 8.0f);
                    diagonalPerMass = glm::vec3(ix, iy, ix);
                    break;
                }
                default: {   // convex: its bounding box
                    const glm::vec3 e = glm::max(s.localHi - s.localLo, glm::vec3(1e-4f));
                    volume = e.x * e.y * e.z;
                    diagonalPerMass = glm::vec3(e.y * e.y + e.z * e.z, e.x * e.x + e.z * e.z, e.x * e.x + e.y * e.y) / 12.0f;
                    break;
                }
            }
        }

        void ComputeMass(Body& b, float mass) {
            b.mass = std::max(mass, 1e-4f);
            b.invMass = b.type == BodyType::Dynamic ? 1.0f / b.mass : 0.0f;
            b.com = glm::vec3(0.0f);
            b.invInertiaLocal = glm::mat3(0.0f);
            if (b.type != BodyType::Dynamic) return;
            float total = 0.0f;
            std::vector<float> volumes(b.shapes.size(), 0.0f);
            std::vector<glm::vec3> diag(b.shapes.size());
            const bool anySolid = std::any_of(b.shapes.begin(), b.shapes.end(), [](const Shape& s) { return !s.trigger; });
            for (size_t i = 0; i < b.shapes.size(); ++i) {
                if (anySolid && b.shapes[i].trigger) continue;
                ShapeMass(b.shapes[i], volumes[i], diag[i]);
                total += volumes[i];
            }
            if (total <= 0.0f) { b.invInertiaLocal = glm::mat3(1.0f / b.mass); return; }
            glm::vec3 centre(0.0f);
            for (size_t i = 0; i < b.shapes.size(); ++i) {
                glm::vec3 c = b.shapes[i].localPosition;
                if (b.shapes[i].type == ShapeType::ConvexMesh) c += b.shapes[i].localRotation * (0.5f * (b.shapes[i].localLo + b.shapes[i].localHi));
                centre += c * volumes[i];
            }
            b.com = centre / total;
            glm::mat3 inertia(0.0f);
            for (size_t i = 0; i < b.shapes.size(); ++i) {
                if (volumes[i] <= 0.0f) continue;
                const Shape& s = b.shapes[i];
                const float m = b.mass * volumes[i] / total;
                const glm::mat3 r = glm::mat3_cast(s.localRotation);
                glm::mat3 local(0.0f);
                local[0][0] = diag[i].x * m; local[1][1] = diag[i].y * m; local[2][2] = diag[i].z * m;
                glm::vec3 c = s.localPosition;
                if (s.type == ShapeType::ConvexMesh) c += s.localRotation * (0.5f * (s.localLo + s.localHi));
                const glm::vec3 d = c - b.com;
                inertia += r * local * glm::transpose(r) + m * (glm::dot(d, d) * glm::mat3(1.0f) - glm::outerProduct(d, d));
            }
            b.invInertiaLocal = glm::inverse(inertia);
        }

        float RelativeSpeed(const Body& a, const Body& b) {
            return glm::length(b.v - a.v) + glm::length(b.w - a.w) * 0.5f;
        }

        bool LayerPairCollides(const Body& a, const Body& b) {
            return (LayerBit(a.layer) & g.collisionMatrix[b.layer]) != 0 && (LayerBit(b.layer) & g.collisionMatrix[a.layer]) != 0;
        }

        // ------------------------------------------------------------------ manifolds
        void UpdateManifold(Manifold& m, const Body& A, const Body& B, const ContactSet& set, float margin) {
            const glm::quat qa = glm::conjugate(A.rotation), qb = glm::conjugate(B.rotation);
            auto makePoint = [&](const Native::ContactPoint& c) {
                ManifoldPoint p;
                p.pointA = c.pointA; p.pointB = c.pointB; p.depth = c.depth;
                p.localA = qa * (c.pointA - A.position);
                p.localB = qb * (c.pointB - B.position);
                return p;
            };
            ManifoldPoint old[4];
            const int oldCount = m.count;
            for (int i = 0; i < oldCount; ++i) old[i] = m.points[i];
            auto warm = [&](ManifoldPoint& p) {
                float best = 0.02f * 0.02f;
                for (int i = 0; i < oldCount; ++i) {
                    const glm::vec3 d = old[i].localA - p.localA;
                    const float d2 = glm::dot(d, d);
                    if (d2 < best) { best = d2; p.jn = old[i].jn; p.jt = old[i].jt; }
                }
            };
            m.normal = set.normal;
            if (!set.incremental) {
                m.count = 0;
                for (int i = 0; i < set.count && m.count < 4; ++i) {
                    ManifoldPoint p = makePoint(set.points[i]);
                    warm(p);
                    m.points[m.count++] = p;
                }
                return;
            }
            // persistent manifold: refresh old points, drop the stale ones, add the new one
            ManifoldPoint kept[5];
            int count = 0;
            for (int i = 0; i < oldCount; ++i) {
                ManifoldPoint p = old[i];
                p.pointA = A.position + A.rotation * p.localA;
                p.pointB = B.position + B.rotation * p.localB;
                const glm::vec3 d = p.pointA - p.pointB;
                p.depth = glm::dot(d, set.normal);
                const glm::vec3 tangential = d - set.normal * p.depth;
                if (p.depth < -margin || glm::dot(tangential, tangential) > 0.02f * 0.02f) continue;
                kept[count++] = p;
            }
            ManifoldPoint fresh = makePoint(set.points[0]);
            int replace = -1;
            for (int i = 0; i < count; ++i) {
                const glm::vec3 d = kept[i].pointB - fresh.pointB;
                if (glm::dot(d, d) < 0.02f * 0.02f) { replace = i; break; }
            }
            if (replace >= 0) { fresh.jn = kept[replace].jn; fresh.jt = kept[replace].jt; kept[replace] = fresh; }
            else kept[count++] = fresh;
            if (count > 4) {
                ContactSet tmp;
                tmp.normal = set.normal;
                tmp.count = count;
                for (int i = 0; i < count; ++i) { tmp.points[i].pointA = kept[i].pointA; tmp.points[i].pointB = kept[i].pointB; tmp.points[i].depth = kept[i].depth; }
                Native::ReduceContacts(tmp);
                ManifoldPoint reduced[4];
                for (int k = 0; k < tmp.count; ++k) {
                    for (int i = 0; i < count; ++i) {
                        if (kept[i].pointB == tmp.points[k].pointB) { reduced[k] = kept[i]; break; }
                    }
                }
                count = tmp.count;
                for (int i = 0; i < count; ++i) kept[i] = reduced[i];
            }
            m.count = count;
            for (int i = 0; i < count; ++i) m.points[i] = kept[i];
        }

        float Margin(const Body& b, float h) {
            return CONTACT_MARGIN + (b.type == BodyType::Static ? 0.0f : (glm::length(b.v) + glm::length(b.w) * 0.5f) * h);
        }

        void CollideShapes(uint32_t ia, uint32_t ib, float h) {
            Body& A = g.bodies[ia];
            Body& B = g.bodies[ib];
            const float margin = Margin(A, h) + Margin(B, h);
            for (uint32_t sa = 0; sa < A.shapes.size(); ++sa) {
                const Shape& shapeA = A.shapes[sa];
                if (shapeA.trigger) continue;
                for (uint32_t sb = 0; sb < B.shapes.size(); ++sb) {
                    const Shape& shapeB = B.shapes[sb];
                    if (shapeB.trigger || !shapeA.box.Expanded(margin).Overlaps(shapeB.box)) continue;
                    if (shapeA.type == ShapeType::TriangleMesh && shapeB.type == ShapeType::TriangleMesh) continue;
                    auto collide = [&](const Convex& ca, const Convex& cb, uint32_t triangle) {
                        ContactSet set;
                        if (!Native::GenerateContacts(ca, cb, margin, set) || set.count == 0) return;
                        Manifold& m = g.manifolds[ManifoldKey{ ia, ib, sa, sb, triangle }];
                        m.a = ia; m.b = ib;
                        m.staticFriction = 0.5f * (shapeA.material.staticFriction + shapeB.material.staticFriction);
                        m.dynamicFriction = 0.5f * (shapeA.material.dynamicFriction + shapeB.material.dynamicFriction);
                        m.restitution = 0.5f * (shapeA.material.restitution + shapeB.material.restitution);
                        UpdateManifold(m, A, B, set, margin);
                        m.touched = m.count > 0;
                    };
                    if (shapeA.type == ShapeType::TriangleMesh || shapeB.type == ShapeType::TriangleMesh) {
                        const bool meshIsA = shapeA.type == ShapeType::TriangleMesh;
                        const Shape& mesh = meshIsA ? shapeA : shapeB;
                        const Convex other = ConvexOf(meshIsA ? shapeB : shapeA);
                        const Aabb otherBox = (meshIsA ? shapeB : shapeA).box.Expanded(margin);
                        for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
                            const Convex tri = TriangleOf(mesh, t);
                            if (!BoundsOf(tri).Overlaps(otherBox)) continue;
                            if (meshIsA) collide(tri, other, uint32_t(t));
                            else collide(other, tri, uint32_t(t));
                        }
                    }
                    else collide(ConvexOf(shapeA), ConvexOf(shapeB), 0xFFFFFFFFu);
                }
            }
        }

        bool ShouldCollide(const Body& a, const Body& b) {
            if (!a.alive || !b.alive) return false;
            const bool movableA = a.type == BodyType::Dynamic, movableB = b.type == BodyType::Dynamic;
            if (!movableA && !movableB) return false;
            if (!a.Awake() && !b.Awake()) {
                // a sleeping body only wakes when something moving touches it
                const bool kinematicMoving = (a.type == BodyType::Kinematic && (a.hasTarget || glm::length(a.v) > 0.0f)) ||
                                             (b.type == BodyType::Kinematic && (b.hasTarget || glm::length(b.v) > 0.0f));
                if (!kinematicMoving) return false;
            }
            return LayerPairCollides(a, b);
        }

        void FindContacts(float h) {
            for (auto& [key, m] : g.manifolds) m.touched = false;
            // sweep and prune on x
            std::vector<uint32_t> order;
            order.reserve(g.bodies.size());
            for (uint32_t i = 0; i < g.bodies.size(); ++i) if (g.bodies[i].alive && !g.bodies[i].shapes.empty()) order.push_back(i);
            std::vector<Aabb> boxes(g.bodies.size());
            for (uint32_t i : order) boxes[i] = g.bodies[i].box.Expanded(Margin(g.bodies[i], h));
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return boxes[a].lo.x < boxes[b].lo.x || (boxes[a].lo.x == boxes[b].lo.x && a < b); });
            std::vector<std::pair<uint32_t, uint32_t>> pairs;
            for (size_t i = 0; i < order.size(); ++i) {
                const uint32_t a = order[i];
                for (size_t j = i + 1; j < order.size(); ++j) {
                    const uint32_t b = order[j];
                    if (boxes[b].lo.x > boxes[a].hi.x) break;
                    if (!boxes[a].Overlaps(boxes[b])) continue;
                    pairs.emplace_back(std::min(a, b), std::max(a, b));
                }
            }
            std::sort(pairs.begin(), pairs.end());
            for (const auto& [a, b] : pairs) {
                Body& A = g.bodies[a];
                Body& B = g.bodies[b];
                if (!ShouldCollide(A, B)) continue;
                // wake a sleeping body touched by a moving one
                if (A.Dynamic() && A.sleeping && !B.sleeping && B.type != BodyType::Static && RelativeSpeed(A, B) > SLEEP_LINEAR) Wake(A);
                if (B.Dynamic() && B.sleeping && !A.sleeping && A.type != BodyType::Static && RelativeSpeed(A, B) > SLEEP_LINEAR) Wake(B);
                CollideShapes(a, b, h);
            }
            for (auto it = g.manifolds.begin(); it != g.manifolds.end();) {
                if (!it->second.touched) it = g.manifolds.erase(it);
                else ++it;
            }
        }

        // ------------------------------------------------------------------ solver
        void ApplyImpulse(Body& a, Body& b, const glm::vec3& rA, const glm::vec3& rB, const glm::vec3& impulse) {
            if (a.Awake()) { a.v -= impulse * a.invMass; a.w -= a.invInertia * glm::cross(rA, impulse); }
            if (b.Awake()) { b.v += impulse * b.invMass; b.w += b.invInertia * glm::cross(rB, impulse); }
        }
        void ApplyPseudo(Body& a, Body& b, const glm::vec3& rA, const glm::vec3& rB, const glm::vec3& impulse) {
            if (a.Awake()) { a.pv -= impulse * a.invMass; a.pw -= a.invInertia * glm::cross(rA, impulse); }
            if (b.Awake()) { b.pv += impulse * b.invMass; b.pw += b.invInertia * glm::cross(rB, impulse); }
        }
        glm::vec3 RelativeVelocity(const Body& a, const Body& b, const glm::vec3& rA, const glm::vec3& rB) {
            return (b.v + glm::cross(b.w, rB)) - (a.v + glm::cross(a.w, rA));
        }
        float EffectiveMass(const Body& a, const Body& b, const glm::vec3& rA, const glm::vec3& rB, const glm::vec3& dir) {
            const glm::vec3 ca = glm::cross(rA, dir), cb = glm::cross(rB, dir);
            const float k = a.InvMass() + b.InvMass() + glm::dot(ca, a.InvInertia() * ca) + glm::dot(cb, b.InvInertia() * cb);
            return k > 1e-12f ? 1.0f / k : 0.0f;
        }
        glm::vec3 Perpendicular(const glm::vec3& n) {
            const glm::vec3 axis = std::fabs(n.x) < 0.57f ? glm::vec3(1, 0, 0) : (std::fabs(n.y) < 0.57f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1));
            return glm::normalize(glm::cross(n, axis));
        }

        void Solve(float h) {
            std::vector<Manifold*> list;
            list.reserve(g.manifolds.size());
            for (auto& [key, m] : g.manifolds) list.push_back(&m);
            // prepare + warm start
            for (Manifold* m : list) {
                Body& A = g.bodies[m->a];
                Body& B = g.bodies[m->b];
                const glm::vec3 comA = A.WorldCom(), comB = B.WorldCom();
                glm::vec3 slip(0.0f);
                for (int i = 0; i < m->count; ++i) {
                    ManifoldPoint& p = m->points[i];
                    p.rA = p.pointA - comA;
                    p.rB = p.pointB - comB;
                    const glm::vec3 vr = RelativeVelocity(A, B, p.rA, p.rB);
                    p.relVn = glm::dot(vr, m->normal);
                    slip += vr - m->normal * p.relVn;
                }
                const float slipSpeed = m->count ? glm::length(slip) / float(m->count) : 0.0f;
                m->t1 = slipSpeed > 1e-3f ? glm::normalize(slip) : Perpendicular(m->normal);
                if (std::fabs(glm::dot(m->t1, m->normal)) > 1e-3f) m->t1 = Perpendicular(m->normal);
                m->t2 = glm::cross(m->normal, m->t1);
                m->friction = slipSpeed < STICK_SPEED ? m->staticFriction : m->dynamicFriction;
                for (int i = 0; i < m->count; ++i) {
                    ManifoldPoint& p = m->points[i];
                    p.massN = EffectiveMass(A, B, p.rA, p.rB, m->normal);
                    p.massT1 = EffectiveMass(A, B, p.rA, p.rB, m->t1);
                    p.massT2 = EffectiveMass(A, B, p.rA, p.rB, m->t2);
                    p.jt1 = glm::dot(p.jt, m->t1);
                    p.jt2 = glm::dot(p.jt, m->t2);
                    p.jp = 0.0f;
                    ApplyImpulse(A, B, p.rA, p.rB, m->normal * p.jn + m->t1 * p.jt1 + m->t2 * p.jt2);
                }
            }
            // velocity iterations
            for (int it = 0; it < VELOCITY_ITERATIONS; ++it) {
                for (Manifold* m : list) {
                    Body& A = g.bodies[m->a];
                    Body& B = g.bodies[m->b];
                    for (int i = 0; i < m->count; ++i) {
                        ManifoldPoint& p = m->points[i];
                        const float maxFriction = m->friction * p.jn;
                        {
                            const glm::vec3 vr = RelativeVelocity(A, B, p.rA, p.rB);
                            const float l1 = -p.massT1 * glm::dot(vr, m->t1);
                            const float n1 = std::clamp(p.jt1 + l1, -maxFriction, maxFriction);
                            ApplyImpulse(A, B, p.rA, p.rB, m->t1 * (n1 - p.jt1));
                            p.jt1 = n1;
                        }
                        {
                            const glm::vec3 vr = RelativeVelocity(A, B, p.rA, p.rB);
                            const float l2 = -p.massT2 * glm::dot(vr, m->t2);
                            const float n2 = std::clamp(p.jt2 + l2, -maxFriction, maxFriction);
                            ApplyImpulse(A, B, p.rA, p.rB, m->t2 * (n2 - p.jt2));
                            p.jt2 = n2;
                        }
                        {
                            const glm::vec3 vr = RelativeVelocity(A, B, p.rA, p.rB);
                            const float vn = glm::dot(vr, m->normal);
                            const float target = p.depth < 0.0f ? p.depth / h : 0.0f;   // speculative: may close the gap
                            const float l = -p.massN * (vn - target);
                            const float nn = std::max(p.jn + l, 0.0f);
                            ApplyImpulse(A, B, p.rA, p.rB, m->normal * (nn - p.jn));
                            p.jn = nn;
                        }
                    }
                }
            }
            // restitution (relative to the approach speed before solving)
            for (Manifold* m : list) {
                if (m->restitution <= 0.0f) continue;
                Body& A = g.bodies[m->a];
                Body& B = g.bodies[m->b];
                for (int i = 0; i < m->count; ++i) {
                    ManifoldPoint& p = m->points[i];
                    if (p.relVn > -BOUNCE_THRESHOLD || p.jn <= 0.0f) continue;
                    const float vn = glm::dot(RelativeVelocity(A, B, p.rA, p.rB), m->normal);
                    const float l = -p.massN * (vn + m->restitution * p.relVn);
                    const float nn = std::max(p.jn + l, 0.0f);
                    ApplyImpulse(A, B, p.rA, p.rB, m->normal * (nn - p.jn));
                    p.jn = nn;
                }
            }
            // split-impulse position correction (pseudo velocities, no energy added)
            for (int it = 0; it < POSITION_ITERATIONS; ++it) {
                for (Manifold* m : list) {
                    Body& A = g.bodies[m->a];
                    Body& B = g.bodies[m->b];
                    for (int i = 0; i < m->count; ++i) {
                        ManifoldPoint& p = m->points[i];
                        const float error = p.depth - LINEAR_SLOP;
                        if (error <= 0.0f) continue;
                        const glm::vec3 vr = (B.pv + glm::cross(B.pw, p.rB)) - (A.pv + glm::cross(A.pw, p.rA));
                        const float target = std::min(BAUMGARTE * error / h, MAX_CORRECTION_SPEED);
                        const float l = -p.massN * (glm::dot(vr, m->normal) - target);
                        const float nn = std::max(p.jp + l, 0.0f);
                        ApplyPseudo(A, B, p.rA, p.rB, m->normal * (nn - p.jp));
                        p.jp = nn;
                    }
                }
            }
            for (Manifold* m : list) for (int i = 0; i < m->count; ++i) m->points[i].jt = m->t1 * m->points[i].jt1 + m->t2 * m->points[i].jt2;
        }

        void RecordTouches() {
            for (auto& [key, m] : g.manifolds) {
                bool touching = false;
                float impulse = 0.0f;
                int deepest = 0;
                for (int i = 0; i < m.count; ++i) {
                    if (m.points[i].jn > 0.0f || m.points[i].depth >= -1e-3f) touching = true;
                    impulse += m.points[i].jn;
                    if (m.points[i].depth > m.points[deepest].depth) deepest = i;
                }
                if (!touching) continue;
                const Body& A = g.bodies[m.a];
                const Body& B = g.bodies[m.b];
                if (A.character >= 0 || B.character >= 0) continue;
                const auto pair = std::make_pair(m.a, m.b);
                auto it = g.touchingThisStep.find(pair);
                if (it != g.touchingThisStep.end()) continue;
                PairInfo info;
                info.point = m.points[deepest].pointB;
                info.normal = m.normal;
                info.impactSpeed = std::fabs(m.points[deepest].relVn);
                info.impulse = impulse;
                g.touchingThisStep.emplace(pair, info);
            }
        }

        void Integrate(float h) {
            for (Body& b : g.bodies) {
                if (!b.alive) continue;
                if (b.type == BodyType::Kinematic) {
                    b.position += b.v * h;
                    const glm::quat spin(0.0f, b.w.x, b.w.y, b.w.z);
                    b.rotation = glm::normalize(b.rotation + 0.5f * h * spin * b.rotation);
                    UpdateWorld(b);
                    continue;
                }
                if (!b.Awake()) continue;
                const glm::vec3 com = b.WorldCom() + (b.v + b.pv) * h;
                const glm::vec3 w = b.w + b.pw;
                const glm::quat spin(0.0f, w.x, w.y, w.z);
                b.rotation = glm::normalize(b.rotation + 0.5f * h * spin * b.rotation);
                b.position = com - b.rotation * b.com;
                b.pv = b.pw = glm::vec3(0.0f);
                UpdateWorld(b);
                // sleeping
                if (glm::length(b.v) < SLEEP_LINEAR && glm::length(b.w) < SLEEP_ANGULAR && glm::length(b.force) == 0.0f) {
                    b.sleepTime += h;
                    if (b.sleepTime > SLEEP_TIME) { b.sleeping = true; b.v = b.w = glm::vec3(0.0f); }
                }
                else b.sleepTime = 0.0f;
            }
        }

        void IntegrateVelocities(float h) {
            for (Body& b : g.bodies) {
                if (!b.alive || !b.Awake()) continue;
                if (b.gravity) b.v += g.settings.gravity * h;
                b.v += b.force * (b.invMass * h);
                b.w += b.invInertia * b.torque * h;
                b.v *= std::max(0.0f, 1.0f - b.linearDamping * h);
                b.w *= std::max(0.0f, 1.0f - b.angularDamping * h);
                const float ws = glm::length(b.w);
                if (ws > MAX_ANGULAR_SPEED) b.w *= MAX_ANGULAR_SPEED / ws;
            }
        }

        // ------------------------------------------------------------------ queries
        struct QueryFilter {
            const QueryOptions& options;
            int ignoreBody = -1, ignoreProxy = -1;
            explicit QueryFilter(const QueryOptions& o) : options(o) {
                if (Body* b = Resolve(o.ignoreBody)) ignoreBody = int(b - g.bodies.data());
                if (Character* c = Resolve(o.ignoreCharacter)) ignoreProxy = int(c->body);
            }
            bool Accepts(uint32_t index) const {
                const auto& b = g.bodies[index];
                return b.alive && int(index) != ignoreBody && int(index) != ignoreProxy && (LayerBit(b.layer) & options.layers) != 0;
            }
            bool ShapeOk(const Shape& s) const { return options.includeTriggers || !s.trigger; }
        };

        RaycastHit MakeHit(uint32_t index, const glm::vec3& point, const glm::vec3& normal, float distance) {
            const Body& b = g.bodies[index];
            RaycastHit hit;
            if (b.character >= 0) { hit.userData = g.characters[b.character].userData; }
            else { hit.body = IdOf(index); hit.userData = b.userData; }
            hit.point = point;
            hit.normal = normal;
            hit.distance = distance;
            return hit;
        }

        bool RayShape(const Shape& s, const glm::vec3& o, const glm::vec3& d, float maxDistance, float& t, glm::vec3& n) {
            if (s.type == ShapeType::TriangleMesh) {
                bool any = false;
                float best = maxDistance;
                for (size_t i = 0; i < s.indices.size() / 3; ++i) {
                    const Convex tri = TriangleOf(s, i);
                    float ti; glm::vec3 ni;
                    if (Native::RaycastTriangle(o, d, tri.a, tri.b, tri.c, best, ti, ni)) { best = ti; t = ti; n = ni; any = true; }
                }
                return any;
            }
            return Native::RaycastConvex(ConvexOf(s), o, d, maxDistance, t, n);
        }

        Aabb RayBox(const glm::vec3& o, const glm::vec3& d, float maxDistance) {
            Aabb box; box.Add(o); box.Add(o + d * maxDistance);
            return box;
        }

        std::vector<RaycastHit> RayAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options, bool nearestOnly) {
            std::vector<RaycastHit> hits;
            if (!g.initialized || glm::length(direction) < 1e-6f) return hits;
            const glm::vec3 d = glm::normalize(direction);
            const QueryFilter filter(options);
            const Aabb rayBox = RayBox(origin, d, maxDistance);
            float limit = maxDistance;
            for (uint32_t i = 0; i < g.bodies.size(); ++i) {
                if (!filter.Accepts(i) || !g.bodies[i].box.Overlaps(rayBox)) continue;
                for (const Shape& s : g.bodies[i].shapes) {
                    if (!filter.ShapeOk(s) || !s.box.Overlaps(rayBox)) continue;
                    float t; glm::vec3 n;
                    if (!RayShape(s, origin, d, nearestOnly ? limit : maxDistance, t, n)) continue;
                    hits.push_back(MakeHit(i, origin + d * t, n, t));
                    if (nearestOnly) limit = std::min(limit, t);
                }
            }
            std::sort(hits.begin(), hits.end(), [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });
            return hits;
        }

        struct SweepHit { bool hit = false; uint32_t body = 0; float t = FLT_MAX; glm::vec3 normal{ 0.0f }, point{ 0.0f }, surface{ 0.0f }; };

        Aabb ConvexBounds(const Convex& c) {
            Aabb box;
            for (const glm::vec3& axis : { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) }) {
                const glm::vec3 hi = c.Support(axis), lo = c.Support(-axis);
                box.Add(hi); box.Add(lo);
            }
            return box.Expanded(c.radius);
        }

        template<class Accept>
        SweepHit SweepConvex(const Convex& shape, const glm::vec3& d, float maxDistance, Accept&& accept) {
            SweepHit best;
            Aabb swept = ConvexBounds(shape);
            Aabb end = swept; end.lo += d * maxDistance; end.hi += d * maxDistance;
            swept.Add(end);
            for (uint32_t i = 0; i < g.bodies.size(); ++i) {
                const Body& b = g.bodies[i];
                if (!b.alive || !b.box.Overlaps(swept)) continue;
                for (const Shape& s : b.shapes) {
                    if (!accept(i, s) || !s.box.Overlaps(swept)) continue;
                    auto test = [&](const Convex& target) {
                        float t; glm::vec3 n, p;
                        if (!Native::Sweep(shape, d, std::min(maxDistance, best.t), target, t, n, p)) return;
                        if (t >= best.t) return;
                        best.hit = true; best.t = t; best.body = i; best.normal = n; best.point = p;
                        // surface normal of the touched feature (a capsule resting on a box edge stands on its top face)
                        best.surface = n;
                        if (target.kind == Convex::Kind::Box) {
                            const glm::vec3 local = glm::transpose(target.rotation) * (p - target.center);
                            int axis = 0; float bestRatio = -1.0f;
                            for (int k = 0; k < 3; ++k) {
                                const float r = std::fabs(local[k]) / std::max(target.halfExtents[k], 1e-6f);
                                if (r > bestRatio + 1e-4f || (std::fabs(r - bestRatio) <= 1e-4f && std::fabs(target.rotation[k].y) > std::fabs(target.rotation[axis].y))) { bestRatio = r; axis = k; }
                            }
                            best.surface = target.rotation[axis] * (local[axis] >= 0.0f ? 1.0f : -1.0f);
                        }
                        else if (target.kind == Convex::Kind::Triangle) {
                            glm::vec3 fn = glm::cross(target.b - target.a, target.c - target.a);
                            if (glm::length(fn) > 1e-12f) { fn = glm::normalize(fn); best.surface = glm::dot(fn, n) >= 0.0f ? fn : -fn; }
                        }
                    };
                    if (s.type == ShapeType::TriangleMesh) {
                        for (size_t k = 0; k < s.indices.size() / 3; ++k) {
                            const Convex tri = TriangleOf(s, k);
                            if (BoundsOf(tri).Overlaps(swept)) test(tri);
                        }
                    }
                    else test(ConvexOf(s));
                }
            }
            return best;
        }

        std::optional<RaycastHit> SweepQuery(const Convex& shape, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
            if (!g.initialized || glm::length(direction) < 1e-6f) return std::nullopt;
            const glm::vec3 d = glm::normalize(direction);
            const QueryFilter filter(options);
            const SweepHit hit = SweepConvex(shape, d, maxDistance, [&](uint32_t i, const Shape& s) { return filter.Accepts(i) && filter.ShapeOk(s); });
            if (!hit.hit) return std::nullopt;
            return MakeHit(hit.body, hit.point, hit.normal, hit.t);
        }

        bool ShapesOverlap(const Convex& c, const Shape& s) {
            auto overlap = [&](const Convex& target) {
                const Native::GjkResult r = Native::Gjk(c, target);
                return r.overlap || r.distance <= c.radius + target.radius;
            };
            if (s.type == ShapeType::TriangleMesh) {
                const Aabb box = ConvexBounds(c);
                for (size_t k = 0; k < s.indices.size() / 3; ++k) {
                    const Convex tri = TriangleOf(s, k);
                    if (BoundsOf(tri).Overlaps(box) && overlap(tri)) return true;
                }
                return false;
            }
            return overlap(ConvexOf(s));
        }

        std::vector<BodyId> OverlapQuery(const Convex& shape, const QueryOptions& options) {
            std::vector<BodyId> result;
            if (!g.initialized) return result;
            const QueryFilter filter(options);
            const Aabb box = ConvexBounds(shape);
            for (uint32_t i = 0; i < g.bodies.size(); ++i) {
                const Body& b = g.bodies[i];
                if (b.character >= 0 || !filter.Accepts(i) || !b.box.Overlaps(box)) continue;
                for (const Shape& s : b.shapes) {
                    if (!filter.ShapeOk(s) || !s.box.Overlaps(box)) continue;
                    if (ShapesOverlap(shape, s)) { result.push_back(IdOf(i)); break; }
                }
            }
            return result;
        }

        Convex CapsuleConvex(const glm::vec3& center, float radius, float halfHeight) {
            Convex c;
            c.kind = Convex::Kind::Segment;
            c.center = center;
            c.a = center + glm::vec3(0, halfHeight, 0);
            c.b = center - glm::vec3(0, halfHeight, 0);
            c.radius = radius;
            return c;
        }

        // ------------------------------------------------------------------ triggers / events
        void UpdateTriggers() {
            std::set<std::pair<uint32_t, uint32_t>> now;
            for (uint32_t i = 0; i < g.bodies.size(); ++i) {
                const Body& t = g.bodies[i];
                if (!t.alive) continue;
                for (const Shape& ts : t.shapes) {
                    if (!ts.trigger) continue;
                    const Convex tc = ConvexOf(ts);
                    for (uint32_t j = 0; j < g.bodies.size(); ++j) {
                        if (j == i || now.count({ i, j })) continue;
                        const Body& o = g.bodies[j];
                        if (!o.alive || (t.type == BodyType::Static && o.type == BodyType::Static) || !LayerPairCollides(t, o) || !ts.box.Overlaps(o.box)) continue;
                        for (const Shape& os : o.shapes) {
                            if (os.trigger || !ts.box.Overlaps(os.box)) continue;
                            if (ts.type == ShapeType::TriangleMesh) continue;
                            if (ShapesOverlap(tc, os)) { now.insert({ i, j }); break; }
                        }
                    }
                }
            }
            auto event = [&](const std::pair<uint32_t, uint32_t>& p, TriggerEvent::Kind kind) {
                const Body& t = g.bodies[p.first];
                const Body& o = g.bodies[p.second];
                if (!t.alive || !o.alive) return;
                TriggerEvent e;
                e.kind = kind;
                e.trigger = IdOf(p.first);
                e.userDataTrigger = t.userData;
                e.otherIsCharacter = o.character >= 0;
                e.other = e.otherIsCharacter ? BodyId{} : IdOf(p.second);
                e.userDataOther = e.otherIsCharacter ? g.characters[o.character].userData : o.userData;
                g.triggerEvents.push_back(e);
            };
            for (const auto& p : now) if (!g.triggerPairs.count(p)) event(p, TriggerEvent::Kind::Enter);
            for (const auto& p : g.triggerPairs) if (!now.count(p)) event(p, TriggerEvent::Kind::Exit);
            g.triggerPairs.swap(now);
        }

        void EmitCollisionEvents() {
            for (const auto& [pair, info] : g.touchingThisStep) {
                if (g.touching.count(pair)) continue;
                CollisionEvent e;
                e.kind = CollisionEvent::Kind::Enter;
                e.a = IdOf(pair.first); e.b = IdOf(pair.second);
                e.userDataA = g.bodies[pair.first].userData; e.userDataB = g.bodies[pair.second].userData;
                e.point = info.point; e.normal = info.normal; e.impactSpeed = info.impactSpeed; e.impulse = info.impulse;
                g.collisionEvents.push_back(e);
            }
            for (const auto& [pair, info] : g.touching) {
                if (g.touchingThisStep.count(pair)) continue;
                const Body& a = g.bodies[pair.first];
                const Body& b = g.bodies[pair.second];
                if (!a.alive || !b.alive) continue;
                // resting bodies keep touching while asleep (no manifold is built for them)
                if ((a.sleeping || a.type != BodyType::Dynamic) && (b.sleeping || b.type != BodyType::Dynamic)) { g.touchingThisStep.emplace(pair, info); continue; }
                CollisionEvent e;
                e.kind = CollisionEvent::Kind::Exit;
                e.a = IdOf(pair.first); e.b = IdOf(pair.second);
                e.userDataA = a.userData; e.userDataB = b.userData;
                g.collisionEvents.push_back(e);
            }
            g.touching.swap(g.touchingThisStep);
            g.touchingThisStep.clear();
        }

        void ForgetBody(uint32_t index) {
            for (auto it = g.manifolds.begin(); it != g.manifolds.end();) {
                if (it->first.a == index || it->first.b == index) it = g.manifolds.erase(it);
                else ++it;
            }
            for (auto it = g.touching.begin(); it != g.touching.end();) {
                if (it->first.first == index || it->first.second == index) {
                    Wake(g.bodies[it->first.first == index ? it->first.second : it->first.first]);   // bodies resting on it
                    it = g.touching.erase(it);
                }
                else ++it;
            }
            for (auto it = g.triggerPairs.begin(); it != g.triggerPairs.end();) {
                if (it->first == index || it->second == index) it = g.triggerPairs.erase(it);
                else ++it;
            }
        }

        uint32_t AllocateBody() {
            uint32_t index;
            if (!g.freeBodies.empty()) { index = g.freeBodies.back(); g.freeBodies.pop_back(); }
            else { index = uint32_t(g.bodies.size()); g.bodies.emplace_back(); }
            const uint32_t generation = g.bodies[index].generation;
            g.bodies[index] = Body{};
            g.bodies[index].generation = generation;
            return index;
        }

        void ReleaseBody(uint32_t index) {
            ForgetBody(index);
            Body& b = g.bodies[index];
            const uint32_t generation = ((b.generation + 1) & 0x3FFFFFFFu) ? ((b.generation + 1) & 0x3FFFFFFFu) : 1u;
            b = Body{};
            b.generation = generation;
            g.freeBodies.push_back(index);
        }

        std::optional<Shape> MakeShape(const ColliderDesc& c, BodyType type) {
            Shape s;
            s.type = c.shape;
            s.halfExtents = glm::max(c.halfExtents, glm::vec3(1e-4f));
            s.radius = std::max(c.radius, 1e-4f);
            s.halfHeight = std::max(c.halfHeight, 1e-4f);
            s.localPosition = c.localPosition;
            s.localRotation = glm::normalize(c.localRotation);
            s.material = c.material;
            s.trigger = c.isTrigger;
            if (c.shape == ShapeType::ConvexMesh) {
                if (c.vertices.size() < 4) { Log::Error() << "Physics: convex collider needs >= 4 vertices"; return std::nullopt; }
                s.points = c.vertices;
                Aabb box;
                for (const glm::vec3& p : s.points) box.Add(p);
                s.localLo = box.lo; s.localHi = box.hi;
            }
            if (c.shape == ShapeType::TriangleMesh) {
                if (type == BodyType::Dynamic) { Log::Error() << "Physics: triangle-mesh colliders are only valid on static/kinematic bodies"; return std::nullopt; }
                if (c.vertices.size() < 3 || c.indices.size() < 3 || c.indices.size() % 3) { Log::Error() << "Physics: invalid triangle mesh"; return std::nullopt; }
                for (uint32_t i : c.indices) if (i >= c.vertices.size()) { Log::Error() << "Physics: triangle index out of range"; return std::nullopt; }
                s.vertices = c.vertices;
                s.indices = c.indices;
                Aabb box;
                for (const glm::vec3& p : s.vertices) box.Add(p);
                s.localLo = box.lo; s.localHi = box.hi;
            }
            return s;
        }

        // ------------------------------------------------------------------ character controller
        SweepHit CharacterSweep(const Character& c, const glm::vec3& center, const glm::vec3& d, float distance) {
            const Convex capsule = CapsuleConvex(center, c.radius, 0.5f * c.height);
            const LayerMask mask = g.collisionMatrix[c.layer];
            return SweepConvex(capsule, d, distance, [&](uint32_t i, const Shape& s) {
                return i != c.body && !s.trigger && (LayerBit(g.bodies[i].layer) & mask) != 0;
            });
        }

        void Depenetrate(const Character& c, glm::vec3& center) {
            const LayerMask mask = g.collisionMatrix[c.layer];
            for (int iteration = 0; iteration < 4; ++iteration) {
                bool moved = false;
                const Convex capsule = CapsuleConvex(center, c.radius, 0.5f * c.height);
                const Aabb box = ConvexBounds(capsule);
                for (uint32_t i = 0; i < g.bodies.size(); ++i) {
                    const Body& b = g.bodies[i];
                    if (!b.alive || i == c.body || (LayerBit(b.layer) & mask) == 0 || !b.box.Overlaps(box)) continue;
                    for (const Shape& s : b.shapes) {
                        if (s.trigger || !s.box.Overlaps(box)) continue;
                        auto push = [&](const Convex& target) {
                            float sep; glm::vec3 n, pa, pb;
                            Native::Distance(CapsuleConvex(center, c.radius, 0.5f * c.height), target, sep, n, pa, pb);
                            if (sep < -1e-4f) { center += n * sep; moved = true; }
                        };
                        if (s.type == ShapeType::TriangleMesh) {
                            for (size_t k = 0; k < s.indices.size() / 3; ++k) {
                                const Convex tri = TriangleOf(s, k);
                                if (BoundsOf(tri).Overlaps(box)) push(tri);
                            }
                        }
                        else push(ConvexOf(s));
                    }
                }
                if (!moved) break;
            }
        }

        enum class Pass { Up, Side, Down };

        // Collide-and-slide; returns the distance travelled along the initial direction's axis.
        void Slide(Character& c, glm::vec3& center, glm::vec3 motion, Pass pass, CharacterMoveResult& result, float dt) {
            const glm::vec3 initialDir = glm::length(motion) > 1e-9f ? glm::normalize(motion) : glm::vec3(0.0f);
            for (int iteration = 0; iteration < 4; ++iteration) {
                const float len = glm::length(motion);
                if (len < 1e-6f) break;
                const glm::vec3 d = motion / len;
                const SweepHit hit = CharacterSweep(c, center, d, len + CHARACTER_SKIN);
                if (!hit.hit) { center += motion; break; }
                const float travel = std::max(0.0f, hit.t - CHARACTER_SKIN);
                center += d * travel;
                glm::vec3 n = hit.normal;
                const glm::vec3 remaining = d * (len - travel);
                const bool walkable = n.y >= c.slopeCos || (pass != Pass::Side && hit.surface.y >= c.slopeCos && n.y > 0.0f);
                switch (pass) {
                    case Pass::Up:
                        if (n.y < -0.3f) result.hitCeiling = true;
                        return;   // stop rising
                    case Pass::Down:
                        if (walkable) { result.grounded = true; return; }
                        break;    // non-walkable: slide down along it
                    case Pass::Side: {
                        result.hitSides = true;
                        Body& other = g.bodies[hit.body];
                        if (other.Dynamic() && std::fabs(n.y) <= 0.7f && c.pushForce > 0.0f) {
                            glm::vec3 push(d.x, 0.0f, d.z);
                            if (glm::length(push) > 1e-6f) {
                                push = glm::normalize(push) * c.pushForce * dt;
                                Wake(other);
                                other.v += push * other.invMass;
                                other.w += other.invInertia * glm::cross(hit.point - other.WorldCom(), push);
                            }
                        }
                        if (!walkable) {   // walls and steep slopes: no climbing
                            n.y = 0.0f;
                            if (glm::length(n) < 1e-4f) return;
                            n = glm::normalize(n);
                        }
                        break;
                    }
                }
                motion = remaining - n * glm::dot(remaining, n);
                if (pass == Pass::Side && glm::dot(motion, initialDir) <= 0.0f) break;
            }
        }

        void SyncProxy(Character& c) {
            Body& proxy = g.bodies[c.body];
            proxy.position = c.foot + glm::vec3(0, c.radius + 0.5f * c.height, 0);
            UpdateWorld(proxy);
        }
    }

    // ====================================================================== lifecycle
    const char* BackendName() { return "Phisiks native"; }

    bool Init(const Settings& settings) {
        if (g.initialized) return true;
        g = State{};
        g.settings = settings;
        g.collisionMatrix.fill(ALL_LAYERS);
        g.initialized = true;
        Log::Info() << "Physics: initialized (native backend, substeps " << settings.substeps << ", default friction "
                    << settings.defaultMaterial.staticFriction << "/" << settings.defaultMaterial.dynamicFriction << ")";
        return true;
    }

    void Shutdown() {
        if (!g.initialized) return;
        g = State{};
    }

    bool IsInitialized() { return g.initialized; }
    const Settings& GetSettings() { return g.settings; }
    void SetGravity(const glm::vec3& gravity) {
        g.settings.gravity = gravity;
        for (Body& b : g.bodies) if (b.alive) Wake(b);
    }
    void SetSubsteps(uint32_t substeps) { g.settings.substeps = std::max(1u, substeps); }
    void SetDefaultMaterial(const PhysicsMaterial& material) { g.settings.defaultMaterial = material; }

    void SetLayersCollide(Layer a, Layer b, bool collide) {
        if (a > 31 || b > 31) return;
        if (collide) { g.collisionMatrix[a] |= LayerBit(b); g.collisionMatrix[b] |= LayerBit(a); }
        else { g.collisionMatrix[a] &= ~LayerBit(b); g.collisionMatrix[b] &= ~LayerBit(a); }
    }
    bool LayersCollide(Layer a, Layer b) { return a < 32 && b < 32 && (g.collisionMatrix[a] & LayerBit(b)) != 0; }
    void RefreshFiltering() {}   // the matrix is read live

    void Step(float dt) {
        if (!g.initialized || dt <= 0.0f) return;
        const uint32_t substeps = std::max(1u, g.settings.substeps);
        const float h = dt / float(substeps);
        // kinematic targets -> velocities for this step
        for (Body& b : g.bodies) {
            if (!b.alive || b.type != BodyType::Kinematic) continue;
            if (b.hasTarget) {
                b.v = (b.targetPosition - b.position) / dt;
                glm::quat dq = b.targetRotation * glm::conjugate(b.rotation);
                if (dq.w < 0.0f) dq = -dq;
                const glm::vec3 axis(dq.x, dq.y, dq.z);
                const float s = glm::length(axis);
                b.w = s > 1e-7f ? axis / s * (2.0f * std::atan2(s, dq.w) / dt) : glm::vec3(0.0f);
            }
            else b.v = b.w = glm::vec3(0.0f);
        }
        for (Body& b : g.bodies) if (b.alive) UpdateWorld(b);
        for (uint32_t i = 0; i < substeps; ++i) {
            IntegrateVelocities(h);
            FindContacts(h);
            Solve(h);
            RecordTouches();
            Integrate(h);
        }
        for (Body& b : g.bodies) {
            if (!b.alive) continue;
            b.force = b.torque = glm::vec3(0.0f);
            if (b.type == BodyType::Kinematic) {
                if (b.hasTarget) { b.position = b.targetPosition; b.rotation = glm::normalize(b.targetRotation); b.hasTarget = false; }
                b.v = b.w = glm::vec3(0.0f);
                UpdateWorld(b);
            }
        }
        EmitCollisionEvents();
        UpdateTriggers();
        ++g.steps;
    }

    void ClearEvents() { g.collisionEvents.clear(); g.triggerEvents.clear(); }
    const std::vector<CollisionEvent>& GetCollisionEvents() { return g.collisionEvents; }
    const std::vector<TriggerEvent>& GetTriggerEvents() { return g.triggerEvents; }

    // ====================================================================== bodies
    BodyId CreateBody(const BodyDesc& desc) {
        if (!g.initialized) return {};
        if (desc.colliders.empty()) { Log::Error() << "Physics: CreateBody without colliders"; return {}; }
        if (desc.layer > 31) { Log::Error() << "Physics: layer out of range"; return {}; }
        std::vector<Shape> shapes;
        for (const ColliderDesc& c : desc.colliders) if (auto s = MakeShape(c, desc.type)) shapes.push_back(std::move(*s));
        if (shapes.empty()) return {};
        const uint32_t index = AllocateBody();
        Body& b = g.bodies[index];
        b.type = desc.type;
        b.layer = desc.layer;
        b.userData = desc.userData;
        b.alive = true;
        b.shapes = std::move(shapes);
        b.position = desc.position;
        b.rotation = glm::normalize(desc.rotation);
        b.linearDamping = desc.linearDamping;
        b.angularDamping = desc.angularDamping;
        b.gravity = desc.enableGravity;
        ComputeMass(b, desc.mass);
        UpdateWorld(b);
        return IdOf(index);
    }

    void DestroyBody(BodyId id) {
        Body* b = Resolve(id);
        if (!b) return;
        ReleaseBody(uint32_t(b - g.bodies.data()));
    }

    void DestroyAll() {
        for (uint32_t i = 0; i < g.characters.size(); ++i) {
            if (g.characters[i].alive) DestroyCharacter(CharacterId{ MakeId(TAG_CHARACTER, g.characters[i].generation, i) });
        }
        for (uint32_t i = 0; i < g.bodies.size(); ++i) if (g.bodies[i].alive) ReleaseBody(i);
        g.manifolds.clear();
        g.touching.clear();
        g.triggerPairs.clear();
        ClearEvents();
    }

    bool IsValid(BodyId id) { return Resolve(id) != nullptr; }
    BodyType GetBodyType(BodyId id) { Body* b = Resolve(id); return b ? b->type : BodyType::Static; }
    uint64_t GetUserData(BodyId id) { Body* b = Resolve(id); return b ? b->userData : 0; }
    glm::vec3 GetPosition(BodyId id) { Body* b = Resolve(id); return b ? b->position : glm::vec3(0.0f); }
    glm::quat GetRotation(BodyId id) { Body* b = Resolve(id); return b ? b->rotation : glm::quat(1, 0, 0, 0); }

    void SetPose(BodyId id, const glm::vec3& position, const glm::quat& rotation) {
        Body* b = Resolve(id);
        if (!b) return;
        b->position = position;
        b->rotation = glm::normalize(rotation);
        b->hasTarget = false;
        if (b->type == BodyType::Dynamic) { b->v = b->w = glm::vec3(0.0f); Wake(*b); }
        UpdateWorld(*b);
    }

    void SetKinematicTarget(BodyId id, const glm::vec3& position, const glm::quat& rotation) {
        Body* b = Resolve(id);
        if (!b || b->type != BodyType::Kinematic) return;
        b->hasTarget = true;
        b->targetPosition = position;
        b->targetRotation = glm::normalize(rotation);
    }

    glm::vec3 GetLinearVelocity(BodyId id) { Body* b = Resolve(id); return (b && b->type != BodyType::Static) ? b->v : glm::vec3(0.0f); }
    glm::vec3 GetAngularVelocity(BodyId id) { Body* b = Resolve(id); return (b && b->type != BodyType::Static) ? b->w : glm::vec3(0.0f); }
    void SetLinearVelocity(BodyId id, const glm::vec3& v) { Body* b = Resolve(id); if (b && b->Dynamic()) { b->v = v; Wake(*b); } }
    void SetAngularVelocity(BodyId id, const glm::vec3& w) { Body* b = Resolve(id); if (b && b->Dynamic()) { b->w = w; Wake(*b); } }
    void AddForce(BodyId id, const glm::vec3& f) { Body* b = Resolve(id); if (b && b->Dynamic()) { b->force += f; Wake(*b); } }
    void AddTorque(BodyId id, const glm::vec3& t) { Body* b = Resolve(id); if (b && b->Dynamic()) { b->torque += t; Wake(*b); } }
    void AddImpulse(BodyId id, const glm::vec3& j) { Body* b = Resolve(id); if (b && b->Dynamic()) { Wake(*b); b->v += j * b->invMass; } }
    void AddImpulseAtPoint(BodyId id, const glm::vec3& j, const glm::vec3& p) {
        Body* b = Resolve(id);
        if (!b || !b->Dynamic()) return;
        Wake(*b);
        b->v += j * b->invMass;
        b->w += b->invInertia * glm::cross(p - b->WorldCom(), j);
    }
    float GetMass(BodyId id) { Body* b = Resolve(id); return (b && b->type != BodyType::Static) ? b->mass : 0.0f; }
    bool IsSleeping(BodyId id) { Body* b = Resolve(id); return b && b->Dynamic() && b->sleeping; }
    void WakeUp(BodyId id) { if (Body* b = Resolve(id)) Wake(*b); }

    void GetBounds(BodyId id, glm::vec3& outMin, glm::vec3& outMax) {
        Body* b = Resolve(id);
        if (!b) { outMin = outMax = glm::vec3(0.0f); return; }
        outMin = b->box.lo;
        outMax = b->box.hi;
    }

    // ====================================================================== queries
    std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        std::vector<RaycastHit> hits = RayAll(origin, direction, maxDistance, options, true);
        if (hits.empty()) return std::nullopt;
        return hits.front();
    }
    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        return RayAll(origin, direction, maxDistance, options, false);
    }
    std::optional<RaycastHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        Convex c; c.kind = Convex::Kind::Point; c.center = origin; c.radius = radius;
        return SweepQuery(c, direction, maxDistance, options);
    }
    std::optional<RaycastHit> BoxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        Convex c; c.kind = Convex::Kind::Box; c.center = center; c.rotation = glm::mat3_cast(glm::normalize(rotation)); c.halfExtents = halfExtents;
        return SweepQuery(c, direction, maxDistance, options);
    }
    std::optional<RaycastHit> CapsuleCast(const glm::vec3& center, float radius, float halfHeight, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        return SweepQuery(CapsuleConvex(center, radius, halfHeight), direction, maxDistance, options);
    }
    std::vector<BodyId> OverlapSphere(const glm::vec3& center, float radius, const QueryOptions& options) {
        Convex c; c.kind = Convex::Kind::Point; c.center = center; c.radius = radius;
        return OverlapQuery(c, options);
    }
    std::vector<BodyId> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const QueryOptions& options) {
        Convex c; c.kind = Convex::Kind::Box; c.center = center; c.rotation = glm::mat3_cast(glm::normalize(rotation)); c.halfExtents = halfExtents;
        return OverlapQuery(c, options);
    }

    // ====================================================================== characters
    CharacterId CreateCharacter(const CharacterDesc& desc) {
        if (!g.initialized || desc.layer > 31) return {};
        uint32_t index;
        if (!g.freeCharacters.empty()) { index = g.freeCharacters.back(); g.freeCharacters.pop_back(); }
        else { index = uint32_t(g.characters.size()); g.characters.emplace_back(); }
        Character& c = g.characters[index];
        c.radius = std::max(desc.radius, 0.01f);
        c.height = std::max(desc.height, 0.01f);
        c.stepOffset = std::max(0.0f, desc.stepOffset);
        c.slopeCos = std::cos(glm::radians(desc.slopeLimitDegrees));
        c.pushForce = desc.pushForce;
        c.layer = desc.layer;
        c.userData = desc.userData;
        c.alive = true;
        c.foot = desc.footPosition;
        // kinematic capsule proxy: blocks dynamic bodies and enters triggers
        const uint32_t body = AllocateBody();
        Body& proxy = g.bodies[body];
        proxy.type = BodyType::Kinematic;
        proxy.layer = desc.layer;
        proxy.alive = true;
        proxy.character = int(index);
        Shape s;
        s.type = ShapeType::Capsule;
        s.radius = c.radius;
        s.halfHeight = 0.5f * c.height;
        proxy.shapes.push_back(s);
        ComputeMass(proxy, 1.0f);
        c.body = body;
        SyncProxy(c);
        return CharacterId{ MakeId(TAG_CHARACTER, c.generation, index) };
    }

    void DestroyCharacter(CharacterId id) {
        Character* c = Resolve(id);
        if (!c) return;
        ReleaseBody(c->body);
        c->alive = false;
        c->generation = ((c->generation + 1) & 0x3FFFFFFFu) ? ((c->generation + 1) & 0x3FFFFFFFu) : 1u;
        g.freeCharacters.push_back(IndexOf(id.value));
    }

    bool IsValid(CharacterId id) { return Resolve(id) != nullptr; }

    CharacterMoveResult MoveCharacter(CharacterId id, const glm::vec3& displacement, float dt) {
        CharacterMoveResult result;
        Character* c = Resolve(id);
        if (!c) return result;
        const float centerOffset = c->radius + 0.5f * c->height;
        glm::vec3 center = c->foot + glm::vec3(0, centerOffset, 0);
        const glm::vec3 before = c->foot;
        Depenetrate(*c, center);
        const glm::vec3 side(displacement.x, 0.0f, displacement.z);
        const float vertical = displacement.y;
        const bool moving = glm::length(side) > 1e-6f;
        // up pass: positive vertical motion plus the step offset (to walk over small obstacles)
        const float stepUp = moving ? c->stepOffset : 0.0f;
        const float up = std::max(vertical, 0.0f) + stepUp;
        float risen = 0.0f;
        if (up > 0.0f) {
            const float y0 = center.y;
            Slide(*c, center, glm::vec3(0, up, 0), Pass::Up, result, dt);
            risen = center.y - y0;
        }
        if (moving) Slide(*c, center, side, Pass::Side, result, dt);
        // down pass: undo the step offset and apply negative vertical motion
        const float down = std::max(-vertical, 0.0f) + std::min(risen, stepUp);
        if (down > 0.0f) Slide(*c, center, glm::vec3(0, -down, 0), Pass::Down, result, dt);
        if (vertical > 0.0f && !result.hitCeiling) result.grounded = false;
        c->foot = center - glm::vec3(0, centerOffset, 0);
        SyncProxy(*c);
        result.displacement = c->foot - before;
        return result;
    }

    glm::vec3 GetCharacterFootPosition(CharacterId id) { Character* c = Resolve(id); return c ? c->foot : glm::vec3(0.0f); }
    void SetCharacterFootPosition(CharacterId id, const glm::vec3& p) {
        Character* c = Resolve(id);
        if (!c) return;
        c->foot = p;
        SyncProxy(*c);
    }
    void GetCharacterBounds(CharacterId id, glm::vec3& outMin, glm::vec3& outMax) {
        Character* c = Resolve(id);
        if (!c) { outMin = outMax = glm::vec3(0.0f); return; }
        const Body& proxy = g.bodies[c->body];
        outMin = proxy.box.lo;
        outMax = proxy.box.hi;
    }

    Stats GetStats() {
        Stats stats;
        for (const Body& b : g.bodies) {
            if (!b.alive || b.character >= 0) continue;
            switch (b.type) {
                case BodyType::Static: ++stats.staticBodies; break;
                case BodyType::Kinematic: ++stats.kinematicBodies; break;
                case BodyType::Dynamic:
                    ++stats.dynamicBodies;
                    if (!b.sleeping) ++stats.awakeDynamicBodies;
                    break;
            }
        }
        for (const Character& c : g.characters) if (c.alive) ++stats.characters;
        stats.stepsSimulated = g.steps;
        return stats;
    }

    void SetBodyState(BodyId id, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& linearVelocity, const glm::vec3& angularVelocity) {
        Body* b = Resolve(id);
        if (!b) return;
        b->position = position;
        b->rotation = glm::normalize(rotation);
        b->hasTarget = false;
        if (b->Dynamic()) {
            b->v = linearVelocity;
            b->w = angularVelocity;
            const bool resting = glm::length(linearVelocity) == 0.0f && glm::length(angularVelocity) == 0.0f;
            b->sleeping = false;
            b->sleepTime = resting ? SLEEP_TIME : 0.0f;
        }
        UpdateWorld(*b);
        for (auto it = g.manifolds.begin(); it != g.manifolds.end(); ++it) {
            if (&g.bodies[it->first.a] == b || &g.bodies[it->first.b] == b) for (int i = 0; i < it->second.count; ++i) { it->second.points[i].jn = 0.0f; it->second.points[i].jt = glm::vec3(0.0f); }
        }
    }
}
