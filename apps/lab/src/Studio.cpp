#include "Studio.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace Phisiks;

namespace Lab::Studio {
    namespace {
        uint32_t g_selected = 0;   // id + 1
        ImGuizmo::OPERATION g_operation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE g_mode = ImGuizmo::WORLD;
        std::string g_path = "Build/scenes/scene.json";
        std::string g_status;
        std::array<float, 240> g_frameMs{}, g_gpuMs{};
        size_t g_historyCursor = 0;
        float g_gravity = -9.81f, g_friction = 0.6f;
        int g_substeps = 4;
        int g_quality = 2;

        struct Lesson { const char* title; const char* scene; const char* about; };
        const Lesson LESSONS[] = {
            { "Transforms & Rotors", "Rotors", "Euler angles, quaternions and geometric-algebra rotors give the same rotation." },
            { "RK4 vs Euler Orbits", "Orbits", "Integrator accuracy: explicit Euler gains energy, RK4 keeps the orbit closed." },
            { "Friction & Raycasts", "Sandbox", "Coulomb friction, raycast picking and the character controller (F2: physics debug)." },
            { "Rigid-body Stacking", "PhysicsTower", "A 16-box tower: solver sub-steps and sleeping." },
            { "PBR Lighting", "PBR", "Roughness x metallic grid: GGX specular, hemisphere ambient, rim light." },
            { "GPU Culling & Particles", "FieldLab", "Frustum + Hi-Z culling, spatial-hash particles in a vortex, N-body moons." },
            { "XPBD Soft Bodies & SDFs", "FieldLab", "Cloth, rope and a soft box (XPBD), the raymarched SDF/CSG visualiser." },
            { "Emitters & Coupling", "Fountain", "A particle emitter whose jet pushes dynamic crates (two-way coupling)." },
        };

        Entity Selected(Scene& scene) { return g_selected ? scene.FromId(g_selected - 1) : Entity(); }

        bool DragVec3(const char* label, glm::vec3& v, float speed = 0.05f) { return ImGui::DragFloat3(label, &v.x, speed); }

        void Hierarchy(Scene& scene, Entity e) {
            const std::vector<Entity> children = e.GetChildren();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            if (g_selected == e.Id() + 1) flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(uintptr_t(e.Id() + 1)), flags, "%s", e.GetName().c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_selected = e.Id() + 1;
            if (open) {
                for (Entity c : children) Hierarchy(scene, c);
                ImGui::TreePop();
            }
        }

