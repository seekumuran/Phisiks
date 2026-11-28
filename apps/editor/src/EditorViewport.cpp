// PHISIKS editor viewport: camera viewport fitting, 4-way view, gizmo, picking, framing.
#include "EditorInternal.h"

#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/UI/ImGuiLayer.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Phisiks;

namespace Editor {
    namespace {
        int g_operation = ImGuizmo::TRANSLATE;
        int g_mode = ImGuizmo::WORLD;
        bool g_gizmoWasUsing = false;
        const char* const QUAD_NAMES[3] = { "TopView", "FrontView", "SideView" };

        struct Bounds { glm::vec3 lo{ std::numeric_limits<float>::max() }, hi{ -std::numeric_limits<float>::max() }; bool valid = false; };

        void Extend(Bounds& b, const glm::vec3& p, float r) {
            b.lo = glm::min(b.lo, p - glm::vec3(r));
            b.hi = glm::max(b.hi, p + glm::vec3(r));
            b.valid = true;
        }

        // Renderable objects below `root` (or the whole scene); `withSize` adds each object's scale.
        Bounds RenderableBounds(Scene& scene, Entity root, bool withSize) {
            Bounds b;
            scene.Each<MeshRenderer>([&](Entity e, MeshRenderer& m) {
                if (!m.visible || scene.IsPendingDestroy(e)) return;
                if (root && !(e == root) && !e.IsDescendantOf(root)) return;
                glm::vec3 lo, hi;
                if (e.Has<CadModel>() && Internal::CadBounds(e, lo, hi)) { Extend(b, lo, 0.0f); Extend(b, hi, 0.0f); return; }   // CAD parts: their real size
                const glm::vec3 s = e.WorldScale();
                const float r = withSize ? 0.5f * std::max({ std::abs(s.x), std::abs(s.y), std::abs(s.z) }) : 0.0f;
                if (!withSize && std::max({ std::abs(s.x), std::abs(s.z) }) > 30.0f) return;   // ground planes do not set the framing
                Extend(b, e.WorldPosition(), r);
            });
            if (root && !b.valid) Extend(b, root.WorldPosition(), 0.5f);
            return b;
        }

        // Normalised camera viewport (x, y from the bottom, w, h) of the editor's centre area.
        glm::vec4 CentreRect(bool panels) {
            const Internal::Layout& L = Internal::GetLayout();
            if (!panels || !Config().insetViewport || L.display.x < 1.0f || L.display.y < 1.0f || L.viewport.z < 1.0f) return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            const glm::vec4 v = L.viewport;
            return glm::vec4(v.x / L.display.x, 1.0f - (v.y + v.w) / L.display.y, v.z / L.display.x, v.w / L.display.y);
        }

        // Pixel rect (top-left origin) of a normalised viewport.
        glm::vec4 ToPixels(const glm::vec4& vp) {
            const ImVec2 d = ImGui::GetIO().DisplaySize;
            return glm::vec4(vp.x * d.x, (1.0f - vp.y - vp.w) * d.y, vp.z * d.x, vp.w * d.y);
        }

        void SetViewport(Entity e, const glm::vec4& vp) {
            if (!e) return;
            if (Camera* c = e.TryGet<Camera>(); c && c->viewport != vp) c->viewport = vp;
        }
    }

    namespace Internal {
        int& GizmoOperation() { return g_operation; }
        int& GizmoMode() { return g_mode; }

        bool MouseInViewport() {
            const glm::vec4 v = GetLayout().viewport;
            const ImVec2 m = ImGui::GetIO().MousePos;
            return m.x >= v.x && m.y >= v.y && m.x < v.x + v.z && m.y < v.y + v.w;
        }

        void ApplyViewportLayout(GameContext&, Scene& scene, bool panels) {
            const glm::vec4 r = CentreRect(panels);
            Entity main = scene.FindByName("MainCamera");
            if (!IsQuadView(scene)) { SetViewport(main, r); return; }
            const float hw = r.z * 0.5f, hh = r.w * 0.5f;
            SetViewport(main, glm::vec4(r.x, r.y + hh, hw, hh));
            SetViewport(scene.FindByName(QUAD_NAMES[0]), glm::vec4(r.x + hw, r.y + hh, hw, hh));
            SetViewport(scene.FindByName(QUAD_NAMES[1]), glm::vec4(r.x, r.y, hw, hh));
            SetViewport(scene.FindByName(QUAD_NAMES[2]), glm::vec4(r.x + hw, r.y, hw, hh));
        }

