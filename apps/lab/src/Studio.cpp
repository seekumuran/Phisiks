#include "Studio.h"
#include "SandboxScenes.h"
#include "Showcase.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Phisiks;
namespace Retro = Phisiks::UI::Retro;

namespace Lab::Studio {
    namespace {
        uint32_t g_selected = 0;   // id + 1
        ImGuizmo::OPERATION g_operation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE g_mode = ImGuizmo::WORLD;
        std::string g_path = "Build/scenes/scene.json";
        std::string g_status;
        std::string g_caption1, g_caption2, g_caption;
        std::array<float, 240> g_frameNorm{};
        size_t g_historyCursor = 0;
        float g_gravity = -9.81f, g_friction = 0.6f;
        int g_substeps = 4;
        int g_quality = 2;

        struct Lesson { const char* title; const char* scene; const char* tag; const char* about; };
        const Lesson LESSONS[] = {
            { "Transforms & Rotors", "Rotors", "MATH", "Euler angles, quaternions and geometric-algebra rotors give the same rotation." },
            { "RK4 vs Euler Orbits", "Orbits", "MATH", "Integrator accuracy: explicit Euler gains energy, RK4 keeps the orbit closed." },
            { "Friction & Raycasts", "Sandbox", "PHYS", "Coulomb friction, raycast picking and the character controller (F2: physics debug)." },
            { "Rigid-body Stacking", "PhysicsTower", "PHYS", "A 16-box tower: solver sub-steps and sleeping." },
            { "PBR Lighting", "PBR", "GFX", "Roughness x metallic grid: GGX specular, hemisphere ambient, rim light." },
            { "GPU Culling & Particles", "FieldLab", "GPU", "Frustum + Hi-Z culling, spatial-hash particles in a vortex, N-body moons." },
            { "XPBD Soft Bodies & SDFs", "FieldLab", "GPU", "Cloth, rope and a soft box (XPBD), the raymarched SDF/CSG visualiser." },
            { "Emitters & Coupling", "Fountain", "GPU", "A particle emitter whose jet pushes dynamic crates (two-way coupling)." },
            { "glTF Skinning & Camera Paths", "Animation", "ANIM", "Dual-quaternion vs linear blend skinning of an imported glTF; Catmull-Rom fly-through (FLY)." },
            { "Bloom, SSAO, Trails & Decals", "VFX", "VFX", "HDR bloom mip chain, SSAO, ribbon trails, orbit paths, a raycast beam and projected decals." },
            { "Terrain, Foliage & NavMesh", "Terrain", "AI", "Splatmapped height-field terrain, instanced foliage, A* crowd, Doppler spatial audio." },
            { "Joints & Lua Scripting", "Joints", "LUA", "Hinge / ball / prismatic / spring joints on both physics backends, driven by Lua scripts." },
        };

        // Text on the dark console panels (headings and readouts in amber).
        ImVec4 Gold() { return ImVec4(Retro::HEADING[0], Retro::HEADING[1], Retro::HEADING[2], 1.0f); }
        ImVec4 Phosphor() { return ImVec4(Retro::READOUT[0], Retro::READOUT[1], Retro::READOUT[2], 1.0f); }

        Entity Selected(Scene& scene) { return g_selected ? scene.FromId(g_selected - 1) : Entity(); }

        bool DragVec3(const char* label, glm::vec3& v, float speed = 0.05f) { return ImGui::DragFloat3(label, &v.x, speed); }

        int CountDescendants(const Entity& e) {
            int n = 0;
            for (Entity c : e.GetChildren()) n += 1 + CountDescendants(c);
            return n;
        }