        void MaterialEditor(const std::string& name) {
            const Render::MaterialDesc* found = Render::FindMaterial(name.empty() ? "Default" : name);
            if (!found) return;
            Render::MaterialDesc m = *found;
            bool changed = false;
            changed |= ImGui::ColorEdit3("base colour", &m.baseColor.x);
            changed |= ImGui::SliderFloat("roughness", &m.roughness, 0.02f, 1.0f);
            changed |= ImGui::SliderFloat("metallic", &m.metallic, 0.0f, 1.0f);
            changed |= ImGui::ColorEdit3("emissive", &m.emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            int pattern = int(m.pattern);
            if (ImGui::Combo("pattern", &pattern, "None\0Checker\0Grid\0Noise\0Wood\0Tiles\0CalibrationGrid\0")) { m.pattern = Render::Pattern(pattern); changed = true; }
            if (m.pattern != Render::Pattern::None) {
                changed |= ImGui::ColorEdit3("pattern colour", &m.patternColor.x);
                changed |= ImGui::SliderFloat("pattern scale", &m.patternScale, 0.1f, 20.0f);
            }
            if (changed) Render::RegisterMaterial(name.empty() ? "Default" : name, m);
        }

        void Inspector(GameContext& ctx, Scene& scene) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 370.0f, 30.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(360.0f, 560.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Inspector");
            Entity e = Selected(scene);
            if (!e) { ImGui::TextDisabled("Select an entity (hierarchy or click in the viewport)."); ImGui::End(); return; }
            char name[128];
            std::snprintf(name, sizeof(name), "%s", e.GetName().c_str());
            if (ImGui::InputText("name", name, sizeof(name))) e.SetName(name);
            ImGui::Text("id %u", e.Id());
            ImGui::SameLine();
            if (ImGui::SmallButton("Destroy")) { e.Destroy(); g_selected = 0; ImGui::End(); return; }

            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                Transform& t = e.LocalTransform();
                bool moved = DragVec3("position", t.position);
                glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
                if (DragVec3("rotation", euler, 0.5f)) { t.rotation = glm::quat(glm::radians(euler)); moved = true; }
                const bool scaled = DragVec3("scale", t.scale, 0.01f);
                if (moved && e.GetBody().Valid()) e.SetWorldPose(e.WorldPosition(), e.WorldRotation());
                if (scaled) e.MarkPhysicsDirty();
                ImGui::RadioButton("translate", reinterpret_cast<int*>(&g_operation), int(ImGuizmo::TRANSLATE)); ImGui::SameLine();
                ImGui::RadioButton("rotate", reinterpret_cast<int*>(&g_operation), int(ImGuizmo::ROTATE)); ImGui::SameLine();
                ImGui::RadioButton("scale", reinterpret_cast<int*>(&g_operation), int(ImGuizmo::SCALE));
                ImGui::RadioButton("world", reinterpret_cast<int*>(&g_mode), int(ImGuizmo::WORLD)); ImGui::SameLine();
                ImGui::RadioButton("local", reinterpret_cast<int*>(&g_mode), int(ImGuizmo::LOCAL));
            }
            if (MeshRenderer* m = e.TryGet<MeshRenderer>(); m && ImGui::CollapsingHeader("Mesh renderer / material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("mesh %s, material %s", m->mesh.c_str(), m->material.empty() ? "Default" : m->material.c_str());
                ImGui::Checkbox("visible", &m->visible); ImGui::SameLine();
                ImGui::Checkbox("casts shadows", &m->castShadows);
                ImGui::DragFloat2("uv scale", &m->uvScale.x, 0.05f, 0.01f, 100.0f);
                MaterialEditor(m->material);
            }
            if (Light* l = e.TryGet<Light>(); l && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                int type = int(l->type);
                if (ImGui::Combo("type", &type, "Directional\0Point\0Spot\0")) l->type = LightType(type);
                ImGui::ColorEdit3("colour", &l->color.x);
                ImGui::DragFloat("intensity", &l->intensity, 0.1f, 0.0f, 1000.0f);
                if (l->type != LightType::Directional) ImGui::DragFloat("range", &l->range, 0.1f, 0.1f, 200.0f);
                if (l->type == LightType::Spot) {
                    ImGui::SliderAngle("inner cone", &l->innerConeAngle, 1.0f, 89.0f);
                    ImGui::SliderAngle("outer cone", &l->outerConeAngle, 1.0f, 89.0f);
                }
                ImGui::Checkbox("shadows", &l->castShadows);
            }
            if (Collider* c = e.TryGet<Collider>(); c && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool dirty = false;
                int shape = int(c->shape.shape);
                if (ImGui::Combo("shape", &shape, "Box\0Sphere\0Capsule\0ConvexMesh\0TriangleMesh\0")) { c->shape.shape = Physics::ShapeType(shape); dirty = true; }
                if (c->shape.shape == Physics::ShapeType::Box) dirty |= DragVec3("half extents", c->shape.halfExtents, 0.01f);
                if (c->shape.shape == Physics::ShapeType::Sphere || c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("radius", &c->shape.radius, 0.01f, 0.01f, 50.0f);
                if (c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("half height", &c->shape.halfHeight, 0.01f, 0.01f, 50.0f);
                dirty |= ImGui::SliderFloat("static friction", &c->shape.material.staticFriction, 0.0f, 1.5f);
                dirty |= ImGui::SliderFloat("dynamic friction", &c->shape.material.dynamicFriction, 0.0f, 1.5f);
                dirty |= ImGui::SliderFloat("restitution", &c->shape.material.restitution, 0.0f, 1.0f);
                dirty |= ImGui::Checkbox("trigger", &c->shape.isTrigger);
                if (dirty) e.MarkPhysicsDirty();
            }
            if (Rigidbody* r = e.TryGet<Rigidbody>(); r && ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool dirty = false;
                int type = int(r->type);
                if (ImGui::Combo("body type", &type, "Static\0Dynamic\0Kinematic\0")) { r->type = Physics::BodyType(type); dirty = true; }
                dirty |= ImGui::DragFloat("mass (kg)", &r->mass, 0.05f, 0.01f, 10000.0f);
                dirty |= ImGui::SliderFloat("linear damping", &r->linearDamping, 0.0f, 5.0f);
                dirty |= ImGui::SliderFloat("angular damping", &r->angularDamping, 0.0f, 5.0f);
                dirty |= ImGui::Checkbox("gravity", &r->useGravity);
                dirty |= ImGui::Checkbox("continuous collision", &r->continuousCollision);
                if (dirty) e.MarkPhysicsDirty();
                const Physics::BodyId body = e.GetBody();
                if (body.Valid()) {
                    const glm::vec3 v = Physics::GetLinearVelocity(body);
                    ImGui::Text("v = (%.2f, %.2f, %.2f) m/s%s", v.x, v.y, v.z, Physics::IsSleeping(body) ? "  [sleeping]" : "");
                    if (ImGui::Button("kick up")) Physics::AddImpulse(body, glm::vec3(0.0f, 4.0f * Physics::GetMass(body), 0.0f));
                }
            }
            if (Physics::ForceField* f = e.TryGet<Physics::ForceField>(); f && ImGui::CollapsingHeader("Force field", ImGuiTreeNodeFlags_DefaultOpen)) {
                int type = int(f->type);
                if (ImGui::Combo("field", &type, "Uniform\0SphericalGravity\0Vortex\0NBody\0")) f->type = Physics::FieldType(type);
                if (f->type == Physics::FieldType::Uniform) DragVec3("vector", f->vector, 0.05f);
                else ImGui::DragFloat("strength", &f->strength, 0.05f);
                if (f->type == Physics::FieldType::SphericalGravity || f->type == Physics::FieldType::Vortex) ImGui::DragFloat("radius", &f->radius, 0.02f, 0.01f, 100.0f);
                if (f->type == Physics::FieldType::Vortex) { ImGui::DragFloat("falloff", &f->falloff, 0.02f, 0.0f, 4.0f); DragVec3("axis", f->axis, 0.02f); }
                ImGui::DragFloat("influence radius", &f->influenceRadius, 0.05f, 0.0f, 200.0f);
                ImGui::Checkbox("rigid", &f->affectsRigidBodies); ImGui::SameLine();
                ImGui::Checkbox("particles", &f->affectsParticles); ImGui::SameLine();
                ImGui::Checkbox("soft", &f->affectsSoftBodies);
            }
            if (Physics::Advanced::ParticleSystem* p = e.TryGet<Physics::Advanced::ParticleSystem>(); p && ImGui::CollapsingHeader("Particle system", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("Edits apply when the system is re-created (Reload, R).");
                int count = int(p->count);
                if (ImGui::DragInt("pool size", &count, 16.0f, 1, 200000)) p->count = uint32_t(count);
                ImGui::DragFloat("emission rate (/s)", &p->emissionRate, 5.0f, 0.0f, 100000.0f);
                ImGui::DragFloat("lifetime (s)", &p->lifetime, 0.05f, 0.0f, 120.0f);
                ImGui::DragFloat("emit speed", &p->emitSpeed, 0.05f, 0.0f, 100.0f);
                ImGui::SliderFloat("cone (rad)", &p->emitSpread, 0.0f, 3.14f);
                ImGui::ColorEdit3("colour", &p->color.x);
                ImGui::Checkbox("push rigid bodies", &p->pushRigidBodies);
            }
            if (Physics::Advanced::SoftBody* s = e.TryGet<Physics::Advanced::SoftBody>(); s && ImGui::CollapsingHeader("Soft body", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("Edits apply when the body is re-created (Reload, R).");
                ImGui::DragFloat("compliance", &s->compliance, 1e-5f, 0.0f, 0.1f, "%.6f");
                ImGui::SliderInt("substeps", &s->substeps, 1, 40);
                ImGui::SliderFloat("damping", &s->damping, 0.0f, 5.0f);
                ImGui::Checkbox("push rigid bodies", &s->pushRigidBodies);
            }
            ImGui::End();
            (void)ctx;
        }

        void Gizmo(Scene& scene) {
            Entity e = Selected(scene);
            Entity camera = scene.GetPrimaryCamera();
            if (!e || !camera || !camera.Has<Camera>()) return;
            const Camera& cam = camera.Get<Camera>();
            const ImGuiIO& io = ImGui::GetIO();
            const float aspect = io.DisplaySize.x / std::max(io.DisplaySize.y, 1.0f);
            const glm::mat4 view = glm::inverse(camera.WorldMatrix());
            const glm::mat4 projection = cam.projection == Projection::Orthographic
                ? glm::ortho(-0.5f * cam.orthoHeight * aspect, 0.5f * cam.orthoHeight * aspect, -0.5f * cam.orthoHeight, 0.5f * cam.orthoHeight, cam.nearPlane, cam.farPlane)
                : glm::perspective(cam.verticalFov, aspect, cam.nearPlane, cam.farPlane);
            ImGuizmo::SetOrthographic(cam.projection == Projection::Orthographic);
            ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
            ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
            glm::mat4 world = e.WorldMatrix();
            if (!ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), g_operation, g_mode, glm::value_ptr(world))) return;
            const Entity parent = e.GetParent();
            const glm::mat4 local = parent ? glm::inverse(parent.WorldMatrix()) * world : world;
            glm::vec3 position, euler, scale;
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(local), &position.x, &euler.x, &scale.x);
            Transform& t = e.LocalTransform();
            t.position = position;
            t.rotation = glm::quat(glm::radians(euler));
            const bool scaled = glm::any(glm::greaterThan(glm::abs(scale - t.scale), glm::vec3(1e-5f)));
            t.scale = scale;
            if (e.GetBody().Valid()) e.SetWorldPose(e.WorldPosition(), e.WorldRotation());
            if (scaled) e.MarkPhysicsDirty();
        }

        void Classroom(GameContext& ctx, Scene* scene) {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Classroom");
            for (const Lesson& lesson : LESSONS) {
                const bool current = scene && scene->Name() == lesson.scene;
                if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                if (ImGui::Button(lesson.title, ImVec2(-1.0f, 0.0f)) && ctx.scenes) ctx.scenes->Request(lesson.scene);
                if (current) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lesson.about);
            }
            ImGui::SeparatorText("Simulation");
            if (ImGui::SliderFloat("gravity (m/s^2)", &g_gravity, -30.0f, 5.0f)) Physics::SetGravity(glm::vec3(0.0f, g_gravity, 0.0f));
            if (ImGui::SliderFloat("friction (all)", &g_friction, 0.0f, 1.5f) && scene) {
                scene->Each<Collider>([&](Entity e, Collider& c) {
                    c.shape.material.staticFriction = g_friction;
                    c.shape.material.dynamicFriction = g_friction * 0.83f;
                    e.MarkPhysicsDirty();
                });
            }
            if (ImGui::SliderInt("solver substeps", &g_substeps, 1, 16)) Physics::SetSubsteps(uint32_t(g_substeps));
            ImGui::SeparatorText("Rendering");
            Render::RenderSettings& r = Render::Settings();
            if (ImGui::Combo("quality", &g_quality, "Low (i3 laptop)\0Medium\0High\0")) Render::ApplyQualityPreset(Render::QualityPreset(g_quality));
            ImGui::SliderFloat("render scale", &r.renderScale, 0.5f, 1.0f);
            int upscaler = int(r.upscaler);
            if (ImGui::Combo("upscaler", &upscaler, "Bilinear\0FSR 1 (EASU + RCAS)\0")) r.upscaler = Render::Upscaler(upscaler);
            ImGui::SliderFloat("RCAS sharpness (stops)", &r.sharpness, 0.0f, 2.0f);
            ImGui::Checkbox("GPU culling", &r.gpuCulling); ImGui::SameLine();
            ImGui::Checkbox("Hi-Z occlusion", &r.occlusionCulling);
            ImGui::Checkbox("shadows", &r.shadowsEnabled); ImGui::SameLine();
            ImGui::Checkbox("fog", &r.fog.enabled);
            ImGui::SliderFloat("exposure", &r.exposure, 0.1f, 4.0f);
            ImGui::Checkbox("physics debug draw", &ctx.physicsDebugDraw);
            ImGui::End();
        }

        void Telemetry(GameContext& ctx) {
            g_frameMs[g_historyCursor] = ctx.stats.cpuFrameMs;
            g_gpuMs[g_historyCursor] = std::max(ctx.stats.gpuFrameMs, 0.0f);
            g_historyCursor = (g_historyCursor + 1) % g_frameMs.size();
            ImGui::SetNextWindowPos(ImVec2(10.0f, ImGui::GetIO().DisplaySize.y - 330.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Telemetry");
            const Render::RendererInfo info = Render::GetRendererInfo();
            const RenderStats rs = GetRenderStats();
            const Physics::Stats ps = Physics::GetStats();
            const Physics::Advanced::Stats as = Physics::Advanced::GetStats();
            ImGui::Text("%s | %s", Platform::OperatingSystem(), info.renderer.c_str());
            ImGui::Text("OpenGL %d.%d, %s tier, physics: %s", info.contextVersion / 10, info.contextVersion % 10, info.compatTier ? "4.1" : "4.6", Physics::BackendName());
            ImGui::Text("CPU frame %.2f ms (update %.2f, physics %.2f, render %.2f)", ctx.stats.cpuFrameMs, ctx.stats.cpuUpdateMs, ctx.stats.cpuPhysicsMs, ctx.stats.cpuRenderMs);
            ImGui::PlotLines("##cpu", g_frameMs.data(), int(g_frameMs.size()), int(g_historyCursor), "CPU ms", 0.0f, 33.0f, ImVec2(-1.0f, 40.0f));
            ImGui::Text("GPU frame %.2f ms", ctx.stats.gpuFrameMs);
            ImGui::PlotLines("##gpu", g_gpuMs.data(), int(g_gpuMs.size()), int(g_historyCursor), "GPU ms", 0.0f, 33.0f, ImVec2(-1.0f, 40.0f));
            ImGui::Text("RAM %.0f MB (of %.1f GB), %u threads", double(Platform::ProcessMemoryBytes()) / 1048576.0, double(Platform::SystemMemoryBytes()) / 1073741824.0, Platform::HardwareThreads());
            ImGui::Text("render %dx%d -> %dx%d%s", rs.internalWidth, rs.internalHeight, rs.outputWidth, rs.outputHeight, rs.upscaled ? " (upscaled)" : "");
            ImGui::Text("drawn %u, culled %u (occlusion %u), %u tris", rs.instancesDrawn, rs.instancesCulled, rs.occlusionCulled, rs.trianglesDrawn);
            ImGui::Text("bodies: %u dynamic (%u awake), %u static", ps.dynamicBodies, ps.awakeDynamicBodies, ps.staticBodies);
            ImGui::Text("compute: %s, %u particles (%u live), %u soft nodes", as.cpuBackend ? "CPU" : as.gpuAvailable ? "GPU" : "-", as.particles, as.particlesAlive, as.softBodyNodes);
            ImGui::End();
        }

        void Timeline(GameContext& ctx, Scene* scene) {
            ImGui::SetNextWindowPos(ImVec2(350.0f, ImGui::GetIO().DisplaySize.y - 120.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - 730.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Timeline");
            Replay& replay = ctx.replay;
            bool enabled = replay.Enabled();
            if (ImGui::Checkbox("record", &enabled)) replay.SetEnabled(enabled);
            ImGui::SameLine();
            const bool paused = ctx.state == GameState::Paused;
            if (ImGui::Button(paused ? "Play" : "Pause")) ctx.state = paused ? GameState::Running : GameState::Paused;
            ImGui::SameLine();
            int index = int(replay.Scrubbed() ? replay.ScrubIndex() : (replay.Size() ? replay.Size() - 1 : 0));
            const int last = int(replay.Size()) - 1;
            if (ImGui::Button("<") && scene && last >= 0) { ctx.state = GameState::Paused; replay.Restore(*scene, size_t(std::max(index - 1, 0))); }
            ImGui::SameLine();
            if (ImGui::Button(">") && scene && last >= 0) { ctx.state = GameState::Paused; replay.Restore(*scene, size_t(std::min(index + 1, last))); }
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) replay.Clear();
            ImGui::SameLine();
            ImGui::Text("%zu steps (%.1f MB)", replay.Size(), double(replay.MemoryBytes()) / 1048576.0);
            if (last >= 0) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##scrub", &index, 0, last, "step %d") && scene) {
                    ctx.state = GameState::Paused;
                    replay.Restore(*scene, size_t(index));
                }
                if (replay.Scrubbed()) ImGui::TextDisabled("step %llu, t = %.2f s: pressing Play continues from here (newer steps are discarded)",
                                                           static_cast<unsigned long long>(replay.At(size_t(index)).step), replay.At(size_t(index)).simulationTime);
            }
            ImGui::End();
        }

        void MenuBar(GameContext& ctx, Scene* scene) {
            if (!ImGui::BeginMainMenuBar()) return;
            if (ImGui::BeginMenu("Scene")) {
                char path[260];
                std::snprintf(path, sizeof(path), "%s", g_path.c_str());
                ImGui::SetNextItemWidth(280.0f);
                if (ImGui::InputText("file", path, sizeof(path))) g_path = path;
                if (ImGui::MenuItem("Save JSON") && scene) {
                    g_status = SceneSerializer::Save(*scene, g_path) ? "saved " + g_path : "save failed: " + g_path;
                }
                if (ImGui::MenuItem("Load JSON") && scene) {
                    std::string error;
                    g_status = SceneSerializer::Load(*scene, g_path, &error) ? "loaded " + g_path : "load failed: " + error;
                    g_selected = 0;
                    ctx.replay.Clear();
                }
                if (ImGui::MenuItem("Reload scene") && ctx.scenes) ctx.scenes->RequestReload();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Lessons")) {
                for (const Lesson& lesson : LESSONS) if (ImGui::MenuItem(lesson.title) && ctx.scenes) ctx.scenes->Request(lesson.scene);
                ImGui::EndMenu();
            }
            if (!g_status.empty()) ImGui::TextDisabled("  %s", g_status.c_str());
            ImGui::EndMainMenuBar();
        }
    }

    void OnSceneLoaded(GameContext& ctx) {
        g_selected = 0;
        ctx.replay.Clear();
        const Physics::Settings& s = Physics::GetSettings();
        g_gravity = s.gravity.y;
        g_substeps = int(s.substeps);
    }

    void Draw(GameContext& ctx) {
        if (!UI::IsActive()) return;
        Scene* scene = ctx.CurrentScene();
        MenuBar(ctx, scene);
        if (scene) {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 360.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(330.0f, 260.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Hierarchy");
            ImGui::Text("%s: %zu entities", scene->Name().c_str(), scene->EntityCount());
            std::vector<Entity> roots;
            scene->Each<Transform>([&](Entity e, Transform&) { if (!e.GetParent()) roots.push_back(e); });
            std::sort(roots.begin(), roots.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
            for (Entity e : roots) Hierarchy(*scene, e);
            ImGui::End();
            Inspector(ctx, *scene);
            Gizmo(*scene);
        }
        Classroom(ctx, scene);
        Telemetry(ctx);
        Timeline(ctx, scene);
        HandleViewportClick(ctx);
    }

    void HandleViewportClick(GameContext& ctx) {
        if (!UI::IsActive() || UI::WantsMouse() || ImGuizmo::IsOver() || ImGuizmo::IsUsing() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        Scene* scene = ctx.CurrentScene();
        if (!scene) return;
        Render::FrameCapture capture;
        if (!Render::CaptureFrame(capture) || capture.width <= 0) return;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const int x = std::clamp(int(mouse.x / std::max(display.x, 1.0f) * float(capture.width)), 0, capture.width - 1);
        const int y = std::clamp(int((1.0f - mouse.y / std::max(display.y, 1.0f)) * float(capture.height)), 0, capture.height - 1);
        g_selected = capture.entityIds[size_t(y) * capture.width + x];
    }

    uint32_t SelectedEntity() { return g_selected; }
    void Select(uint32_t id) { g_selected = id; }
    std::string& ScenePath() { return g_path; }
}