        void DrawViewportLabels(Scene& scene) {
            if (!IsQuadView(scene)) return;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float s = GetLayout().scale;
            const char* labels[4] = { "PERSPECTIVE", "TOP", "FRONT", "SIDE" };
            Entity cams[4] = { scene.FindByName("MainCamera"), scene.FindByName(QUAD_NAMES[0]), scene.FindByName(QUAD_NAMES[1]), scene.FindByName(QUAD_NAMES[2]) };
            for (int i = 0; i < 4; ++i) {
                const Camera* c = cams[i] ? cams[i].TryGet<Camera>() : nullptr;
                if (!c) continue;
                const glm::vec4 p = ToPixels(c->viewport);
                dl->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + p.z, p.y + p.w), IM_COL32(0x3E, 0x43, 0x56, 0xFF));
                const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
                dl->AddRectFilled(ImVec2(p.x + 6.0f * s, p.y + 6.0f * s), ImVec2(p.x + 14.0f * s + ts.x, p.y + 10.0f * s + ts.y), IM_COL32(0x12, 0x13, 0x1A, 0xD0));
                dl->AddText(ImVec2(p.x + 10.0f * s, p.y + 8.0f * s), IM_COL32(0xFF, 0xBE, 0x0B, 0xFF), labels[i]);
            }
        }

        bool CameraMatrices(Scene& scene, glm::mat4& view, glm::mat4& projection, glm::vec4& rect, bool* orthographic) {
            Entity camera = scene.GetPrimaryCamera();
            if (!camera || !camera.Has<Camera>()) return false;
            const Camera& cam = camera.Get<Camera>();
            rect = ToPixels(glm::clamp(cam.viewport, glm::vec4(0.0f), glm::vec4(1.0f)));
            if (rect.z < 2.0f || rect.w < 2.0f) return false;
            const float aspect = rect.z / rect.w;   // the renderer's projection for this view (Renderer.cpp MakeProjection)
            view = glm::inverse(camera.WorldMatrix());
            projection = cam.projection == Projection::Orthographic
                ? glm::ortho(-0.5f * cam.orthoHeight * aspect, 0.5f * cam.orthoHeight * aspect, -0.5f * cam.orthoHeight, 0.5f * cam.orthoHeight, cam.nearPlane, cam.farPlane)
                : glm::perspective(cam.verticalFov, aspect, cam.nearPlane, cam.farPlane);
            if (orthographic) *orthographic = cam.projection == Projection::Orthographic;
            return true;
        }

        void DrawGizmo(GameContext&, Scene& scene) {
            Entity e = Selected(scene);
            Entity camera = scene.GetPrimaryCamera();
            if (!e || !camera || !camera.Has<Camera>() || e == camera || CadBlocksGizmo()) { g_gizmoWasUsing = false; return; }
            glm::mat4 view, projection;
            glm::vec4 rect;
            bool ortho = false;
            if (!CameraMatrices(scene, view, projection, rect, &ortho)) return;
            ImGuizmo::SetOrthographic(ortho);
            ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
            ImGuizmo::SetRect(rect.x, rect.y, rect.z, rect.w);
            glm::mat4 world = e.WorldMatrix();
            const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), ImGuizmo::OPERATION(g_operation), ImGuizmo::MODE(g_mode), glm::value_ptr(world));
            if (ImGuizmo::IsUsing() && !g_gizmoWasUsing) NoteEdit();
            g_gizmoWasUsing = ImGuizmo::IsUsing();
            if (!changed) return;
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

        void HandleViewportClick(GameContext&, Scene& scene) {
            if (CadBlocksGizmo()) return;   // the CAD sketch / pick tools own viewport clicks
            if (UI::WantsMouse() || ImGuizmo::IsOver() || ImGuizmo::IsUsing() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || !MouseInViewport()) return;
            Render::FrameCapture capture;
            if (!Render::CaptureFrame(capture) || capture.width <= 0) return;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            const int x = std::clamp(int(mouse.x / std::max(display.x, 1.0f) * float(capture.width)), 0, capture.width - 1);
            const int y = std::clamp(int((1.0f - mouse.y / std::max(display.y, 1.0f)) * float(capture.height)), 0, capture.height - 1);
            const uint32_t id = capture.entityIds[size_t(y) * capture.width + x];
            Select(id && scene.FromId(id - 1) ? id : 0);
        }

        void FrameSelected(GameContext& ctx, Scene& scene) {
            if (ctx.camera.GetMode() != CameraMode::Orbit) return;   // first person: F4 switches to the orbit camera first
            const Entity sel = Selected(scene);
            Bounds b = RenderableBounds(scene, sel, true);
            if (!b.valid) b = RenderableBounds(scene, Entity(), false);
            if (!b.valid) return;
            const glm::vec3 centre = 0.5f * (b.lo + b.hi);
            const float radius = std::max(0.5f * glm::length(b.hi - b.lo), 0.05f);
            const float fov = std::max(scene.GetPrimaryCamera() && scene.GetPrimaryCamera().Has<Camera>() ? scene.GetPrimaryCamera().Get<Camera>().verticalFov : 1.0f, 0.2f);
            const float distance = radius / std::sin(0.5f * fov) * 1.1f;
            ctx.camera.SetOrbit(centre, distance, ctx.camera.GetYaw(), ctx.camera.GetPitch());
            if (Entity camera = scene.FindByName("MainCamera")) if (Camera* c = camera.TryGet<Camera>()) {
                c->nearPlane = std::min(c->nearPlane, std::max(distance * 0.01f, 0.005f));
                c->farPlane = std::max(c->farPlane, distance * 20.0f);
            }
        }
    }

    // ===================================================================== 4-way view
    void SetQuadView(Scene& scene, bool enabled, const QuadFraming* framing) {
        for (const char* name : QUAD_NAMES) if (Entity e = scene.FindByName(name); e.Valid()) scene.DestroyImmediate(e);
        if (Entity main = scene.FindByName("MainCamera")) if (Camera* c = main.TryGet<Camera>()) c->viewport = glm::vec4(0.0f, enabled ? 0.5f : 0.0f, enabled ? 0.5f : 1.0f, enabled ? 0.5f : 1.0f);
        if (!enabled) return;
        QuadFraming f;
        if (framing) f = *framing;
        else {   // fit the scene's renderable objects
            Bounds b = RenderableBounds(scene, Entity(), false);
            if (!b.valid) Extend(b, glm::vec3(0.0f), 1.0f);
            const glm::vec3 e = glm::max(0.5f * (b.hi - b.lo), glm::vec3(0.25f));
            f.center = 0.5f * (b.lo + b.hi);
            f.topHeight = 2.6f * std::max(e.x, e.z);
            f.sideHeight = 2.6f * std::max(e.y, 0.5f * std::max(e.x, e.z));
            f.distance = 4.0f * std::max({ e.x, e.y, e.z }) + 5.0f;
        }
        struct Def { const char* name; glm::vec3 offset, direction, up; glm::vec4 viewport; float height; };
        const Def defs[] = {
            { QUAD_NAMES[0], { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, f.topHeight },
            { QUAD_NAMES[1], { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.5f, 0.5f }, f.sideHeight },
            { QUAD_NAMES[2], { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.0f, 0.5f, 0.5f }, f.sideHeight },
        };
        int order = 1;
        for (const Def& d : defs) {
            Entity e = scene.CreateEntity(d.name);
            Transform& t = e.Get<Transform>();
            t.position = f.center + d.offset * f.distance;
            t.rotation = glm::quatLookAt(d.direction, d.up);
            Camera& c = e.Add<Camera>();
            c.projection = Projection::Orthographic;
            c.orthoHeight = d.height;
            c.nearPlane = std::max(0.02f * f.distance, 0.01f);
            c.farPlane = 2.5f * f.distance;
            c.viewport = d.viewport;
            c.order = order++;
            c.primary = false;
        }
    }

    bool IsQuadView(Scene& scene) { return scene.FindByName(QUAD_NAMES[0]).Valid(); }
}
