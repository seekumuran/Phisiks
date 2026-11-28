#pragma once
// Phisiks::Physics public types. No backend (PhysX) types appear in the public API.

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace Phisiks::Physics {

    // Generational handle: stale ids (destroyed bodies) are detected, never aliased.
    struct BodyId {
        uint64_t value = 0;
        bool Valid() const { return value != 0; }
        friend bool operator==(BodyId a, BodyId b) { return a.value == b.value; }
        friend bool operator!=(BodyId a, BodyId b) { return a.value != b.value; }
    };
    struct CharacterId {
        uint64_t value = 0;
        bool Valid() const { return value != 0; }
        friend bool operator==(CharacterId a, CharacterId b) { return a.value == b.value; }
    };
    struct JointId {
        uint64_t value = 0;
        bool Valid() const { return value != 0; }
        friend bool operator==(JointId a, JointId b) { return a.value == b.value; }
        friend bool operator!=(JointId a, JointId b) { return a.value != b.value; }
    };

    enum class BodyType { Static, Dynamic, Kinematic };
    enum class ShapeType { Box, Sphere, Capsule, ConvexMesh, TriangleMesh, HeightField };

    // Friction/restitution. Defaults are realistic for wood/plastic-like surfaces
    // (a box starts sliding on a slope steeper than atan(0.6) ~= 31 degrees).
    struct PhysicsMaterial {
        float staticFriction = 0.6f;
        float dynamicFriction = 0.5f;
        float restitution = 0.0f;
    };

    // Layers 0..31. Default: every layer collides with every layer (see SetLayersCollide).
    using Layer = uint8_t;
    using LayerMask = uint32_t;
    inline constexpr Layer LAYER_DEFAULT = 0;
    inline constexpr LayerMask ALL_LAYERS = 0xFFFFFFFFu;
    inline constexpr LayerMask LayerBit(Layer layer) { return 1u << layer; }

    struct ColliderDesc {
        ShapeType shape = ShapeType::Box;
        glm::vec3 halfExtents{ 0.5f };          // Box
        float radius = 0.5f;                    // Sphere, Capsule
        float halfHeight = 0.5f;                // Capsule: half length of the cylinder part, along local +Y
        std::vector<glm::vec3> vertices;        // ConvexMesh / TriangleMesh
        std::vector<uint32_t> indices;          // TriangleMesh (3 per triangle)
        // HeightField (static only): heights[z * columns + x] in metres, a grid of columns x rows
        // samples `cellSize` apart, centred on the collider's local origin (x right, z forward).
        std::vector<float> heights;
        uint32_t heightfieldColumns = 0, heightfieldRows = 0;
        float heightfieldCellSize = 1.0f;
        glm::vec3 localPosition{ 0.0f };
        glm::quat localRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        PhysicsMaterial material;
        bool isTrigger = false;                 // reports trigger events; no contact response, ignored by queries by default
    };

    struct BodyDesc {
        BodyType type = BodyType::Dynamic;
        glm::vec3 position{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        std::vector<ColliderDesc> colliders;
        float mass = 1.0f;                      // Dynamic only; inertia derived from colliders
        float linearDamping = 0.0f;
        float angularDamping = 0.05f;
        bool continuousCollision = false;       // CCD for fast-moving bodies
        bool enableGravity = true;
        Layer layer = LAYER_DEFAULT;
        uint64_t userData = 0;                  // returned in hits/events (e.g. an entity id)
    };

    struct RaycastHit {
        BodyId body;
        uint64_t userData = 0;
        glm::vec3 point{ 0.0f };
        glm::vec3 normal{ 0.0f };
        float distance = 0.0f;
    };

    struct QueryOptions {
        LayerMask layers = ALL_LAYERS;
        bool includeTriggers = false;
        BodyId ignoreBody;                      // e.g. the caster itself
        CharacterId ignoreCharacter;
    };

    struct CollisionEvent {
        enum class Kind { Enter, Exit } kind = Kind::Enter;
        BodyId a, b;
        uint64_t userDataA = 0, userDataB = 0;
        glm::vec3 point{ 0.0f };                // first contact point (Enter only)
        glm::vec3 normal{ 0.0f };               // from A towards B (Enter only)
        float impactSpeed = 0.0f;               // relative speed along the normal at first contact (Enter only)
        float impulse = 0.0f;                   // solver impulse in the reporting sub-step; often 0 on Enter,
                                                // because first contact is detected speculatively (before touching)
    };

    struct TriggerEvent {
        enum class Kind { Enter, Exit } kind = Kind::Enter;
        BodyId trigger;                         // body owning the trigger collider
        BodyId other;                           // body (or character controller's proxy) entering/leaving
        uint64_t userDataTrigger = 0, userDataOther = 0;
        bool otherIsCharacter = false;
    };

    struct CharacterDesc {
        glm::vec3 footPosition{ 0.0f };
        float height = 1.1f;                    // cylinder part; total = height + 2 * radius
        float radius = 0.3f;
        float stepOffset = 0.3f;                // max obstacle height walked over automatically
        float slopeLimitDegrees = 50.0f;        // steeper surfaces cannot be walked up
        float pushForce = 150.0f;               // N applied (horizontally) to dynamic bodies it walks into
        Layer layer = LAYER_DEFAULT;
        uint64_t userData = 0;
    };

    struct CharacterMoveResult {
        bool grounded = false;                  // touched ground while moving down
        bool hitCeiling = false;
        bool hitSides = false;
        glm::vec3 displacement{ 0.0f };         // what the controller actually moved
    };

    struct Settings {
        glm::vec3 gravity{ 0.0f, -9.81f, 0.0f };
        uint32_t substeps = 4;                  // sub-steps per fixed step (stability for stacks/fast bodies)
        uint32_t positionIterations = 4;
        uint32_t velocityIterations = 1;
        PhysicsMaterial defaultMaterial;        // used when a collider does not override it
        uint32_t workerThreads = 2;
    };

    // ---- Joints (constraints between two bodies, or a body and the world)
    //   Hinge         shared anchor point, rotation only about `axis` (optional angle limits / motor)
    //   BallSocket    shared anchor point, free rotation
    //   Prismatic     slides along `axis`, no rotation (optional travel limits / motor)
    //   SpringDamper  soft distance constraint between `anchor` (on A) and `anchorB` (on B)
    enum class JointType { Hinge, BallSocket, Prismatic, SpringDamper };

    struct JointDesc {
        JointType type = JointType::BallSocket;
        BodyId bodyA;                           // invalid = the static world
        BodyId bodyB;
        glm::vec3 anchor{ 0.0f };               // world space, at creation
        glm::vec3 anchorB{ 0.0f };              // SpringDamper: world anchor on B
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };     // world space: hinge axis / slide direction
        bool enableLimits = false;
        float lowerLimit = 0.0f, upperLimit = 0.0f;   // radians (hinge) or metres (prismatic), relative to creation
        bool enableMotor = false;
        float motorSpeed = 0.0f;                // rad/s or m/s (B relative to A)
        float maxMotorForce = 0.0f;             // N*m or N
        float stiffness = 200.0f;               // SpringDamper: N/m
        float damping = 5.0f;                   // SpringDamper: N*s/m
        float restLength = -1.0f;               // SpringDamper: metres, < 0 = distance at creation
        bool collideConnected = false;
    };

    struct JointState {
        float position = 0.0f;                  // hinge angle (rad), slide (m), spring length (m), 0 for ball
        float speed = 0.0f;                     // d(position)/dt
        glm::vec3 anchorA{ 0.0f }, anchorB{ 0.0f };   // current world anchors on A and B
    };

    struct Stats {
        uint32_t staticBodies = 0, dynamicBodies = 0, kinematicBodies = 0, characters = 0;
        uint32_t joints = 0;
        uint32_t awakeDynamicBodies = 0;
        uint64_t stepsSimulated = 0;
    };
}
