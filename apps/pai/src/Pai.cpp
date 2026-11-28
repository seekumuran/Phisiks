// PAI dispatcher: JSON-RPC 2.0 methods over the editor's public API (see Pai.h, docs/PAI.md).
#include "Pai.h"

#include "Editor.h"
#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Version.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>

using json = nlohmann::json;
using namespace Phisiks;

namespace Pai {
    namespace {
        enum Code { ParseError = -32700, InvalidRequest = -32600, MethodNotFound = -32601, InvalidParams = -32602, InternalError = -32603,
                    PermissionDenied = -32001, NotNow = -32002, NotFound = -32003, LimitExceeded = -32004 };

        struct RpcError : std::runtime_error {
            int code;
            RpcError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
        };
        [[noreturn]] void Fail(int code, const std::string& message) { throw RpcError(code, message); }

        enum class Needs { Read, Write, Simulate };
        const char* NeedsName(Needs n) { return n == Needs::Read ? "read" : n == Needs::Write ? "write" : "simulate"; }

        struct Context {
            GameContext& ctx;
            const Limits& limits;
            Stats& stats;
        };
        using Handler = std::function<json(Context&, const json& params)>;
        struct Method {
            Needs needs;
            const char* params;        // human-readable parameter list (manifest)
            const char* description;
            Handler handler;
        };

        // stepping state (sim.step): frames left to run, consumed by the stdio pump / FrameStep()
        int g_stepFrames = 0;

        std::string Hex(uint64_t v) { char t[20]; std::snprintf(t, sizeof(t), "%016llx", static_cast<unsigned long long>(v)); return t; }
        uint64_t ParseHex(const json& j, const char* what) {
            if (!j.is_string()) Fail(InvalidParams, std::string(what) + " must be a 16-digit hex string");
            const std::string s = j.get<std::string>();
            if (s.empty() || s.size() > 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) Fail(InvalidParams, std::string(what) + " must be a 16-digit hex string");
            return std::stoull(s, nullptr, 16);
        }
        Scene& RequireScene(Context& c) {
            Scene* s = c.ctx.CurrentScene();
            if (!s) Fail(NotNow, "no scene is open");
            return *s;
        }
        Entity EntityParam(Context& c, const json& p, const char* key = "id") {
            Scene& scene = RequireScene(c);
            if (p.contains(key)) {
                Entity e = scene.FindByGuid(ParseHex(p.at(key), key));
                if (!e || Editor::IsEditorEntity(e)) Fail(NotFound, std::string("no entity with ") + key + " " + p.at(key).get<std::string>());
                return e;
            }
            if (p.contains("name") && std::string(key) == "id") {
                if (!p.at("name").is_string()) Fail(InvalidParams, "name must be a string");
                Entity e = scene.FindByName(p.at("name").get<std::string>());
                if (!e) Fail(NotFound, "no entity named '" + p.at("name").get<std::string>() + "'");
                return e;
            }
            Fail(InvalidParams, std::string("missing '") + key + "' (entity id)");
        }
        json Vec(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
        glm::vec3 VecParam(const json& j, const char* what) {
            if (!j.is_array() || j.size() != 3) Fail(InvalidParams, std::string(what) + " must be [x, y, z]");
            glm::vec3 v;
            for (int i = 0; i < 3; ++i) {
                if (!j[size_t(i)].is_number()) Fail(InvalidParams, std::string(what) + " must contain numbers");
                v[i] = j[size_t(i)].get<float>();
                if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e7f) Fail(InvalidParams, std::string(what) + " out of range");
            }
            return v;
        }
        std::string StringParam(Context& c, const json& p, const char* key, bool required = true) {
            if (!p.contains(key)) { if (required) Fail(InvalidParams, std::string("missing '") + key + "'"); return {}; }
            if (!p.at(key).is_string()) Fail(InvalidParams, std::string(key) + " must be a string");
            const std::string s = p.at(key).get<std::string>();
            if (s.size() > c.limits.maxNameLength) Fail(LimitExceeded, std::string(key) + " is longer than " + std::to_string(c.limits.maxNameLength) + " characters");
            return s;
        }

        json Summary(Entity e) {
            json comps = json::array();
            for (const std::string& k : SceneSerializer::ComponentKeys()) if (SceneSerializer::HasComponent(e, k)) comps.push_back(k);
            const Entity parent = e.GetParent();
            return { { "id", Hex(e.GetGuid()) }, { "name", e.GetName() }, { "parent", parent ? json(Hex(parent.GetGuid())) : json(nullptr) },
                     { "children", e.GetChildren().size() }, { "components", comps } };
        }

