// Phisiks::Physics implementation (PhysX 5 backend). Written against the PhysX SDK documentation.
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Core/Log.h"
#include "physics/physx/PhysicsCore.h"

#pragma warning(push, 0)
#include <PxPhysicsAPI.h>
#pragma warning(pop)

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <tuple>

using namespace physx;

namespace Phisiks::Physics {
    namespace {
        // ------------------------------------------------------------------ conversions
        PxVec3 ToPx(const glm::vec3& v) { return PxVec3(v.x, v.y, v.z); }
        PxQuat ToPx(const glm::quat& q) { return PxQuat(q.x, q.y, q.z, q.w); }
        glm::vec3 ToGlm(const PxVec3& v) { return glm::vec3(v.x, v.y, v.z); }
        glm::vec3 ToGlm(const PxExtendedVec3& v) { return glm::vec3(float(v.x), float(v.y), float(v.z)); }
        glm::quat ToGlm(const PxQuat& q) { return glm::quat(q.w, q.x, q.y, q.z); }
        PxTransform ToPx(const glm::vec3& p, const glm::quat& q) { return PxTransform(ToPx(p), ToPx(glm::normalize(q))); }

        // ------------------------------------------------------------------ ids
        // value = (tag << 62) | (generation << 32) | (index + 1)
        constexpr uint64_t TAG_BODY = 1, TAG_CHARACTER = 2, TAG_JOINT = 3;
        uint64_t MakeId(uint64_t tag, uint32_t generation, uint32_t index) {
            return (tag << 62) | (uint64_t(generation & 0x3FFFFFFFu) << 32) | uint64_t(index + 1);
        }
        uint64_t TagOf(uint64_t value) { return value >> 62; }
        uint32_t IndexOf(uint64_t value) { return uint32_t(value & 0xFFFFFFFFu) - 1u; }
        uint32_t GenerationOf(uint64_t value) { return uint32_t((value >> 32) & 0x3FFFFFFFu); }

        struct BodySlot {
            PxRigidActor* actor = nullptr;
            BodyType type = BodyType::Static;
            Layer layer = LAYER_DEFAULT;
            uint64_t userData = 0;
            uint32_t generation = 1;
            bool alive = false;
        };
        // Joints: PxRevoluteJoint (hinge), PxSphericalJoint, PxD6Joint (prismatic: X free/limited,
        // velocity drive as the motor), PxDistanceJoint with a spring (spring-damper).
        struct JointSlot {
            PxJoint* joint = nullptr;
            JointType type = JointType::BallSocket;
            BodyId a, b;
            uint32_t generation = 1;
            bool alive = false;
        };
        struct CharacterSlot {
            PxController* controller = nullptr;
            float pushForce = 0.0f;
            Layer layer = LAYER_DEFAULT;
            uint64_t userData = 0;
            uint32_t generation = 1;
            bool alive = false;
        };

        // ------------------------------------------------------------------ filtering
        // Shape filter data: word0 = own layer bit, word1 = layers it collides with.
        PxFilterFlags FilterShader(PxFilterObjectAttributes attributes0, PxFilterData data0,
                                   PxFilterObjectAttributes attributes1, PxFilterData data1,
                                   PxPairFlags& pairFlags, const void*, PxU32) {
            const bool layersCollide = (data0.word0 & data1.word1) != 0 && (data1.word0 & data0.word1) != 0;
            if (!layersCollide) return PxFilterFlag::eKILL;

            if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1)) {
                pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
                return PxFilterFlag::eDEFAULT;
            }
            // Static/kinematic vs static/kinematic: no response, no events (they're kept only so
            // triggers fire for kinematic bodies and character controllers).
            const bool movable0 = PxGetFilterObjectType(attributes0) == PxFilterObjectType::eRIGID_DYNAMIC && !PxFilterObjectIsKinematic(attributes0);
            const bool movable1 = PxGetFilterObjectType(attributes1) == PxFilterObjectType::eRIGID_DYNAMIC && !PxFilterObjectIsKinematic(attributes1);
            if (!movable0 && !movable1) return PxFilterFlag::eSUPPRESS;

