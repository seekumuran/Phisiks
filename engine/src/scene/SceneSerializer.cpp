// JSON scene files (see Phisiks/Scene/SceneSerializer.h). Uses nlohmann/json (MIT, vendor/).
//
// Every component type is described once, by a codec (key, write, patch), and every operation goes
// through the same table: scene files, entity JSON (undo history, duplicate, clipboard), component
// JSON (inspector undo, the agent interface) and play-mode snapshots.
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Phisiks::SceneSerializer {
    namespace {
        using json = nlohmann::json;
        using namespace Physics;
        using namespace Physics::Advanced;

        // ---------------------------------------------------------------- value helpers
        json V(const glm::vec2& v) { return json::array({ v.x, v.y }); }
        json V(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
        json V(const glm::vec4& v) { return json::array({ v.x, v.y, v.z, v.w }); }
        json V(const glm::ivec2& v) { return json::array({ v.x, v.y }); }
        json V(const glm::ivec3& v) { return json::array({ v.x, v.y, v.z }); }
        json Q(const glm::quat& q) { return json::array({ q.w, q.x, q.y, q.z }); }   // w first

        template<class T> void Get(const json& j, const char* key, T& out) { if (j.contains(key)) out = j.at(key).get<T>(); }
        void Get(const json& j, const char* key, glm::vec2& out) { if (j.contains(key)) { const json& a = j.at(key); out = { a.at(0).get<float>(), a.at(1).get<float>() }; } }
        void Get(const json& j, const char* key, glm::vec3& out) { if (j.contains(key)) { const json& a = j.at(key); out = { a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>() }; } }
        void Get(const json& j, const char* key, glm::vec4& out) { if (j.contains(key)) { const json& a = j.at(key); out = { a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>(), a.at(3).get<float>() }; } }
        void Get(const json& j, const char* key, glm::ivec2& out) { if (j.contains(key)) { const json& a = j.at(key); out = { a.at(0).get<int>(), a.at(1).get<int>() }; } }
        void Get(const json& j, const char* key, glm::ivec3& out) { if (j.contains(key)) { const json& a = j.at(key); out = { a.at(0).get<int>(), a.at(1).get<int>(), a.at(2).get<int>() }; } }
        void GetQ(const json& j, const char* key, glm::quat& out) { if (j.contains(key)) { const json& a = j.at(key); out = glm::quat(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>(), a.at(3).get<float>()); } }
        template<class E> void GetEnum(const json& j, const char* key, E& out) { if (j.contains(key)) out = E(j.at(key).get<int>()); }
        json Points(const std::vector<glm::vec3>& points) { json a = json::array(); for (const glm::vec3& p : points) a.push_back(V(p)); return a; }
        void GetPoints(const json& j, const char* key, std::vector<glm::vec3>& out) {
            if (!j.contains(key)) return;
            out.clear();
            for (const json& v : j.at(key)) out.push_back({ v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>() });
        }

        std::string GuidText(uint64_t guid) { char text[20]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(guid)); return text; }
        uint64_t GuidFrom(const json& j) {
            if (!j.contains("id")) return 0;
            const std::string text = j.at("id").get<std::string>();
            if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("invalid entity id '" + text + "'");
            return std::stoull(text, nullptr, 16);
        }

        // ---------------------------------------------------------------- enums as names
        const char* ShapeName(ShapeType s) { switch (s) { case ShapeType::Box: return "Box"; case ShapeType::Sphere: return "Sphere"; case ShapeType::Capsule: return "Capsule"; case ShapeType::ConvexMesh: return "ConvexMesh"; case ShapeType::TriangleMesh: return "TriangleMesh"; } return "Box"; }
        ShapeType ShapeOf(const std::string& n) { for (ShapeType s : { ShapeType::Box, ShapeType::Sphere, ShapeType::Capsule, ShapeType::ConvexMesh, ShapeType::TriangleMesh }) if (n == ShapeName(s)) return s; return ShapeType::Box; }
        const char* BodyName(BodyType t) { return t == BodyType::Static ? "Static" : t == BodyType::Kinematic ? "Kinematic" : "Dynamic"; }
        BodyType BodyOf(const std::string& n) { return n == "Static" ? BodyType::Static : n == "Kinematic" ? BodyType::Kinematic : BodyType::Dynamic; }
        const char* LightName(LightType t) { return t == LightType::Directional ? "Directional" : t == LightType::Spot ? "Spot" : "Point"; }
        LightType LightOf(const std::string& n) { return n == "Directional" ? LightType::Directional : n == "Spot" ? LightType::Spot : LightType::Point; }
        const char* FieldName(FieldType t) { switch (t) { case FieldType::Uniform: return "Uniform"; case FieldType::SphericalGravity: return "SphericalGravity"; case FieldType::Vortex: return "Vortex"; case FieldType::NBody: return "NBody"; } return "Uniform"; }
        FieldType FieldOf(const std::string& n) { for (FieldType t : { FieldType::Uniform, FieldType::SphericalGravity, FieldType::Vortex, FieldType::NBody }) if (n == FieldName(t)) return t; return FieldType::Uniform; }
        const char* SoftName(SoftBodyKind k) { return k == SoftBodyKind::Rope ? "Rope" : k == SoftBodyKind::Box ? "Box" : "Cloth"; }
        SoftBodyKind SoftOf(const std::string& n) { return n == "Rope" ? SoftBodyKind::Rope : n == "Box" ? SoftBodyKind::Box : SoftBodyKind::Cloth; }
        const char* JointName(JointType t) { switch (t) { case JointType::Hinge: return "Hinge"; case JointType::BallSocket: return "BallSocket"; case JointType::Prismatic: return "Prismatic"; case JointType::SpringDamper: return "SpringDamper"; } return "BallSocket"; }
        JointType JointOf(const std::string& n) { for (JointType t : { JointType::Hinge, JointType::BallSocket, JointType::Prismatic, JointType::SpringDamper }) if (n == JointName(t)) return t; return JointType::BallSocket; }
        template<class F> std::string NameOr(const json& j, const char* key, F def) { return j.contains(key) ? j.at(key).get<std::string>() : std::string(def); }

        // ---------------------------------------------------------------- materials
        json MaterialJson(const Render::MaterialDesc& m) {
            return { { "baseColor", V(m.baseColor) }, { "roughness", m.roughness }, { "metallic", m.metallic }, { "emissive", V(m.emissive) },
                     { "pattern", int(m.pattern) }, { "patternColor", V(m.patternColor) }, { "patternScale", m.patternScale }, { "lineWidth", m.lineWidth },
                     { "patternRoughness", m.patternRoughness }, { "patternGlow", m.patternGlow }, { "albedoTexture", m.albedoTexture }, { "textureScale", m.textureScale },
                     { "textureUsesMeshUV", m.textureUsesMeshUV }, { "opacity", m.opacity } };
        }
        void MaterialPatch(const json& j, Render::MaterialDesc& m) {
            Get(j, "baseColor", m.baseColor); Get(j, "roughness", m.roughness); Get(j, "metallic", m.metallic); Get(j, "emissive", m.emissive);
            GetEnum(j, "pattern", m.pattern); Get(j, "patternColor", m.patternColor); Get(j, "patternScale", m.patternScale); Get(j, "lineWidth", m.lineWidth);
            Get(j, "patternRoughness", m.patternRoughness); Get(j, "patternGlow", m.patternGlow); Get(j, "albedoTexture", m.albedoTexture); Get(j, "textureScale", m.textureScale);
            Get(j, "textureUsesMeshUV", m.textureUsesMeshUV); Get(j, "opacity", m.opacity);
        }
        json PhysMaterial(const PhysicsMaterial& m) { return { { "staticFriction", m.staticFriction }, { "dynamicFriction", m.dynamicFriction }, { "restitution", m.restitution } }; }
        void PhysMaterialPatch(const json& j, PhysicsMaterial& m) { Get(j, "staticFriction", m.staticFriction); Get(j, "dynamicFriction", m.dynamicFriction); Get(j, "restitution", m.restitution); }

        // ---------------------------------------------------------------- component codecs
        // write: the authored fields (runtime fields are never written). patch: fields present in
        // the JSON override the value; others keep what the value had (the current component, or the
        // defaults for a new one). Patching never touches runtime fields.
        void Write(json& j, const MeshRenderer& m) { j = { { "mesh", m.mesh }, { "material", m.material }, { "uvScale", V(m.uvScale) }, { "visible", m.visible }, { "castShadows", m.castShadows } }; }
        void Patch(const json& j, MeshRenderer& r) { Get(j, "mesh", r.mesh); Get(j, "material", r.material); Get(j, "uvScale", r.uvScale); Get(j, "visible", r.visible); Get(j, "castShadows", r.castShadows); }

        void Write(json& j, const Light& l) {
            j = { { "type", LightName(l.type) }, { "color", V(l.color) }, { "intensity", l.intensity }, { "range", l.range },
                  { "innerConeAngle", l.innerConeAngle }, { "outerConeAngle", l.outerConeAngle }, { "castShadows", l.castShadows } };
        }
        void Patch(const json& j, Light& l) {
            if (j.contains("type")) l.type = LightOf(j.at("type").get<std::string>());
            Get(j, "color", l.color); Get(j, "intensity", l.intensity); Get(j, "range", l.range);
            Get(j, "innerConeAngle", l.innerConeAngle); Get(j, "outerConeAngle", l.outerConeAngle); Get(j, "castShadows", l.castShadows);
        }

        void Write(json& j, const Camera& c) {
            j = { { "orthographic", c.projection == Projection::Orthographic }, { "verticalFov", c.verticalFov }, { "orthoHeight", c.orthoHeight },
                  { "nearPlane", c.nearPlane }, { "farPlane", c.farPlane }, { "viewport", V(c.viewport) }, { "order", c.order },
                  { "target", c.target }, { "targetSize", V(c.targetSize) }, { "entityIdColors", c.entityIdColors }, { "enabled", c.enabled }, { "primary", c.primary } };
        }
        void Patch(const json& j, Camera& c) {
            if (j.contains("orthographic")) c.projection = j.at("orthographic").get<bool>() ? Projection::Orthographic : Projection::Perspective;
            Get(j, "verticalFov", c.verticalFov); Get(j, "orthoHeight", c.orthoHeight); Get(j, "nearPlane", c.nearPlane); Get(j, "farPlane", c.farPlane);
            Get(j, "viewport", c.viewport); Get(j, "order", c.order); Get(j, "target", c.target); Get(j, "targetSize", c.targetSize);
            Get(j, "entityIdColors", c.entityIdColors); Get(j, "enabled", c.enabled); Get(j, "primary", c.primary);
        }

        void Write(json& j, const Collider& c) {
            const ColliderDesc& s = c.shape;
            j = { { "shape", ShapeName(s.shape) }, { "halfExtents", V(s.halfExtents) }, { "radius", s.radius }, { "halfHeight", s.halfHeight },
                  { "localPosition", V(s.localPosition) }, { "localRotation", Q(s.localRotation) }, { "material", PhysMaterial(s.material) },
                  { "isTrigger", s.isTrigger }, { "layer", int(c.layer) } };
            if (!s.vertices.empty()) j["vertices"] = Points(s.vertices);
            if (!s.indices.empty()) j["indices"] = s.indices;
        }
        void Patch(const json& j, Collider& c) {
            if (j.contains("shape")) c.shape.shape = ShapeOf(j.at("shape").get<std::string>());
            Get(j, "halfExtents", c.shape.halfExtents); Get(j, "radius", c.shape.radius); Get(j, "halfHeight", c.shape.halfHeight);
            Get(j, "localPosition", c.shape.localPosition); GetQ(j, "localRotation", c.shape.localRotation);
            if (j.contains("material")) PhysMaterialPatch(j.at("material"), c.shape.material);
            Get(j, "isTrigger", c.shape.isTrigger);
            if (j.contains("layer")) c.layer = Layer(j.at("layer").get<int>());
            GetPoints(j, "vertices", c.shape.vertices);
            if (j.contains("indices")) c.shape.indices = j.at("indices").get<std::vector<uint32_t>>();
        }

        void Write(json& j, const Rigidbody& r) {
            j = { { "type", BodyName(r.type) }, { "mass", r.mass }, { "linearDamping", r.linearDamping }, { "angularDamping", r.angularDamping },
                  { "continuousCollision", r.continuousCollision }, { "useGravity", r.useGravity } };
        }
        void Patch(const json& j, Rigidbody& r) {
            if (j.contains("type")) r.type = BodyOf(j.at("type").get<std::string>());
            Get(j, "mass", r.mass); Get(j, "linearDamping", r.linearDamping); Get(j, "angularDamping", r.angularDamping);
            Get(j, "continuousCollision", r.continuousCollision); Get(j, "useGravity", r.useGravity);
        }

        void Write(json& j, const Joint& k) {
            j = { { "type", JointName(k.type) }, { "connectedTo", k.connectedTo }, { "anchor", V(k.anchor) }, { "axis", V(k.axis) },
                  { "connectedAnchor", V(k.connectedAnchor) }, { "enableLimits", k.enableLimits }, { "lowerLimit", k.lowerLimit }, { "upperLimit", k.upperLimit },
                  { "enableMotor", k.enableMotor }, { "motorSpeed", k.motorSpeed }, { "maxMotorForce", k.maxMotorForce },
                  { "stiffness", k.stiffness }, { "damping", k.damping }, { "restLength", k.restLength }, { "collideConnected", k.collideConnected } };
        }
        void Patch(const json& j, Joint& k) {
            if (j.contains("type")) k.type = JointOf(j.at("type").get<std::string>());
            Get(j, "connectedTo", k.connectedTo); Get(j, "anchor", k.anchor); Get(j, "axis", k.axis); Get(j, "connectedAnchor", k.connectedAnchor);
            Get(j, "enableLimits", k.enableLimits); Get(j, "lowerLimit", k.lowerLimit); Get(j, "upperLimit", k.upperLimit);
            Get(j, "enableMotor", k.enableMotor); Get(j, "motorSpeed", k.motorSpeed); Get(j, "maxMotorForce", k.maxMotorForce);
            Get(j, "stiffness", k.stiffness); Get(j, "damping", k.damping); Get(j, "restLength", k.restLength); Get(j, "collideConnected", k.collideConnected);
        }

        void Write(json& j, const AudioSource& a) {
            j = { { "clip", a.clip }, { "volume", a.volume }, { "loop", a.loop }, { "playOnStart", a.playOnStart }, { "spatial", a.spatial },
                  { "minDistance", a.minDistance }, { "maxDistance", a.maxDistance }, { "dopplerFactor", a.dopplerFactor } };
        }
        void Patch(const json& j, AudioSource& a) {
            Get(j, "clip", a.clip); Get(j, "volume", a.volume); Get(j, "loop", a.loop); Get(j, "playOnStart", a.playOnStart); Get(j, "spatial", a.spatial);
            Get(j, "minDistance", a.minDistance); Get(j, "maxDistance", a.maxDistance); Get(j, "dopplerFactor", a.dopplerFactor);
        }

        void Write(json& j, const ForceField& f) {
            j = { { "type", FieldName(f.type) }, { "vector", V(f.vector) }, { "strength", f.strength }, { "radius", f.radius },
                  { "falloff", f.falloff }, { "axis", V(f.axis) }, { "influenceRadius", f.influenceRadius }, { "softening", f.softening },
                  { "affectsRigidBodies", f.affectsRigidBodies }, { "affectsParticles", f.affectsParticles }, { "affectsSoftBodies", f.affectsSoftBodies } };
        }
        void Patch(const json& j, ForceField& f) {
            if (j.contains("type")) f.type = FieldOf(j.at("type").get<std::string>());
            Get(j, "vector", f.vector); Get(j, "strength", f.strength); Get(j, "radius", f.radius); Get(j, "falloff", f.falloff); Get(j, "axis", f.axis);
            Get(j, "influenceRadius", f.influenceRadius); Get(j, "softening", f.softening);
            Get(j, "affectsRigidBodies", f.affectsRigidBodies); Get(j, "affectsParticles", f.affectsParticles); Get(j, "affectsSoftBodies", f.affectsSoftBodies);
        }

        void Write(json& j, const ParticleSystem& p) {
            j = { { "count", p.count }, { "spawnSize", V(p.spawnSize) }, { "initialVelocity", V(p.initialVelocity) }, { "radius", p.radius },
                  { "mass", p.mass }, { "friction", p.friction }, { "damping", p.damping }, { "useGravity", p.useGravity },
                  { "color", V(p.color) }, { "fastColor", V(p.fastColor) }, { "fastSpeed", p.fastSpeed }, { "seed", p.seed },
                  { "emissionRate", p.emissionRate }, { "lifetime", p.lifetime }, { "emitDirection", V(p.emitDirection) },
                  { "emitSpeed", p.emitSpeed }, { "emitSpread", p.emitSpread }, { "pushRigidBodies", p.pushRigidBodies } };
        }
        void Patch(const json& j, ParticleSystem& p) {
            Get(j, "count", p.count); Get(j, "spawnSize", p.spawnSize); Get(j, "initialVelocity", p.initialVelocity); Get(j, "radius", p.radius);
            Get(j, "mass", p.mass); Get(j, "friction", p.friction); Get(j, "damping", p.damping); Get(j, "useGravity", p.useGravity);
            Get(j, "color", p.color); Get(j, "fastColor", p.fastColor); Get(j, "fastSpeed", p.fastSpeed); Get(j, "seed", p.seed);
            Get(j, "emissionRate", p.emissionRate); Get(j, "lifetime", p.lifetime); Get(j, "emitDirection", p.emitDirection);
            Get(j, "emitSpeed", p.emitSpeed); Get(j, "emitSpread", p.emitSpread); Get(j, "pushRigidBodies", p.pushRigidBodies);
        }

        void Write(json& j, const SoftBody& s) {
            j = { { "kind", SoftName(s.kind) }, { "size", V(s.size) }, { "resolution", V(s.resolution) }, { "mass", s.mass },
                  { "compliance", s.compliance }, { "bendCompliance", s.bendCompliance }, { "volumeCompliance", s.volumeCompliance },
                  { "pinned", s.pinned }, { "substeps", s.substeps }, { "damping", s.damping }, { "friction", s.friction },
                  { "material", s.material }, { "pushRigidBodies", s.pushRigidBodies } };
        }
        void Patch(const json& j, SoftBody& s) {
            if (j.contains("kind")) s.kind = SoftOf(j.at("kind").get<std::string>());
            Get(j, "size", s.size); Get(j, "resolution", s.resolution); Get(j, "mass", s.mass); Get(j, "compliance", s.compliance);
            Get(j, "bendCompliance", s.bendCompliance); Get(j, "volumeCompliance", s.volumeCompliance); Get(j, "pinned", s.pinned);
            Get(j, "substeps", s.substeps); Get(j, "damping", s.damping); Get(j, "friction", s.friction); Get(j, "material", s.material);
            Get(j, "pushRigidBodies", s.pushRigidBodies);
        }

        void Write(json& j, const Script& s) { j = { { "source", s.source }, { "file", s.file }, { "enabled", s.enabled } }; }
        void Patch(const json& j, Script& s) {
            const std::string source = s.source, file = s.file;
            Get(j, "source", s.source); Get(j, "file", s.file); Get(j, "enabled", s.enabled);
            if (s.source != source || s.file != file) { s.started = false; s.error.clear(); s.sourceStamp = 0; }   // run the new code (on_start again)
        }

        json NavDesc(const AI::NavGridDesc& d) {
            return { { "boundsMin", V(d.boundsMin) }, { "boundsMax", V(d.boundsMax) }, { "cellSize", d.cellSize }, { "agentRadius", d.agentRadius },
                     { "agentHeight", d.agentHeight }, { "maxStepHeight", d.maxStepHeight }, { "maxSlopeDegrees", d.maxSlopeDegrees }, { "layers", d.layers } };
        }
        void NavDescPatch(const json& j, AI::NavGridDesc& d) {
            Get(j, "boundsMin", d.boundsMin); Get(j, "boundsMax", d.boundsMax); Get(j, "cellSize", d.cellSize); Get(j, "agentRadius", d.agentRadius);
            Get(j, "agentHeight", d.agentHeight); Get(j, "maxStepHeight", d.maxStepHeight); Get(j, "maxSlopeDegrees", d.maxSlopeDegrees); Get(j, "layers", d.layers);
        }
        void Write(json& j, const NavigationSurface& n) { j = { { "desc", NavDesc(n.desc) } }; }
        void Patch(const json& j, NavigationSurface& n) { if (j.contains("desc")) { NavDescPatch(j.at("desc"), n.desc); n.rebuild = true; } }

        void Write(json& j, const NavAgent& a) {
            j = { { "target", V(a.target) }, { "hasTarget", a.hasTarget }, { "maxSpeed", a.maxSpeed }, { "maxAcceleration", a.maxAcceleration },
                  { "radius", a.radius }, { "arriveRadius", a.arriveRadius }, { "separation", a.separation }, { "avoidance", a.avoidance },
                  { "heightOffset", a.heightOffset }, { "repathInterval", a.repathInterval } };
        }
        void Patch(const json& j, NavAgent& a) {
            Get(j, "target", a.target); Get(j, "hasTarget", a.hasTarget); Get(j, "maxSpeed", a.maxSpeed); Get(j, "maxAcceleration", a.maxAcceleration);
            Get(j, "radius", a.radius); Get(j, "arriveRadius", a.arriveRadius); Get(j, "separation", a.separation); Get(j, "avoidance", a.avoidance);
            Get(j, "heightOffset", a.heightOffset); Get(j, "repathInterval", a.repathInterval);
        }

        void Write(json& j, const Terrain& t) {
            const World::TerrainGenerator& g = t.generator;
            json layers = json::array(), rules = json::array();
            for (const Render::SplatLayer& l : t.layers) layers.push_back({ { "color", V(l.color) }, { "roughness", l.roughness }, { "detail", l.detail } });
            for (const World::SplatRule& r : t.rules)
                rules.push_back({ { "minHeight", r.minHeight }, { "maxHeight", r.maxHeight }, { "minSlope", r.minSlope }, { "maxSlope", r.maxSlope }, { "blend", r.blend }, { "noise", r.noise } });
            j = { { "generator", { { "resolution", g.resolution }, { "cellSize", g.cellSize }, { "height", g.height }, { "frequency", g.frequency },
                                   { "octaves", g.octaves }, { "persistence", g.persistence }, { "ridge", g.ridge }, { "seed", g.seed },
                                   { "flatRadius", g.flatRadius }, { "flatBlend", g.flatBlend } } },
                  { "heightmapImage", t.heightmapImage }, { "imageHeight", t.imageHeight }, { "layers", layers }, { "rules", rules },
                  { "splatmapSize", t.splatmapSize }, { "detailScale", t.detailScale }, { "collision", t.collision } };
        }
        void Patch(const json& j, Terrain& t);
        void PatchTerrain(const json& j, Terrain& t) {
            if (j.contains("generator")) {
                const json& g = j.at("generator");
                World::TerrainGenerator& o = t.generator;
                Get(g, "resolution", o.resolution); Get(g, "cellSize", o.cellSize); Get(g, "height", o.height); Get(g, "frequency", o.frequency);
                Get(g, "octaves", o.octaves); Get(g, "persistence", o.persistence); Get(g, "ridge", o.ridge); Get(g, "seed", o.seed);
                Get(g, "flatRadius", o.flatRadius); Get(g, "flatBlend", o.flatBlend);
            }
            Get(j, "heightmapImage", t.heightmapImage); Get(j, "imageHeight", t.imageHeight);
            if (j.contains("layers")) for (size_t i = 0; i < 4 && i < j.at("layers").size(); ++i) {
                const json& l = j.at("layers")[i];
                Get(l, "color", t.layers[i].color); Get(l, "roughness", t.layers[i].roughness); Get(l, "detail", t.layers[i].detail);
            }
            if (j.contains("rules")) for (size_t i = 0; i < 4 && i < j.at("rules").size(); ++i) {
                const json& r = j.at("rules")[i];
                World::SplatRule& o = t.rules[i];
                Get(r, "minHeight", o.minHeight); Get(r, "maxHeight", o.maxHeight); Get(r, "minSlope", o.minSlope); Get(r, "maxSlope", o.maxSlope);
                Get(r, "blend", o.blend); Get(r, "noise", o.noise);
            }
            Get(j, "splatmapSize", t.splatmapSize); Get(j, "detailScale", t.detailScale); Get(j, "collision", t.collision);
        }
        void Patch(const json& j, Terrain& t) {   // rebuilt only when an authored field changed
            json before; Write(before, t);
            PatchTerrain(j, t);
            json after; Write(after, t);
            if (before != after) t.built = false;
        }

        void Write(json& j, const FoliageScatter& f) {
            const World::ScatterDesc& s = f.scatter;
            j = { { "mesh", f.mesh }, { "material", f.material }, { "castShadows", f.castShadows },
                  { "scatter", { { "count", s.count }, { "seed", s.seed }, { "minHeight", s.minHeight }, { "maxHeight", s.maxHeight },
                                 { "maxSlopeDegrees", s.maxSlopeDegrees }, { "minScale", s.minScale }, { "maxScale", s.maxScale },
                                 { "minSpacing", s.minSpacing }, { "keepOutRadius", s.keepOutRadius }, { "sink", s.sink }, { "alignToNormal", s.alignToNormal } } } };
        }
        void PatchFoliage(const json& j, FoliageScatter& f);
        void Patch(const json& j, FoliageScatter& f) {
            json before; Write(before, f);
            PatchFoliage(j, f);
            json after; Write(after, f);
            if (before != after) f.built = false;
        }
        void PatchFoliage(const json& j, FoliageScatter& f) {
            Get(j, "mesh", f.mesh); Get(j, "material", f.material); Get(j, "castShadows", f.castShadows);
            if (j.contains("scatter")) {
                const json& s = j.at("scatter");
                World::ScatterDesc& o = f.scatter;
                Get(s, "count", o.count); Get(s, "seed", o.seed); Get(s, "minHeight", o.minHeight); Get(s, "maxHeight", o.maxHeight);
                Get(s, "maxSlopeDegrees", o.maxSlopeDegrees); Get(s, "minScale", o.minScale); Get(s, "maxScale", o.maxScale);
                Get(s, "minSpacing", o.minSpacing); Get(s, "keepOutRadius", o.keepOutRadius); Get(s, "sink", o.sink); Get(s, "alignToNormal", o.alignToNormal);
            }
        }

        void Write(json& j, const Decal& d) {
            j = { { "size", V(d.size) }, { "pattern", int(d.pattern) }, { "blend", int(d.blend) }, { "color", V(d.color) },
                  { "angleFade", d.angleFade }, { "seed", d.seed }, { "visible", d.visible } };
        }
        void Patch(const json& j, Decal& d) {
            Get(j, "size", d.size); GetEnum(j, "pattern", d.pattern); GetEnum(j, "blend", d.blend); Get(j, "color", d.color);
            Get(j, "angleFade", d.angleFade); Get(j, "seed", d.seed); Get(j, "visible", d.visible);
        }

        void Write(json& j, const Trail& t) {
            j = { { "lifetime", t.lifetime }, { "width", t.width }, { "tailWidth", t.tailWidth }, { "headColor", V(t.headColor) }, { "tailColor", V(t.tailColor) },
                  { "minDistance", t.minDistance }, { "maxPoints", t.maxPoints }, { "emitting", t.emitting }, { "offset", V(t.offset) } };
        }
        void Patch(const json& j, Trail& t) {
            Get(j, "lifetime", t.lifetime); Get(j, "width", t.width); Get(j, "tailWidth", t.tailWidth); Get(j, "headColor", t.headColor); Get(j, "tailColor", t.tailColor);
            Get(j, "minDistance", t.minDistance); Get(j, "maxPoints", t.maxPoints); Get(j, "emitting", t.emitting); Get(j, "offset", t.offset);
        }

        void Write(json& j, const Beam& b) {
            j = { { "target", b.target }, { "endPoint", V(b.endPoint) }, { "offset", V(b.offset) }, { "width", b.width }, { "color", V(b.color) },
                  { "jitter", b.jitter }, { "frequency", b.frequency }, { "segments", b.segments }, { "raycast", b.raycast }, { "enabled", b.enabled } };
        }
        void Patch(const json& j, Beam& b) {
            Get(j, "target", b.target); Get(j, "endPoint", b.endPoint); Get(j, "offset", b.offset); Get(j, "width", b.width); Get(j, "color", b.color);
            Get(j, "jitter", b.jitter); Get(j, "frequency", b.frequency); Get(j, "segments", b.segments); Get(j, "raycast", b.raycast); Get(j, "enabled", b.enabled);
        }

        void Write(json& j, const OrbitPath& o) {
            j = { { "radiusX", o.radiusX }, { "radiusZ", o.radiusZ }, { "width", o.width }, { "color", V(o.color) }, { "segments", o.segments }, { "dash", o.dash } };
        }
        void Patch(const json& j, OrbitPath& o) {
            Get(j, "radiusX", o.radiusX); Get(j, "radiusZ", o.radiusZ); Get(j, "width", o.width); Get(j, "color", o.color); Get(j, "segments", o.segments); Get(j, "dash", o.dash);
        }

        void Write(json& j, const SkinnedModel& m) {
            j = { { "path", m.path }, { "clip", m.clip }, { "speed", m.speed }, { "time", m.time }, { "loop", m.loop }, { "playing", m.playing },
                  { "method", m.method == Animation::SkinningMethod::Linear ? "Linear" : "DualQuaternion" } };
        }
        void Patch(const json& j, SkinnedModel& m) {
            Get(j, "path", m.path); Get(j, "clip", m.clip); Get(j, "speed", m.speed); Get(j, "time", m.time); Get(j, "loop", m.loop); Get(j, "playing", m.playing);
            if (j.contains("method")) m.method = j.at("method").get<std::string>() == "Linear" ? Animation::SkinningMethod::Linear : Animation::SkinningMethod::DualQuaternion;
        }

        void Write(json& j, const CameraPath& c) {
            j = { { "type", c.type == Animation::SplineType::Bezier ? "Bezier" : "CatmullRom" }, { "points", Points(c.points) }, { "lookAt", Points(c.lookAt) },
                  { "duration", c.duration }, { "time", c.time }, { "loop", c.loop }, { "closed", c.closed }, { "playing", c.playing } };
        }
        void Patch(const json& j, CameraPath& c) {
            if (j.contains("type")) c.type = j.at("type").get<std::string>() == "Bezier" ? Animation::SplineType::Bezier : Animation::SplineType::CatmullRom;
            GetPoints(j, "points", c.points); GetPoints(j, "lookAt", c.lookAt);
            Get(j, "duration", c.duration); Get(j, "time", c.time); Get(j, "loop", c.loop); Get(j, "closed", c.closed); Get(j, "playing", c.playing);
            if (j.contains("points") || j.contains("lookAt") || j.contains("type") || j.contains("closed")) c.built = false;
        }

        void Write(json& j, const Health& h) { j = { { "current", h.current }, { "max", h.max }, { "dead", h.dead } }; }
        void Patch(const json& j, Health& h) { Get(j, "current", h.current); Get(j, "max", h.max); Get(j, "dead", h.dead); }
        void Write(json& j, const Interactable& i) { j = { { "prompt", i.prompt } }; }
        void Patch(const json& j, Interactable& i) { Get(j, "prompt", i.prompt); }

        // The CAD document is stored as a JSON object (not a string) so scene files stay readable.
        void Write(json& j, const CadModel& c) {
            json doc = json::object();
            if (!c.document.empty()) {
                try { doc = json::parse(c.document); } catch (...) { doc = c.document; }
            }
            j = { { "document", doc } };
        }
        void Patch(const json& j, CadModel& c) {
            if (!j.contains("document")) return;
            const json& d = j.at("document");
            std::string text;
            if (d.is_object()) {
                if (!d.empty() && d.value("format", std::string()) != "phisiks-cad") throw std::runtime_error("cadModel.document must be a PHISIKS CAD document (\"format\": \"phisiks-cad\")");
                text = d.empty() ? std::string() : d.dump();
            }
            else throw std::runtime_error("cadModel.document must be an object");
            if (text != c.document) { c.document = std::move(text); ++c.revision; }
        }

        struct Codec {
            const char* key;
            std::function<bool(Entity)> has;
            std::function<json(Entity)> write;
            std::function<void(Entity, const json&)> patch;   // adds (from the defaults) or patches in place
            std::function<void(Entity)> remove;
            std::function<std::string(Entity)> material;      // material name used, if any
        };

        template<class T>
        Codec Make(const char* key, std::function<std::string(const T&)> material = {}) {
            Codec c;
            c.key = key;
            c.has = [](Entity e) { return e.Has<T>(); };
            c.write = [](Entity e) { json j; Write(j, e.Get<T>()); return j; };
            c.patch = [](Entity e, const json& j) {
                if (!j.is_object()) throw std::runtime_error("component value must be an object");
                T value = e.Has<T>() ? e.Get<T>() : T{};
                Patch(j, value);
                e.Add<T>(std::move(value));   // emplace_or_replace: on_construct / on_update hooks run (physics rebuild, ...)
            };
            c.remove = [](Entity e) { e.Remove<T>(); };
            if (material) c.material = [material](Entity e) { return material(e.Get<T>()); };
            return c;
        }

        // Order = the order keys appear in entity JSON.
        const std::vector<Codec>& Codecs() {
            static const std::vector<Codec> codecs = {
                Make<MeshRenderer>("meshRenderer", [](const MeshRenderer& m) { return m.material; }),
                Make<Light>("light"),
                Make<Camera>("camera"),
                Make<Collider>("collider"),
                Make<Rigidbody>("rigidbody"),
                Make<Joint>("joint"),
                Make<AudioSource>("audioSource"),
                Make<ForceField>("forceField"),
                Make<ParticleSystem>("particleSystem"),
                Make<SoftBody>("softBody", [](const SoftBody& s) { return s.material; }),
                Make<Script>("script"),
                Make<NavigationSurface>("navigationSurface"),
                Make<NavAgent>("navAgent"),
                Make<Terrain>("terrain"),
                Make<FoliageScatter>("foliageScatter", [](const FoliageScatter& f) { return f.material; }),
                Make<Decal>("decal"),
                Make<Trail>("trail"),
                Make<Beam>("beam"),
                Make<OrbitPath>("orbitPath"),
                Make<SkinnedModel>("skinnedModel"),
                Make<CameraPath>("cameraPath"),
                Make<Health>("health"),
                Make<Interactable>("interactable"),
                Make<CadModel>("cadModel"),
            };
            return codecs;
        }
        const Codec* FindCodec(const std::string& key) {
            for (const Codec& c : Codecs()) if (key == c.key) return &c;
            return nullptr;
        }

        // ---------------------------------------------------------------- entity JSON
        json EntityJson(Entity e, std::set<std::string>* materials) {
            json j;
            j["id"] = GuidText(e.GetGuid());
            j["name"] = e.GetName();
            const Transform& t = e.Get<Transform>();
            j["transform"] = { { "position", V(t.position) }, { "rotation", Q(t.rotation) }, { "scale", V(t.scale) } };
            for (const Codec& c : Codecs()) {
                if (!c.has(e)) continue;
                j[c.key] = c.write(e);
                if (materials && c.material) { const std::string m = c.material(e); if (!m.empty()) materials->insert(m); }
            }
            return j;
        }

        // Validates the shape of an entity object before anything is changed (the codecs throw on
        // type errors; running them on a scratch scene would need a scene, so the checks are
        // structural here and the codecs' own exceptions are caught by the callers).
        void CheckEntity(const json& j) {
            if (!j.is_object()) throw std::runtime_error("entity must be an object");
            if (j.contains("name") && !j.at("name").is_string()) throw std::runtime_error("entity name must be a string");
            if (j.contains("transform") && !j.at("transform").is_object()) throw std::runtime_error("transform must be an object");
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string& key = it.key();
                if (key == "id" || key == "name" || key == "parent" || key == "transform" || key == "children") continue;
                if (FindCodec(key) && !it.value().is_object()) throw std::runtime_error("component '" + key + "' must be an object");
            }
            GuidFrom(j);
        }

        // Name, transform and components of `e` become exactly those of `j` (components absent from
        // `j` are removed). Parent and children are not touched.
        void ApplyEntity(Entity e, const json& j) {
            if (j.contains("name")) e.SetName(j.at("name").get<std::string>());
            Transform t;
            if (j.contains("transform")) {
                const json& tj = j.at("transform");
                Get(tj, "position", t.position); GetQ(tj, "rotation", t.rotation); Get(tj, "scale", t.scale);
            }
            e.Get<Transform>() = t;
            for (const Codec& c : Codecs()) {
                if (j.contains(c.key)) c.patch(e, j.at(c.key));
                else if (c.has(e)) c.remove(e);
            }
            if (e.Has<Collider>()) e.MarkPhysicsDirty();   // rebuild the body at the restored pose
        }

        void RegisterMaterials(const json& root) {
            if (!root.contains("materials") || !root.at("materials").is_object()) return;
            for (auto it = root.at("materials").begin(); it != root.at("materials").end(); ++it) {
                Render::MaterialDesc m;
                if (const Render::MaterialDesc* existing = Render::FindMaterial(it.key())) m = *existing;
                MaterialPatch(it.value(), m);
                Render::RegisterMaterial(it.key(), m);
            }
        }
        json MaterialsJson(const std::set<std::string>& names) {
            json mats = json::object();
            for (const std::string& name : names) if (const Render::MaterialDesc* m = Render::FindMaterial(name)) mats[name] = MaterialJson(*m);
            return mats;
        }

        // A recursive entity tree (EntityToJson with children): creates `j` and its children under `parent`.
        Entity CreateTree(Scene& scene, const json& j, Entity parent, bool keepGuids) {
            Entity e = scene.CreateEntity(j.value("name", std::string("Entity")));
            if (keepGuids) {
                const uint64_t guid = GuidFrom(j);
                if (guid && !scene.SetGuid(e, guid)) Log::Warning() << "SceneSerializer: entity id " << GuidText(guid) << " is in use; '" << e.GetName() << "' got a new id";
            }
            ApplyEntity(e, j);
            if (parent) e.SetParent(parent, false);
            if (j.contains("children")) for (const json& child : j.at("children")) CreateTree(scene, child, e, keepGuids);
            return e;
        }
        void CheckTree(const json& j, std::set<uint64_t>& guids) {
            CheckEntity(j);
            if (const uint64_t g = GuidFrom(j); g && !guids.insert(g).second) throw std::runtime_error("duplicate entity id " + GuidText(g));
            if (j.contains("children")) {
                if (!j.at("children").is_array()) throw std::runtime_error("children must be an array");
                for (const json& c : j.at("children")) CheckTree(c, guids);
            }
        }

        json TreeJson(Entity e, std::set<std::string>* materials) {
            json j = EntityJson(e, materials);
            json children = json::array();
            for (Entity child : e.GetChildren()) if (!e.GetScene()->IsPendingDestroy(child)) children.push_back(TreeJson(child, materials));
            if (!children.empty()) j["children"] = children;
            return j;
        }

        // Scene files: parse + validate (version, structure, parent indices, ids).
        json ParseScene(const std::string& text) {
            json root = json::parse(text);
            if (!root.is_object() || root.value("format", std::string()) != "phisiks-scene" || !root.contains("entities") || !root["entities"].is_array())
                throw std::runtime_error("not a phisiks-scene file");
            const int version = !root.contains("version") ? 1 : root["version"].is_number_integer() ? root["version"].get<int>() : -1;   // no key: version 1
            if (version < 1 || version > FORMAT_VERSION)
                throw std::runtime_error("unsupported phisiks-scene version " + std::to_string(version) + " (this build reads 1.." + std::to_string(FORMAT_VERSION) + ")");
            const json& list = root["entities"];
            std::set<uint64_t> guids;
            for (size_t i = 0; i < list.size(); ++i) {
                const json& j = list[i];
                CheckEntity(j);
                const int parent = j.value("parent", -1);
                if (parent >= 0 && (size_t(parent) >= list.size() || size_t(parent) == i)) throw std::runtime_error("entity " + std::to_string(i) + " has an invalid parent index");
                if (const uint64_t g = GuidFrom(j); g && !guids.insert(g).second) throw std::runtime_error("duplicate entity id " + GuidText(g));
            }
            if (root.contains("materials") && !root["materials"].is_object()) throw std::runtime_error("materials must be an object");
            return root;
        }

        // Runs `f`; turns codec / JSON exceptions into `error`.
        template<class F> bool Guarded(std::string* error, const char* what, F&& f) {
            try { f(); return true; }
            catch (const std::exception& e) { if (error) *error = std::string(what) + e.what(); return false; }
        }
    }

    // ==================================================================== scene files
    std::string ToJson(Scene& scene, bool pretty) {
        json root;
        root["format"] = "phisiks-scene";
        root["version"] = FORMAT_VERSION;
        root["name"] = scene.Name();
        std::vector<Entity> entities;   // stable order: by entity id
        scene.Each<Transform>([&](Entity e, Transform&) { if (!scene.IsPendingDestroy(e)) entities.push_back(e); });
        std::sort(entities.begin(), entities.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
        std::map<uint32_t, int> index;
        for (size_t i = 0; i < entities.size(); ++i) index[entities[i].Id()] = int(i);
        std::set<std::string> materials;
        json list = json::array();
        for (Entity e : entities) {
            json j = EntityJson(e, &materials);
            const Entity parent = e.GetParent();
            j["parent"] = parent && index.count(parent.Id()) ? index[parent.Id()] : -1;
            list.push_back(j);
        }
        root["entities"] = list;
        root["materials"] = MaterialsJson(materials);
        return pretty ? root.dump(2) : root.dump();
    }

    // Two-phase: the whole file is parsed and validated first; the scene is cleared and rebuilt only
    // when that succeeded, so a damaged or incompatible file never destroys the open scene.
    bool FromJson(Scene& scene, const std::string& text, std::string* error) {
        json root;
        if (!Guarded(error, "invalid scene (nothing was changed): ", [&] { root = ParseScene(text); })) return false;
        // Dry run of every component value on a scratch scene: catches type errors inside values.
        {
            Scene scratch("validate");
            if (!Guarded(error, "invalid scene (nothing was changed): ", [&] {
                for (const json& j : root["entities"]) ApplyEntity(scratch.CreateEntity(), j);
            })) return false;
        }
        RegisterMaterials(root);
        scene.Clear();
        const json& list = root["entities"];
        std::vector<Entity> created;
        created.reserve(list.size());
        for (const json& j : list) {
            Entity e = scene.CreateEntity(j.value("name", std::string("Entity")));
            if (const uint64_t guid = GuidFrom(j)) scene.SetGuid(e, guid);   // version 1 files: the new random ids stay
            ApplyEntity(e, j);
            created.push_back(e);
        }
        for (size_t i = 0; i < list.size(); ++i) {
            const int parent = list[i].value("parent", -1);
            if (parent >= 0) created[i].SetParent(created[size_t(parent)], false);
        }
        return true;
    }

    bool RestoreSnapshot(Scene& scene, const std::string& snapshot, std::string* error) {
        json root;
        if (!Guarded(error, "invalid snapshot (nothing was changed): ", [&] { root = ParseScene(snapshot); })) return false;
        const json& list = root["entities"];
        RegisterMaterials(root);
        std::unordered_map<uint64_t, size_t> wanted;
        for (size_t i = 0; i < list.size(); ++i) if (const uint64_t g = GuidFrom(list[i])) wanted[g] = i;
        // 1. entities that are not in the snapshot (created since) go
        std::vector<Entity> extra;
        scene.Each<Transform>([&](Entity e, Transform&) { if (!wanted.count(e.GetGuid())) extra.push_back(e); });
        for (Entity e : extra) if (e) scene.DestroyImmediate(e);
        // 2. every snapshot entity: the live one keeps its handle; a missing one is re-created
        std::vector<Entity> entities(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            const uint64_t guid = GuidFrom(list[i]);
            Entity e = guid ? scene.FindByGuid(guid) : Entity();
            if (!e) { e = scene.CreateEntity(); if (guid) scene.SetGuid(e, guid); }
            entities[i] = e;
        }
        if (!Guarded(error, "snapshot restore failed: ", [&] {
            for (size_t i = 0; i < list.size(); ++i) ApplyEntity(entities[i], list[i]);
        })) return false;
        for (size_t i = 0; i < list.size(); ++i) {
            const int parent = list[i].value("parent", -1);
            const Entity want = parent >= 0 ? entities[size_t(parent)] : Entity();
            if (entities[i].GetParent() != want) entities[i].SetParent(want, false);
        }
        return true;
    }

    // ==================================================================== entities and components
    std::string EntityToJson(Entity entity, bool withChildren, bool pretty) {
        if (!entity) return {};
        std::set<std::string> materials;
        json j = withChildren ? TreeJson(entity, &materials) : EntityJson(entity, &materials);
        j["materials"] = MaterialsJson(materials);
        return pretty ? j.dump(2) : j.dump();
    }

    Entity EntityFromJson(Scene& scene, const std::string& text, Entity parent, bool keepGuids, std::string* error) {
        json j;
        if (!Guarded(error, "invalid entity JSON: ", [&] { j = json::parse(text); std::set<uint64_t> g; CheckTree(j, g); })) return {};
        {
            Scene scratch("validate");
            if (!Guarded(error, "invalid entity JSON: ", [&] { CreateTree(scratch, j, Entity(), false); })) return {};
        }
        RegisterMaterials(j);
        return CreateTree(scene, j, parent, keepGuids);
    }

    bool ApplyEntityJson(Entity entity, const std::string& text, std::string* error) {
        if (!entity) { if (error) *error = "invalid entity"; return false; }
        json j;
        if (!Guarded(error, "invalid entity JSON: ", [&] { j = json::parse(text); CheckEntity(j); })) return false;
        {
            Scene scratch("validate");
            if (!Guarded(error, "invalid entity JSON: ", [&] { ApplyEntity(scratch.CreateEntity(), j); })) return false;
        }
        RegisterMaterials(j);
        ApplyEntity(entity, j);
        return true;
    }

    const std::vector<std::string>& ComponentKeys() {
        static const std::vector<std::string> keys = [] { std::vector<std::string> k; for (const Codec& c : Codecs()) k.push_back(c.key); return k; }();
        return keys;
    }

    bool HasComponent(Entity entity, const std::string& key) {
        const Codec* c = FindCodec(key);
        return entity && c && c->has(entity);
    }

    std::string ComponentToJson(Entity entity, const std::string& key) {
        const Codec* c = FindCodec(key);
        if (!entity || !c || !c->has(entity)) return {};
        return c->write(entity).dump();
    }

    bool SetComponentJson(Entity entity, const std::string& key, const std::string& text, std::string* error) {
        const Codec* c = FindCodec(key);
        if (!entity) { if (error) *error = "invalid entity"; return false; }
        if (!c) { if (error) *error = "unknown component '" + key + "'"; return false; }
        json j;
        if (!Guarded(error, "invalid component JSON: ", [&] { j = json::parse(text); if (!j.is_object()) throw std::runtime_error("component value must be an object"); })) return false;
        {
            Scene scratch("validate");
            Entity probe = scratch.CreateEntity();
            if (!Guarded(error, "invalid component JSON: ", [&] { if (c->has(entity)) c->patch(probe, c->write(entity)); c->patch(probe, j); })) return false;
        }
        c->patch(entity, j);
        return true;
    }

    bool RemoveComponent(Entity entity, const std::string& key) {
        const Codec* c = FindCodec(key);
        if (!entity || !c || !c->has(entity)) return false;
        c->remove(entity);
        return true;
    }

    std::string MaterialToJson(const std::string& name) {
        const Render::MaterialDesc* m = Render::FindMaterial(name);
        return m ? MaterialJson(*m).dump() : std::string();
    }

    bool SetMaterialJson(const std::string& name, const std::string& text, std::string* error) {
        if (name.empty()) { if (error) *error = "empty material name"; return false; }
        Render::MaterialDesc m;
        if (const Render::MaterialDesc* existing = Render::FindMaterial(name)) m = *existing;
        if (!Guarded(error, "invalid material JSON: ", [&] { const json j = json::parse(text); if (!j.is_object()) throw std::runtime_error("material must be an object"); MaterialPatch(j, m); })) return false;
        Render::RegisterMaterial(name, m);
        return true;
    }

    std::vector<UnsavedComponent> UnsavedComponents(Scene& scene) {
        // Since format version 2 every component's data is stored. What remains is code: the C++
        // callbacks of the gameplay components (re-attach them in code after loading).
        size_t deathCallbacks = 0, interactCallbacks = 0;
        scene.Each<Health>([&](Entity, Health& h) { if (h.onDeath) ++deathCallbacks; });
        scene.Each<Interactable>([&](Entity, Interactable& i) { if (i.onInteract) ++interactCallbacks; });
        std::vector<UnsavedComponent> out;
        if (deathCallbacks) out.push_back({ "Health.onDeath (C++ callback)", deathCallbacks });
        if (interactCallbacks) out.push_back({ "Interactable.onInteract (C++ callback)", interactCallbacks });
        return out;
    }

    // Atomic save: the scene is written to "<path>.tmp" and renamed over the target only when the
    // write succeeded, so an interrupted or failed save leaves the previous file intact.
    bool Save(Scene& scene, const std::string& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path target(path);
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        const fs::path temp = fs::path(path + ".tmp");
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) { Log::Error() << "SceneSerializer: cannot write " << temp.string(); return false; }
            file << ToJson(scene);
            file.flush();
            if (!file) { file.close(); fs::remove(temp, ec); Log::Error() << "SceneSerializer: write failed for " << temp.string(); return false; }
        }
        fs::rename(temp, target, ec);
        if (ec) {
            Log::Error() << "SceneSerializer: cannot replace " << path << ": " << ec.message();
            fs::remove(temp, ec);
            return false;
        }
        Log::Info() << "SceneSerializer: saved '" << scene.Name() << "' to " << path;
        return true;
    }

    bool Load(Scene& scene, const std::string& path, std::string* error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) { if (error) *error = "cannot open " + path; return false; }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string localError;
        const bool ok = FromJson(scene, buffer.str(), &localError);
        if (!ok) Log::Error() << "SceneSerializer: " << path << ": " << localError;
        else Log::Info() << "SceneSerializer: loaded " << path << " into '" << scene.Name() << "' (" << scene.EntityCount() << " entities)";
        if (error) *error = localError;
        return ok;
    }
}
