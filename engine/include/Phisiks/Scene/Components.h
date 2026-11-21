#pragma once
// Built-in components. Plain data: systems in Phisiks::Scene turn them into physics bodies,
// render objects, lights, camera and audio. Fields marked "runtime" are owned by the engine.

#include "Phisiks/Physics/PhysicsTypes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>

namespace Phisiks {

    struct Name {
        std::string value;
    };

    // Local transform (relative to the parent entity, or the world for root entities).
    struct Transform {
        glm::vec3 position{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    // Built-in meshes are unit-sized: "Cube" spans 1 m on each axis, "Sphere" has a 1 m diameter,
    // "Plane" is a 1x1 m XZ quad facing +Y. Size them with Transform::scale.
    struct MeshRenderer {
        std::string mesh = "Cube";          // built-in or Render::RegisterMesh name
        std::string material;               // Render::RegisterMaterial name; empty = default material
        glm::vec2 uvScale{ 1.0f };          // multiplies mesh UVs (procedural patterns repeat per UV unit)
        bool visible = true;
        bool castShadows = true;
    };

    // Directional and spot lights shine along the entity's local -Z axis.
    enum class LightType { Directional, Point, Spot };

    // Physically based units: Directional = illuminance (lux-like, ~3 for a bright sun at
    // exposure 1); Point/Spot = luminous intensity (candela-like, falls off with 1/d^2 and fades
    // smoothly to zero at `range`).
    struct Light {
        LightType type = LightType::Point;
        glm::vec3 color{ 1.0f };
        float intensity = 10.0f;
        float range = 10.0f;                 // metres (point/spot)
        float innerConeAngle = 0.35f;        // radians, spot: full intensity inside
        float outerConeAngle = 0.6f;         // radians, spot: zero outside
        bool castShadows = false;
    };

    // Looks down local -Z with +Y up. Every enabled camera is rendered, in ascending `order`, into
    // its viewport rectangle of the window (or of an offscreen target when `target` is set; read
    // it back with Render::GetTargetTexture). `primary` marks the camera gameplay code treats as
    // "the" camera (Scene::GetPrimaryCamera).
    enum class Projection { Perspective, Orthographic };

    struct Camera {
        Projection projection = Projection::Perspective;
        float verticalFov = 1.0f;            // radians (perspective)
        float orthoHeight = 10.0f;           // metres visible vertically (orthographic)
        float nearPlane = 0.05f;             // metres
        float farPlane = 500.0f;
        glm::vec4 viewport{ 0.0f, 0.0f, 1.0f, 1.0f };   // x, y (from the bottom-left), width, height in [0, 1]
        int order = 0;                       // lower draws first
        std::string target;                  // empty = window, else an offscreen target name
        glm::ivec2 targetSize{ 512, 512 };   // offscreen target resolution
        bool enabled = true;
        bool primary = true;
    };

    // Collision shape. Sizes are in local units and are multiplied by the entity's world scale
    // when the physics body is built (like Unity). A Collider without a Rigidbody is static.
    struct Collider {
        Physics::ColliderDesc shape;
        Physics::Layer layer = Physics::LAYER_DEFAULT;
    };

    struct Rigidbody {
        Physics::BodyType type = Physics::BodyType::Dynamic;
        float mass = 1.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.05f;
        bool continuousCollision = false;
        bool useGravity = true;
    };

    // Joint between this entity's body and `connectedTo` (an entity name; empty = the world).
    // Both need a Collider; this one must be Dynamic or Kinematic. Anchors and axes are in local
    // units of their entities (scaled with them). The scene creates the joint once both bodies
    // exist and re-creates it when a body is rebuilt.
    struct Joint {
        Physics::JointType type = Physics::JointType::BallSocket;
        std::string connectedTo;
        glm::vec3 anchor{ 0.0f };            // on this entity (local)
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };  // hinge axis / slide direction (local to this entity)
        glm::vec3 connectedAnchor{ 0.0f };   // SpringDamper: on the connected entity (local; world point if no entity)
        bool enableLimits = false;
        float lowerLimit = 0.0f, upperLimit = 0.0f;   // radians / metres
        bool enableMotor = false;
        float motorSpeed = 0.0f, maxMotorForce = 0.0f;
        float stiffness = 200.0f, damping = 5.0f, restLength = -1.0f;
        bool collideConnected = false;
        Physics::JointId runtime;            // runtime: the live joint (invalid until created)
    };

    // Plays a clip. Spatial sources are positioned at the entity (stereo panning, inverse-square
    // attenuation, Doppler shift from the entity / listener velocities); the listener is the
    // primary camera.
    struct AudioSource {
        std::string clip;                    // file name inside the audio asset folder
        float volume = 1.0f;
        bool loop = false;
        bool playOnStart = false;
        bool spatial = false;
        float minDistance = 1.0f;            // metres: full volume inside
        float maxDistance = 80.0f;           // metres: silent beyond (smooth fade over the last 20 %)
        float dopplerFactor = 1.0f;          // 0 = no pitch shift
        // Control: set by Play()/Stop(), consumed by the audio system.
        void Play() { playRequested = true; stopRequested = false; }
        void Stop() { stopRequested = true; playRequested = false; }
        bool playRequested = false;
        bool stopRequested = false;
        // runtime
        bool started = false;                // playOnStart has been handled
        bool isPlaying = false;              // updated by the audio system each frame
        uint64_t voice = 0;
        glm::vec3 lastPosition{ 0.0f };      // runtime: velocity estimate for Doppler
        bool hasLastPosition = false;
    };
}