            pairFlags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT |
                        PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_TOUCH_LOST | PxPairFlag::eNOTIFY_CONTACT_POINTS;
            return PxFilterFlag::eDEFAULT;
        }

        class ErrorCallback : public PxErrorCallback {
        public:
            void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
                if (code == PxErrorCode::eDEBUG_INFO) return;
                Log::Error() << "PhysX (" << int(code) << ") " << message << " [" << file << ":" << line << "]";
            }
        };

        class EventCollector;
        class CharacterHitReport;

        struct State {
            bool initialized = false;
            Settings settings;
            PxDefaultCpuDispatcher* dispatcher = nullptr;
            PxScene* scene = nullptr;
            PxControllerManager* controllers = nullptr;
            std::vector<BodySlot> bodies;
            std::vector<uint32_t> freeBodies;
            std::vector<CharacterSlot> characters;
            std::vector<uint32_t> freeCharacters;
            std::vector<JointSlot> joints;
            std::vector<uint32_t> freeJoints;
            std::map<std::tuple<float, float, float>, PxMaterial*> materials;
            std::array<LayerMask, 32> collisionMatrix{};
            std::vector<CollisionEvent> collisionEvents;
            std::vector<TriggerEvent> triggerEvents;
            uint64_t steps = 0;
            float currentStepDt = 1.0f / 60.0f;
            // AddForce / AddTorque accumulate here and act on every sub-step of the next Step()
            // (PhysX clears its force accumulators after each simulate()).
            std::vector<std::pair<BodyId, glm::vec3>> pendingForces, pendingTorques;
        };
        State g;

        // ------------------------------------------------------------------ core (foundation)
        struct CoreState {
            PxDefaultAllocator allocator;
            ErrorCallback errors;
            PxFoundation* foundation = nullptr;
            PxPhysics* physics = nullptr;
            int references = 0;
        };
        CoreState& CoreRef() { static CoreState core; return core; }

        // ------------------------------------------------------------------ helpers
        uint64_t ActorTag(const PxActor* actor) { return actor ? uint64_t(reinterpret_cast<uintptr_t>(actor->userData)) : 0; }

        BodySlot* Resolve(BodyId id) {
            if (!g.initialized || TagOf(id.value) != TAG_BODY) return nullptr;
            const uint32_t index = IndexOf(id.value);
            if (index >= g.bodies.size()) return nullptr;
            BodySlot& slot = g.bodies[index];
            return (slot.alive && slot.generation == GenerationOf(id.value)) ? &slot : nullptr;
        }

        CharacterSlot* Resolve(CharacterId id) {
            if (!g.initialized || TagOf(id.value) != TAG_CHARACTER) return nullptr;
            const uint32_t index = IndexOf(id.value);
            if (index >= g.characters.size()) return nullptr;
            CharacterSlot& slot = g.characters[index];
            return (slot.alive && slot.generation == GenerationOf(id.value)) ? &slot : nullptr;
        }

        JointSlot* Resolve(JointId id) {
            if (!g.initialized || TagOf(id.value) != TAG_JOINT) return nullptr;
            const uint32_t index = IndexOf(id.value);
            if (index >= g.joints.size()) return nullptr;
            JointSlot& slot = g.joints[index];
            return (slot.alive && slot.generation == GenerationOf(id.value)) ? &slot : nullptr;
        }

        void ReleaseJointSlot(uint32_t index) {
            JointSlot& slot = g.joints[index];
            if (!slot.alive) return;
            if (slot.joint) slot.joint->release();
            slot.joint = nullptr;
            slot.alive = false;
            slot.generation = (slot.generation + 1) & 0x3FFFFFFFu;
            if (slot.generation == 0) slot.generation = 1;
            g.freeJoints.push_back(index);
        }

        PxRigidDynamic* AsDynamic(BodyId id) {
            BodySlot* slot = Resolve(id);
            return (slot && slot->actor) ? slot->actor->is<PxRigidDynamic>() : nullptr;
        }

        uint64_t UserDataOf(uint64_t actorTag) {
            if (TagOf(actorTag) == TAG_BODY) { if (BodySlot* s = Resolve(BodyId{ actorTag })) return s->userData; }
            if (TagOf(actorTag) == TAG_CHARACTER) { if (CharacterSlot* s = Resolve(CharacterId{ actorTag })) return s->userData; }
            return 0;
        }

        PxMaterial* GetMaterial(const PhysicsMaterial& m) {
            const auto key = std::make_tuple(m.staticFriction, m.dynamicFriction, m.restitution);
            auto it = g.materials.find(key);
            if (it != g.materials.end()) return it->second;
            PxMaterial* material = Core::SDK()->createMaterial(m.staticFriction, m.dynamicFriction, m.restitution);
            g.materials[key] = material;
            return material;
        }

        PxFilterData SimulationFilter(Layer layer) {
            PxFilterData data;
            data.word0 = LayerBit(layer);
            data.word1 = g.collisionMatrix[layer];
            return data;
        }

        void ApplyFilter(PxRigidActor* actor, Layer layer) {
            const PxU32 count = actor->getNbShapes();
            std::vector<PxShape*> shapes(count);
            actor->getShapes(shapes.data(), count);
            const PxFilterData data = SimulationFilter(layer);
            for (PxShape* shape : shapes) {
                shape->setSimulationFilterData(data);
                shape->setQueryFilterData(data);
            }
        }

        // Cooked meshes are owned by the caller until the shape referencing them has been created.
        struct Geometry { PxGeometryHolder holder; PxBase* cookedMesh = nullptr; };

        std::optional<Geometry> MakeGeometry(const ColliderDesc& c) {
            PxCookingParams cooking(Core::SDK()->getTolerancesScale());
            switch (c.shape) {
                case ShapeType::Box:    return Geometry{ PxGeometryHolder(PxBoxGeometry(ToPx(glm::max(c.halfExtents, glm::vec3(1e-4f))))) };
                case ShapeType::Sphere: return Geometry{ PxGeometryHolder(PxSphereGeometry(std::max(c.radius, 1e-4f))) };
                case ShapeType::Capsule: return Geometry{ PxGeometryHolder(PxCapsuleGeometry(std::max(c.radius, 1e-4f), std::max(c.halfHeight, 1e-4f))) };
                case ShapeType::ConvexMesh: {
                    if (c.vertices.size() < 4) { Log::Error() << "Physics: convex collider needs >= 4 vertices"; return std::nullopt; }
                    PxConvexMeshDesc desc;
                    desc.points.count = PxU32(c.vertices.size());
                    desc.points.stride = sizeof(glm::vec3);
                    desc.points.data = c.vertices.data();
                    desc.flags = PxConvexFlag::eCOMPUTE_CONVEX;
                    PxConvexMesh* mesh = PxCreateConvexMesh(cooking, desc, Core::SDK()->getPhysicsInsertionCallback());
                    if (!mesh) { Log::Error() << "Physics: convex hull cooking failed"; return std::nullopt; }
                    return Geometry{ PxGeometryHolder(PxConvexMeshGeometry(mesh)), mesh };
                }
                case ShapeType::TriangleMesh: {
                    if (c.vertices.size() < 3 || c.indices.size() < 3 || c.indices.size() % 3) { Log::Error() << "Physics: invalid triangle mesh"; return std::nullopt; }
                    PxTriangleMeshDesc desc;
                    desc.points.count = PxU32(c.vertices.size());
                    desc.points.stride = sizeof(glm::vec3);
                    desc.points.data = c.vertices.data();
                    desc.triangles.count = PxU32(c.indices.size() / 3);
                    desc.triangles.stride = 3 * sizeof(uint32_t);
                    desc.triangles.data = c.indices.data();
                    PxTriangleMesh* mesh = PxCreateTriangleMesh(cooking, desc, Core::SDK()->getPhysicsInsertionCallback());
                    if (!mesh) { Log::Error() << "Physics: triangle mesh cooking failed"; return std::nullopt; }
                    return Geometry{ PxGeometryHolder(PxTriangleMeshGeometry(mesh)), mesh };
                }
                case ShapeType::HeightField: {
                    const uint32_t columns = c.heightfieldColumns, rows = c.heightfieldRows;
                    if (columns < 2 || rows < 2 || c.heights.size() != size_t(columns) * rows || c.heightfieldCellSize <= 0.0f) {
                        Log::Error() << "Physics: invalid height field"; return std::nullopt;
                    }
                    // PhysX rows run along local X, columns along local Z; heights are 16-bit, scaled.
                    float maxAbs = 1e-3f;
                    for (float h : c.heights) maxAbs = std::max(maxAbs, std::fabs(h));
                    const float heightScale = maxAbs / 32767.0f;
                    std::vector<PxHeightFieldSample> samples(size_t(columns) * rows);
                    for (uint32_t x = 0; x < columns; ++x) {
                        for (uint32_t z = 0; z < rows; ++z) {
                            PxHeightFieldSample& s = samples[size_t(x) * rows + z];
                            s.height = PxI16(std::lround(c.heights[size_t(z) * columns + x] / heightScale));
                            s.materialIndex0 = 0;
                            s.materialIndex1 = 0;
                        }
                    }
                    PxHeightFieldDesc desc;
                    desc.format = PxHeightFieldFormat::eS16_TM;
                    desc.nbRows = columns;
                    desc.nbColumns = rows;
                    desc.samples.data = samples.data();
                    desc.samples.stride = sizeof(PxHeightFieldSample);
                    PxHeightField* field = PxCreateHeightField(desc, Core::SDK()->getPhysicsInsertionCallback());
                    if (!field) { Log::Error() << "Physics: height field creation failed"; return std::nullopt; }
                    return Geometry{ PxGeometryHolder(PxHeightFieldGeometry(field, PxMeshGeometryFlags(), heightScale, c.heightfieldCellSize, c.heightfieldCellSize)), field };
                }
            }
            return std::nullopt;
        }

        // ------------------------------------------------------------------ events
        class EventCollector : public PxSimulationEventCallback {
        public:
            void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
            void onWake(PxActor**, PxU32) override {}
            void onSleep(PxActor**, PxU32) override {}
            void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}

            void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override {
                if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1)) return;
                const uint64_t a = ActorTag(header.actors[0]), b = ActorTag(header.actors[1]);
                for (PxU32 i = 0; i < count; ++i) {
                    const PxContactPair& pair = pairs[i];
                    CollisionEvent event;
                    event.a = BodyId{ a };
                    event.b = BodyId{ b };
                    event.userDataA = UserDataOf(a);
                    event.userDataB = UserDataOf(b);
                    if (pair.events & PxPairFlag::eNOTIFY_TOUCH_FOUND) {
                        event.kind = CollisionEvent::Kind::Enter;
                        PxContactPairPoint points[16];
                        const PxU32 n = pair.extractContacts(points, 16);
                        float impulse = 0.0f;
                        for (PxU32 p = 0; p < n; ++p) impulse += points[p].impulse.magnitude();
                        if (n > 0) {
                            event.point = ToGlm(points[0].position);
                            event.normal = ToGlm(points[0].normal);
                            auto velocity = [](const PxActor* actor) {
                                const PxRigidBody* body = actor ? actor->is<PxRigidBody>() : nullptr;
                                return body ? body->getLinearVelocity() : PxVec3(0.0f);
                            };
                            const PxVec3 relative = velocity(header.actors[0]) - velocity(header.actors[1]);
                            event.impactSpeed = std::fabs(relative.dot(points[0].normal));
                        }
                        event.impulse = impulse;
                        g.collisionEvents.push_back(event);
                    }
                    else if (pair.events & PxPairFlag::eNOTIFY_TOUCH_LOST) {
                        event.kind = CollisionEvent::Kind::Exit;
                        g.collisionEvents.push_back(event);
                    }
                }
            }

            void onTrigger(PxTriggerPair* pairs, PxU32 count) override {
                for (PxU32 i = 0; i < count; ++i) {
                    const PxTriggerPair& pair = pairs[i];
                    if (pair.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER)) continue;
                    TriggerEvent event;
                    event.kind = (pair.status & PxPairFlag::eNOTIFY_TOUCH_FOUND) ? TriggerEvent::Kind::Enter : TriggerEvent::Kind::Exit;
                    const uint64_t trigger = ActorTag(pair.triggerActor), other = ActorTag(pair.otherActor);
                    event.trigger = BodyId{ trigger };
                    event.otherIsCharacter = TagOf(other) == TAG_CHARACTER;
                    event.other = BodyId{ event.otherIsCharacter ? 0 : other };
                    event.userDataTrigger = UserDataOf(trigger);
                    event.userDataOther = UserDataOf(other);
                    g.triggerEvents.push_back(event);
                }
            }
        };
        EventCollector g_events;

        // Characters push dynamic bodies they walk into.
        class CharacterHitReport : public PxUserControllerHitReport {
        public:
            void onShapeHit(const PxControllerShapeHit& hit) override {
                PxRigidDynamic* body = hit.actor ? hit.actor->is<PxRigidDynamic>() : nullptr;
                if (!body || (body->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) return;
                if (std::fabs(hit.dir.y) > 0.7f) return;   // standing on / bumping head: no push
                CharacterSlot* slot = Resolve(CharacterId{ ActorTag(hit.controller->getActor()) });
                if (!slot || slot->pushForce <= 0.0f) return;
                // Constant push force (N) over this move's time step, horizontal only.
                const PxVec3 impulse = PxVec3(hit.dir.x, 0.0f, hit.dir.z).getNormalized() * slot->pushForce * g.currentStepDt;
                PxRigidBodyExt::addForceAtPos(*body, impulse, PxVec3(float(hit.worldPos.x), float(hit.worldPos.y), float(hit.worldPos.z)), PxForceMode::eIMPULSE);
            }
            void onControllerHit(const PxControllersHit&) override {}
            void onObstacleHit(const PxControllerObstacleHit&) override {}
        };
        CharacterHitReport g_hitReport;

        // ------------------------------------------------------------------ queries
        class QueryFilter : public PxQueryFilterCallback {
        public:
            explicit QueryFilter(const QueryOptions& options) : m_options(options) {
                if (CharacterSlot* c = Resolve(options.ignoreCharacter)) m_ignoreActor = c->controller->getActor();
                if (BodySlot* b = Resolve(options.ignoreBody)) m_ignoreBody = b->actor;
            }
            PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor, PxHitFlags&) override {
                if (actor == m_ignoreActor || actor == m_ignoreBody) return PxQueryHitType::eNONE;
                if (!m_options.includeTriggers && (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
                if ((shape->getQueryFilterData().word0 & m_options.layers) == 0) return PxQueryHitType::eNONE;
                return m_touchMode ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
            }
            PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
                return m_touchMode ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
            }
            bool m_touchMode = false;
        private:
            QueryOptions m_options;
            const PxRigidActor* m_ignoreActor = nullptr;
            const PxRigidActor* m_ignoreBody = nullptr;
        };

        PxQueryFilterData QueryData(bool touchMode) {
            PxQueryFilterData data;
            data.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER;
            if (touchMode) data.flags |= PxQueryFlag::eNO_BLOCK;
            return data;
        }

        RaycastHit MakeHit(const PxRigidActor* actor, const PxVec3& position, const PxVec3& normal, float distance) {
            RaycastHit hit;
            const uint64_t tag = ActorTag(actor);
            hit.body = BodyId{ TagOf(tag) == TAG_BODY ? tag : 0 };
            hit.userData = UserDataOf(tag);
            hit.point = ToGlm(position);
            hit.normal = ToGlm(normal);
            hit.distance = distance;
            return hit;
        }

        std::optional<RaycastHit> Sweep(const PxGeometry& geometry, const PxTransform& pose, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
            if (!g.initialized || glm::length(direction) < 1e-6f) return std::nullopt;
            QueryFilter filter(options);
            PxSweepBuffer buffer;
            const PxVec3 dir = ToPx(glm::normalize(direction));
            if (!g.scene->sweep(geometry, pose, dir, maxDistance, buffer, PxHitFlag::eDEFAULT, QueryData(false), &filter) || !buffer.hasBlock) return std::nullopt;
            return MakeHit(buffer.block.actor, buffer.block.position, buffer.block.normal, buffer.block.distance);
        }

        std::vector<BodyId> Overlap(const PxGeometry& geometry, const PxTransform& pose, const QueryOptions& options) {
            std::vector<BodyId> result;
            if (!g.initialized) return result;
            QueryFilter filter(options);
            filter.m_touchMode = true;
            PxOverlapBufferN<256> buffer;
            g.scene->overlap(geometry, pose, buffer, QueryData(true), &filter);
            for (PxU32 i = 0; i < buffer.getNbTouches(); ++i) {
                const uint64_t tag = ActorTag(buffer.getTouch(i).actor);
                if (TagOf(tag) != TAG_BODY) continue;
                const BodyId id{ tag };
                if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
            }
            return result;
        }

        // Capsule colliders/queries are Y-up in the engine; PhysX capsules extend along local X.
        const PxQuat CAPSULE_UP(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f));
    }

    // ====================================================================== Core
    namespace Core {
        bool Acquire() {
            CoreState& core = CoreRef();
            if (core.references++ > 0) return true;
            core.foundation = PxCreateFoundation(PX_PHYSICS_VERSION, core.allocator, core.errors);
            if (!core.foundation) { core.references = 0; return false; }
            core.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *core.foundation, PxTolerancesScale(), false, nullptr);
            if (!core.physics) {
                core.foundation->release();
                core.foundation = nullptr;
                core.references = 0;
                return false;
            }
            PxInitExtensions(*core.physics, nullptr);   // joints
            return true;
        }
        void Release() {
            CoreState& core = CoreRef();
            if (core.references == 0 || --core.references > 0) return;
            if (core.physics) PxCloseExtensions();
            if (core.physics) core.physics->release();
            if (core.foundation) core.foundation->release();
            core.physics = nullptr;
            core.foundation = nullptr;
        }
        PxFoundation* Foundation() { return CoreRef().foundation; }
        PxPhysics* SDK() { return CoreRef().physics; }
    }

    // ====================================================================== lifecycle
    bool Init(const Settings& settings) {
        if (g.initialized) return true;
        if (!Core::Acquire()) { Log::Error() << "Physics: PhysX initialization failed"; return false; }
        g = State{};
        g.settings = settings;
        g.collisionMatrix.fill(ALL_LAYERS);

        g.dispatcher = PxDefaultCpuDispatcherCreate(std::max(1u, settings.workerThreads));
        PxSceneDesc desc(Core::SDK()->getTolerancesScale());
        desc.gravity = ToPx(settings.gravity);
        desc.cpuDispatcher = g.dispatcher;
        desc.filterShader = FilterShader;
        desc.simulationEventCallback = &g_events;
        desc.solverType = PxSolverType::eTGS;
        desc.flags |= PxSceneFlag::eENABLE_CCD | PxSceneFlag::eENABLE_PCM;
        desc.staticKineFilteringMode = PxPairFilteringMode::eKEEP;   // triggers for kinematics/characters
        desc.kineKineFilteringMode = PxPairFilteringMode::eKEEP;
        g.scene = Core::SDK()->createScene(desc);
        if (!g.scene) { Log::Error() << "Physics: scene creation failed"; Core::Release(); return false; }
        g.controllers = PxCreateControllerManager(*g.scene);
        g.initialized = true;
        Log::Info() << "Physics: initialized (PhysX " << PX_PHYSICS_VERSION_MAJOR << "." << PX_PHYSICS_VERSION_MINOR
                    << ", substeps " << settings.substeps << ", default friction " << settings.defaultMaterial.staticFriction
                    << "/" << settings.defaultMaterial.dynamicFriction << ")";
        return true;
    }

    void Shutdown() {
        if (!g.initialized) return;
        DestroyAll();
        if (g.controllers) g.controllers->release();
        for (auto& [key, material] : g.materials) material->release();
        if (g.scene) g.scene->release();
        if (g.dispatcher) g.dispatcher->release();
        g = State{};
        Core::Release();
    }

    bool IsInitialized() { return g.initialized; }
    const char* BackendName() {
        static const std::string name = "PhysX " + std::to_string(PX_PHYSICS_VERSION_MAJOR) + "." + std::to_string(PX_PHYSICS_VERSION_MINOR);
        return name.c_str();
    }
    const Settings& GetSettings() { return g.settings; }

    void SetGravity(const glm::vec3& gravity) {
        g.settings.gravity = gravity;
        if (g.scene) g.scene->setGravity(ToPx(gravity));
    }
    void SetSubsteps(uint32_t substeps) { g.settings.substeps = std::max(1u, substeps); }
    void SetDefaultMaterial(const PhysicsMaterial& material) { g.settings.defaultMaterial = material; }

    void SetLayersCollide(Layer a, Layer b, bool collide) {
        if (a > 31 || b > 31) return;
        if (collide) { g.collisionMatrix[a] |= LayerBit(b); g.collisionMatrix[b] |= LayerBit(a); }
        else { g.collisionMatrix[a] &= ~LayerBit(b); g.collisionMatrix[b] &= ~LayerBit(a); }
    }
    bool LayersCollide(Layer a, Layer b) { return a < 32 && b < 32 && (g.collisionMatrix[a] & LayerBit(b)) != 0; }

    void RefreshFiltering() {
        for (BodySlot& slot : g.bodies) if (slot.alive) ApplyFilter(slot.actor, slot.layer);
        for (CharacterSlot& slot : g.characters) if (slot.alive) ApplyFilter(slot.controller->getActor(), slot.layer);
    }

    namespace {
        PxRigidDynamic* MovableDynamic(BodyId id) {
            BodySlot* s = Resolve(id);
            return (s && s->type == BodyType::Dynamic) ? s->actor->is<PxRigidDynamic>() : nullptr;
        }
    }

    void Step(float dt) {
        if (!g.initialized || dt <= 0.0f) return;
        const uint32_t substeps = std::max(1u, g.settings.substeps);
        const float h = dt / float(substeps);
        g.currentStepDt = dt;
        for (uint32_t i = 0; i < substeps; ++i) {
            for (const auto& [id, f] : g.pendingForces) if (PxRigidDynamic* d = MovableDynamic(id)) d->addForce(ToPx(f), PxForceMode::eFORCE);
            for (const auto& [id, t] : g.pendingTorques) if (PxRigidDynamic* d = MovableDynamic(id)) d->addTorque(ToPx(t), PxForceMode::eFORCE);
            g.scene->simulate(h);
            g.scene->fetchResults(true);
        }
        g.pendingForces.clear();
        g.pendingTorques.clear();
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

        PxPhysics& sdk = *Core::SDK();
        const PxTransform pose = ToPx(desc.position, desc.rotation);
        PxRigidActor* actor = nullptr;
        PxRigidDynamic* dynamic = nullptr;
        if (desc.type == BodyType::Static) actor = sdk.createRigidStatic(pose);
        else actor = dynamic = sdk.createRigidDynamic(pose);
        if (!actor) return {};

        bool hasSimulationShape = false;
        for (const ColliderDesc& collider : desc.colliders) {
            if ((collider.shape == ShapeType::TriangleMesh || collider.shape == ShapeType::HeightField) && desc.type == BodyType::Dynamic) {
                Log::Error() << "Physics: triangle-mesh / height-field colliders are only valid on static/kinematic bodies";
                continue;
            }
            std::optional<Geometry> geometry = MakeGeometry(collider);
            if (!geometry) continue;
            PxShape* shape = sdk.createShape(geometry->holder.any(), *GetMaterial(collider.material), true);
            if (geometry->cookedMesh) geometry->cookedMesh->release();   // the shape now holds its own reference
            if (!shape) continue;
            PxQuat localRotation = ToPx(glm::normalize(collider.localRotation));
            if (collider.shape == ShapeType::Capsule) localRotation = localRotation * CAPSULE_UP;
            glm::vec3 localPosition = collider.localPosition;
            if (collider.shape == ShapeType::HeightField) {   // PhysX height fields start at the local origin; the engine centres them
                const glm::vec3 corner(-0.5f * float(collider.heightfieldColumns - 1) * collider.heightfieldCellSize, 0.0f,
                                       -0.5f * float(collider.heightfieldRows - 1) * collider.heightfieldCellSize);
                localPosition += glm::normalize(collider.localRotation) * corner;
            }
            shape->setLocalPose(PxTransform(ToPx(localPosition), localRotation));
            if (collider.isTrigger) {
                shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
                shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
            }
            else hasSimulationShape = true;
            actor->attachShape(*shape);
            shape->release();   // the actor holds the reference
        }
        if (actor->getNbShapes() == 0) { actor->release(); return {}; }

        if (dynamic) {
            if (desc.type == BodyType::Kinematic) dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            if (hasSimulationShape) PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, std::max(desc.mass, 1e-4f));
            else dynamic->setMass(std::max(desc.mass, 1e-4f));
            dynamic->setLinearDamping(desc.linearDamping);
            dynamic->setAngularDamping(desc.angularDamping);
            dynamic->setSolverIterationCounts(g.settings.positionIterations, g.settings.velocityIterations);
            if (desc.continuousCollision && desc.type == BodyType::Dynamic) dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
            if (!desc.enableGravity) dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
        }

        uint32_t index;
        if (!g.freeBodies.empty()) { index = g.freeBodies.back(); g.freeBodies.pop_back(); }
        else { index = uint32_t(g.bodies.size()); g.bodies.emplace_back(); }
        BodySlot& slot = g.bodies[index];
        slot.actor = actor;
        slot.type = desc.type;
        slot.layer = desc.layer;
        slot.userData = desc.userData;
        slot.alive = true;
        const BodyId id{ MakeId(TAG_BODY, slot.generation, index) };
        actor->userData = reinterpret_cast<void*>(uintptr_t(id.value));
        ApplyFilter(actor, desc.layer);
        g.scene->addActor(*actor);
        return id;
    }

    void DestroyBody(BodyId id) {
        BodySlot* slot = Resolve(id);
        if (!slot) return;
        for (uint32_t k = 0; k < g.joints.size(); ++k) {
            if (g.joints[k].alive && (g.joints[k].a == id || g.joints[k].b == id)) ReleaseJointSlot(k);
        }
        g.scene->removeActor(*slot->actor);
        slot->actor->release();
        slot->actor = nullptr;
        slot->alive = false;
        slot->generation = (slot->generation + 1) & 0x3FFFFFFFu;
        if (slot->generation == 0) slot->generation = 1;
        g.freeBodies.push_back(IndexOf(id.value));
    }

    void DestroyAll() {
        for (uint32_t i = 0; i < g.bodies.size(); ++i) {
            if (g.bodies[i].alive) DestroyBody(BodyId{ MakeId(TAG_BODY, g.bodies[i].generation, i) });
        }
        for (uint32_t i = 0; i < g.characters.size(); ++i) {
            if (g.characters[i].alive) DestroyCharacter(CharacterId{ MakeId(TAG_CHARACTER, g.characters[i].generation, i) });
        }
        ClearEvents();
    }

    bool IsValid(BodyId id) { return Resolve(id) != nullptr; }
    BodyType GetBodyType(BodyId id) { BodySlot* s = Resolve(id); return s ? s->type : BodyType::Static; }
    uint64_t GetUserData(BodyId id) { BodySlot* s = Resolve(id); return s ? s->userData : 0; }

    glm::vec3 GetPosition(BodyId id) { BodySlot* s = Resolve(id); return s ? ToGlm(s->actor->getGlobalPose().p) : glm::vec3(0.0f); }
    glm::quat GetRotation(BodyId id) { BodySlot* s = Resolve(id); return s ? ToGlm(s->actor->getGlobalPose().q) : glm::quat(1, 0, 0, 0); }

    void SetPose(BodyId id, const glm::vec3& position, const glm::quat& rotation) {
        BodySlot* s = Resolve(id);
        if (!s) return;
        s->actor->setGlobalPose(ToPx(position, rotation));
        if (PxRigidDynamic* d = s->actor->is<PxRigidDynamic>(); d && s->type == BodyType::Dynamic) {
            d->setLinearVelocity(PxVec3(0.0f));
            d->setAngularVelocity(PxVec3(0.0f));
            d->wakeUp();
        }
    }

    void SetBodyState(BodyId id, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& linearVelocity, const glm::vec3& angularVelocity) {
        BodySlot* s = Resolve(id);
        if (!s) return;
        s->actor->setGlobalPose(ToPx(position, rotation));
        if (PxRigidDynamic* d = s->actor->is<PxRigidDynamic>(); d && s->type == BodyType::Dynamic) {
            d->setLinearVelocity(ToPx(linearVelocity));
            d->setAngularVelocity(ToPx(angularVelocity));
            d->wakeUp();
        }
    }

    void SetKinematicTarget(BodyId id, const glm::vec3& position, const glm::quat& rotation) {
        BodySlot* s = Resolve(id);
        if (!s || s->type != BodyType::Kinematic) return;
        s->actor->is<PxRigidDynamic>()->setKinematicTarget(ToPx(position, rotation));
    }

    glm::vec3 GetLinearVelocity(BodyId id)  { PxRigidDynamic* d = AsDynamic(id); return d ? ToGlm(d->getLinearVelocity()) : glm::vec3(0.0f); }
    glm::vec3 GetAngularVelocity(BodyId id) { PxRigidDynamic* d = AsDynamic(id); return d ? ToGlm(d->getAngularVelocity()) : glm::vec3(0.0f); }

    void SetLinearVelocity(BodyId id, const glm::vec3& v)  { if (PxRigidDynamic* d = MovableDynamic(id)) d->setLinearVelocity(ToPx(v)); }
    void SetAngularVelocity(BodyId id, const glm::vec3& v) { if (PxRigidDynamic* d = MovableDynamic(id)) d->setAngularVelocity(ToPx(v)); }
    void AddForce(BodyId id, const glm::vec3& f)            { if (MovableDynamic(id)) { g.pendingForces.emplace_back(id, f); } }
    void AddImpulse(BodyId id, const glm::vec3& j)          { if (PxRigidDynamic* d = MovableDynamic(id)) d->addForce(ToPx(j), PxForceMode::eIMPULSE); }
    void AddTorque(BodyId id, const glm::vec3& t)           { if (MovableDynamic(id)) { g.pendingTorques.emplace_back(id, t); } }
    void AddImpulseAtPoint(BodyId id, const glm::vec3& j, const glm::vec3& p) {
        if (PxRigidDynamic* d = MovableDynamic(id)) PxRigidBodyExt::addForceAtPos(*d, ToPx(j), ToPx(p), PxForceMode::eIMPULSE, true);
    }
    float GetMass(BodyId id)    { PxRigidDynamic* d = AsDynamic(id); return d ? d->getMass() : 0.0f; }
    bool IsSleeping(BodyId id)  { PxRigidDynamic* d = MovableDynamic(id); return d ? d->isSleeping() : false; }
    void WakeUp(BodyId id)      { if (PxRigidDynamic* d = MovableDynamic(id)) d->wakeUp(); }

    void GetBounds(BodyId id, glm::vec3& outMin, glm::vec3& outMax) {
        BodySlot* s = Resolve(id);
        if (!s) { outMin = outMax = glm::vec3(0.0f); return; }
        const PxBounds3 bounds = s->actor->getWorldBounds();
        outMin = ToGlm(bounds.minimum);
        outMax = ToGlm(bounds.maximum);
    }

    // ====================================================================== queries
    std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        if (!g.initialized || glm::length(direction) < 1e-6f) return std::nullopt;
        QueryFilter filter(options);
        PxRaycastBuffer buffer;
        if (!g.scene->raycast(ToPx(origin), ToPx(glm::normalize(direction)), maxDistance, buffer, PxHitFlag::eDEFAULT, QueryData(false), &filter) || !buffer.hasBlock) return std::nullopt;
        return MakeHit(buffer.block.actor, buffer.block.position, buffer.block.normal, buffer.block.distance);
    }

    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        std::vector<RaycastHit> hits;
        if (!g.initialized || glm::length(direction) < 1e-6f) return hits;
        QueryFilter filter(options);
        filter.m_touchMode = true;
        PxRaycastBufferN<128> buffer;
        g.scene->raycast(ToPx(origin), ToPx(glm::normalize(direction)), maxDistance, buffer, PxHitFlag::eDEFAULT, QueryData(true), &filter);
        for (PxU32 i = 0; i < buffer.getNbTouches(); ++i) {
            const PxRaycastHit& h = buffer.getTouch(i);
            hits.push_back(MakeHit(h.actor, h.position, h.normal, h.distance));
        }
        std::sort(hits.begin(), hits.end(), [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });
        return hits;
    }

    std::optional<RaycastHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        return Sweep(PxSphereGeometry(radius), PxTransform(ToPx(origin)), direction, maxDistance, options);
    }
    std::optional<RaycastHit> BoxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        return Sweep(PxBoxGeometry(ToPx(halfExtents)), ToPx(center, rotation), direction, maxDistance, options);
    }
    std::optional<RaycastHit> CapsuleCast(const glm::vec3& center, float radius, float halfHeight, const glm::vec3& direction, float maxDistance, const QueryOptions& options) {
        return Sweep(PxCapsuleGeometry(radius, halfHeight), PxTransform(ToPx(center), CAPSULE_UP), direction, maxDistance, options);
    }
    std::vector<BodyId> OverlapSphere(const glm::vec3& center, float radius, const QueryOptions& options) {
        return Overlap(PxSphereGeometry(radius), PxTransform(ToPx(center)), options);
    }
    std::vector<BodyId> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const QueryOptions& options) {
        return Overlap(PxBoxGeometry(ToPx(halfExtents)), ToPx(center, rotation), options);
    }

    // ====================================================================== characters
    CharacterId CreateCharacter(const CharacterDesc& desc) {
        if (!g.initialized || desc.layer > 31) return {};
        PxCapsuleControllerDesc cd;
        cd.radius = std::max(desc.radius, 0.01f);
        cd.height = std::max(desc.height, 0.01f);
        cd.stepOffset = std::max(0.0f, desc.stepOffset);
        cd.slopeLimit = std::cos(glm::radians(desc.slopeLimitDegrees));
        cd.contactOffset = 0.01f;
        cd.upDirection = PxVec3(0.0f, 1.0f, 0.0f);
        cd.material = GetMaterial(g.settings.defaultMaterial);
        cd.reportCallback = &g_hitReport;
        cd.nonWalkableMode = PxControllerNonWalkableMode::ePREVENT_CLIMBING_AND_FORCE_SLIDING;
        cd.position = PxExtendedVec3(desc.footPosition.x, desc.footPosition.y + cd.radius + cd.height * 0.5f, desc.footPosition.z);
        if (!cd.isValid()) { Log::Error() << "Physics: invalid character description"; return {}; }
        PxController* controller = g.controllers->createController(cd);
        if (!controller) return {};

        uint32_t index;
        if (!g.freeCharacters.empty()) { index = g.freeCharacters.back(); g.freeCharacters.pop_back(); }
        else { index = uint32_t(g.characters.size()); g.characters.emplace_back(); }
        CharacterSlot& slot = g.characters[index];
        slot.controller = controller;
        slot.pushForce = desc.pushForce;
        slot.layer = desc.layer;
        slot.userData = desc.userData;
        slot.alive = true;
        const CharacterId id{ MakeId(TAG_CHARACTER, slot.generation, index) };
        controller->getActor()->userData = reinterpret_cast<void*>(uintptr_t(id.value));
        ApplyFilter(controller->getActor(), desc.layer);
        controller->setFootPosition(PxExtendedVec3(desc.footPosition.x, desc.footPosition.y, desc.footPosition.z));
        return id;
    }

    void DestroyCharacter(CharacterId id) {
        CharacterSlot* slot = Resolve(id);
        if (!slot) return;
        slot->controller->release();
        slot->controller = nullptr;
        slot->alive = false;
        slot->generation = (slot->generation + 1) & 0x3FFFFFFFu;
        if (slot->generation == 0) slot->generation = 1;
        g.freeCharacters.push_back(IndexOf(id.value));
    }

    bool IsValid(CharacterId id) { return Resolve(id) != nullptr; }

    namespace {
        // The controller sweeps against the scene; exclude triggers and layers it doesn't collide with.
        class CharacterQueryFilter : public PxQueryFilterCallback {
        public:
            explicit CharacterQueryFilter(LayerMask mask) : m_mask(mask) {}
            PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor*, PxHitFlags&) override {
                if (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) return PxQueryHitType::eNONE;
                if ((shape->getQueryFilterData().word0 & m_mask) == 0) return PxQueryHitType::eNONE;
                return PxQueryHitType::eBLOCK;
            }
            PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override { return PxQueryHitType::eBLOCK; }
        private:
            LayerMask m_mask;
        };
    }

    CharacterMoveResult MoveCharacter(CharacterId id, const glm::vec3& displacement, float dt) {
        CharacterMoveResult result;
        CharacterSlot* slot = Resolve(id);
        if (!slot) return result;
        g.currentStepDt = std::max(dt, 1e-4f);
        CharacterQueryFilter filter(g.collisionMatrix[slot->layer]);
        PxControllerFilters filters(nullptr, &filter);
        const glm::vec3 before = ToGlm(slot->controller->getFootPosition());
        const PxControllerCollisionFlags flags = slot->controller->move(ToPx(displacement), 0.0001f, dt, filters);
        result.grounded = flags.isSet(PxControllerCollisionFlag::eCOLLISION_DOWN);
        result.hitCeiling = flags.isSet(PxControllerCollisionFlag::eCOLLISION_UP);
        result.hitSides = flags.isSet(PxControllerCollisionFlag::eCOLLISION_SIDES);
        result.displacement = ToGlm(slot->controller->getFootPosition()) - before;
        return result;
    }

    glm::vec3 GetCharacterFootPosition(CharacterId id) {
        CharacterSlot* slot = Resolve(id);
        return slot ? ToGlm(slot->controller->getFootPosition()) : glm::vec3(0.0f);
    }

    void SetCharacterFootPosition(CharacterId id, const glm::vec3& p) {
        if (CharacterSlot* slot = Resolve(id)) slot->controller->setFootPosition(PxExtendedVec3(p.x, p.y, p.z));
    }

    void GetCharacterBounds(CharacterId id, glm::vec3& outMin, glm::vec3& outMax) {
        CharacterSlot* slot = Resolve(id);
        if (!slot) { outMin = outMax = glm::vec3(0.0f); return; }
        const PxBounds3 bounds = slot->controller->getActor()->getWorldBounds();
        outMin = ToGlm(bounds.minimum);
        outMax = ToGlm(bounds.maximum);
    }

    Stats GetStats() {
        Stats stats;
        for (const BodySlot& slot : g.bodies) {
            if (!slot.alive) continue;
            switch (slot.type) {
                case BodyType::Static:    ++stats.staticBodies; break;
                case BodyType::Kinematic: ++stats.kinematicBodies; break;
                case BodyType::Dynamic:
                    ++stats.dynamicBodies;
                    if (!slot.actor->is<PxRigidDynamic>()->isSleeping()) ++stats.awakeDynamicBodies;
                    break;
            }
        }
        for (const CharacterSlot& slot : g.characters) if (slot.alive) ++stats.characters;
        for (const JointSlot& slot : g.joints) if (slot.alive) ++stats.joints;
        stats.stepsSimulated = g.steps;
        return stats;
    }

    // ====================================================================== joints
    namespace {
        // Joint frame: origin at the anchor, local X along `axis` (PhysX twist / slide axis).
        PxQuat FrameRotation(const glm::vec3& axis) {
            const PxVec3 x(1.0f, 0.0f, 0.0f), a = ToPx(glm::normalize(axis));
            const float d = x.dot(a);
            if (d > 0.99999f) return PxQuat(PxIdentity);
            if (d < -0.99999f) return PxQuat(PxPi, PxVec3(0.0f, 1.0f, 0.0f));
            const PxVec3 c = x.cross(a);
            return PxQuat(c.x, c.y, c.z, 1.0f + d).getNormalized();
        }
        PxTransform LocalFrame(PxRigidActor* actor, const PxTransform& world) {
            return actor ? actor->getGlobalPose().transformInv(world) : world;
        }
        glm::vec3 PointVelocity(PxRigidActor* actor, const PxVec3& point) {
            PxRigidDynamic* d = actor ? actor->is<PxRigidDynamic>() : nullptr;
            if (!d) return glm::vec3(0.0f);
            const PxVec3 com = d->getGlobalPose().transform(d->getCMassLocalPose().p);
            return ToGlm(d->getLinearVelocity() + d->getAngularVelocity().cross(point - com));
        }
    }

    JointId CreateJoint(const JointDesc& desc) {
        if (!g.initialized) return {};
        BodySlot* a = desc.bodyA.Valid() ? Resolve(desc.bodyA) : nullptr;
        BodySlot* b = desc.bodyB.Valid() ? Resolve(desc.bodyB) : nullptr;
        if ((desc.bodyA.Valid() && !a) || (desc.bodyB.Valid() && !b) || (!a && !b) || a == b) {
            Log::Error() << "Physics: CreateJoint needs one or two distinct valid bodies";
            return {};
        }
        PxPhysics& sdk = *Core::SDK();
        PxRigidActor* actor0 = a ? a->actor : nullptr;
        PxRigidActor* actor1 = b ? b->actor : nullptr;
        const PxQuat frameRotation = FrameRotation(glm::length(desc.axis) > 1e-6f ? desc.axis : glm::vec3(0, 1, 0));
        const PxTransform frameA(ToPx(desc.anchor), frameRotation);
        const PxTransform frameB(ToPx(desc.type == JointType::SpringDamper ? desc.anchorB : desc.anchor), frameRotation);
        const PxTransform local0 = LocalFrame(actor0, frameA), local1 = LocalFrame(actor1, frameB);
        PxJoint* joint = nullptr;
        switch (desc.type) {
            case JointType::Hinge: {
                PxRevoluteJoint* j = PxRevoluteJointCreate(sdk, actor0, local0, actor1, local1);
                if (!j) break;
                if (desc.enableLimits) {
                    j->setLimit(PxJointAngularLimitPair(std::min(desc.lowerLimit, desc.upperLimit), std::max(desc.lowerLimit, desc.upperLimit)));
                    j->setRevoluteJointFlag(PxRevoluteJointFlag::eLIMIT_ENABLED, true);
                }
                if (desc.enableMotor) {
                    j->setDriveVelocity(desc.motorSpeed);
                    j->setDriveForceLimit(std::max(desc.maxMotorForce, 0.0f));
                    j->setRevoluteJointFlag(PxRevoluteJointFlag::eDRIVE_ENABLED, true);
                }
                joint = j;
                break;
            }
            case JointType::BallSocket:
                joint = PxSphericalJointCreate(sdk, actor0, local0, actor1, local1);
                break;
            case JointType::Prismatic: {
                PxD6Joint* j = PxD6JointCreate(sdk, actor0, local0, actor1, local1);
                if (!j) break;
                if (desc.enableLimits) {
                    j->setMotion(PxD6Axis::eX, PxD6Motion::eLIMITED);
                    j->setLinearLimit(PxD6Axis::eX, PxJointLinearLimitPair(std::min(desc.lowerLimit, desc.upperLimit), std::max(desc.lowerLimit, desc.upperLimit), PxSpring(0.0f, 0.0f)));
                }
                else j->setMotion(PxD6Axis::eX, PxD6Motion::eFREE);
                if (desc.enableMotor) {
                    j->setDrive(PxD6Drive::eX, PxD6JointDrive(0.0f, 1e6f, std::max(desc.maxMotorForce, 0.0f), false));
                    j->setDriveVelocity(PxVec3(desc.motorSpeed, 0.0f, 0.0f), PxVec3(0.0f));
                }
                joint = j;
                break;
            }
            case JointType::SpringDamper: {
                PxDistanceJoint* j = PxDistanceJointCreate(sdk, actor0, local0, actor1, local1);
                if (!j) break;
                const float rest = desc.restLength >= 0.0f ? desc.restLength : glm::length(desc.anchorB - desc.anchor);
                j->setMinDistance(rest);
                j->setMaxDistance(rest);
                j->setStiffness(std::max(desc.stiffness, 0.0f));
                j->setDamping(std::max(desc.damping, 0.0f));
                j->setDistanceJointFlags(PxDistanceJointFlag::eSPRING_ENABLED | PxDistanceJointFlag::eMIN_DISTANCE_ENABLED | PxDistanceJointFlag::eMAX_DISTANCE_ENABLED);
                joint = j;
                break;
            }
        }
        if (!joint) { Log::Error() << "Physics: joint creation failed"; return {}; }
        joint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, desc.collideConnected);
        if (PxRigidDynamic* d = actor0 ? actor0->is<PxRigidDynamic>() : nullptr; d && a->type == BodyType::Dynamic) d->wakeUp();
        if (PxRigidDynamic* d = actor1 ? actor1->is<PxRigidDynamic>() : nullptr; d && b->type == BodyType::Dynamic) d->wakeUp();

        uint32_t index;
        if (!g.freeJoints.empty()) { index = g.freeJoints.back(); g.freeJoints.pop_back(); }
        else { index = uint32_t(g.joints.size()); g.joints.emplace_back(); }
        JointSlot& slot = g.joints[index];
        slot.joint = joint;
        slot.type = desc.type;
        slot.a = desc.bodyA;
        slot.b = desc.bodyB;
        slot.alive = true;
        return JointId{ MakeId(TAG_JOINT, slot.generation, index) };
    }

    void DestroyJoint(JointId id) {
        if (Resolve(id)) ReleaseJointSlot(IndexOf(id.value));
    }

    bool IsValid(JointId id) { return Resolve(id) != nullptr; }

    JointState GetJointState(JointId id) {
        JointState s;
        JointSlot* slot = Resolve(id);
        if (!slot) return s;
        PxRigidActor* actor0 = nullptr;
        PxRigidActor* actor1 = nullptr;
        slot->joint->getActors(actor0, actor1);
        const PxTransform pose0 = actor0 ? actor0->getGlobalPose() * slot->joint->getLocalPose(PxJointActorIndex::eACTOR0) : slot->joint->getLocalPose(PxJointActorIndex::eACTOR0);
        const PxTransform pose1 = actor1 ? actor1->getGlobalPose() * slot->joint->getLocalPose(PxJointActorIndex::eACTOR1) : slot->joint->getLocalPose(PxJointActorIndex::eACTOR1);
        s.anchorA = ToGlm(pose0.p);
        s.anchorB = ToGlm(pose1.p);
        const glm::vec3 axis = ToGlm(pose0.q.getBasisVector0());
        switch (slot->type) {
            case JointType::Hinge: {
                PxRevoluteJoint* j = slot->joint->is<PxRevoluteJoint>();
                s.position = j->getAngle();
                s.speed = j->getVelocity();
                break;
            }
            case JointType::Prismatic: {
                const glm::vec3 d = s.anchorB - s.anchorA;
                s.position = glm::dot(axis, d);
                s.speed = glm::dot(PointVelocity(actor1, pose1.p) - PointVelocity(actor0, pose1.p), axis);
                break;
            }
            case JointType::SpringDamper: {
                const glm::vec3 d = s.anchorB - s.anchorA;
                s.position = glm::length(d);
                const glm::vec3 n = s.position > 1e-6f ? d / s.position : axis;
                s.speed = glm::dot(PointVelocity(actor1, pose1.p) - PointVelocity(actor0, pose0.p), n);
                break;
            }
            case JointType::BallSocket: break;
        }
        return s;
    }

    void SetJointMotor(JointId id, bool enable, float speed, float maxForce) {
        JointSlot* slot = Resolve(id);
        if (!slot) return;
        if (PxRevoluteJoint* j = slot->joint->is<PxRevoluteJoint>()) {
            j->setDriveVelocity(speed);
            j->setDriveForceLimit(std::max(maxForce, 0.0f));
            j->setRevoluteJointFlag(PxRevoluteJointFlag::eDRIVE_ENABLED, enable);
        }
        else if (PxD6Joint* j = slot->joint->is<PxD6Joint>()) {
            j->setDrive(PxD6Drive::eX, PxD6JointDrive(0.0f, enable ? 1e6f : 0.0f, enable ? std::max(maxForce, 0.0f) : 0.0f, false));
            j->setDriveVelocity(PxVec3(speed, 0.0f, 0.0f), PxVec3(0.0f));
        }
        PxRigidActor* actor0 = nullptr;
        PxRigidActor* actor1 = nullptr;
        slot->joint->getActors(actor0, actor1);
        for (PxRigidActor* actor : { actor0, actor1 }) {
            PxRigidDynamic* d = actor ? actor->is<PxRigidDynamic>() : nullptr;
            if (d && !(d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) d->wakeUp();
        }
    }
}