        // File-like fields may only hold built-in mesh names or project asset references: no paths,
        // so a client cannot make the editor read arbitrary files. Lua code cannot be written at all.
        void CheckValueSafety(const std::string& key, const json& value) {
            if (key == "script") Fail(PermissionDenied, "scripts cannot be written through PAI (no code execution)");
            auto check = [&](const char* field, bool builtinAllowed) {
                if (!value.contains(field)) return;
                if (!value.at(field).is_string()) Fail(InvalidParams, key + "." + field + " must be a string");
                const std::string v = value.at(field).get<std::string>();
                if (v.empty() || Assets::IsReference(v)) return;
                const bool pathLike = v.find_first_of("/\\:.") != std::string::npos;
                if (builtinAllowed && !pathLike) return;   // "Cube", "Sphere", a registered mesh name
                Fail(PermissionDenied, key + "." + field + " accepts " + (builtinAllowed ? "a built-in mesh name or " : "") + "a project asset reference (asset:<id>/<name>), not a file path");
            };
            if (key == "meshRenderer") check("mesh", true);
            if (key == "foliageScatter") check("mesh", true);
            if (key == "audioSource") check("clip", false);
            if (key == "skinnedModel") check("path", false);
            if (key == "terrain") check("heightmapImage", false);
        }

        // A mutation: refused while playing; one undo step.
        template<class F> json Mutate(Context& c, const std::string& label, F&& f) {
            RequireScene(c);
            if (Editor::CurrentMode() == Editor::Mode::Play) Fail(NotNow, "the simulation is playing: stop it first (sim.stop) to edit the document");
            if (!Editor::BeginEdit(c.ctx, "PAI: " + label)) Fail(NotNow, "editing is not possible now");
            json result;
            try { result = f(); }
            catch (...) { Editor::EndEdit(c.ctx); throw; }
            Editor::EndEdit(c.ctx);
            ++c.stats.mutations;
            return result;
        }

        json AssetJson(const AssetInfo& a) {
            return { { "id", Hex(a.guid) }, { "type", ToString(a.type) }, { "path", a.path }, { "name", a.name }, { "size", a.size },
                     { "hash", Hex(a.hash) }, { "revision", a.revision }, { "missing", a.missing }, { "source", a.source }, { "imported", a.imported },
                     { "reference", Editor::ProjectAssets().Reference(a.guid) } };
        }

