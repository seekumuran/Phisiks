#include "Phisiks/Framework/DebugOverlay.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/UI/ImGuiLayer.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Phisiks::DebugOverlay {
    namespace {
        constexpr float SCALE = 2.0f;
        const glm::vec4 WHITE{ 1.0f, 1.0f, 1.0f, 1.0f };
        const glm::vec4 GREY{ 0.72f, 0.72f, 0.72f, 1.0f };
        const glm::vec4 YELLOW{ 1.0f, 0.8f, 0.3f, 1.0f };
        const glm::vec4 GREEN{ 0.5f, 1.0f, 0.5f, 1.0f };

        std::string Format(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), fmt, a, b, c, d);
            return buffer;
        }

        using Line = std::pair<glm::vec4, std::string>;
        int g_telemetryLines = 0;

        void Lines(const std::vector<Line>& lines, float x, float y) {
            const float step = Render::MeasureText("X", SCALE).y + 4.0f;
            for (const Line& line : lines) {
                Render::DrawScreenText(line.second, glm::vec2(x, y), line.first, SCALE);
                y += step;
            }
        }

        void Centered(const std::string& text, glm::vec2 center, const glm::vec4& color, float scale) {
            const glm::vec2 size = Render::MeasureText(text, scale);
            Render::DrawScreenText(text, center - size * 0.5f, color, scale);
        }
    }

    void Draw(GameContext& ctx) {
        const glm::vec2 screen = glm::vec2(Render::GetFramebufferSize());
        const glm::vec2 center = screen * 0.5f;
        Centered("+", center, WHITE, SCALE);

        const TraceHit target = Gameplay::TraceFromCamera(ctx, 6.0f);
        if (target.hit && target.entity.Valid()) {
            Entity entity = target.entity;
            std::string label = entity.GetName();
            if (const Health* health = entity.TryGet<Health>()) label += Format("  HP %.0f/%.0f", health->current, health->max);
            if (const Interactable* interactable = entity.TryGet<Interactable>()) label += "  [E] " + interactable->prompt;
            if (const Rigidbody* rb = entity.TryGet<Rigidbody>(); rb && rb->type == Physics::BodyType::Dynamic) label += "  [LMB] push";
            Centered(label, center + glm::vec2(0.0f, 40.0f), YELLOW, SCALE);
        }

        if (ctx.state == GameState::Paused) {
            Centered("PAUSED  -  ESC resume   Q quit", center - glm::vec2(0.0f, 80.0f), YELLOW, SCALE * 1.5f);
        }

        g_telemetryLines = 0;
        // The studio panels (Dear ImGui) carry the same telemetry; the bitmap text block would sit
        // behind the left-hand panels, so it is only drawn while they are hidden.
        if (!ctx.overlayVisible || UI::PanelsVisible()) return;

        const FrameStats& s = ctx.stats;
        const glm::vec3 feet = ctx.player.GetFeetPosition();
        const glm::vec3 vel = ctx.player.GetVelocity();
        const glm::vec3 cam = ctx.camera.GetPosition();
        const Scene* scene = ctx.CurrentScene();
        const Physics::Stats physics = Physics::GetStats();
        const RenderStats render = GetRenderStats();
        const Render::ShaderCacheStats shaders = Render::GetShaderCacheStats();

        std::vector<Line> lines;
        lines.push_back({ GREEN, "PHISIKS" });
        lines.push_back({ WHITE, Format("FPS %.0f   CPU frame %.1f ms   GPU frame %.1f ms", s.cpuFrameMs > 0 ? 1000.0 / s.cpuFrameMs : 0.0, s.cpuFrameMs, s.gpuFrameMs) });
        lines.push_back({ WHITE, Format("CPU  update %.2f   physics %.2f   render %.2f ms", s.cpuUpdateMs, s.cpuPhysicsMs, s.cpuRenderMs) });
        lines.push_back({ WHITE, Format("Fixed steps: %.0f this frame, %.0f total   Sim time %.1f s", s.fixedStepsThisFrame, double(s.totalFixedSteps), s.simulationTime) });
        lines.push_back({ WHITE, "State: " + std::string(ToString(ctx.state)) + "   Scene: " + (ctx.scenes ? ctx.scenes->CurrentName() : std::string("-")) });
        lines.push_back({ WHITE, Format("Entities %.0f   Bodies: dynamic %.0f  static %.0f", double(scene ? scene->EntityCount() : 0), double(physics.dynamicBodies), double(physics.staticBodies)) });
        lines.push_back({ WHITE, Format("Render: %.0f drawn  %.0f culled  %.0f draw cmds  %.0f lights", render.instancesDrawn, render.instancesCulled, render.drawCommands, render.lights) });
        lines.push_back({ WHITE, Format("        %.0f tris  %.0f shadow views   shaders %.0f cached / %.0f compiled", render.trianglesDrawn, render.shadowViews, shaders.loadedFromCache, shaders.compiled) });
        lines.push_back({ WHITE, Format("Player feet  %.2f %.2f %.2f", feet.x, feet.y, feet.z) + (ctx.player.IsGrounded() ? "  grounded" : "  airborne") });
        lines.push_back({ WHITE, Format("Player vel   %.2f %.2f %.2f", vel.x, vel.y, vel.z) });
        lines.push_back({ WHITE, "Camera: " + std::string(ToString(ctx.camera.GetMode())) + Format("  pos %.1f %.1f %.1f", cam.x, cam.y, cam.z) });
        lines.push_back({ WHITE, " " });
        for (const std::string& event : ctx.eventLog) lines.push_back({ GREY, event });
        lines.push_back({ WHITE, " " });
        lines.push_back({ GREY, "WASD move  SHIFT sprint  SPACE jump  MOUSE look  E interact  LMB push/damage" });
        lines.push_back({ GREY, "V camera  F1 overlay  F2 physics debug  R reload  N next scene  ESC pause" });
        Lines(lines, 16.0f, 12.0f);
        g_telemetryLines = int(lines.size());
    }

    int LastTelemetryLineCount() { return g_telemetryLines; }

    void DrawPhysicsDebug(GameContext& ctx) {
        if (Scene* scene = ctx.CurrentScene()) {
            scene->Each<Collider>([](Entity entity, Collider&) {
                const Physics::BodyId body = entity.GetBody();
                if (!body.Valid()) return;
                glm::vec3 lo, hi;
                Physics::GetBounds(body, lo, hi);
                const bool dynamic = Physics::GetBodyType(body) == Physics::BodyType::Dynamic;
                const glm::vec4 color = !dynamic ? glm::vec4(0.3f, 0.6f, 1.0f, 1.0f)
                                      : Physics::IsSleeping(body) ? glm::vec4(0.5f, 0.5f, 0.5f, 1.0f) : glm::vec4(0.2f, 1.0f, 0.2f, 1.0f);
                Render::DrawAABB(lo, hi, color);
            });
        }

        if (ctx.player.IsSpawned()) {
            glm::vec3 lo, hi;
            Physics::GetCharacterBounds(ctx.player.GetCharacter(), lo, hi);
            Render::DrawAABB(lo, hi, glm::vec4(1.0f, 0.8f, 0.2f, 1.0f));
        }

        const TraceHit hit = Gameplay::TraceFromCamera(ctx, 6.0f);
        if (hit.hit) {
            Render::DrawCross(hit.point, 0.08f, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
            Render::DrawLine(hit.point, hit.point + hit.normal * 0.3f, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
        }
    }
}