        void Hierarchy(Scene& scene, Entity e) {
            const std::vector<Entity> children = e.GetChildren();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            const bool selected = g_selected == e.Id() + 1;
            if (selected) flags |= ImGuiTreeNodeFlags_Selected;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(uintptr_t(e.Id() + 1)), flags, "%s", e.GetName().c_str());
            if (selected) ImGui::PopStyleColor();
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_selected = e.Id() + 1;
            if (!children.empty()) {   // right-aligned descendant count
                char count[16];
                std::snprintf(count, sizeof(count), "%d", CountDescendants(e));
                const float w = ImGui::CalcTextSize(count).x;
                ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - w - 4.0f, 0.0f));
                ImGui::TextColored(Gold(), "%s", count);
            }
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
            if (ImGui::Combo("pattern", &pattern, "None\0Checker\0Grid\0Noise\0Wood\0Tiles\0CalibrationGrid\0Splat\0")) { m.pattern = Render::Pattern(pattern); changed = true; }
            if (m.pattern == Render::Pattern::Splat) {
                for (int k = 0; k < 4; ++k) {
                    ImGui::PushID(k);
                    changed |= ImGui::ColorEdit3("layer", &m.splat[k].color.x);
                    ImGui::PopID();
                }
            }
            else if (m.pattern != Render::Pattern::None) {
                changed |= ImGui::ColorEdit3("pattern colour", &m.patternColor.x);
                changed |= ImGui::SliderFloat("pattern scale", &m.patternScale, 0.1f, 20.0f);
            }
            if (changed) Render::RegisterMaterial(name.empty() ? "Default" : name, m);
        }

        void Inspector(Scene& scene) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (!Retro::BeginPanel("INSPECTOR", display.x - 370.0f, 30.0f, 360.0f, std::floor((display.y - 50.0f) * 0.6f))) { Retro::EndPanel(); return; }
            Entity e = Selected(scene);
            if (!e) { ImGui::TextDisabled("Select an entity (hierarchy or click in the viewport)."); Retro::EndPanel(); return; }
            char name[128];
            std::snprintf(name, sizeof(name), "%s", e.GetName().c_str());
            if (ImGui::InputText("name", name, sizeof(name))) e.SetName(name);
            ImGui::Text("id %u", e.Id());
            ImGui::SameLine();
            if (Retro::Button("DESTROY")) { e.Destroy(); g_selected = 0; Retro::EndPanel(); return; }

            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                Transform& t = e.LocalTransform();
                bool moved = DragVec3("position", t.position);
                glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
                if (DragVec3("rotation", euler, 0.5f)) { t.rotation = glm::quat(glm::radians(euler)); moved = true; }
                const bool scaled = DragVec3("scale", t.scale, 0.01f);
                if (moved && e.GetBody().Valid()) e.SetWorldPose(e.WorldPosition(), e.WorldRotation());
                if (scaled) e.MarkPhysicsDirty();
                if (Retro::Button("MOVE", 0.0f, g_operation == ImGuizmo::TRANSLATE ? 1 : 0)) g_operation = ImGuizmo::TRANSLATE;
                ImGui::SameLine();
                if (Retro::Button("ROTATE", 0.0f, g_operation == ImGuizmo::ROTATE ? 1 : 0)) g_operation = ImGuizmo::ROTATE;
                ImGui::SameLine();
                if (Retro::Button("SCALE", 0.0f, g_operation == ImGuizmo::SCALE ? 1 : 0)) g_operation = ImGuizmo::SCALE;
                ImGui::SameLine();
                if (Retro::Button(g_mode == ImGuizmo::WORLD ? "WORLD" : "LOCAL", 0.0f, 2)) g_mode = g_mode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            }
            if (MeshRenderer* m = e.TryGet<MeshRenderer>(); m && ImGui::CollapsingHeader("Mesh renderer / material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("mesh %s, material %s", m->mesh.c_str(), m->material.empty() ? "Default" : m->material.c_str());
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
                if (c->shape.shape == Physics::ShapeType::HeightField) ImGui::Text("height field %ux%u, %.2f m cells", c->shape.heightfieldColumns, c->shape.heightfieldRows, double(c->shape.heightfieldCellSize));
                else if (ImGui::Combo("shape", &shape, "Box\0Sphere\0Capsule\0ConvexMesh\0TriangleMesh\0")) { c->shape.shape = Physics::ShapeType(shape); dirty = true; }
                if (c->shape.shape == Physics::ShapeType::Box) dirty |= DragVec3("half extents", c->shape.halfExtents, 0.01f);
                if (c->shape.shape == Physics::ShapeType::Sphere || c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("radius", &c->shape.radius, 0.01f, 0.01f, 50.0f);
                if (c->shape.shape == Physics::ShapeType::Capsule) dirty |= ImGui::DragFloat("half height", &c->shape.halfHeight, 0.01f, 0.01f, 50.0f);
                dirty |= Retro::EqSlider("static friction", &c->shape.material.staticFriction, 0.0f, 1.5f);
                dirty |= Retro::EqSlider("dynamic friction", &c->shape.material.dynamicFriction, 0.0f, 1.5f);
                dirty |= Retro::EqSlider("restitution", &c->shape.material.restitution, 0.0f, 1.0f);
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
                    if (Retro::Button("KICK UP")) Physics::AddImpulse(body, glm::vec3(0.0f, 4.0f * Physics::GetMass(body), 0.0f));
                }
            }
            if (Joint* j = e.TryGet<Joint>(); j && ImGui::CollapsingHeader("Joint", ImGuiTreeNodeFlags_DefaultOpen)) {
                static const char* TYPES[] = { "Hinge", "Ball-and-socket", "Prismatic", "Spring-damper" };
                ImGui::Text("%s to %s%s", TYPES[int(j->type)], j->connectedTo.empty() ? "the world" : j->connectedTo.c_str(), Physics::IsValid(j->runtime) ? "" : " (not created)");
                const Physics::JointState s = Physics::GetJointState(j->runtime);
                const bool angular = j->type == Physics::JointType::Hinge;
                ImGui::Text("position %.3f %s, speed %.3f %s", angular ? double(glm::degrees(s.position)) : double(s.position), angular ? "deg" : "m",
                            angular ? double(glm::degrees(s.speed)) : double(s.speed), angular ? "deg/s" : "m/s");
                if (j->type == Physics::JointType::Hinge || j->type == Physics::JointType::Prismatic) {
                    bool changed = Retro::LedToggle("MOTOR", &j->enableMotor);
                    changed |= Retro::EqSlider("motor speed", &j->motorSpeed, -6.0f, 6.0f);
                    changed |= Retro::EqSlider("max force", &j->maxMotorForce, 0.0f, 2000.0f, "%.0f");
                    if (changed) Physics::SetJointMotor(j->runtime, j->enableMotor, j->motorSpeed, j->maxMotorForce);
                }
            }
            if (SkinnedModel* m = e.TryGet<SkinnedModel>(); m && ImGui::CollapsingHeader("Skinned model (glTF)", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("%s  clip '%s'  %u vertices%s%s", m->path.c_str(), m->clip.c_str(), m->vertexCount, m->error.empty() ? "" : "  ERROR: ", m->error.c_str());
                Retro::LedToggle("PLAY", &m->playing);
                ImGui::SameLine();
                bool dq = m->method == Animation::SkinningMethod::DualQuaternion;
                if (Retro::LedToggle("DUAL QUAT", &dq)) m->method = dq ? Animation::SkinningMethod::DualQuaternion : Animation::SkinningMethod::Linear;
                Retro::EqSlider("speed", &m->speed, 0.0f, 3.0f);
                ImGui::DragFloat("time (s)", &m->time, 0.01f);
            }
            if (CameraPath* p = e.TryGet<CameraPath>(); p && ImGui::CollapsingHeader("Camera path", ImGuiTreeNodeFlags_DefaultOpen)) {
                Retro::LedToggle("FLY", &p->playing);
                ImGui::SameLine();
                ImGui::Text("%s, %zu points, %.1f m", p->type == Animation::SplineType::CatmullRom ? "Catmull-Rom" : "Bezier", p->points.size(), double(p->positionSpline.Length()));
                Retro::EqSlider("duration (s)", &p->duration, 2.0f, 60.0f, "%.1f");
            }
            if (NavAgent* n = e.TryGet<NavAgent>(); n && ImGui::CollapsingHeader("Nav agent", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("target (%.1f, %.1f, %.1f)%s, %zu waypoints, %u plans", n->target.x, n->target.y, n->target.z, n->arrived ? " ARRIVED" : "", n->path.size(), n->replans);
                Retro::EqSlider("max speed", &n->maxSpeed, 0.2f, 6.0f);
                Retro::EqSlider("avoidance", &n->avoidance, 0.0f, 6.0f);
            }
            if (Script* s = e.TryGet<Script>(); s && ImGui::CollapsingHeader("Lua script", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!s->file.empty()) ImGui::Text("file %s (re-runs on save)", s->file.c_str());
                else ImGui::TextDisabled("inline source, %zu bytes", s->source.size());
                if (!s->error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", s->error.c_str());
                Retro::LedToggle("ENABLED", &s->enabled);
                ImGui::SameLine();
                if (Retro::Button("RELOAD ALL")) Scripting::Reload(scene);
            }
            if (AudioSource* a = e.TryGet<AudioSource>(); a && ImGui::CollapsingHeader("Audio source", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("%s%s", a->clip.c_str(), a->isPlaying ? "  [playing]" : "");
                Retro::EqSlider("volume", &a->volume, 0.0f, 1.0f);
                Audio::SpatialInfo info;
                if (a->spatial && Audio::GetSpatialInfo(a->voice, info)) {
                    Retro::LedMeter("L", info.gainLeft, nullptr);
                    Retro::LedMeter("R", info.gainRight, nullptr);
                    ImGui::Text("%.1f m, gain %.2f, pan %+.2f, Doppler x%.3f", info.distance, info.distanceGain, info.pan, info.dopplerPitch);
                }
                if (Retro::Button("PLAY")) a->Play();
            }
            if (Terrain* t = e.TryGet<Terrain>(); t && ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("%u x %u samples, %.2f m cells, %u triangles", t->generator.resolution, t->generator.resolution, double(t->generator.cellSize), t->triangles);
            }
            if (Decal* d = e.TryGet<Decal>(); d && ImGui::CollapsingHeader("Decal", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("visible", &d->visible);
                ImGui::ColorEdit4("colour", &d->color.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                DragVec3("size", d->size, 0.02f);
            }
            if (Trail* t = e.TryGet<Trail>(); t && ImGui::CollapsingHeader("Trail", ImGuiTreeNodeFlags_DefaultOpen)) {
                Retro::EqSlider("lifetime", &t->lifetime, 0.1f, 5.0f);
                Retro::EqSlider("width", &t->width, 0.01f, 0.6f);
                ImGui::Text("%zu points", t->points.size());
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
            Retro::EndPanel();
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

        CameraPath* FindCameraPath(Scene* scene) {
            if (!scene) return nullptr;
            CameraPath* found = nullptr;
            scene->Each<CameraPath>([&](Entity, CameraPath& p) { if (!found) found = &p; });
            return found;
        }

        void Classroom(GameContext& ctx, Scene* scene) {
            if (!Retro::BeginPanel("PHISIKS LAB", 10.0f, 30.0f, 330.0f, std::max(ImGui::GetIO().DisplaySize.y - 340.0f, 300.0f))) { Retro::EndPanel(); return; }
            ImGui::TextColored(Gold(), "CLASSROOM PLAYLIST");
            if (Retro::BeginPlaylist("##lessons", (ImGui::GetTextLineHeight() + 4.0f) * 6.5f)) {
                int n = 1;
                for (const Lesson& lesson : LESSONS) {
                    const bool current = scene && scene->Name() == lesson.scene;
                    if (Retro::PlaylistRow(n++, lesson.title, lesson.tag, current) && ctx.scenes) ctx.scenes->Request(lesson.scene);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lesson.about);
                }
            }
            Retro::EndPlaylist();
            ImGui::TextColored(Gold(), "SHOWCASE PLAYLIST");
            if (Retro::BeginPlaylist("##showcases", (ImGui::GetTextLineHeight() + 4.0f) * 4.5f)) {
                int n = 1;
                for (const Showcase::DemoInfo& demo : Showcase::Demos()) {
                    const bool current = scene && scene->Name() == demo.scene;
                    if (Retro::PlaylistRow(n++, demo.title, demo.industry, current) && ctx.scenes) ctx.scenes->Request(demo.scene);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(also: %s)", demo.blurb, demo.executable);
                }
            }
            Retro::EndPlaylist();
            if (!g_caption1.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Gold());
                ImGui::TextWrapped("%s", g_caption1.c_str());
                ImGui::PopStyleColor();
                ImGui::TextWrapped("%s", g_caption2.c_str());
            }

            ImGui::SeparatorText("SIM EQUALISER");
            if (Retro::EqSlider("GRAV", &g_gravity, -30.0f, 5.0f, "%.1f", true)) Physics::SetGravity(glm::vec3(0.0f, g_gravity, 0.0f));
            ImGui::SameLine();
            if (Retro::EqSlider("FRIC", &g_friction, 0.0f, 1.5f, "%.2f", true) && scene) {
                scene->Each<Collider>([&](Entity e, Collider& c) {
                    c.shape.material.staticFriction = g_friction;
                    c.shape.material.dynamicFriction = g_friction * 0.83f;
                    e.MarkPhysicsDirty();
                });
            }
            ImGui::SameLine();
            if (Retro::EqSliderInt("SUBS", &g_substeps, 1, 16, true)) Physics::SetSubsteps(uint32_t(g_substeps));
            Render::RenderSettings& r = Render::Settings();
            ImGui::SameLine();
            Retro::EqSlider("EXPO", &r.exposure, 0.1f, 4.0f, "%.2f", true);
            ImGui::SameLine();
            Retro::EqSlider("SCALE", &r.renderScale, 0.5f, 1.0f, "%.2f", true);
            ImGui::SameLine();
            Retro::EqSlider("BLOOM", &r.bloom.intensity, 0.0f, 0.3f, "%.2f", true);

            ImGui::SeparatorText("RENDER DECK");
            if (ImGui::Combo("quality", &g_quality, "Low (i3 laptop)\0Medium\0High\0")) Render::ApplyQualityPreset(Render::QualityPreset(g_quality));
            int upscaler = int(r.upscaler);
            if (ImGui::Combo("upscaler", &upscaler, "Bilinear\0FSR 1 (EASU + RCAS)\0")) r.upscaler = Render::Upscaler(upscaler);
            Retro::EqSlider("RCAS sharpness", &r.sharpness, 0.0f, 2.0f);
            Retro::EqSlider("SSAO radius", &r.ssao.radius, 0.1f, 2.0f);
            Retro::LedToggle("CULL", &r.gpuCulling); ImGui::SameLine();
            Retro::LedToggle("HI-Z", &r.occlusionCulling); ImGui::SameLine();
            Retro::LedToggle("SHADOW", &r.shadowsEnabled); ImGui::SameLine();
            Retro::LedToggle("FOG", &r.fog.enabled);
            Retro::LedToggle("BLOOM", &r.bloom.enabled); ImGui::SameLine();
            Retro::LedToggle("SSAO", &r.ssao.enabled); ImGui::SameLine();
            Retro::LedToggle("DECAL", &r.decals); ImGui::SameLine();
            Retro::LedToggle("DEBUG", &ctx.physicsDebugDraw);
            if (CameraPath* path = FindCameraPath(scene)) { Retro::LedToggle("FLY-THROUGH", &path->playing); ImGui::SameLine(); }
            const Render::ShaderReloadStatus reload = Render::GetShaderReloadStatus();
            if (Retro::Button("RELOAD SHADERS", 0.0f, reload.enabled ? (reload.failures ? 2 : 1) : 0)) {
                if (!reload.enabled) Render::EnableShaderHotReload("engine/shaders");
                else Render::ReloadShadersNow();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Watches engine/shaders: edits rebuild programs live (%u reloads, %u failed)%s%s",
                                                          reload.reloads, reload.failures, reload.lastError.empty() ? "" : "\n", reload.lastError.c_str());
            Retro::EndPanel();
        }

        void Telemetry(GameContext& ctx) {
            g_frameNorm[g_historyCursor] = ctx.stats.cpuFrameMs / 33.3f;
            g_historyCursor = (g_historyCursor + 1) % g_frameNorm.size();
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (!Retro::BeginPanel("TELEMETRY", 10.0f, display.y - 300.0f, 330.0f, 290.0f)) { Retro::EndPanel(); return; }
            const double t = ctx.stats.simulationTime;
            const int minutes = int(t / 60.0);
            char big[32], small[96];
            std::snprintf(big, sizeof(big), "%02d:%04.1f", minutes % 100, t - minutes * 60.0);
            const float fps = ctx.stats.cpuFrameMs > 0.0f ? 1000.0f / ctx.stats.cpuFrameMs : 0.0f;
            std::snprintf(small, sizeof(small), "FPS %3.0f  STEP %llu  %s", double(fps), static_cast<unsigned long long>(ctx.stats.totalFixedSteps), ToString(ctx.state));
            Retro::Lcd(big, small);
            char value[32];
            std::snprintf(value, sizeof(value), "%.1f ms", double(ctx.stats.cpuFrameMs));
            Retro::LedMeter("CPU", ctx.stats.cpuFrameMs / 33.3f, value);
            std::snprintf(value, sizeof(value), "%.1f ms", double(std::max(ctx.stats.gpuFrameMs, 0.0f)));
            Retro::LedMeter("GPU", std::max(ctx.stats.gpuFrameMs, 0.0f) / 33.3f, value);
            std::snprintf(value, sizeof(value), "%.2f ms", double(ctx.stats.cpuPhysicsMs));
            Retro::LedMeter("PHYS", ctx.stats.cpuPhysicsMs / 16.7f, value);
            const double ram = double(Platform::ProcessMemoryBytes()), total = std::max(double(Platform::SystemMemoryBytes()), 1.0);
            std::snprintf(value, sizeof(value), "%.0f MB", ram / 1048576.0);
            Retro::LedMeter("RAM", float(ram / total) * 4.0f, value);   // full scale = 25 % of system memory
            Retro::Analyzer("##frames", g_frameNorm.data(), int(g_frameNorm.size()), int(g_historyCursor), 40.0f);
            const Render::RendererInfo info = Render::GetRendererInfo();
            const RenderStats rs = GetRenderStats();
            const Physics::Stats ps = Physics::GetStats();
            ImGui::TextDisabled("%s | GL %d.%d %s tier | %s", Platform::OperatingSystem(), info.contextVersion / 10, info.contextVersion % 10, info.compatTier ? "4.1" : "4.6", Physics::BackendName());
            ImGui::TextDisabled("%dx%d -> %dx%d  drawn %u  culled %u  bodies %u  joints %u", rs.internalWidth, rs.internalHeight, rs.outputWidth, rs.outputHeight,
                                rs.instancesDrawn, rs.instancesCulled, ps.dynamicBodies, ps.joints);
            Retro::EndPanel();
        }

        void Timeline(GameContext& ctx, Scene* scene) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (!Retro::BeginPanel("REPLAY TIMELINE", 350.0f, display.y - 112.0f, std::max(display.x - 740.0f, 300.0f), 102.0f)) { Retro::EndPanel(); return; }
            Replay& replay = ctx.replay;
            int index = int(replay.Scrubbed() ? replay.ScrubIndex() : (replay.Size() ? replay.Size() - 1 : 0));
            const int last = int(replay.Size()) - 1;
            const bool paused = ctx.state == GameState::Paused;
            auto restore = [&](int i) { if (scene && last >= 0) { ctx.state = GameState::Paused; replay.Restore(*scene, size_t(std::clamp(i, 0, last))); } };
            if (Retro::TransportButton("##back", Retro::Glyph::StepBack, false, "Step back")) restore(index - 1);
            ImGui::SameLine();
            if (Retro::TransportButton("##play", Retro::Glyph::Play, !paused, "Play (continues from the scrubbed step)")) ctx.state = GameState::Running;
            ImGui::SameLine();
            if (Retro::TransportButton("##pause", Retro::Glyph::Pause, paused, "Pause")) ctx.state = GameState::Paused;
            ImGui::SameLine();
            if (Retro::TransportButton("##stop", Retro::Glyph::Stop, false, "Stop: rewind to the first recorded step")) restore(0);
            ImGui::SameLine();
            if (Retro::TransportButton("##forward", Retro::Glyph::StepForward, false, "Step forward")) restore(index + 1);
            ImGui::SameLine();
            const bool enabled = replay.Enabled();
            if (Retro::TransportButton("##record", Retro::Glyph::Record, enabled, "Record replay")) replay.SetEnabled(!enabled);
            ImGui::SameLine();
            if (Retro::TransportButton("##eject", Retro::Glyph::Eject, false, "Clear the timeline")) replay.Clear();
            ImGui::SameLine();
            ImGui::TextColored(Phosphor(), "%zu steps  %.1f MB", replay.Size(), double(replay.MemoryBytes()) / 1048576.0);
            if (last >= 0) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##scrub", &index, 0, last, "step %d")) restore(index);
                if (replay.Scrubbed()) ImGui::TextDisabled("step %llu, t = %.2f s: Play continues from here (newer steps are discarded)",
                                                           static_cast<unsigned long long>(replay.At(size_t(index)).step), replay.At(size_t(index)).simulationTime);
            }
            Retro::EndPanel();
        }

        void HierarchyPanel(Scene& scene) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;   // right column, under the inspector
            const float top = 40.0f + std::floor((display.y - 50.0f) * 0.6f), height = std::max(display.y - 10.0f - top, 120.0f);
            if (!Retro::BeginPanel("SCENE HIERARCHY", display.x - 370.0f, top, 360.0f, height)) { Retro::EndPanel(); return; }
            ImGui::TextColored(Gold(), "%s", scene.Name().c_str());
            ImGui::SameLine();
            ImGui::TextColored(Phosphor(), "%zu entities", scene.EntityCount());
            if (Retro::BeginPlaylist("##hierarchy", -1.0f)) {
                Retro::PushPlaylistTreeStyle();
                std::vector<Entity> roots;
                scene.Each<Transform>([&](Entity e, Transform&) { if (!e.GetParent()) roots.push_back(e); });
                std::sort(roots.begin(), roots.end(), [](const Entity& a, const Entity& b) { return a.Id() < b.Id(); });
                for (Entity e : roots) Hierarchy(scene, e);
                Retro::PopPlaylistTreeStyle();
            }
            Retro::EndPlaylist();
            Retro::EndPanel();
        }

        // Console menu bar: File | View | Simulation | CAD | Help.
        void MenuBar(GameContext& ctx, Scene* scene) {
            if (!Retro::BeginMenuBar("PHISIKS LAB")) return;
            if (ImGui::BeginMenu("File")) {
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
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Hide studio panels", "Tab")) UI::SetPanelsVisible(false);
                if (ImGui::MenuItem("Telemetry overlay", "F1", ctx.overlayVisible)) ctx.overlayVisible = !ctx.overlayVisible;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Simulation")) {
                const bool paused = ctx.state == GameState::Paused;
                if (ImGui::MenuItem(paused ? "Resume" : "Pause", "Esc")) ctx.state = paused ? GameState::Running : GameState::Paused;
                ImGui::Separator();
                if (ImGui::BeginMenu("Lessons")) {
                    for (const Lesson& lesson : LESSONS) if (ImGui::MenuItem(lesson.title) && ctx.scenes) ctx.scenes->Request(lesson.scene);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Showcases")) {
                    for (const Showcase::DemoInfo& demo : Showcase::Demos()) if (ImGui::MenuItem(demo.title, demo.industry) && ctx.scenes) ctx.scenes->Request(demo.scene);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("CAD")) {
                const bool quad = scene && IsQuadView(*scene);
                if (ImGui::MenuItem("4-way orthographic views", "F3", quad) && scene) SetQuadView(*scene, !quad);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                ImGui::TextDisabled("Tab  panels   F1  telemetry   F2  physics debug   F3  4-way views");
                ImGui::TextDisabled("RMB drag orbit   MMB pan   wheel zoom   LMB select / gizmo");
                ImGui::EndMenu();
            }
            if (!g_status.empty()) ImGui::TextDisabled("  %s", g_status.c_str());
            Retro::EndMenuBar();
        }
    }

    void OnSceneLoaded(GameContext& ctx) {
        g_selected = 0;
        ctx.replay.Clear();
        g_caption1.clear();
        g_caption2.clear();
        const Physics::Settings& s = Physics::GetSettings();
        g_gravity = s.gravity.y;
        g_substeps = int(s.substeps);
    }

    void Draw(GameContext& ctx) {
        if (!UI::IsActive()) return;
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) UI::SetPanelsVisible(!UI::PanelsVisible());
        if (!UI::PanelsVisible()) return;
        if (Showcase::Current(ctx)) {   // showcase scenes bring their own RetroDeck HUD
            Showcase::DrawGui(ctx, "Sandbox");
            return;
        }
        Scene* scene = ctx.CurrentScene();
        MenuBar(ctx, scene);
        Classroom(ctx, scene);
        if (scene) {
            HierarchyPanel(*scene);
            Inspector(*scene);
            Gizmo(*scene);
        }
        Telemetry(ctx);
        Timeline(ctx, scene);
        HandleViewportClick(ctx);
    }

    void HandleViewportClick(GameContext& ctx) {
        if (!UI::PanelsVisible() || UI::WantsMouse() || ImGuizmo::IsOver() || ImGuizmo::IsUsing() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
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
    void SetCaption(const std::string& line1, const std::string& line2) {
        g_caption1 = line1;
        g_caption2 = line2;
        g_caption = line1 + "\n" + line2;
    }
    const std::string& Caption() { return g_caption; }
}