        const std::map<std::string, Method>& Methods() {
            static const std::map<std::string, Method> methods = {
                // ------------------------------------------------------------------ discovery
                { "pai.describe", { Needs::Read, "{}", "the capability manifest (this list, with permissions and limits)", nullptr } },
                { "initialize", { Needs::Read, "{ client?: string }", "handshake: protocol, version, engine, granted permissions", nullptr } },
                // ------------------------------------------------------------------ read
                { "project.info", { Needs::Read, "{}", "project folder, open scene and file, edit / play mode, unsaved changes, history", [](Context& c, const json&) {
                    Scene* s = c.ctx.CurrentScene();
                    const Editor::HistoryState h = Editor::History();
                    return json{ { "project", Editor::ProjectDirectory() }, { "scene", s ? json(s->Name()) : json(nullptr) }, { "sceneFile", Editor::ScenePath() },
                                 { "mode", Editor::CurrentMode() == Editor::Mode::Play ? "play" : "edit" }, { "unsaved", Editor::HasUnsavedChanges(c.ctx) },
                                 { "history", { { "undo", h.undoCount }, { "redo", h.redoCount }, { "undoLabel", h.undoLabel }, { "redoLabel", h.redoLabel } } } };
                } } },
                { "engine.state", { Needs::Read, "{}", "engine version, simulation state and timing, render statistics", [](Context& c, const json&) {
                    const RenderStats r = GetRenderStats();
                    return json{ { "version", PHISIKS_VERSION_STRING }, { "state", ToString(c.ctx.state) }, { "frame", c.ctx.stats.frameIndex },
                                 { "simulationTime", c.ctx.stats.simulationTime }, { "fixedSteps", c.ctx.stats.totalFixedSteps },
                                 { "cpuFrameMs", c.ctx.stats.cpuFrameMs }, { "renderObjects", r.meshObjects }, { "drawCommands", r.drawCommands }, { "trianglesDrawn", r.trianglesDrawn } };
                } } },
                { "scene.components", { Needs::Read, "{}", "the component keys entities can have (SceneSerializer)", [](Context&, const json&) {
                    return json{ { "components", SceneSerializer::ComponentKeys() } };
                } } },
                { "scene.list", { Needs::Read, "{ name?: substring, component?: key, offset?: n, limit?: n }", "entities (id, name, parent, child count, component keys)", [](Context& c, const json& p) {
                    Scene& scene = RequireScene(c);
                    const std::string name = p.contains("name") ? StringParam(c, p, "name") : std::string();
                    const std::string comp = p.contains("component") ? StringParam(c, p, "component") : std::string();
                    const size_t offset = p.value("offset", size_t(0));
                    const size_t limit = std::min(p.value("limit", c.limits.maxListed), c.limits.maxListed);
                    std::vector<Entity> all;
                    scene.Each<Transform>([&](Entity e, Transform&) {
                        if (scene.IsPendingDestroy(e) || Editor::IsEditorEntity(e)) return;
                        if (!name.empty() && e.GetName().find(name) == std::string::npos) return;
                        if (!comp.empty() && !SceneSerializer::HasComponent(e, comp)) return;
                        all.push_back(e);
                    });
                    std::sort(all.begin(), all.end(), [](const Entity& a, const Entity& b) { return a.GetGuid() < b.GetGuid(); });
                    json list = json::array();
                    for (size_t i = offset; i < all.size() && list.size() < limit; ++i) list.push_back(Summary(all[i]));
                    return json{ { "total", all.size() }, { "offset", offset }, { "entities", list } };
                } } },
                { "scene.get", { Needs::Read, "{ id | name }", "one entity: transform (local, world), components with their values, children", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    json full = json::parse(SceneSerializer::EntityToJson(e, false));
                    json comps = json::object();
                    for (const std::string& k : SceneSerializer::ComponentKeys()) if (full.contains(k)) comps[k] = full[k];
                    json children = json::array();
                    for (Entity ch : e.GetChildren()) children.push_back(Hex(ch.GetGuid()));
                    json out = Summary(e);
                    out["transform"] = full["transform"];
                    out["world"] = { { "position", Vec(e.WorldPosition()) } };
                    out["components"] = comps;
                    out["childIds"] = children;
                    if (full.contains("materials")) out["materials"] = full["materials"];
                    return out;
                } } },
                { "assets.types", { Needs::Read, "{}", "asset types and their file formats", [](Context&, const json&) {
                    return json{ { "mesh", { ".obj", ".stl" } }, { "model", { ".gltf", ".glb" } }, { "texture", { ".png", ".jpg", ".jpeg", ".tga", ".bmp" } },
                                 { "audio", { ".wav" } }, { "scene", { ".scene.json" } }, { "script", { ".lua" } }, { "cad", { ".step", ".stp", ".iges", ".igs" } },
                                 { "cad-document", { ".phcad" } } };
                } } },
                { "assets.list", { Needs::Read, "{ type?: string, folder?: string, offset?: n, limit?: n }", "assets of the open project", [](Context& c, const json& p) {
                    const std::string type = p.contains("type") ? StringParam(c, p, "type") : std::string();
                    const std::string folder = p.contains("folder") ? StringParam(c, p, "folder") : std::string();
                    const size_t offset = p.value("offset", size_t(0));
                    const size_t limit = std::min(p.value("limit", c.limits.maxListed), c.limits.maxListed);
                    json list = json::array();
                    size_t total = 0, index = 0;
                    for (const AssetInfo& a : Editor::ProjectAssets().List(folder)) {
                        if (!type.empty() && type != ToString(a.type)) continue;
                        ++total;
                        if (index++ < offset || list.size() >= limit) continue;
                        list.push_back(AssetJson(a));
                    }
                    return json{ { "project", Editor::ProjectDirectory() }, { "total", total }, { "assets", list } };
                } } },
                { "assets.get", { Needs::Read, "{ id }", "one asset's metadata and how often the scene uses it", [](Context& c, const json& p) {
                    if (!p.contains("id")) Fail(InvalidParams, "missing 'id'");
                    AssetInfo a;
                    if (!Editor::ProjectAssets().Find(ParseHex(p.at("id"), "id"), a)) Fail(NotFound, "no asset " + p.at("id").get<std::string>());
                    json out = AssetJson(a);
                    size_t uses = 0;
                    if (Scene* s = c.ctx.CurrentScene()) for (const AssetReference& r : AssetDatabase::CollectReferences(*s)) uses += r.asset == a.guid;
                    out["usedInScene"] = uses;
                    return out;
                } } },
                { "assets.missing", { Needs::Read, "{}", "asset references in the scene that do not resolve", [](Context& c, const json&) {
                    json list = json::array();
                    for (const AssetReference& r : AssetDatabase::CollectReferences(RequireScene(c)))
                        if (!r.resolved) list.push_back({ { "entity", r.entity ? json(Hex(r.entity)) : json(nullptr) }, { "component", r.component }, { "field", r.field }, { "value", r.value } });
                    return json{ { "missing", list } };
                } } },
                { "history.get", { Needs::Read, "{}", "undo / redo state", [](Context&, const json&) {
                    const Editor::HistoryState h = Editor::History();
                    return json{ { "canUndo", h.canUndo }, { "canRedo", h.canRedo }, { "undoLabel", h.undoLabel }, { "redoLabel", h.redoLabel },
                                 { "undoCount", h.undoCount }, { "redoCount", h.redoCount }, { "bytes", h.bytes } };
                } } },
                { "sim.status", { Needs::Read, "{}", "edit / play mode, running or paused, simulated time, frames left to step", [](Context& c, const json&) {
                    return json{ { "mode", Editor::CurrentMode() == Editor::Mode::Play ? "play" : "edit" }, { "state", ToString(c.ctx.state) },
                                 { "simulationTime", c.ctx.stats.simulationTime }, { "fixedSteps", c.ctx.stats.totalFixedSteps }, { "stepping", g_stepFrames } };
                } } },
                { "sim.metrics", { Needs::Read, "{ ids?: [id], component?: key }", "world pose and linear velocity of entities (default: every rigid body)", [](Context& c, const json& p) {
                    Scene& scene = RequireScene(c);
                    std::vector<Entity> list;
                    if (p.contains("ids")) {
                        if (!p.at("ids").is_array() || p.at("ids").size() > c.limits.maxListed) Fail(InvalidParams, "ids must be an array (at most maxListed)");
                        for (const json& id : p.at("ids")) { Entity e = scene.FindByGuid(ParseHex(id, "ids[]")); if (!e) Fail(NotFound, "no entity " + id.get<std::string>()); list.push_back(e); }
                    }
                    else scene.Each<Rigidbody>([&](Entity e, Rigidbody&) { if (list.size() < c.limits.maxListed) list.push_back(e); });
                    json out = json::array();
                    for (Entity e : list) {
                        const glm::quat q = e.WorldRotation();
                        out.push_back({ { "id", Hex(e.GetGuid()) }, { "name", e.GetName() }, { "position", Vec(e.WorldPosition()) },
                                        { "rotation", json::array({ q.w, q.x, q.y, q.z }) }, { "linearVelocity", Vec(e.GetLinearVelocity()) } });
                    }
                    return json{ { "simulationTime", c.ctx.stats.simulationTime }, { "entities", out } };
                } } },
                // ------------------------------------------------------------------ write
                { "scene.create", { Needs::Write, "{ name, parent?: id, position?: [3], rotationDegrees?: [3], scale?: [3], components?: { key: value } }",
                    "create an entity (one undo step); component values are validated like scene files", [](Context& c, const json& p) {
                    const std::string name = StringParam(c, p, "name");
                    if (p.contains("components") && (!p.at("components").is_object() || p.at("components").size() > SceneSerializer::ComponentKeys().size())) Fail(InvalidParams, "components must be an object of component values");
                    if (p.contains("components")) for (auto it = p.at("components").begin(); it != p.at("components").end(); ++it) CheckValueSafety(it.key(), it.value());
                    Entity parent = p.contains("parent") && !p.at("parent").is_null() ? EntityParam(c, p, "parent") : Entity();
                    return Mutate(c, "create '" + name + "'", [&] {
                        Scene& scene = RequireScene(c);
                        Entity e = scene.CreateEntity(name);
                        try {
                            if (p.contains("position")) e.Get<Transform>().position = VecParam(p.at("position"), "position");
                            if (p.contains("rotationDegrees")) e.Get<Transform>().rotation = glm::quat(glm::radians(VecParam(p.at("rotationDegrees"), "rotationDegrees")));
                            if (p.contains("scale")) e.Get<Transform>().scale = VecParam(p.at("scale"), "scale");
                            if (parent) e.SetParent(parent, false);
                            if (p.contains("components")) for (auto it = p.at("components").begin(); it != p.at("components").end(); ++it) {
                                std::string error;
                                if (!SceneSerializer::SetComponentJson(e, it.key(), it.value().dump(), &error)) Fail(InvalidParams, error);
                            }
                        }
                        catch (...) { scene.DestroyImmediate(e); throw; }
                        return Summary(e);
                    });
                } } },
                { "scene.delete", { Needs::Write, "{ id | name }", "delete an entity and its children (one undo step)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string id = Hex(e.GetGuid()), name = e.GetName();
                    return Mutate(c, "delete '" + name + "'", [&] {
                        if (Editor::SelectedEntity() == e.Id() + 1) Editor::Select(0);
                        RequireScene(c).DestroyImmediate(e);
                        return json{ { "deleted", id } };
                    });
                } } },
                { "scene.rename", { Needs::Write, "{ id, newName }", "rename an entity", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string name = StringParam(c, p, "newName");
                    return Mutate(c, "rename '" + e.GetName() + "'", [&] { e.SetName(name); return Summary(e); });
                } } },
                { "scene.set_transform", { Needs::Write, "{ id, position?: [3], rotationDegrees?: [3] | rotation?: [w,x,y,z], scale?: [3] }", "set the local transform", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    Transform t = e.Get<Transform>();
                    if (p.contains("position")) t.position = VecParam(p.at("position"), "position");
                    if (p.contains("rotationDegrees")) t.rotation = glm::quat(glm::radians(VecParam(p.at("rotationDegrees"), "rotationDegrees")));
                    if (p.contains("rotation")) {
                        const json& q = p.at("rotation");
                        if (!q.is_array() || q.size() != 4) Fail(InvalidParams, "rotation must be [w, x, y, z]");
                        glm::quat r(q[0].get<float>(), q[1].get<float>(), q[2].get<float>(), q[3].get<float>());
                        if (!(glm::length(r) > 1e-6f)) Fail(InvalidParams, "rotation must not be zero");
                        t.rotation = glm::normalize(r);
                    }
                    if (p.contains("scale")) t.scale = VecParam(p.at("scale"), "scale");
                    return Mutate(c, "move '" + e.GetName() + "'", [&] {
                        e.Get<Transform>() = t;
                        if (e.Has<Collider>()) e.MarkPhysicsDirty();
                        return json{ { "position", Vec(t.position) }, { "rotation", json::array({ t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z }) }, { "scale", Vec(t.scale) } };
                    });
                } } },
                { "scene.set_component", { Needs::Write, "{ id, component: key, value: { field: value } }", "add a component or change fields of one (validated; other fields keep their values)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string key = StringParam(c, p, "component");
                    if (!p.contains("value") || !p.at("value").is_object()) Fail(InvalidParams, "value must be an object");
                    CheckValueSafety(key, p.at("value"));
                    return Mutate(c, "edit " + key + " of '" + e.GetName() + "'", [&] {
                        std::string error;
                        if (!SceneSerializer::SetComponentJson(e, key, p.at("value").dump(), &error)) Fail(InvalidParams, error);
                        return json::parse(SceneSerializer::ComponentToJson(e, key));
                    });
                } } },
                { "scene.remove_component", { Needs::Write, "{ id, component: key }", "remove a component", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string key = StringParam(c, p, "component");
                    if (!SceneSerializer::HasComponent(e, key)) Fail(NotFound, "'" + e.GetName() + "' has no " + key);
                    return Mutate(c, "remove " + key + " from '" + e.GetName() + "'", [&] { SceneSerializer::RemoveComponent(e, key); return Summary(e); });
                } } },
                { "scene.set_parent", { Needs::Write, "{ id, parent: id | null, keepWorld?: bool }", "re-parent an entity (cycles are refused)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    if (!p.contains("parent")) Fail(InvalidParams, "missing 'parent' (an id, or null for the root)");
                    Entity parent = p.at("parent").is_null() ? Entity() : EntityParam(c, p, "parent");
                    const bool keepWorld = p.value("keepWorld", true);
                    if (parent && (parent == e || parent.IsDescendantOf(e))) Fail(InvalidParams, "that would create a cycle");
                    return Mutate(c, "re-parent '" + e.GetName() + "'", [&] { e.SetParent(parent, keepWorld); return Summary(e); });
                } } },
                { "assets.assign", { Needs::Write, "{ entity: id, asset: id }", "assign a project asset to an entity (mesh, texture, audio, model)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p, "entity");
                    if (!p.contains("asset")) Fail(InvalidParams, "missing 'asset'");
                    const uint64_t asset = ParseHex(p.at("asset"), "asset");
                    AssetInfo a;
                    if (!Editor::ProjectAssets().Find(asset, a)) Fail(NotFound, "no asset " + p.at("asset").get<std::string>());
                    if (a.type == AssetType::Script) Fail(PermissionDenied, "scripts cannot be assigned through PAI (no code execution)");
                    if (Editor::CurrentMode() == Editor::Mode::Play) Fail(NotNow, "the simulation is playing: stop it first");
                    std::string status;
                    if (!Editor::AssignAsset(c.ctx, e, asset, &status)) Fail(InvalidParams, status);
                    ++c.stats.mutations;
                    return json{ { "status", status }, { "entity", Summary(e) } };
                } } },
                { "history.undo", { Needs::Write, "{}", "undo the last step", [](Context& c, const json&) {
                    if (!Editor::History().canUndo) Fail(NotNow, Editor::CurrentMode() == Editor::Mode::Play ? "not while playing" : "nothing to undo");
                    const std::string label = Editor::History().undoLabel;
                    Editor::Undo(c.ctx);
                    return json{ { "undone", label } };
                } } },
                { "history.redo", { Needs::Write, "{}", "redo the last undone step", [](Context& c, const json&) {
                    if (!Editor::History().canRedo) Fail(NotNow, "nothing to redo");
                    const std::string label = Editor::History().redoLabel;
                    Editor::Redo(c.ctx);
                    return json{ { "redone", label } };
                } } },
                { "scene.save", { Needs::Write, "{}", "save the open scene to its own file (no other path can be written)", [](Context& c, const json&) {
                    RequireScene(c);
                    const std::string path = Editor::ScenePath();
                    if (path.empty()) Fail(NotNow, "the scene has no file yet: save it once in the editor (File > Save as)");
                    std::string status;
                    if (!Editor::SaveScene(c.ctx, path, &status)) Fail(NotNow, status);
                    return json{ { "saved", path } };
                } } },
                // ------------------------------------------------------------------ CAD parts
                { "cad.get", { Needs::Read, "{ id | name, document?: bool }", "a CAD part: features and their status, parameters, volume, area, bounds, validity (and the document)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string report = Editor::CadPartReport(e);
                    if (report.empty()) Fail(NotFound, "'" + e.GetName() + "' is not a CAD part");
                    json out = json::parse(report);
                    out["id"] = Hex(e.GetGuid());
                    if (p.value("document", false)) out["document"] = json::parse(SceneSerializer::ComponentToJson(e, "cadModel")).value("document", json::object());
                    return out;
                } } },
                { "cad.create", { Needs::Write, "{ name, document?: { format: phisiks-cad, ... } }", "create a CAD part (one undo step); the document is validated", [](Context& c, const json& p) {
                    RequireScene(c);
                    if (Editor::CurrentMode() == Editor::Mode::Play) Fail(NotNow, "the simulation is playing: stop it first (sim.stop)");
                    const std::string name = StringParam(c, p, "name");
                    std::string doc;
                    if (p.contains("document")) { if (!p.at("document").is_object()) Fail(InvalidParams, "document must be an object"); doc = p.at("document").dump(); }
                    std::string status;
                    Entity e = Editor::CreateCadPart(c.ctx, name, doc, &status);
                    if (!e) Fail(InvalidParams, status);
                    ++c.stats.mutations;
                    json out = json::parse(Editor::CadPartReport(e));
                    out["id"] = Hex(e.GetGuid());
                    return out;
                } } },
                { "cad.set_document", { Needs::Write, "{ id | name, document: { format: phisiks-cad, ... } }", "replace a part's parametric document (validated; one undo step); returns the rebuilt part", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    if (!p.contains("document") || !p.at("document").is_object()) Fail(InvalidParams, "document must be an object");
                    if (Editor::CurrentMode() == Editor::Mode::Play) Fail(NotNow, "the simulation is playing: stop it first (sim.stop)");
                    std::string status;
                    if (!Editor::SetCadDocument(c.ctx, e, p.at("document").dump(), &status)) Fail(InvalidParams, status);
                    ++c.stats.mutations;
                    json out = json::parse(Editor::CadPartReport(e));
                    out["id"] = Hex(e.GetGuid());
                    return out;
                } } },
                { "cad.set_parameter", { Needs::Write, "{ id | name, parameter, expression }", "add or change a part parameter (\"40\", \"width / 2\"); the part rebuilds (one undo step)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string param = StringParam(c, p, "parameter"), expr = StringParam(c, p, "expression");
                    if (!e.Has<CadModel>()) Fail(NotFound, "'" + e.GetName() + "' is not a CAD part");
                    if (Editor::CurrentMode() == Editor::Mode::Play) Fail(NotNow, "the simulation is playing: stop it first (sim.stop)");
                    json doc = json::parse(SceneSerializer::ComponentToJson(e, "cadModel")).value("document", json::object());
                    if (doc.empty()) doc = { { "format", "phisiks-cad" }, { "version", 1 }, { "features", json::array() } };
                    json& params = doc["parameters"];
                    if (!params.is_array()) params = json::array();
                    bool found = false;
                    for (json& q : params) if (q.value("name", std::string()) == param) { q["expression"] = expr; found = true; }
                    if (!found) params.push_back({ { "name", param }, { "expression", expr } });
                    std::string status;
                    if (!Editor::SetCadDocument(c.ctx, e, doc.dump(), &status)) Fail(InvalidParams, status);
                    ++c.stats.mutations;
                    json out = json::parse(Editor::CadPartReport(e));
                    for (const json& q : out["parameters"]) if (q.value("name", std::string()) == param && !q.value("error", std::string()).empty()) {
                        Editor::Undo(c.ctx);   // an invalid expression is not kept
                        Fail(InvalidParams, "parameter '" + param + "': " + q.value("error", std::string()));
                    }
                    out["id"] = Hex(e.GetGuid());
                    return out;
                } } },
                { "cad.export", { Needs::Write, "{ id | name, format: step | iges | stl | brep | phcad }", "export a part into the project's assets/exports folder (no other path)", [](Context& c, const json& p) {
                    Entity e = EntityParam(c, p);
                    const std::string format = StringParam(c, p, "format");
                    std::string path, status;
                    if (!Editor::ExportCadPart(e, format, &path, &status)) Fail(InvalidParams, status);
                    return json{ { "path", path }, { "status", status } };
                } } },
                // ------------------------------------------------------------------ simulate
                { "sim.play", { Needs::Simulate, "{}", "enter play mode (the edit state is captured) and run", [](Context& c, const json&) {
                    std::string status;
                    if (!Editor::EnterPlay(c.ctx, &status)) Fail(NotNow, status);
                    g_stepFrames = 0;
                    return json{ { "mode", "play" }, { "status", status } };
                } } },
                { "sim.pause", { Needs::Simulate, "{}", "pause (play mode)", [](Context& c, const json&) {
                    if (Editor::CurrentMode() != Editor::Mode::Play) Fail(NotNow, "not playing");
                    c.ctx.state = GameState::Paused;
                    g_stepFrames = 0;
                    return json{ { "state", "Paused" } };
                } } },
                { "sim.resume", { Needs::Simulate, "{}", "resume (play mode)", [](Context& c, const json&) {
                    if (Editor::CurrentMode() != Editor::Mode::Play) Fail(NotNow, "not playing: use sim.play");
                    c.ctx.state = GameState::Running;
                    return json{ { "state", "Running" } };
                } } },
                { "sim.step", { Needs::Simulate, "{ frames?: n }", "run n frames, then pause (enters play mode if needed); poll sim.status", [](Context& c, const json& p) {
                    const int frames = p.value("frames", 1);
                    if (frames < 1 || frames > c.limits.maxStepFrames) Fail(LimitExceeded, "frames must be 1.." + std::to_string(c.limits.maxStepFrames));
                    if (Editor::CurrentMode() == Editor::Mode::Edit && !Editor::EnterPlay(c.ctx, nullptr, false)) Fail(NotNow, "cannot enter play mode");
                    g_stepFrames = frames;
                    c.ctx.state = GameState::Running;
                    return json{ { "stepping", frames } };
                } } },
                { "sim.stop", { Needs::Simulate, "{}", "stop: back to the edit state (everything changed while playing is discarded)", [](Context& c, const json&) {
                    std::string status;
                    g_stepFrames = 0;
                    if (!Editor::StopPlay(c.ctx, &status)) Fail(NotNow, status);
                    return json{ { "mode", "edit" }, { "status", status } };
                } } },
            };
            return methods;
        }

        json Hello(const Permissions& perm, const Limits& limits) {
            json methods = json::array();
            for (const auto& [name, m] : Methods()) {
                const bool granted = m.needs == Needs::Read ? perm.read : m.needs == Needs::Write ? perm.write : perm.simulate;
                if (granted) methods.push_back(name);
            }
            return { { "protocol", PROTOCOL }, { "protocolVersion", PROTOCOL_VERSION }, { "engine", "PHISIKS" }, { "engineVersion", PHISIKS_VERSION_STRING },
                     { "transport", "stdio, newline-delimited JSON-RPC 2.0" },
                     { "permissions", { { "read", perm.read }, { "write", perm.write }, { "simulate", perm.simulate } } },
                     { "limits", { { "maxRequestBytes", limits.maxRequestBytes }, { "maxQueued", limits.maxQueued }, { "maxPerFrame", limits.maxPerFrame },
                                   { "maxListed", limits.maxListed }, { "maxStepFrames", limits.maxStepFrames } } },
                     { "methods", methods } };
        }

        json ErrorObject(const json& id, int code, const std::string& message) {
            return { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
        }
    }

    // ==================================================================== Dispatcher
    Dispatcher::Dispatcher(Permissions permissions, Limits limits) : m_permissions(permissions), m_limits(limits) {}

    std::string Dispatcher::Manifest() const {
        json methods = json::object();
        for (const auto& [name, m] : Methods()) {
            const bool granted = m.needs == Needs::Read ? m_permissions.read : m.needs == Needs::Write ? m_permissions.write : m_permissions.simulate;
            methods[name] = { { "permission", NeedsName(m.needs) }, { "granted", granted }, { "params", m.params }, { "description", m.description } };
        }
        json j = Hello(m_permissions, m_limits);
        j["methods"] = methods;
        j["errors"] = { { "-32700", "parse error" }, { "-32600", "invalid request" }, { "-32601", "method not found" }, { "-32602", "invalid params" },
                        { "-32603", "internal error" }, { "-32001", "permission denied" }, { "-32002", "not possible now (mode / state)" },
                        { "-32003", "not found" }, { "-32004", "limit exceeded" } };
        return j.dump();
    }

    std::string Dispatcher::Handle(GameContext& ctx, const std::string& text) {
        ++m_stats.requests;
        if (text.size() > m_limits.maxRequestBytes) { ++m_stats.rejected; ++m_stats.errors; return ErrorObject(nullptr, InvalidRequest, "request larger than " + std::to_string(m_limits.maxRequestBytes) + " bytes").dump(); }
        json request;
        try { request = json::parse(text); }
        catch (const std::exception& e) { ++m_stats.errors; return ErrorObject(nullptr, ParseError, std::string("parse error: ") + e.what()).dump(); }
        auto one = [&](const json& r) -> json {
            json id = nullptr;
            const bool hasId = r.is_object() && r.contains("id");
            if (hasId) id = r.at("id");
            try {
                if (!r.is_object() || r.value("jsonrpc", std::string()) != "2.0" || !r.contains("method") || !r.at("method").is_string())
                    Fail(InvalidRequest, "a request is an object with \"jsonrpc\": \"2.0\", a string \"method\" and an optional \"params\" object");
                if (hasId && !id.is_string() && !id.is_number_integer() && !id.is_null()) Fail(InvalidRequest, "id must be a string, an integer or null");
                const std::string method = r.at("method").get<std::string>();
                const json params = r.contains("params") ? r.at("params") : json::object();
                if (!params.is_object()) Fail(InvalidParams, "params must be an object (named parameters)");
                const auto it = Methods().find(method);
                if (it == Methods().end()) Fail(MethodNotFound, "unknown method '" + method + "' (pai.describe lists them)");
                const Method& m = it->second;
                const bool granted = m.needs == Needs::Read ? m_permissions.read : m.needs == Needs::Write ? m_permissions.write : m_permissions.simulate;
                if (!granted) Fail(PermissionDenied, "'" + method + "' needs the " + NeedsName(m.needs) + " permission (start the editor with --pai-" + NeedsName(m.needs) + ")");
                json result;
                if (method == "pai.describe") result = json::parse(Manifest());
                else if (method == "initialize") result = Hello(m_permissions, m_limits);
                else {
                    Context c{ ctx, m_limits, m_stats };
                    result = m.handler(c, params);
                }
                if (!hasId) return json();   // notification: no response
                return { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
            }
            catch (const RpcError& e) { ++m_stats.errors; return hasId || e.code == InvalidRequest ? ErrorObject(id, e.code, e.what()) : json(); }
            catch (const json::exception& e) { ++m_stats.errors; return hasId ? ErrorObject(id, InvalidParams, std::string("invalid params: ") + e.what()) : json(); }
            catch (const std::exception& e) { ++m_stats.errors; return hasId ? ErrorObject(id, InternalError, e.what()) : json(); }
        };
        if (request.is_array()) {
            if (request.empty()) { ++m_stats.errors; return ErrorObject(nullptr, InvalidRequest, "empty batch").dump(); }
            if (request.size() > 16) { ++m_stats.errors; ++m_stats.rejected; return ErrorObject(nullptr, LimitExceeded, "a batch holds at most 16 requests").dump(); }
            json responses = json::array();
            for (const json& r : request) { json out = one(r); if (!out.is_null()) responses.push_back(out); }
            return responses.empty() ? std::string() : responses.dump();
        }
        const json out = one(request);
        return out.is_null() ? std::string() : out.dump();
    }

    // Frames left for sim.step (the transport calls this once per frame).
    void FrameStep(GameContext& ctx) {
        if (g_stepFrames <= 0) return;
        if (--g_stepFrames == 0) ctx.state = GameState::Paused;
    }
}
