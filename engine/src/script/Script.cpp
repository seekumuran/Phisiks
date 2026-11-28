// Embedded Lua 5.4 scripting (see Phisiks/Script/Script.h). One VM per scene instance; each
// Script component's chunk runs with its own _ENV table that falls back to the shared globals.
#include "Phisiks/Script/Script.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"

#include <lua.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace Phisiks::Scripting {
    namespace {
        constexpr const char* ENTITY_META = "Phisiks.Entity";
        constexpr double CALL_BUDGET_SECONDS = 0.2;

        struct Vm {
            lua_State* L = nullptr;
            uint64_t sceneSerial = 0;
            Scene* scene = nullptr;
            GameContext* ctx = nullptr;
            double callStart = 0.0;
            double nextFileCheck = 0.0;
        };
        Vm g;
        Stats g_stats;

        struct LuaEntity { uint32_t id; };

        // ---------------------------------------------------------------- helpers
        void PushEntity(lua_State* L, Entity e) {
            if (!e.Valid()) { lua_pushnil(L); return; }
            auto* u = static_cast<LuaEntity*>(lua_newuserdatauv(L, sizeof(LuaEntity), 0));
            u->id = e.Id();
            luaL_setmetatable(L, ENTITY_META);
        }
        Entity ToEntity(lua_State* L, int index) {
            auto* u = static_cast<LuaEntity*>(luaL_checkudata(L, index, ENTITY_META));
            return g.scene ? g.scene->FromId(u->id) : Entity();
        }
        Entity CheckEntity(lua_State* L, int index) {
            Entity e = ToEntity(L, index);
            if (!e.Valid()) luaL_error(L, "entity is no longer valid");
            return e;
        }
        glm::vec3 CheckVec3(lua_State* L, int first) {
            return glm::vec3(float(luaL_checknumber(L, first)), float(luaL_checknumber(L, first + 1)), float(luaL_checknumber(L, first + 2)));
        }
        int PushVec3(lua_State* L, const glm::vec3& v) {
            lua_pushnumber(L, v.x); lua_pushnumber(L, v.y); lua_pushnumber(L, v.z);
            return 3;
        }

        void Hook(lua_State* L, lua_Debug*) {
            if (Time::NowSeconds() - g.callStart > CALL_BUDGET_SECONDS) luaL_error(L, "script timeout (%d ms budget exceeded)", int(CALL_BUDGET_SECONDS * 1000.0));
        }

        int Traceback(lua_State* L) {
            const char* message = lua_tostring(L, 1);
            luaL_traceback(L, L, message ? message : "(error object is not a string)", 1);
            return 1;
        }

        // pcall with traceback; returns false and sets `error` on failure.
        bool ProtectedCall(lua_State* L, int args, int results, std::string& error) {
            const int base = lua_gettop(L) - args;
            lua_pushcfunction(L, Traceback);
            lua_insert(L, base);
            g.callStart = Time::NowSeconds();
            const int status = lua_pcall(L, args, results, base);
            lua_remove(L, base);
            if (status != LUA_OK) {
                error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown Lua error";
                lua_pop(L, 1);
                return false;
            }
            return true;
        }

        // ---------------------------------------------------------------- entity methods
        int EName(lua_State* L) { lua_pushstring(L, CheckEntity(L, 1).GetName().c_str()); return 1; }
        int EValid(lua_State* L) { lua_pushboolean(L, ToEntity(L, 1).Valid()); return 1; }
        int EPosition(lua_State* L) { return PushVec3(L, CheckEntity(L, 1).WorldPosition()); }
        int ESetPosition(lua_State* L) { Entity e = CheckEntity(L, 1); e.SetWorldPose(CheckVec3(L, 2), e.WorldRotation()); return 0; }
        int ERotation(lua_State* L) {
            const glm::quat q = CheckEntity(L, 1).WorldRotation();
            lua_pushnumber(L, q.x); lua_pushnumber(L, q.y); lua_pushnumber(L, q.z); lua_pushnumber(L, q.w);
            return 4;
        }
        int EEuler(lua_State* L) { return PushVec3(L, glm::degrees(glm::eulerAngles(CheckEntity(L, 1).WorldRotation()))); }
        int ESetEuler(lua_State* L) { Entity e = CheckEntity(L, 1); e.SetWorldPose(e.WorldPosition(), glm::quat(glm::radians(CheckVec3(L, 2)))); return 0; }
        int ERotate(lua_State* L) {
            Entity e = CheckEntity(L, 1);
            const glm::vec3 axis = CheckVec3(L, 2);
            const float degrees = float(luaL_checknumber(L, 5));
            if (glm::length(axis) < 1e-6f) return 0;
            e.SetWorldPose(e.WorldPosition(), glm::normalize(glm::angleAxis(glm::radians(degrees), glm::normalize(axis)) * e.WorldRotation()));
            return 0;
        }
        int EScale(lua_State* L) { return PushVec3(L, CheckEntity(L, 1).LocalTransform().scale); }
        int ESetScale(lua_State* L) { Entity e = CheckEntity(L, 1); e.LocalTransform().scale = CheckVec3(L, 2); if (e.Has<Collider>()) e.MarkPhysicsDirty(); return 0; }
        int EVelocity(lua_State* L) { return PushVec3(L, CheckEntity(L, 1).GetLinearVelocity()); }
        int ESetVelocity(lua_State* L) { const Physics::BodyId b = CheckEntity(L, 1).GetBody(); if (b.Valid()) Physics::SetLinearVelocity(b, CheckVec3(L, 2)); return 0; }
        int EImpulse(lua_State* L) { CheckEntity(L, 1).AddImpulse(CheckVec3(L, 2)); return 0; }
        int EForce(lua_State* L) { const Physics::BodyId b = CheckEntity(L, 1).GetBody(); if (b.Valid()) Physics::AddForce(b, CheckVec3(L, 2)); return 0; }
        int ETorque(lua_State* L) { const Physics::BodyId b = CheckEntity(L, 1).GetBody(); if (b.Valid()) Physics::AddTorque(b, CheckVec3(L, 2)); return 0; }
        int EMass(lua_State* L) { const Physics::BodyId b = CheckEntity(L, 1).GetBody(); lua_pushnumber(L, b.Valid() ? Physics::GetMass(b) : 0.0); return 1; }
        int EDestroy(lua_State* L) { CheckEntity(L, 1).Destroy(); return 0; }
        int EParent(lua_State* L) { PushEntity(L, CheckEntity(L, 1).GetParent()); return 1; }
        int ELightIntensity(lua_State* L) { Light* l = CheckEntity(L, 1).TryGet<Light>(); lua_pushnumber(L, l ? l->intensity : 0.0); return 1; }
        int ESetLightIntensity(lua_State* L) { if (Light* l = CheckEntity(L, 1).TryGet<Light>()) l->intensity = float(luaL_checknumber(L, 2)); return 0; }
        int ESetVisible(lua_State* L) { if (MeshRenderer* m = CheckEntity(L, 1).TryGet<MeshRenderer>()) m->visible = lua_toboolean(L, 2) != 0; return 0; }
        int ESetColor(lua_State* L) {
            Entity e = CheckEntity(L, 1);
            MeshRenderer* m = e.TryGet<MeshRenderer>();
            if (!m) return 0;
            const std::string own = "script:" + std::to_string(e.Id());
            if (m->material != own) {
                const Render::MaterialDesc* base = Render::FindMaterial(m->material.empty() ? "Default" : m->material);
                Render::RegisterMaterial(own, base ? *base : Render::MaterialDesc{});
                m->material = own;
            }
            if (const Render::MaterialDesc* d = Render::FindMaterial(own)) {
                Render::MaterialDesc copy = *d;
                copy.baseColor = CheckVec3(L, 2);
                Render::RegisterMaterial(own, copy);
            }
            return 0;
        }
        int EPlaySound(lua_State* L) {
            Entity e = CheckEntity(L, 1);
            lua_pushinteger(L, lua_Integer(Audio::PlaySpatial(luaL_checkstring(L, 2), e.WorldPosition(), float(luaL_optnumber(L, 3, 1.0)))));
            return 1;
        }
        int EJointMotor(lua_State* L) {
            Joint* j = CheckEntity(L, 1).TryGet<Joint>();
            if (j) Physics::SetJointMotor(j->runtime, true, float(luaL_checknumber(L, 2)), float(luaL_optnumber(L, 3, j->maxMotorForce)));
            return 0;
        }
        int EJointState(lua_State* L) {
            Joint* j = CheckEntity(L, 1).TryGet<Joint>();
            const Physics::JointState s = j ? Physics::GetJointState(j->runtime) : Physics::JointState{};
            lua_pushnumber(L, s.position);
            lua_pushnumber(L, s.speed);
            return 2;
        }
        int EEq(lua_State* L) {
            auto* a = static_cast<LuaEntity*>(luaL_checkudata(L, 1, ENTITY_META));
            auto* b = static_cast<LuaEntity*>(luaL_checkudata(L, 2, ENTITY_META));
            lua_pushboolean(L, a->id == b->id);
            return 1;
        }
        int EToString(lua_State* L) {
            Entity e = ToEntity(L, 1);
            lua_pushfstring(L, "entity(%s)", e.Valid() ? e.GetName().c_str() : "destroyed");
            return 1;
        }

        // ---------------------------------------------------------------- scene / input / log
        int SFind(lua_State* L) { PushEntity(L, g.scene ? g.scene->FindByName(luaL_checkstring(L, 1)) : Entity()); return 1; }
        int SSpawn(lua_State* L) {
            if (!g.scene) return 0;
            Entity e = g.scene->CreateEntity(luaL_checkstring(L, 1));
            e.Add<MeshRenderer>().mesh = luaL_optstring(L, 2, "Cube");
            e.LocalTransform().position = glm::vec3(float(luaL_optnumber(L, 3, 0.0)), float(luaL_optnumber(L, 4, 0.0)), float(luaL_optnumber(L, 5, 0.0)));
            const std::string shape = luaL_optstring(L, 6, "");
            if (shape == "box" || shape == "sphere") {
                Collider& c = e.Add<Collider>();
                c.shape.shape = shape == "box" ? Physics::ShapeType::Box : Physics::ShapeType::Sphere;
                c.shape.halfExtents = glm::vec3(0.5f);
                c.shape.radius = 0.5f;
                e.Add<Rigidbody>().mass = float(luaL_optnumber(L, 7, 1.0));
            }
            PushEntity(L, e);
            return 1;
        }
        int STime(lua_State* L) { lua_pushnumber(L, g.ctx ? g.ctx->stats.simulationTime : 0.0); return 1; }
        int SFrame(lua_State* L) { lua_pushinteger(L, g.ctx ? lua_Integer(g.ctx->stats.frameIndex) : 0); return 1; }
        int SRaycast(lua_State* L) {
            const glm::vec3 o = CheckVec3(L, 1), d = CheckVec3(L, 4);
            const float maxDistance = float(luaL_optnumber(L, 7, 100.0));
            if (glm::length(d) < 1e-6f || !g.scene) { lua_pushnil(L); return 1; }
            const auto hit = Physics::Raycast(o, glm::normalize(d), maxDistance);
            if (!hit) { lua_pushnil(L); return 1; }
            PushEntity(L, g.scene->FromBody(hit->body));
            if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
            PushVec3(L, hit->point);
            lua_pushnumber(L, hit->distance);
            return 5;
        }
        int SGravity(lua_State* L) {
            if (lua_gettop(L) >= 3) Physics::SetGravity(CheckVec3(L, 1));
            return PushVec3(L, Physics::GetSettings().gravity);
        }
        int SEvent(lua_State* L) { if (g.ctx) g.ctx->PushEvent(luaL_checkstring(L, 1)); return 0; }

        bool ActionOf(const std::string& name, Action& out) {
            static const std::pair<const char*, Action> table[] = {
                { "forward", Action::MoveForward }, { "back", Action::MoveBack }, { "left", Action::MoveLeft }, { "right", Action::MoveRight },
                { "jump", Action::Jump }, { "sprint", Action::Sprint }, { "crouch", Action::Crouch }, { "interact", Action::Interact },
                { "primary", Action::Primary }, { "secondary", Action::Secondary },
            };
            for (const auto& [n, a] : table) if (name == n) { out = a; return true; }
            return false;
        }
        int IDown(lua_State* L) { Action a; lua_pushboolean(L, g.ctx && ActionOf(luaL_checkstring(L, 1), a) && g.ctx->input.Down(a)); return 1; }
        int IPressed(lua_State* L) { Action a; lua_pushboolean(L, g.ctx && ActionOf(luaL_checkstring(L, 1), a) && g.ctx->input.Pressed(a)); return 1; }

        int LuaLog(lua_State* L) {
            std::string line;
            const int n = lua_gettop(L);
            for (int i = 1; i <= n; ++i) {
                size_t length = 0;
                const char* s = luaL_tolstring(L, i, &length);
                if (i > 1) line += ' ';
                line.append(s, length);
                lua_pop(L, 1);
            }
            Log::Info() << "[lua] " << line;
            return 0;
        }

        void Register(lua_State* L, const char* table, const luaL_Reg* functions) {
            lua_newtable(L);
            luaL_setfuncs(L, functions, 0);
            lua_setglobal(L, table);
        }

        bool CreateVm(Scene& scene) {
            if (g.L) lua_close(g.L);
            g.L = luaL_newstate();
            if (!g.L) return false;
            lua_State* L = g.L;
            // sandboxed standard libraries
            const luaL_Reg libs[] = { { LUA_GNAME, luaopen_base }, { LUA_TABLIBNAME, luaopen_table }, { LUA_STRLIBNAME, luaopen_string },
                                      { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine } };
            for (const luaL_Reg& lib : libs) { luaL_requiref(L, lib.name, lib.func, 1); lua_pop(L, 1); }
            for (const char* unsafe : { "dofile", "loadfile" }) { lua_pushnil(L); lua_setglobal(L, unsafe); }
            lua_pushcfunction(L, LuaLog); lua_setglobal(L, "log");
            lua_pushcfunction(L, LuaLog); lua_setglobal(L, "print");
            // entity metatable
            const luaL_Reg methods[] = {
                { "name", EName }, { "valid", EValid }, { "position", EPosition }, { "set_position", ESetPosition }, { "rotation", ERotation },
                { "euler", EEuler }, { "set_euler", ESetEuler }, { "rotate", ERotate }, { "scale", EScale }, { "set_scale", ESetScale },
                { "velocity", EVelocity }, { "set_velocity", ESetVelocity }, { "impulse", EImpulse }, { "force", EForce }, { "torque", ETorque },
                { "mass", EMass }, { "destroy", EDestroy }, { "parent", EParent }, { "light_intensity", ELightIntensity },
                { "set_light_intensity", ESetLightIntensity }, { "set_color", ESetColor }, { "set_visible", ESetVisible }, { "play_sound", EPlaySound },
                { "joint_motor", EJointMotor }, { "joint_state", EJointState },
                { nullptr, nullptr } };
            luaL_newmetatable(L, ENTITY_META);
            lua_newtable(L);
            luaL_setfuncs(L, methods, 0);
            lua_setfield(L, -2, "__index");
            lua_pushcfunction(L, EEq); lua_setfield(L, -2, "__eq");
            lua_pushcfunction(L, EToString); lua_setfield(L, -2, "__tostring");
            lua_pop(L, 1);
            const luaL_Reg sceneFns[] = { { "find", SFind }, { "spawn", SSpawn }, { "time", STime }, { "frame", SFrame }, { "raycast", SRaycast },
                                          { "gravity", SGravity }, { "event", SEvent }, { nullptr, nullptr } };
            Register(L, "scene", sceneFns);
            const luaL_Reg inputFns[] = { { "down", IDown }, { "pressed", IPressed }, { nullptr, nullptr } };
            Register(L, "input", inputFns);
            lua_sethook(L, Hook, LUA_MASKCOUNT, 20000);
            g.sceneSerial = scene.Serial();
            Log::Info() << "Script: Lua " << LUA_VERSION_MAJOR << "." << LUA_VERSION_MINOR << "." << LUA_VERSION_RELEASE << " VM for scene '" << scene.Name() << "'";
            return true;
        }

        bool EnsureVm(Scene& scene, GameContext& ctx) {
            g.scene = &scene;
            g.ctx = &ctx;
            if (g.L && g.sceneSerial == scene.Serial()) return true;
            return CreateVm(scene);
        }

        uint64_t FileStamp(const std::string& path) {
            std::error_code ec;
            const std::string resolved = Assets::Resolve(path);
            if (resolved.empty()) return 0;
            const auto time = std::filesystem::last_write_time(resolved, ec);
            return ec ? 0 : uint64_t(time.time_since_epoch().count());
        }

        bool SourceOf(Script& s, std::string& source, std::string& chunk) {
            if (!s.file.empty()) {
                std::vector<uint8_t> bytes;
                if (!Assets::ReadBytes(s.file, bytes)) { s.error = "script file not found: " + s.file; return false; }
                source.assign(bytes.begin(), bytes.end());
                chunk = "@" + s.file;
                s.sourceStamp = FileStamp(s.file);
                return true;
            }
            source = s.source;
            chunk = "=script";
            return true;
        }

        void ReportError(Script& s, Entity e, const std::string& error) {
            s.error = error;
            g_stats.lastError = error;
            Log::Error() << "Script on '" << e.GetName() << "': " << error;
        }

        // Runs the script's chunk in its environment (created on first load; reused on reload).
        bool Load(Script& s, Entity e) {
            lua_State* L = g.L;
            std::string source, chunk;
            if (!SourceOf(s, source, chunk)) { ReportError(s, e, s.error); return false; }
            if (s.reference == 0) {
                lua_newtable(L);                       // env
                PushEntity(L, e);
                lua_setfield(L, -2, "entity");
                lua_newtable(L);                       // metatable: fall back to globals
                lua_pushglobaltable(L);
                lua_setfield(L, -2, "__index");
                lua_setmetatable(L, -2);
                s.reference = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
                const std::string error = lua_tostring(L, -1);
                lua_pop(L, 1);
                ReportError(s, e, error);
                return false;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, s.reference);
            lua_setupvalue(L, -2, 1);                  // _ENV of the chunk
            std::string error;
            if (!ProtectedCall(L, 0, 0, error)) { ReportError(s, e, error); return false; }
            s.error.clear();
            return true;
        }

        // Calls env[name](env, ...) if it exists. `push` pushes the extra arguments and returns their count.
        template<class Push>
        void Call(Script& s, Entity e, const char* name, Push&& push) {
            if (!s.enabled || !s.error.empty() || s.reference == 0) return;
            lua_State* L = g.L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, s.reference);
            lua_getfield(L, -1, name);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
            lua_insert(L, -2);                         // function, env
            const int args = 1 + push(L);
            const double start = Time::NowSeconds();
            std::string error;
            if (!ProtectedCall(L, args, 0, error)) ReportError(s, e, error);
            g_stats.callbacks++;
            g_stats.ms += float((Time::NowSeconds() - start) * 1000.0);
        }
    }

    void Update(Scene& scene, GameContext& ctx, float dt) {
        g_stats.ms = 0.0f;
        if (scene.Registry().view<Script>().size() == 0) { g_stats.scripts = g_stats.running = 0; return; }
        if (!EnsureVm(scene, ctx)) return;
        const bool checkFiles = Time::NowSeconds() >= g.nextFileCheck;
        if (checkFiles) g.nextFileCheck = Time::NowSeconds() + 0.5;
        std::vector<entt::entity> handles;
        for (auto [handle, s] : scene.Registry().view<Script>().each()) handles.push_back(handle);
        uint32_t running = 0, errors = 0;
        for (entt::entity handle : handles) {
            if (!scene.Registry().valid(handle)) continue;
            Script& s = scene.Registry().get<Script>(handle);
            Entity e(&scene, handle);
            if (!s.started) {
                s.started = true;
                if (Load(s, e)) Call(s, e, "on_start", [](lua_State*) { return 0; });
            }
            else if (checkFiles && !s.file.empty() && s.sourceStamp != 0 && FileStamp(s.file) != s.sourceStamp) {
                Log::Info() << "Script: '" << s.file << "' changed, reloading";
                if (Load(s, e)) ++g_stats.reloads;
            }
            if (!scene.Registry().valid(handle)) continue;
            Script& current = scene.Registry().get<Script>(handle);
            Call(current, e, "on_update", [dt](lua_State* L) { lua_pushnumber(L, dt); return 1; });
            if (scene.Registry().valid(handle)) {
                const Script& after = scene.Registry().get<Script>(handle);
                if (after.error.empty() && after.enabled) ++running;
                if (!after.error.empty()) ++errors;
            }
        }
        g_stats.scripts = uint32_t(handles.size());
        g_stats.running = running;
        g_stats.errors = errors;
    }

    void FixedUpdate(Scene& scene, GameContext& ctx, float dt) {
        if (!g.L || g.sceneSerial != scene.Serial()) return;
        g.scene = &scene;
        g.ctx = &ctx;
        std::vector<entt::entity> handles;
        for (auto [handle, s] : scene.Registry().view<Script>().each()) if (s.started) handles.push_back(handle);
        for (entt::entity handle : handles) {
            if (!scene.Registry().valid(handle)) continue;
            Call(scene.Registry().get<Script>(handle), Entity(&scene, handle), "on_fixed_update", [dt](lua_State* L) { lua_pushnumber(L, dt); return 1; });
        }
    }

    void DispatchPhysicsEvents(Scene& scene, GameContext& ctx) {
        if (!g.L || g.sceneSerial != scene.Serial()) return;
        g.scene = &scene;
        g.ctx = &ctx;
        auto deliver = [&](Entity self, Entity other, const char* name, auto&& extra) {
            if (!self.Valid()) return;
            Script* s = self.TryGet<Script>();
            if (!s || !s->started) return;
            Call(*s, self, name, [&](lua_State* L) { PushEntity(L, other); return 1 + extra(L); });
        };
        for (const Physics::CollisionEvent& e : Physics::GetCollisionEvents()) {
            if (e.kind != Physics::CollisionEvent::Kind::Enter) continue;
            const Entity a = scene.FromBody(e.a), b = scene.FromBody(e.b);
            const float speed = e.impactSpeed;
            auto pushSpeed = [speed](lua_State* L) { lua_pushnumber(L, speed); return 1; };
            deliver(a, b, "on_collision", pushSpeed);
            deliver(b, a, "on_collision", pushSpeed);
        }
        for (const Physics::TriggerEvent& e : Physics::GetTriggerEvents()) {
            const Entity trigger = scene.FromBody(e.trigger), other = e.otherIsCharacter ? Entity() : scene.FromBody(e.other);
            const bool entered = e.kind == Physics::TriggerEvent::Kind::Enter;
            auto pushEntered = [entered](lua_State* L) { lua_pushboolean(L, entered); return 1; };
            deliver(trigger, other, "on_trigger", pushEntered);
            deliver(other, trigger, "on_trigger", pushEntered);
        }
    }

    bool Run(Scene& scene, GameContext& ctx, const std::string& code, std::string* error) {
        if (!EnsureVm(scene, ctx)) { if (error) *error = "no Lua VM"; return false; }
        lua_State* L = g.L;
        if (luaL_loadbufferx(L, code.data(), code.size(), "=run", "t") != LUA_OK) {
            if (error) *error = lua_tostring(L, -1);
            lua_pop(L, 1);
            return false;
        }
        std::string message;
        if (!ProtectedCall(L, 0, 0, message)) { if (error) *error = message; return false; }
        return true;
    }

    void Reload(Scene& scene) {
        if (!g.L || g.sceneSerial != scene.Serial()) return;
        for (auto [handle, s] : scene.Registry().view<Script>().each()) {
            if (!s.started) continue;
            if (Load(s, Entity(&scene, handle))) ++g_stats.reloads;
        }
    }

    void Shutdown() {
        if (g.L) lua_close(g.L);
        g = Vm{};
    }

    Stats GetStats() { return g_stats; }
}
