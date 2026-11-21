// JSON scene files (see Phisiks/Scene/SceneSerializer.h). Uses nlohmann/json (MIT, vendor/).
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace Phisiks::SceneSerializer {
    namespace {
        using json = nlohmann::json;
        using namespace Physics;
        using namespace Physics::Advanced;

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
        template<class F> std::string NameOr(const json& j, const char* key, F def) { return j.contains(key) ? j.at(key).get<std::string>() : std::string(def); }

        // ---------------------------------------------------------------- materials
        json MaterialJson(const Render::MaterialDesc& m) {
            return { { "baseColor", V(m.baseColor) }, { "roughness", m.roughness }, { "metallic", m.metallic }, { "emissive", V(m.emissive) },
                     { "pattern", int(m.pattern) }, { "patternColor", V(m.patternColor) }, { "patternScale", m.patternScale }, { "lineWidth", m.lineWidth },
                     { "patternRoughness", m.patternRoughness }, { "patternGlow", m.patternGlow }, { "albedoTexture", m.albedoTexture }, { "textureScale", m.textureScale } };
        }
        Render::MaterialDesc MaterialFrom(const json& j) {
            Render::MaterialDesc m;
            Get(j, "baseColor", m.baseColor); Get(j, "roughness", m.roughness); Get(j, "metallic", m.metallic); Get(j, "emissive", m.emissive);
            GetEnum(j, "pattern", m.pattern); Get(j, "patternColor", m.patternColor); Get(j, "patternScale", m.patternScale); Get(j, "lineWidth", m.lineWidth);
            Get(j, "patternRoughness", m.patternRoughness); Get(j, "patternGlow", m.patternGlow); Get(j, "albedoTexture", m.albedoTexture); Get(j, "textureScale", m.textureScale);
            return m;
        }

        json PhysMaterial(const PhysicsMaterial& m) { return { { "staticFriction", m.staticFriction }, { "dynamicFriction", m.dynamicFriction }, { "restitution", m.restitution } }; }
        PhysicsMaterial PhysMaterialFrom(const json& j) { PhysicsMaterial m; Get(j, "staticFriction", m.staticFriction); Get(j, "dynamicFriction", m.dynamicFriction); Get(j, "restitution", m.restitution); return m; }
    }

    std::string ToJson(Scene& scene, bool pretty) {
        json root;
        root["format"] = "phisiks-scene";
        root["version"] = 1;
        root["name"] = scene.Name();
        // stable order: by entity id
        std::vector<Entity> entities;
        scene.Each<Transform>([&](Entity e, Transform&) { if (!scene.IsPendingDestroy(e)) entities.push_back(e); });
        std::sort(entities.begin(), entities.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
        std::map<uint32_t, int> index;
        for (size_t i = 0; i < entities.size(); ++i) index[entities[i].Id()] = int(i);
        std::set<std::string> materials;
        json list = json::array();
        for (Entity e : entities) {
            json j;
            j["name"] = e.GetName();
            const Entity parent = e.GetParent();
            j["parent"] = parent && index.count(parent.Id()) ? index[parent.Id()] : -1;
            const Transform& t = e.Get<Transform>();
            j["transform"] = { { "position", V(t.position) }, { "rotation", Q(t.rotation) }, { "scale", V(t.scale) } };
            if (const MeshRenderer* m = e.TryGet<MeshRenderer>()) {
                j["meshRenderer"] = { { "mesh", m->mesh }, { "material", m->material }, { "uvScale", V(m->uvScale) }, { "visible", m->visible }, { "castShadows", m->castShadows } };
                if (!m->material.empty()) materials.insert(m->material);
            }
            if (const Light* l = e.TryGet<Light>()) {
                j["light"] = { { "type", LightName(l->type) }, { "color", V(l->color) }, { "intensity", l->intensity }, { "range", l->range },
                               { "innerConeAngle", l->innerConeAngle }, { "outerConeAngle", l->outerConeAngle }, { "castShadows", l->castShadows } };
            }
            if (const Camera* c = e.TryGet<Camera>()) {
                j["camera"] = { { "orthographic", c->projection == Projection::Orthographic }, { "verticalFov", c->verticalFov }, { "orthoHeight", c->orthoHeight },
                                { "nearPlane", c->nearPlane }, { "farPlane", c->farPlane }, { "viewport", V(c->viewport) }, { "order", c->order },
                                { "target", c->target }, { "targetSize", V(c->targetSize) }, { "enabled", c->enabled }, { "primary", c->primary } };
            }
            if (const Collider* c = e.TryGet<Collider>()) {
                const ColliderDesc& s = c->shape;
                json cj = { { "shape", ShapeName(s.shape) }, { "halfExtents", V(s.halfExtents) }, { "radius", s.radius }, { "halfHeight", s.halfHeight },
                            { "localPosition", V(s.localPosition) }, { "localRotation", Q(s.localRotation) }, { "material", PhysMaterial(s.material) },
                            { "isTrigger", s.isTrigger }, { "layer", int(c->layer) } };
                if (!s.vertices.empty()) {
                    json verts = json::array();
                    for (const glm::vec3& v : s.vertices) verts.push_back(V(v));
                    cj["vertices"] = verts;
                }
                if (!s.indices.empty()) cj["indices"] = s.indices;
                j["collider"] = cj;
            }
            if (const Rigidbody* r = e.TryGet<Rigidbody>()) {
                j["rigidbody"] = { { "type", BodyName(r->type) }, { "mass", r->mass }, { "linearDamping", r->linearDamping }, { "angularDamping", r->angularDamping },
                                   { "continuousCollision", r->continuousCollision }, { "useGravity", r->useGravity } };
            }
            if (const AudioSource* a = e.TryGet<AudioSource>()) {
                j["audioSource"] = { { "clip", a->clip }, { "volume", a->volume }, { "loop", a->loop }, { "playOnStart", a->playOnStart } };
            }
            if (const ForceField* f = e.TryGet<ForceField>()) {
                j["forceField"] = { { "type", FieldName(f->type) }, { "vector", V(f->vector) }, { "strength", f->strength }, { "radius", f->radius },
                                    { "falloff", f->falloff }, { "axis", V(f->axis) }, { "influenceRadius", f->influenceRadius }, { "softening", f->softening },
                                    { "affectsRigidBodies", f->affectsRigidBodies }, { "affectsParticles", f->affectsParticles }, { "affectsSoftBodies", f->affectsSoftBodies } };
            }
            if (const ParticleSystem* p = e.TryGet<ParticleSystem>()) {
                j["particleSystem"] = { { "count", p->count }, { "spawnSize", V(p->spawnSize) }, { "initialVelocity", V(p->initialVelocity) }, { "radius", p->radius },
                                        { "mass", p->mass }, { "friction", p->friction }, { "damping", p->damping }, { "useGravity", p->useGravity },
                                        { "color", V(p->color) }, { "fastColor", V(p->fastColor) }, { "fastSpeed", p->fastSpeed }, { "seed", p->seed },
                                        { "emissionRate", p->emissionRate }, { "lifetime", p->lifetime }, { "emitDirection", V(p->emitDirection) },
                                        { "emitSpeed", p->emitSpeed }, { "emitSpread", p->emitSpread }, { "pushRigidBodies", p->pushRigidBodies } };
            }
            if (const SoftBody* s = e.TryGet<SoftBody>()) {
                j["softBody"] = { { "kind", SoftName(s->kind) }, { "size", V(s->size) }, { "resolution", V(s->resolution) }, { "mass", s->mass },
                                  { "compliance", s->compliance }, { "bendCompliance", s->bendCompliance }, { "volumeCompliance", s->volumeCompliance },
                                  { "pinned", s->pinned }, { "substeps", s->substeps }, { "damping", s->damping }, { "friction", s->friction },
                                  { "material", s->material }, { "pushRigidBodies", s->pushRigidBodies } };
                if (!s->material.empty()) materials.insert(s->material);
            }
            list.push_back(j);
        }
        root["entities"] = list;
        json mats = json::object();
        for (const std::string& name : materials) if (const Render::MaterialDesc* m = Render::FindMaterial(name)) mats[name] = MaterialJson(*m);
        root["materials"] = mats;
        return pretty ? root.dump(2) : root.dump();
    }

    bool FromJson(Scene& scene, const std::string& text, std::string* error) {
        json root;
        try { root = json::parse(text); }
        catch (const std::exception& e) { if (error) *error = std::string("parse error: ") + e.what(); return false; }
        if (!root.is_object() || root.value("format", std::string()) != "phisiks-scene" || !root.contains("entities") || !root["entities"].is_array()) {
            if (error) *error = "not a phisiks-scene file";
            return false;
        }
        try {
            if (root.contains("materials")) {
                for (auto it = root["materials"].begin(); it != root["materials"].end(); ++it) Render::RegisterMaterial(it.key(), MaterialFrom(it.value()));
            }
            scene.Clear();
            const json& list = root["entities"];
            std::vector<Entity> created;
            for (const json& j : list) {
                Entity e = scene.CreateEntity(j.value("name", std::string("Entity")));
                created.push_back(e);
                if (j.contains("transform")) {
                    Transform& t = e.Get<Transform>();
                    Get(j["transform"], "position", t.position); GetQ(j["transform"], "rotation", t.rotation); Get(j["transform"], "scale", t.scale);
                }
                if (j.contains("meshRenderer")) {
                    const json& m = j["meshRenderer"];
                    MeshRenderer& r = e.Add<MeshRenderer>();
                    Get(m, "mesh", r.mesh); Get(m, "material", r.material); Get(m, "uvScale", r.uvScale); Get(m, "visible", r.visible); Get(m, "castShadows", r.castShadows);
                }
                if (j.contains("light")) {
                    const json& l = j["light"];
                    Light& light = e.Add<Light>();
                    light.type = LightOf(NameOr(l, "type", "Point"));
                    Get(l, "color", light.color); Get(l, "intensity", light.intensity); Get(l, "range", light.range);
                    Get(l, "innerConeAngle", light.innerConeAngle); Get(l, "outerConeAngle", light.outerConeAngle); Get(l, "castShadows", light.castShadows);
                }
                if (j.contains("camera")) {
                    const json& c = j["camera"];
                    Camera& cam = e.Add<Camera>();
                    if (c.value("orthographic", false)) cam.projection = Projection::Orthographic;
                    Get(c, "verticalFov", cam.verticalFov); Get(c, "orthoHeight", cam.orthoHeight); Get(c, "nearPlane", cam.nearPlane); Get(c, "farPlane", cam.farPlane);
                    Get(c, "viewport", cam.viewport); Get(c, "order", cam.order); Get(c, "target", cam.target); Get(c, "targetSize", cam.targetSize);
                    Get(c, "enabled", cam.enabled); Get(c, "primary", cam.primary);
                }
                if (j.contains("collider")) {
                    const json& c = j["collider"];
                    Collider col;
                    col.shape.shape = ShapeOf(NameOr(c, "shape", "Box"));
                    Get(c, "halfExtents", col.shape.halfExtents); Get(c, "radius", col.shape.radius); Get(c, "halfHeight", col.shape.halfHeight);
                    Get(c, "localPosition", col.shape.localPosition); GetQ(c, "localRotation", col.shape.localRotation);
                    if (c.contains("material")) col.shape.material = PhysMaterialFrom(c["material"]);
                    Get(c, "isTrigger", col.shape.isTrigger);
                    if (c.contains("layer")) col.layer = Layer(c["layer"].get<int>());
                    if (c.contains("vertices")) for (const json& v : c["vertices"]) col.shape.vertices.push_back({ v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>() });
                    if (c.contains("indices")) col.shape.indices = c["indices"].get<std::vector<uint32_t>>();
                    e.Add<Collider>(col);
                }
                if (j.contains("rigidbody")) {
                    const json& r = j["rigidbody"];
                    Rigidbody rb;
                    rb.type = BodyOf(NameOr(r, "type", "Dynamic"));
                    Get(r, "mass", rb.mass); Get(r, "linearDamping", rb.linearDamping); Get(r, "angularDamping", rb.angularDamping);
                    Get(r, "continuousCollision", rb.continuousCollision); Get(r, "useGravity", rb.useGravity);
                    e.Add<Rigidbody>(rb);
                }
                if (j.contains("audioSource")) {
                    const json& a = j["audioSource"];
                    AudioSource& s = e.Add<AudioSource>();
                    Get(a, "clip", s.clip); Get(a, "volume", s.volume); Get(a, "loop", s.loop); Get(a, "playOnStart", s.playOnStart);
                }
                if (j.contains("forceField")) {
                    const json& f = j["forceField"];
                    ForceField ff;
                    ff.type = FieldOf(NameOr(f, "type", "Uniform"));
                    Get(f, "vector", ff.vector); Get(f, "strength", ff.strength); Get(f, "radius", ff.radius); Get(f, "falloff", ff.falloff); Get(f, "axis", ff.axis);
                    Get(f, "influenceRadius", ff.influenceRadius); Get(f, "softening", ff.softening);
                    Get(f, "affectsRigidBodies", ff.affectsRigidBodies); Get(f, "affectsParticles", ff.affectsParticles); Get(f, "affectsSoftBodies", ff.affectsSoftBodies);
                    e.Add<ForceField>(ff);
                }
                if (j.contains("particleSystem")) {
                    const json& p = j["particleSystem"];
                    ParticleSystem ps;
                    Get(p, "count", ps.count); Get(p, "spawnSize", ps.spawnSize); Get(p, "initialVelocity", ps.initialVelocity); Get(p, "radius", ps.radius);
                    Get(p, "mass", ps.mass); Get(p, "friction", ps.friction); Get(p, "damping", ps.damping); Get(p, "useGravity", ps.useGravity);
                    Get(p, "color", ps.color); Get(p, "fastColor", ps.fastColor); Get(p, "fastSpeed", ps.fastSpeed); Get(p, "seed", ps.seed);
                    Get(p, "emissionRate", ps.emissionRate); Get(p, "lifetime", ps.lifetime); Get(p, "emitDirection", ps.emitDirection);
                    Get(p, "emitSpeed", ps.emitSpeed); Get(p, "emitSpread", ps.emitSpread); Get(p, "pushRigidBodies", ps.pushRigidBodies);
                    e.Add<ParticleSystem>(ps);
                }
                if (j.contains("softBody")) {
                    const json& s = j["softBody"];
                    SoftBody sb;
                    sb.kind = SoftOf(NameOr(s, "kind", "Cloth"));
                    Get(s, "size", sb.size); Get(s, "resolution", sb.resolution); Get(s, "mass", sb.mass); Get(s, "compliance", sb.compliance);
                    Get(s, "bendCompliance", sb.bendCompliance); Get(s, "volumeCompliance", sb.volumeCompliance); Get(s, "pinned", sb.pinned);
                    Get(s, "substeps", sb.substeps); Get(s, "damping", sb.damping); Get(s, "friction", sb.friction); Get(s, "material", sb.material);
                    Get(s, "pushRigidBodies", sb.pushRigidBodies);
                    e.Add<SoftBody>(sb);
                }
            }
            for (size_t i = 0; i < list.size() && i < created.size(); ++i) {
                const int parent = list[i].value("parent", -1);
                if (parent >= 0 && size_t(parent) < created.size() && size_t(parent) != i) created[i].SetParent(created[size_t(parent)], false);
            }
        }
        catch (const std::exception& e) {
            if (error) *error = std::string("invalid scene: ") + e.what();
            return false;
        }
        return true;
    }

    bool Save(Scene& scene, const std::string& path) {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream file(path, std::ios::binary);
        if (!file) { Log::Error() << "SceneSerializer: cannot write " << path; return false; }
        file << ToJson(scene);
        Log::Info() << "SceneSerializer: saved '" << scene.Name() << "' to " << path;
        return bool(file);
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
