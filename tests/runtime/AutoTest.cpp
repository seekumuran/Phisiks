#include "AutoTest.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "SandboxScenes.h"
#include "FieldLab.h"
#include "SceneBuilder.h"
#include "P9Scenes.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Framework/DebugOverlay.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/UI/RetroDeck.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"
#include "Phisiks/Compute/Compute.h"
#include "Phisiks/Framework/Replay.h"
#include "Phisiks/Render/Upscaler.h"
#include "Phisiks/Scene/SceneSerializer.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/Physics/Advanced.h"
#include "Phisiks/Physics/Fields.h"
#include "Phisiks/Physics/Xpbd.h"
#include "Phisiks/Render/Renderer.h"
#include <glm/gtc/constants.hpp>

#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Audio/Audio.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <sstream>

namespace Phisiks {
    namespace {
        std::string F(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), fmt, a, b, c, d);
            return buffer;
        }
        std::string V(const glm::vec3& v) { return F("(%.3f, %.3f, %.3f)", v.x, v.y, v.z); }

        using namespace Lab::Layout;
        using Lab::PhysicsTowerScene;

        size_t EntityCount(GameContext& ctx) { return ctx.CurrentScene() ? ctx.CurrentScene()->EntityCount() : 0; }

        // Entities with a static physics body (Collider without Rigidbody).
        size_t CountStaticEntities(GameContext& ctx) {
            size_t count = 0;
            if (Scene* scene = ctx.CurrentScene()) {
                scene->Each<Collider>([&](Entity e, Collider&) { if (!e.Has<Rigidbody>() && e.GetBody().Valid()) ++count; });
            }
            return count;
        }

        size_t CountRenderedEntities(GameContext& ctx) {
            size_t count = 0;
            if (Scene* scene = ctx.CurrentScene()) scene->Each<MeshRenderer>([&](Entity, MeshRenderer&) { ++count; });
            return count;
        }

        Entity Find(GameContext& ctx, const std::string& name) {
            return ctx.CurrentScene() ? ctx.CurrentScene()->FindByName(name) : Entity();
        }

        // ---- pixel helpers (capture rows are bottom-up; ids are Entity::Id() + 1, 0 = none)
        uint32_t PixelId(const Entity& e) { return e.Valid() ? e.Id() + 1 : 0xFFFFFFFFu; }
        double Luma(const Render::FrameCapture& c, size_t i) {
            return 0.2126 * c.rgba[i * 4] + 0.7152 * c.rgba[i * 4 + 1] + 0.0722 * c.rgba[i * 4 + 2];
        }
        size_t CountId(const Render::FrameCapture& c, uint32_t id) {
            size_t n = 0;
            for (uint32_t v : c.entityIds) n += v == id;
            return n;
        }
        glm::dvec3 MeanColor(const Render::FrameCapture& c, uint32_t id) {
            glm::dvec3 sum(0.0);
            size_t n = 0;
            for (size_t i = 0; i < c.entityIds.size(); ++i) {
                if (c.entityIds[i] != id) continue;
                sum += glm::dvec3(c.rgba[i * 4], c.rgba[i * 4 + 1], c.rgba[i * 4 + 2]);
                ++n;
            }
            return n ? sum / double(n) : glm::dvec3(0.0);
        }
        double LumaStdDev(const Render::FrameCapture& c, uint32_t id, bool all = false) {
            double sum = 0.0, sum2 = 0.0;
            size_t n = 0;
            for (size_t i = 0; i < c.entityIds.size(); ++i) {
                if (!all && c.entityIds[i] != id) continue;
                const double l = Luma(c, i);
                sum += l;
                sum2 += l * l;
                ++n;
            }
            if (n < 2) return 0.0;
            const double mean = sum / n;
            return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
        }
        uint32_t CenterId(const Render::FrameCapture& c) {
            return c.entityIds.empty() ? 0 : c.entityIds[size_t(c.height / 2) * c.width + c.width / 2];
        }

        void AimAt(GameContext& ctx, const glm::vec3& target) {
            ctx.camera.LookAt(ctx.player.GetEyePosition(), target);
        }

        glm::vec3 EntityPosition(GameContext& ctx, const std::string& name) {
            const Entity e = Find(ctx, name);
            return e.Valid() ? e.WorldPosition() : glm::vec3(NAN);
        }
    }

    AutoTest::AutoTest(std::string runDirectory) : m_runDirectory(std::move(runDirectory)) {
        BuildSteps();
        Log::Info() << "AutoTest armed with " << m_steps.size() << " steps";
    }

    double AutoTest::StepSimTime(const GameContext& ctx) const {
        return ctx.stats.simulationTime - m_stepStartSim;
    }

    void AutoTest::Expect(const std::string& name, bool condition, const std::string& detail) {
        const std::string step = m_stepIndex < m_steps.size() ? m_steps[m_stepIndex].name : "Shutdown";
        m_checks.push_back({ step, name, condition, detail });
        if (condition) Log::Info() << "[autotest] PASS " << step << " / " << name << " : " << detail;
        else           Log::Error() << "[autotest] FAIL " << step << " / " << name << " : " << detail;
    }

    void AutoTest::BeforeInput(GameContext& ctx) {
        if (m_finished || m_stepIndex >= m_steps.size()) return;
        if (!m_stepStarted) {
            m_stepStarted = true;
            m_stepStartSim = ctx.stats.simulationTime;
            m_stepStartFrame = ctx.stats.frameIndex;
            m_phase = 0;
            ctx.input.ClearInjected();
            Log::Info() << "[autotest] step " << (m_stepIndex + 1) << "/" << m_steps.size() << ": " << m_steps[m_stepIndex].name;
        }
    }

    void AutoTest::AfterSimulation(GameContext& ctx) {
        if (m_finished || !m_stepStarted || m_stepIndex >= m_steps.size()) return;
        m_stepFrames = ctx.stats.frameIndex - m_stepStartFrame;

        Step& step = m_steps[m_stepIndex];
        bool done = step.run(ctx);
        if (!done && (StepSimTime(ctx) > step.timeoutSimSeconds || m_stepFrames > 2000)) {
            Expect("completed", false, F("timed out after %.1f s sim / %.0f frames (phase %.0f)", StepSimTime(ctx), double(m_stepFrames), m_phase));
            done = true;
        }
        if (done) {
            ctx.input.ClearInjected();
            m_stepStarted = false;
            if (++m_stepIndex >= m_steps.size()) {
                Log::Info() << "[autotest] all steps finished";
                m_finished = true;
            }
        }
    }

    void AutoTest::AfterRender(GameContext& /*ctx*/) {
        if (m_backbufferRequested) {   // after the studio UI was drawn: what the window shows
            m_backbufferRequested = false;
            m_backbufferReady = Render::CaptureBackbuffer(m_backbuffer);
        }
        if (m_captureRequested) {
            m_captureRequested = false;
            m_captureReady = Render::CaptureFrame(m_capture);
            if (!m_captureReady) Log::Error() << "[autotest] frame capture failed";
        }
        for (const std::string& name : m_pendingScreenshots) {
            const std::string path = m_runDirectory + "/" + name + ".png";
            if (CaptureScreenshot(path)) m_writtenScreenshots.push_back(path);
            else Log::Error() << "[autotest] screenshot failed: " << path;
        }
        m_pendingScreenshots.clear();
    }

    void AutoTest::AfterUnload(GameContext& ctx) {
        m_stepIndex = m_steps.size();
        Expect("scene unload releases entities", ctx.CurrentScene() == nullptr, ctx.CurrentScene() ? "scene still loaded" : "no scene");
        Expect("no OpenGL errors during the run", Render::GetGLErrorCount() == 0, F("%.0f GL_DEBUG_TYPE_ERROR messages", Render::GetGLErrorCount()));
        Expect("scene unload releases rigid dynamics", int(Physics::GetStats().dynamicBodies) == 0, F("rigid dynamics %.0f", int(Physics::GetStats().dynamicBodies)));
        Expect("scene unload releases character controller", int(Physics::GetStats().characters) == 0, F("character controllers %.0f", int(Physics::GetStats().characters)));
        const RenderStats render = GetRenderStats();
        Expect("scene unload releases render objects", render.meshObjects == 0 && render.lights == 0 && render.planes == 0,
               F("meshes %.0f, lights %.0f, planes %.0f", render.meshObjects, render.lights, render.planes));
    }

    int AutoTest::ExitCode() const {
        int failures = 0;
        for (const Check& c : m_checks) failures += c.passed ? 0 : 1;
        if (!m_finished) failures += 1;
        return failures;
    }

    void AutoTest::WriteReport() const {
        int passed = 0, failed = 0;
        for (const Check& c : m_checks) (c.passed ? passed : failed)++;
        std::ofstream out(m_runDirectory + "/autotest_report.txt");
        out << "Phisiks PhisiksLab - automated runtime verification report\n";
        out << "Checks: " << passed << " passed, " << failed << " failed" << (m_finished ? "" : "  (SEQUENCE DID NOT FINISH)") << "\n\n";
        std::string lastStep;
        for (const Check& c : m_checks) {
            if (c.step != lastStep) { out << "\n== " << c.step << "\n"; lastStep = c.step; }
            out << (c.passed ? "  PASS  " : "  FAIL  ") << c.name << "  --  " << c.detail << "\n";
        }
        out << "\nScreenshots:\n";
        for (const std::string& s : m_writtenScreenshots) out << "  " << s << "\n";
        Log::Info() << "[autotest] report written: " << passed << " passed, " << failed << " failed";
    }

    void AutoTest::BuildSteps() {
        // 1. Boot: scene loaded, camera bound to renderer, baseline resource counts.
        m_steps.push_back({ "Boot into Sandbox", [this](GameContext& ctx) {
            if (ctx.scenes->CurrentName() != "Sandbox" || m_stepFrames < 5) return false;
            Expect("scene active", true, "Sandbox");
            Expect("entities created", EntityCount(ctx) >= 12, F("%.0f entities", double(EntityCount(ctx))));
            Expect("scene camera drives renderer", GetRenderStats().cameraBound && Find(ctx, "MainCamera").Has<Camera>(), "MainCamera entity bound");
            Expect("player character controller exists", ctx.player.IsSpawned() && int(Physics::GetStats().characters) == 1,
                   F("character controllers %.0f", int(Physics::GetStats().characters)));
            const RenderStats render = GetRenderStats();
            Expect("render objects match MeshRenderer entities", render.meshObjects == CountRenderedEntities(ctx),
                   F("render objects %.0f, MeshRenderer entities %.0f", render.meshObjects, double(CountRenderedEntities(ctx))));
            Expect("ground plane created", render.planes == 1, F("planes %.0f", render.planes));
            m_baseline.entities = EntityCount(ctx);
            m_baseline.rigidDynamics = int(Physics::GetStats().dynamicBodies);
            m_baseline.rigidStatics = int(Physics::GetStats().staticBodies);
            m_baseline.gameObjects = render.meshObjects;
            m_baseline.lights = render.lights;
            m_baseline.characterControllers = int(Physics::GetStats().characters);
            m_scratch[7] = float(m_baseline.rigidStatics - int(CountStaticEntities(ctx)));  // statics not owned by entities
            Screenshot("01_sandbox_boot");
            return true;
        } });

        // 1b. Pixel-level verification of the first rendered view (entity-id buffer + final image).
        m_steps.push_back({ "Pixels: boot view", [this](GameContext& ctx) {
            if (m_phase == 0) {
                // Textures stream in on the asset worker; wait for them (bounded) before looking.
                const RenderStats r = GetRenderStats();
                if ((r.texturesPending > 0 || r.texturesResident < 2) && m_stepFrames < 600) return false;
                RequestCapture();
                m_phase = 1;
                return false;
            }
            if (!m_captureReady) return false;
            const Render::FrameCapture& c = m_capture;
            const double pixels = double(c.entityIds.size());
            Expect("frame is not blank", LumaStdDev(c, 0, true) > 10.0, F("luma std dev %.1f", LumaStdDev(c, 0, true)));

            const uint32_t ground = PixelId(Find(ctx, "Ground"));
            const double groundShare = CountId(c, ground) / pixels;
            Expect("ground plane covers the lower view", groundShare > 0.1, F("%.1f%% of pixels", groundShare * 100.0));
            Expect("ground grid pattern rendered", LumaStdDev(c, ground) > 6.0, F("ground luma std dev %.1f", LumaStdDev(c, ground)));

            const uint32_t wall = PixelId(Find(ctx, "Wall"));
            const glm::dvec3 wallColor = MeanColor(c, wall);
            const double wallSpread = std::max({ wallColor.r, wallColor.g, wallColor.b }) - std::min({ wallColor.r, wallColor.g, wallColor.b });
            (void)wallSpread;
            Expect("wall drawn with its brick texture (red-brown, textured)",
                   CountId(c, wall) / pixels > 0.005 && wallColor.r > 1.25 * wallColor.g && wallColor.r > 1.25 * wallColor.b && LumaStdDev(c, wall) > 4.0,
                   F("%.1f%% of pixels, mean colour (%.0f, %.0f, ", CountId(c, wall) / pixels * 100.0, wallColor.r, wallColor.g) +
                   F("%.0f), luma std dev %.1f", wallColor.b, LumaStdDev(c, wall)));

            const RenderStats assetsRender = GetRenderStats();
            const Assets::Stats assetStats = Assets::GetStats();
            Expect("textures streamed in on demand", assetsRender.texturesResident == 2 && assetsRender.texturesPending == 0 && assetStats.failures == 0,
                   F("%.0f resident, %.0f pending, %.0f decode failures", assetsRender.texturesResident, assetsRender.texturesPending, assetStats.failures));
            const size_t torusPixels = CountId(c, PixelId(Find(ctx, "Torus")));
            Expect("OBJ mesh loaded on demand and drawn", assetsRender.meshesLoaded == 4 && torusPixels > 100,
                   F("%.0f meshes registered, torus %.0f pixels", assetsRender.meshesLoaded, double(torusPixels)));

            size_t unknown = 0;
            std::set<uint32_t> ids;
            for (uint32_t id : c.entityIds) if (id) ids.insert(id);
            for (uint32_t id : ids) if (!ctx.CurrentScene()->FromId(id - 1).Valid()) ++unknown;
            Expect("every id in the id buffer belongs to a live entity", unknown == 0 && ids.size() >= 5, F("%.0f distinct ids, %.0f unknown", double(ids.size()), double(unknown)));

            const Render::ShaderCacheStats cache = Render::GetShaderCacheStats();
            const bool compat = Render::GetRendererInfo().compatTier;
            Expect("shader programs built (cache or compile), none failed",
                   cache.programs == (compat ? 24u : 41u) && cache.failures == 0 && cache.loadedFromCache + cache.compiled == cache.programs && cache.written == cache.compiled,
                   F("%.0f programs: %.0f from cache, %.0f compiled, ", cache.programs, cache.loadedFromCache, cache.compiled) + F("%.0f written, %.0f failed", cache.written, cache.failures));
            const RenderStats render = GetRenderStats();
            Expect("indirect drawing: at most one command per mesh type", render.drawCommands <= render.meshesLoaded && render.instancesDrawn >= 10,
                   F("%.0f commands for %.0f instances (%.0f culled, ", render.drawCommands, render.instancesDrawn, render.instancesCulled) +
                   F("%.0f mesh types)", render.meshesLoaded));
            return true;
        } });

        // 2. Gravity and resting contact.
        m_steps.push_back({ "Gravity and resting contact", [this](GameContext& ctx) {
            if (StepSimTime(ctx) < 3.0) return false;
            const glm::vec3 box = EntityPosition(ctx, "FallingBox");
            const float speed = glm::length(Find(ctx, "FallingBox").GetLinearVelocity());
            Expect("falling box rests on ground", std::abs(box.y - FALLING_BOX_SIZE * 0.5f) < 0.05f && speed < 0.1f,
                   "pos " + V(box) + F(" expected y %.2f, speed %.3f", FALLING_BOX_SIZE * 0.5, speed));
            const glm::vec3 top = EntityPosition(ctx, "Stack_" + std::to_string(STACK_COUNT - 1));
            Expect("box stack is stable", std::abs(top.y - (0.3f + (STACK_COUNT - 1) * 0.6f)) < 0.1f, "top " + V(top));
            const glm::vec3 ball = EntityPosition(ctx, "Ball_0");
            Expect("sphere rests on ground", std::abs(ball.y - 0.3f) < 0.05f, "pos " + V(ball));
            const glm::vec3 feet = ctx.player.GetFeetPosition();
            Expect("player grounded on ground plane", ctx.player.IsGrounded() && feet.y > -0.05f && feet.y < 0.1f, "feet " + V(feet));
            Screenshot("02_settled");
            return true;
        } });

        // 3. Walking.
        m_steps.push_back({ "Player movement", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.player.Teleport(PLAYER_SPAWN); ctx.camera.SetYawPitch(0.0f, -0.1f); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
                    { const glm::vec3 p = ctx.player.GetFeetPosition(); m_scratch[1] = p.x; m_scratch[2] = p.z; }
                    m_scratch[0] = float(StepSimTime(ctx));
                    ctx.input.InjectHold(Action::MoveForward, true);
                    m_phase = 2; return false;
                case 2:
                    if (StepSimTime(ctx) - m_scratch[0] < 1.0) return false;
                    ctx.input.InjectHold(Action::MoveForward, false);
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 3; return false;
                default: {
                    if (StepSimTime(ctx) - m_scratch[0] < 0.4) return false;
                    const glm::vec3 p = ctx.player.GetFeetPosition();
                    const float dx = p.x - m_scratch[1], dz = p.z - m_scratch[2];
                    Expect("forward input moves player along -Z (yaw 0)", dz < -2.0f && std::abs(dx) < 0.2f, F("moved dx %.3f dz %.3f in 1.0 s (walk 3.5 m/s)", dx, dz));
                    Expect("player stops when input released", glm::length(glm::vec2(ctx.player.GetVelocity().x, ctx.player.GetVelocity().z)) < 0.05f,
                           "velocity " + V(ctx.player.GetVelocity()));
                    Screenshot("03_after_walk");
                    return true;
                }
            }
        } });

        // 4. Character controller vs static wall.
        m_steps.push_back({ "Collision: player vs wall", [this](GameContext& ctx) {
            const float wallFace = WALL_CENTER.z + WALL_SIZE.z * 0.5f;
            switch (m_phase) {
                case 0: ctx.player.Teleport(glm::vec3(0.0f, 0.05f, -5.0f)); ctx.camera.SetYawPitch(0.0f, 0.0f); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.3) return false;
                    ctx.input.InjectHold(Action::MoveForward, true);
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 2; return false;
                default: {
                    if (StepSimTime(ctx) - m_scratch[0] < 2.5) return false;
                    const glm::vec3 p = ctx.player.GetFeetPosition();
                    const float radius = ctx.player.GetSettings().capsuleRadius;
                    Expect("player blocked by wall", p.z > wallFace && p.z < wallFace + radius + 0.25f,
                           F("feet z %.3f, wall face z %.3f, capsule radius %.2f (walked 2.5 s toward it)", p.z, wallFace, radius));
                    Screenshot("04_at_wall");
                    ctx.input.InjectHold(Action::MoveForward, false);
                    return true;
                }
            }
        } });

        // 5. Jumping.
        m_steps.push_back({ "Player jump", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.player.Teleport(PLAYER_SPAWN); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
                    Expect("grounded before jump", ctx.player.IsGrounded(), "feet " + V(ctx.player.GetFeetPosition()));
                    m_scratch[1] = ctx.player.GetFeetPosition().y;
                    m_scratch[2] = m_scratch[1];
                    ctx.input.InjectPress(Action::Jump);
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 2; return false;
                default: {
                    m_scratch[2] = std::max(m_scratch[2], ctx.player.GetFeetPosition().y);
                    if (StepSimTime(ctx) - m_scratch[0] < 1.5) return false;
                    const float apex = m_scratch[2] - m_scratch[1];
                    Expect("jump reaches expected height", apex > 0.8f && apex < 1.5f, F("apex %.3f m (v=4.8 m/s, g=9.81 -> ~1.17 m)", apex));
                    Expect("lands and is grounded again", ctx.player.IsGrounded() && ctx.player.GetFeetPosition().y < 0.1f, "feet " + V(ctx.player.GetFeetPosition()));
                    return true;
                }
            }
        } });

        // 6. Camera controllers.
        m_steps.push_back({ "Camera modes", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.camera.SetMode(CameraMode::FirstPerson); ctx.camera.SetYawPitch(0.3f, -0.1f); m_scratch[0] = float(m_stepFrames); m_phase = 1; return false;
                case 1: {
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    const float d = glm::length(ctx.camera.GetPosition() - ctx.player.GetEyePosition());
                    Expect("first person camera at eye", d < 0.01f, F("distance to eye %.4f m", d));
                    Entity body = ctx.player.GetBodyEntity();
                    Expect("player body hidden in first person", body.Valid() && !body.Get<MeshRenderer>().visible, "PlayerBody");
                    RequestCapture();
                    m_phase = 10; return false;
                }
                case 10: {
                    if (!m_captureReady) return false;
                    const size_t bodyPixels = CountId(m_capture, PixelId(ctx.player.GetBodyEntity()));
                    Expect("pixels: no player body pixels in first person", bodyPixels == 0, F("%.0f PlayerBody pixels", double(bodyPixels)));
                    ctx.input.InjectPress(Action::CycleCamera);
                    m_scratch[0] = float(m_stepFrames); m_phase = 2; return false;
                }
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    const glm::vec3 pivot = ctx.player.GetFeetPosition() + glm::vec3(0.0f, ctx.camera.Settings().thirdPersonPivotHeight, 0.0f);
                    const glm::vec3 offset = ctx.camera.GetPosition() - pivot;
                    const float d = glm::length(offset);
                    Expect("third person mode selected via input", ctx.camera.GetMode() == CameraMode::ThirdPerson, ToString(ctx.camera.GetMode()));
                    Expect("third person camera orbits behind player", d > 0.5f && d < ctx.camera.Settings().thirdPersonDistance + 0.01f && glm::dot(offset, ctx.camera.GetForward()) < 0.0f,
                           F("distance %.3f m (max %.2f)", d, ctx.camera.Settings().thirdPersonDistance));
                    {
                        Entity body = ctx.player.GetBodyEntity();
                        const glm::vec3 feet = ctx.player.GetFeetPosition();
                        const glm::vec3 bodyPos = body.Valid() ? body.WorldPosition() : glm::vec3(NAN);
                        Expect("player body visible in third person and follows controller",
                               body.Valid() && body.Get<MeshRenderer>().visible && std::abs(bodyPos.x - feet.x) < 0.01f && std::abs(bodyPos.z - feet.z) < 0.01f,
                               body.Valid() ? "body " + V(bodyPos) + " feet " + V(feet) : std::string("no body entity"));
                    }
                    Screenshot("05_third_person");
                    RequestCapture();
                    m_phase = 11; return false;
                }
                case 11: {
                    if (!m_captureReady) return false;
                    const double share = double(CountId(m_capture, PixelId(ctx.player.GetBodyEntity()))) / double(m_capture.entityIds.size());
                    Expect("pixels: player body visible in third person", share > 0.01, F("%.2f%% of pixels", share * 100.0));
                    ctx.input.InjectPress(Action::CycleCamera);
                    m_scratch[0] = float(m_stepFrames); m_phase = 3; return false;
                }
                case 3: {
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    Expect("free fly mode selected via input", ctx.camera.GetMode() == CameraMode::FreeFly, ToString(ctx.camera.GetMode()));
                    const glm::vec3 cam = ctx.camera.GetPosition(), feet = ctx.player.GetFeetPosition();
                    m_scratch[1] = cam.x; m_scratch[2] = cam.y; m_scratch[3] = cam.z;
                    m_scratch[4] = feet.x; m_scratch[5] = feet.z;
                    ctx.input.InjectHold(Action::MoveForward, true);
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 4; return false;
                }
                case 4: {
                    if (StepSimTime(ctx) - m_scratch[0] < 0.6) return false;
                    ctx.input.InjectHold(Action::MoveForward, false);
                    const float camMoved = glm::length(ctx.camera.GetPosition() - glm::vec3(m_scratch[1], m_scratch[2], m_scratch[3]));
                    const glm::vec3 feet = ctx.player.GetFeetPosition();
                    const float feetMoved = glm::length(glm::vec2(feet.x - m_scratch[4], feet.z - m_scratch[5]));
                    Expect("free fly moves camera, not the player", camMoved > 1.0f && feetMoved < 0.05f, F("camera moved %.2f m, player moved %.3f m", camMoved, feetMoved));
                    ctx.input.InjectPress(Action::CycleCamera);
                    m_scratch[0] = float(m_stepFrames); m_phase = 5; return false;
                }
                default:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    Expect("cycles back to first person", ctx.camera.GetMode() == CameraMode::FirstPerson, ToString(ctx.camera.GetMode()));
                    return true;
            }
        } });

        // 7. Interaction trace hits the right entity.
        m_steps.push_back({ "Interaction raycast", [this](GameContext& ctx) {
            const Entity target = Find(ctx, "TargetBox");
            switch (m_phase) {
                case 0: ctx.player.Teleport(TARGET_CENTER + glm::vec3(0.0f, -TARGET_SIZE * 0.5f + 0.05f, 3.0f)); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.4) return false;
                    AimAt(ctx, EntityPosition(ctx, "TargetBox"));
                    m_scratch[0] = float(m_stepFrames); m_phase = 2; return false;
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    const TraceHit hit = Gameplay::TraceFromCamera(ctx, 6.0f);
                    Expect("camera ray hits TargetBox entity", hit.hit && target.Valid() && hit.entity == target,
                           std::string("hit ") + (hit.entity.Valid() ? hit.entity.GetName() : std::string(hit.hit ? "<non-entity>" : "<nothing>")) + F(" at %.2f m", hit.distance));
                    Screenshot("06_aim_target");
                    RequestCapture();
                    m_phase = 3; return false;
                }
                case 3: {
                    if (!m_captureReady) return false;
                    const uint32_t center = CenterId(m_capture);
                    Expect("pixels: crosshair pixel belongs to TargetBox", target.Valid() && center == PixelId(target),
                           center ? "centre pixel entity " + ctx.CurrentScene()->FromId(center - 1).GetName() : std::string("centre pixel is background"));
                    const glm::dvec3 c = MeanColor(m_capture, PixelId(target));
                    Expect("pixels: TargetBox drawn with its red material", c.r > 1.5 * c.g && c.r > 1.5 * c.b && c.r > 40.0,
                           F("mean colour (%.0f, %.0f, %.0f)", c.r, c.g, c.b));
                    return true;
                }
            }
            return false;
        } });

        // 8. Push + damage until destroyed (4 x 25 on 100 HP).
        m_steps.push_back({ "Push, damage and destruction", [this](GameContext& ctx) {
            Entity target = Find(ctx, "TargetBox");
            const int hitsDone = int(m_scratch[6]);
            switch (m_phase) {
                case 0:
                    m_scratch[6] = 0;
                    m_scratch[4] = float(EntityCount(ctx));
                    m_scratch[5] = float(int(Physics::GetStats().dynamicBodies));
                    m_scratch[3] = float(GetRenderStats().meshObjects);
                    m_scratchId = PixelId(target);
                    m_phase = 1; return false;
                case 1: {   // position player 2.5 m from the target, aim
                    const glm::vec3 t = EntityPosition(ctx, "TargetBox");
                    ctx.player.Teleport(glm::vec3(t.x, 0.05f, t.z + 2.5f));
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 2; return false;
                }
                case 2: {
                    if (StepSimTime(ctx) - m_scratch[0] < 0.3) return false;
                    const glm::vec3 t = EntityPosition(ctx, "TargetBox");
                    AimAt(ctx, t);
                    m_scratch[1] = t.x; m_scratch[2] = t.z;
                    ctx.input.InjectPress(Action::Primary);
                    m_scratch[0] = float(StepSimTime(ctx)); m_phase = 3; return false;
                }
                case 3: {
                    if (StepSimTime(ctx) - m_scratch[0] < 0.8) return false;
                    m_scratch[6] = float(hitsDone + 1);
                    if (hitsDone + 1 < 4) {
                        const Health* health = target.TryGet<Health>();
                        const float expected = 100.0f - 25.0f * (hitsDone + 1);
                        Expect(F("hit %.0f applies 25 damage", hitsDone + 1), health && std::abs(health->current - expected) < 0.01f,
                               health ? F("health %.0f (expected %.0f)", health->current, expected) : std::string("target missing"));
                        if (hitsDone == 0 && health) {
                            const glm::vec3 p = target.WorldPosition();
                            const float moved = glm::length(glm::vec2(p.x - m_scratch[1], p.z - m_scratch[2]));
                            // Re-derived for the realistic default material (mu_d = 0.5). The 40 N*s push is
                            // horizontal plus 0.2 lift (8 N*s): on the 12 kg target v0 = 3.33 m/s, vy = 0.67 m/s.
                            //   no hop:   slides d = v0^2 / (2 mu_d g)                 = 1.13 m (stops in 0.68 s)
                            //   full hop: airborne 2 vy / g = 0.136 s (no friction) adds v0 * 0.136 = 0.45 m -> 1.59 m
                            // The measured distance must lie between these bounds (+-10%).
                            // With the earlier unrealistic 7.0/0.85 friction it moved only ~0.29 m.
                            const double v0 = 40.0 / 12.0, vy = 8.0 / 12.0, g = 9.81, mu = 0.5;
                            const double slideOnly = v0 * v0 / (2.0 * mu * g);
                            const double withHop = slideOnly + v0 * (2.0 * vy / g);
                            Expect("impulse slide distance within analytic bounds", moved > 0.9 * slideOnly && moved < 1.1 * withHop,
                                   F("moved %.3f m (bounds %.3f .. %.3f m)", moved, slideOnly, withHop));
                        }
                        m_phase = 1; return false;
                    }
                    m_scratch[0] = float(m_stepFrames); m_phase = 4; return false;
                }
                case 4: {
                    if (m_stepFrames - m_scratch[0] < 3) return false;   // physics removal is flushed next frame
                    Expect("entity destroyed at 0 health", !target.Valid(), !target.Valid() ? "TargetBox gone" : "TargetBox still exists");
                    Expect("registry count decremented", EntityCount(ctx) == size_t(m_scratch[4]) - 1, F("%.0f -> %.0f", m_scratch[4], double(EntityCount(ctx))));
                    Expect("rigid dynamic released", int(Physics::GetStats().dynamicBodies) == int(m_scratch[5]) - 1, F("%.0f -> %.0f", m_scratch[5], int(Physics::GetStats().dynamicBodies)));
                    Expect("render object released", GetRenderStats().meshObjects == uint32_t(m_scratch[3]) - 1, F("%.0f -> %.0f", m_scratch[3], GetRenderStats().meshObjects));
                    RequestCapture();
                    m_phase = 5; return false;
                }
                case 5: {
                    if (!m_captureReady) return false;
                    const size_t left = CountId(m_capture, m_scratchId);
                    Expect("pixels: destroyed TargetBox no longer drawn", left == 0, F("%.0f pixels with its id", double(left)));
                    return true;
                }
            }
            return false;
        } });

        // 9. Interaction callback creates an entity that physically lands on the spawner.
        m_steps.push_back({ "Interact: spawner", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.player.Teleport(SPAWNER_CENTER + glm::vec3(0.0f, -0.45f, 2.5f)); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.3) return false;
                    AimAt(ctx, SPAWNER_CENTER);
                    m_scratch[1] = float(EntityCount(ctx));
                    ctx.input.InjectPress(Action::Interact);
                    m_scratch[0] = float(StepSimTime(ctx)); m_scratch[2] = float(m_stepFrames); m_phase = 2; return false;
                case 2: {
                    // Interaction runs this frame and presentation sync at its end: the voice exists from the next frame.
                    if (m_stepFrames - m_scratch[2] < 2) return false;
                    Entity spawner = Find(ctx, "Spawner");
                    const AudioSource* sound = spawner.Valid() ? spawner.TryGet<AudioSource>() : nullptr;
                    Expect("interaction plays spawner AudioSource", sound && sound->voice != 0,
                           sound ? std::string(SPAWNER_SOUND) + F(" voice %.0f, playing %.0f", double(sound->voice), sound->isPlaying ? 1.0 : 0.0) : std::string("no spawner"));
                    const Audio::Stats audio = Audio::GetStats();
                    Expect("Phisiks::Audio mixer running with the generated clip", audio.framesMixed > 0 && audio.clipsLoaded >= 1 && audio.clipLoadFailures == 0,
                           std::string(audio.deviceOpen ? "WASAPI device" : "null device") +
                           F(", %.0f Hz, %.0f frames mixed, %.0f clips", audio.sampleRate, double(audio.framesMixed), audio.clipsLoaded));
                    m_phase = 3; return false;
                }
                default: {
                    if (StepSimTime(ctx) - m_scratch[0] < 2.0) return false;
                    Expect("interaction created one entity", EntityCount(ctx) == size_t(m_scratch[1]) + 1, F("%.0f -> %.0f", m_scratch[1], double(EntityCount(ctx))));
                    Entity spawned;
                    ctx.CurrentScene()->Each<Name>([&](Entity e, Name& n) { if (n.value.rfind("Spawned_", 0) == 0) spawned = e; });
                    const float expectedY = SPAWNER_CENTER.y + 0.5f + 0.25f;
                    const glm::vec3 p = spawned.Valid() ? spawned.WorldPosition() : glm::vec3(NAN);
                    Expect("spawned box lands on spawner", spawned.Valid() && std::abs(p.y - expectedY) < 0.08f,
                           spawned.Valid() ? "pos " + V(p) + F(" expected y %.2f", expectedY) : std::string("not found"));
                    Screenshot("07_spawned");
                    return true;
                }
            }
        } });

        // 9b. Shadows and fog, verified by A/B frames of the same static view.
        m_steps.push_back({ "Pixels: shadows and fog", [this](GameContext& ctx) {
            Render::RenderSettings& settings = Render::Settings();
            const uint32_t ground = PixelId(Find(ctx, "Ground"));
            switch (m_phase) {
                case 0: ctx.player.Teleport(PLAYER_SPAWN); m_scratch[0] = float(StepSimTime(ctx)); m_phase = 1; return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
                    AimAt(ctx, glm::vec3(3.0f, 0.0f, -1.0f));      // box stack and its shadows
                    m_scratch[0] = float(m_stepFrames); m_phase = 2; return false;
                case 2:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    RequestCapture(); m_phase = 3; return false;
                case 3:                                            // A: shadows + fog on
                    if (!m_captureReady) return false;
                    m_reference = m_capture;
                    Screenshot("07b_shadows_fog");
                    settings.shadowsEnabled = false;
                    m_scratch[0] = float(m_stepFrames); m_phase = 4; return false;
                case 4:
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    RequestCapture(); m_phase = 5; return false;
                case 5: {                                          // B: shadows off
                    if (!m_captureReady) return false;
                    settings.shadowsEnabled = true;
                    size_t groundPixels = 0, darkened = 0, brightened = 0;
                    for (size_t i = 0; i < m_capture.entityIds.size(); ++i) {
                        if (m_capture.entityIds[i] != ground || m_reference.entityIds[i] != ground) continue;
                        ++groundPixels;
                        const double on = Luma(m_reference, i), off = Luma(m_capture, i);
                        if (on < 0.75 * off - 2.0) ++darkened;
                        if (on > 1.03 * off + 3.0) ++brightened;
                    }
                    const double share = groundPixels ? double(darkened) / groundPixels : 0.0;
                    Expect("shadows darken parts of the ground", share > 0.005, F("%.2f%% of %.0f ground pixels darker by > 25%%", share * 100.0, double(groundPixels)));
                    Expect("shadows never brighten the ground", brightened <= groundPixels / 500, F("%.0f brighter pixels", double(brightened)));
                    settings.fog.enabled = false;
                    m_scratch[0] = float(m_stepFrames); m_phase = 6; return false;
                }
                case 6:
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    RequestCapture(); m_phase = 7; return false;
                default: {                                         // C: fog off, compared with A
                    if (!m_captureReady) return false;
                    settings.fog.enabled = true;
                    int minRow = m_capture.height, maxRow = -1;
                    for (size_t i = 0; i < m_capture.entityIds.size(); ++i) {
                        if (m_capture.entityIds[i] != ground) continue;
                        const int row = int(i / size_t(m_capture.width));
                        minRow = std::min(minRow, row);
                        maxRow = std::max(maxRow, row);
                    }
                    const int span = std::max(maxRow - minRow, 1);
                    double nearDelta = 0.0, farDelta = 0.0;
                    size_t nearN = 0, farN = 0;
                    for (size_t i = 0; i < m_capture.entityIds.size(); ++i) {
                        if (m_capture.entityIds[i] != ground || m_reference.entityIds[i] != ground) continue;
                        const int row = int(i / size_t(m_capture.width));   // bottom-up: low rows are near
                        double delta = 0.0;
                        for (int ch = 0; ch < 3; ++ch) delta += std::abs(double(m_reference.rgba[i * 4 + ch]) - double(m_capture.rgba[i * 4 + ch]));
                        if (row < minRow + span / 4) { nearDelta += delta; ++nearN; }
                        else if (row > maxRow - span / 4) { farDelta += delta; ++farN; }
                    }
                    nearDelta = nearN ? nearDelta / nearN : 0.0;
                    farDelta = farN ? farDelta / farN : 0.0;
                    Expect("fog changes distant ground more than near ground", farDelta > 2.5 && farDelta > 2.0 * nearDelta,
                           F("mean |delta| far %.1f vs near %.1f (sum of RGB)", farDelta, nearDelta));
                    return true;
                }
            }
        } });

        // 9c. Multi-viewport: perspective + orthographic top/front/side, measured in pixels.
        m_steps.push_back({ "Quad view (multi-viewport, orthographic)", [this](GameContext& ctx) {
            Scene& scene = *ctx.CurrentScene();
            switch (m_phase) {
                case 0: Lab::SetQuadView(scene, true); m_scratch[0] = float(m_stepFrames); m_phase = 1; return false;
                case 1:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    Screenshot("07c_quad_view");
                    RequestCapture(); m_phase = 2; return false;
                case 2: {
                    if (!m_captureReady) return false;
                    const Render::FrameCapture& c = m_capture;
                    Expect("quad view renders four cameras", GetRenderStats().views == 4, F("%.0f views", GetRenderStats().views));
                    // Quadrants in capture coordinates (rows bottom-up): perspective top-left, top view top-right,
                    // front view bottom-left, side view bottom-right.
                    const int hw = c.width / 2, hh = c.height / 2;
                    auto distinctIds = [&](int x0, int y0) {
                        std::set<uint32_t> ids;
                        for (int y = y0; y < y0 + hh; ++y)
                            for (int x = x0; x < x0 + hw; ++x)
                                if (uint32_t id = c.entityIds[size_t(y) * c.width + x]) ids.insert(id);
                        return ids.size();
                    };
                    const size_t persp = distinctIds(0, hh), top = distinctIds(hw, hh), front = distinctIds(0, 0), side = distinctIds(hw, 0);
                    Expect("every viewport shows scene objects", persp >= 3 && top >= 3 && front >= 3 && side >= 3,
                           F("distinct entities: perspective %.0f, top %.0f, front %.0f, ", double(persp), double(top), double(front)) + F("side %.0f", double(side)));

                    // Orthographic scale: pixels per metre = viewport height / ortho height, independent of depth.
                    const uint32_t wall = PixelId(Find(ctx, "Wall"));
                    auto extent = [&](int x0, int y0, bool horizontal) {
                        int lo = 1 << 30, hi = -1;
                        for (int y = y0; y < y0 + hh; ++y)
                            for (int x = x0; x < x0 + hw; ++x)
                                if (c.entityIds[size_t(y) * c.width + x] == wall) {
                                    const int v = horizontal ? x : y;
                                    lo = std::min(lo, v);
                                    hi = std::max(hi, v);
                                }
                        return hi >= lo ? hi - lo + 1 : 0;
                    };
                    const double topWidth = extent(hw, hh, true);
                    const double topExpected = WALL_SIZE.x * hh / QUAD_TOP_ORTHO_HEIGHT;
                    Expect("top view (ortho): wall width matches the projection", std::abs(topWidth - topExpected) <= 0.03 * topExpected + 2.0,
                           F("%.0f px (expected %.1f = 12 m x %.0f px / 24 m)", topWidth, topExpected, double(hh)));
                    const double frontHeight = extent(0, 0, false);
                    const double frontExpected = WALL_SIZE.y * hh / QUAD_SIDE_ORTHO_HEIGHT;
                    Expect("front view (ortho): wall height matches the projection", std::abs(frontHeight - frontExpected) <= 0.03 * frontExpected + 2.0,
                           F("%.0f px (expected %.1f = 3 m x %.0f px / 12 m)", frontHeight, frontExpected, double(hh)));
                    Screenshot("07c2_switch_frame");   // the frame that switches back to one view
                    Lab::SetQuadView(scene, false);
                    m_scratch[0] = float(m_stepFrames); m_phase = 3; return false;
                }
                default:
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    Expect("single view restored", GetRenderStats().views == 1 && !Lab::IsQuadView(scene), F("%.0f views", GetRenderStats().views));
                    Screenshot("07d_single_view_restored");
                    return true;
            }
        } });

        // 9d. CAD-style orbit camera: exact orbit pose, wheel zoom and middle-mouse pan through input actions.
        m_steps.push_back({ "Orbit camera", [this](GameContext& ctx) {
            CameraController& cam = ctx.camera;
            switch (m_phase) {
                case 0:
                    cam.SetOrbit(WALL_CENTER, 10.0f, 0.3f, -0.35f);
                    m_scratch[0] = float(m_stepFrames); m_phase = 1; return false;
                case 1: {
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    const glm::vec3 expected = WALL_CENTER - cam.GetForward() * 10.0f;
                    Expect("orbit pose: camera = target - forward x distance", cam.GetMode() == CameraMode::Orbit && glm::length(cam.GetPosition() - expected) < 1e-4f,
                           "camera " + V(cam.GetPosition()) + " expected " + V(expected));
                    RequestCapture();
                    m_phase = 2; return false;
                }
                case 2: {
                    if (!m_captureReady) return false;
                    const uint32_t center = CenterId(m_capture);
                    Expect("pixels: orbit target (wall) at the screen centre", center == PixelId(Find(ctx, "Wall")),
                           center ? "centre pixel entity " + ctx.CurrentScene()->FromId(center - 1).GetName() : std::string("centre pixel is background"));
                    ctx.input.InjectScroll(2.0f);   // two notches towards the target
                    m_scratch[0] = float(m_stepFrames); m_phase = 3; return false;
                }
                case 3: {
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    const float expected = 10.0f * std::pow(cam.Settings().orbitZoomPerNotch, 2.0f);
                    Expect("wheel zoom scales the orbit distance", std::abs(cam.GetOrbitDistance() - expected) < 1e-4f,
                           F("distance %.4f m (expected %.4f)", cam.GetOrbitDistance(), expected));
                    m_scratch[1] = cam.GetOrbitTarget().x; m_scratch[2] = cam.GetOrbitTarget().y; m_scratch[3] = cam.GetOrbitTarget().z;
                    ctx.input.InjectHold(Action::Pan, true);
                    ctx.input.InjectMouseDelta(glm::vec2(100.0f, 0.0f));
                    m_scratch[0] = float(m_stepFrames); m_phase = 4; return false;
                }
                default: {
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    ctx.input.InjectHold(Action::Pan, false);
                    const glm::vec3 forward = cam.GetForward();
                    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                    const glm::vec3 before(m_scratch[1], m_scratch[2], m_scratch[3]);
                    const glm::vec3 expected = before - right * 100.0f * cam.GetOrbitDistance() * cam.Settings().orbitPanSpeed;
                    Expect("middle-mouse pan moves the target sideways", glm::length(cam.GetOrbitTarget() - expected) < 1e-3f,
                           "target " + V(cam.GetOrbitTarget()) + " expected " + V(expected));
                    cam.SetMode(CameraMode::FirstPerson);
                    return true;
                }
            }
        } });

        // 9e. GPU-driven culling: the compute path must produce exactly the CPU path's image and counts.
        m_steps.push_back({ "GPU culling: compute shader vs CPU (identical frame)", [this](GameContext& ctx) {
            if (Render::GetRendererInfo().compatTier) {
                Expect("4.1 tier: CPU frustum culling (no compute shaders)", !GetRenderStats().gpuCulling && GetRenderStats().compatTier, "GPU culling checks skipped");
                return true;
            }
            Render::RenderSettings& rs = Render::Settings();
            switch (m_phase) {
                case 0:
                    ctx.camera.SetMode(CameraMode::FirstPerson);
                    ctx.player.Teleport(PLAYER_SPAWN);
                    ctx.camera.SetYawPitch(0.0f, -0.1f);
                    rs.gpuCulling = true;
                    rs.occlusionCulling = false;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 1;
                    return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
                    ctx.input.InjectPress(Action::Pause);          // freeze animation: both frames must be identical
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 8) return false;   // GPU counters are read back 2 frames late
                    const RenderStats r = GetRenderStats();
                    m_scratch[1] = float(r.instancesDrawn);
                    m_scratch[2] = float(r.instancesCulled);
                    m_scratch[3] = r.gpuCulling ? 1.0f : 0.0f;
                    RequestCapture();
                    m_phase = 3;
                    return false;
                }
                case 3:
                    if (!m_captureReady) return false;
                    m_reference = m_capture;
                    rs.gpuCulling = false;
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 4;
                    return false;
                case 4:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    RequestCapture();
                    m_phase = 5;
                    return false;
                case 99: return ctx.state == GameState::Running;
                default: {
                    if (!m_captureReady) return false;
                    const RenderStats cpu = GetRenderStats();
                    Expect("GPU culling ran in a compute shader", m_scratch[3] == 1.0f && !cpu.gpuCulling, "gpuCulled flag set with GPU path, clear with CPU path");
                    Expect("GPU visible / culled counts equal the CPU frustum test",
                           double(m_scratch[1]) == double(cpu.instancesDrawn) && double(m_scratch[2]) == double(cpu.instancesCulled) && cpu.instancesDrawn > 0,
                           F("GPU %.0f drawn / %.0f culled, CPU %.0f drawn / ", m_scratch[1], m_scratch[2], cpu.instancesDrawn) + F("%.0f culled", cpu.instancesCulled));
                    size_t differing = 0;
                    const size_t n = std::min(m_capture.entityIds.size(), m_reference.entityIds.size());
                    for (size_t i = 0; i < n; ++i) differing += m_capture.entityIds[i] != m_reference.entityIds[i];
                    Expect("entity-id image identical with GPU and CPU culling", n > 0 && differing == 0,
                           F("%.0f of %.0f pixels differ", double(differing), double(n)));
                    rs.gpuCulling = true;
                    rs.occlusionCulling = true;
                    ctx.input.InjectPress(Action::Pause);   // resume (injected input is cleared when a step ends)
                    m_phase = 99;
                    return false;
                }
            }
        } });

        // 9f. Hi-Z occlusion: a box hidden behind the wall is culled, nothing visible is.
        m_steps.push_back({ "GPU culling: Hi-Z occlusion", [this](GameContext& ctx) {
            if (Render::GetRendererInfo().compatTier) {
                Expect("4.1 tier: no Hi-Z pass", !GetRenderStats().occlusionActive, "occlusion checks skipped");
                return true;
            }
            Render::RenderSettings& rs = Render::Settings();
            Scene& scene = *ctx.CurrentScene();
            switch (m_phase) {
                case 0:
                    ctx.camera.SetMode(CameraMode::FirstPerson);
                    ctx.player.Teleport(glm::vec3(0.0f, 0.05f, -5.0f));   // 2.75 m in front of the wall
                    ctx.camera.SetYawPitch(0.0f, 0.0f);
                    rs.gpuCulling = true;
                    rs.occlusionCulling = true;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 1;
                    return false;
                case 1:
                    if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
                    ctx.input.InjectPress(Action::Pause);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 8) return false;
                    const RenderStats r = GetRenderStats();
                    m_scratch[1] = float(r.occlusionCulled);
                    m_scratch[4] = r.occlusionActive ? 1.0f : 0.0f;
                    // 0.8 m box 3 m behind the wall, entirely below the wall's top edge from this eye point
                    Lab::SceneBuilder::AddStaticBox(scene, "OcclusionProbe", glm::vec3(0.0f, 0.4f, WALL_CENTER.z - 3.0f), glm::vec3(0.8f), "Sandbox/Ground");
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 3;
                    return false;
                }
                case 3: {
                    if (m_stepFrames - m_scratch[0] < 8) return false;
                    m_scratch[2] = float(GetRenderStats().occlusionCulled);
                    RequestCapture();
                    m_phase = 4;
                    return false;
                }
                case 4:
                    if (!m_captureReady) return false;
                    m_reference = m_capture;
                    rs.occlusionCulling = false;
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 5;
                    return false;
                case 5:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    RequestCapture();
                    m_phase = 6;
                    return false;
                case 99:
                    if (ctx.state != GameState::Running) return false;
                    ctx.player.Teleport(PLAYER_SPAWN);
                    return true;
                default: {
                    if (!m_captureReady) return false;
                    const RenderStats off = GetRenderStats();
                    Expect("Hi-Z occlusion active for the window view", m_scratch[4] == 1.0f, "previous-frame depth pyramid valid");
                    Expect("hidden probe box is occlusion-culled (count rises by exactly 1)", m_scratch[2] == m_scratch[1] + 1.0f,
                           F("occlusion-culled %.0f without the probe, %.0f with it", m_scratch[1], m_scratch[2]));
                    const Entity probe = Find(ctx, "OcclusionProbe");
                    Expect("probe is really hidden (0 pixels with occlusion off)", CountId(m_capture, PixelId(probe)) == 0,
                           F("%.0f probe pixels", double(CountId(m_capture, PixelId(probe)))));
                    size_t differing = 0;
                    const size_t n = std::min(m_capture.entityIds.size(), m_reference.entityIds.size());
                    for (size_t i = 0; i < n; ++i) differing += m_capture.entityIds[i] != m_reference.entityIds[i];
                    Expect("occlusion culling removes nothing visible (identical id image)", n > 0 && differing == 0 && !off.occlusionActive,
                           F("%.0f of %.0f pixels differ", double(differing), double(n)));
                    if (probe.Valid()) scene.DestroyImmediate(probe);
                    rs.occlusionCulling = true;
                    ctx.input.InjectPress(Action::Pause);
                    m_phase = 99;
                    return false;
                }
            }
        } });

        // 10. Scene transition releases everything from the previous scene.
        m_steps.push_back({ "Scene transition to PhysicsTower", [this](GameContext& ctx) {
            if (m_phase == 0) { ctx.scenes->Request("PhysicsTower"); m_phase = 1; return false; }
            if (ctx.scenes->CurrentName() != "PhysicsTower") return false;
            if (m_phase == 1) { m_scratch[0] = float(StepSimTime(ctx)); m_phase = 2; return false; }
            if (StepSimTime(ctx) - m_scratch[0] < 1.5) return false;
            // Ground + ground collider + sun + 2 lights + main camera + player body.
            const size_t expectedEntities = 7 + PhysicsTowerScene::TOWER_BOXES + PhysicsTowerScene::SPHERES;
            Expect("tower entity count", EntityCount(ctx) == expectedEntities, F("%.0f (expected %.0f)", double(EntityCount(ctx)), double(expectedEntities)));
            Expect("no rigid dynamics leaked from Sandbox", int(Physics::GetStats().dynamicBodies) == PhysicsTowerScene::TOWER_BOXES + PhysicsTowerScene::SPHERES,
                   F("%.0f (expected %.0f)", int(Physics::GetStats().dynamicBodies), double(PhysicsTowerScene::TOWER_BOXES + PhysicsTowerScene::SPHERES)));
            Expect("no rigid statics leaked from Sandbox", int(Physics::GetStats().staticBodies) == int(CountStaticEntities(ctx)) + int(m_scratch[7]),
                   F("%.0f (entity statics %.0f + engine %.0f)", int(Physics::GetStats().staticBodies), double(CountStaticEntities(ctx)), m_scratch[7]));
            const RenderStats render = GetRenderStats();
            Expect("render objects match", render.meshObjects == CountRenderedEntities(ctx), F("%.0f render objects, %.0f MeshRenderer entities", render.meshObjects, double(CountRenderedEntities(ctx))));
            Expect("exactly one ground plane (previous scene's released)", render.planes == 1, F("planes %.0f", render.planes));
            Expect("one character controller", int(Physics::GetStats().characters) == 1, F("%.0f", int(Physics::GetStats().characters)));
            Screenshot("08_physics_tower");
            return true;
        } });

        // 10b. FieldLab: GPU particles, XPBD cloth / rope / soft box, force fields on rigid bodies.
        {
            auto snapshot = std::make_shared<std::vector<glm::vec4>>();
            m_steps.push_back({ "FieldLab: GPU particles, XPBD and force fields", [this, snapshot](GameContext& ctx) {
                using namespace Lab::FieldLayout;
                namespace Adv = Physics::Advanced;
                switch (m_phase) {
                    case 0: ctx.scenes->Request("FieldLab"); m_phase = 1; return false;
                    case 1:
                        if (ctx.scenes->CurrentName() != "FieldLab") return false;
                        m_scratch[0] = float(StepSimTime(ctx));
                        m_phase = 2;
                        return false;
                    case 2: {
                        if (StepSimTime(ctx) - m_scratch[0] < 2.0) return false;
                        const Adv::Stats s = Adv::GetStats();
                        const bool compat = Render::GetRendererInfo().compatTier;
                        Expect("compute backend matches the renderer tier", compat ? (s.cpuBackend && !s.gpuAvailable) : s.gpuAvailable,
                               compat ? "OpenGL 4.1 tier: CPU worker-pool backend" : "OpenGL 4.6 compute");
                        Expect("GPU systems created", s.particles == PARTICLES && s.particleSystems == 1 && s.softBodies == 4 && s.colorsMax >= 2,
                               F("%.0f particles, %.0f soft bodies, %.0f nodes, ", s.particles, s.softBodies, s.softBodyNodes) +
                               F("%.0f constraints in up to %.0f colours", s.constraints, s.colorsMax));
                        // fused solver: 1 solve + (cloth/box) 1 normals dispatch per soft body; particles: hash count,
                        // 3 scan passes, scatter, step (+ n-body if enabled); far below substeps * colours.
                        Expect(compat ? "CPU physics step ran on the worker pool" : "GPU physics step is a handful of dispatches (fused XPBD)",
                               compat ? (s.dispatchesLastStep == 0 && s.cpuMsLastStep > 0.0f) : (s.dispatchesLastStep > 0 && s.dispatchesLastStep <= 16),
                               F("%.0f dispatches, %.2f ms CPU to record the step", s.dispatchesLastStep, s.cpuMsLastStep));
                        const RenderStats r = GetRenderStats();
                        Expect("particles and rope nodes drawn as sprites, cloth and jelly as meshes",
                               r.particlesDrawn == PARTICLES + 27 && r.softBodiesDrawn == 3,
                               F("%.0f sprites, %.0f soft-body meshes", r.particlesDrawn, r.softBodiesDrawn));

                        // particles: all finite and above the ground
                        Adv::ReadParticles(Find(ctx, "Particles"), *snapshot);
                        bool finite = snapshot->size() == PARTICLES;
                        float minY = 1e9f;
                        for (const glm::vec4& p : *snapshot) { finite = finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); minY = std::min(minY, p.y); }
                        Expect("particles finite and resting on or above the ground", finite && minY >= PARTICLE_RADIUS - 0.005f,
                               F("%.0f particles, lowest centre y %.4f m (radius %.3f)", double(snapshot->size()), minY, PARTICLE_RADIUS));

                        // soft bodies
                        std::vector<glm::vec4> x;
                        auto allFinite = [&] { for (const glm::vec4& p : x) if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false; return !x.empty(); };
                        Adv::ReadSoftBody(Find(ctx, "Curtain"), x);
                        int pins = 0; float pinY = 0.0f;
                        for (const glm::vec4& p : x) if (p.w == 0.0f) { ++pins; pinY = p.y; }
                        Expect("curtain hangs from its two pinned corners", allFinite() && pins == 2 && std::abs(pinY - 3.6f) < 1e-4f,
                               F("%.0f nodes, %.0f pinned at y %.4f", double(x.size()), pins, pinY));
                        Adv::ReadSoftBody(Find(ctx, "Drape"), x);
                        float closest = 1e9f, highest = -1e9f;
                        for (const glm::vec4& p : x) { closest = std::min(closest, glm::length(glm::vec3(p) - DRAPE_SPHERE_CENTER)); highest = std::max(highest, p.y); }
                        Expect("draped cloth rests on the sphere collider without passing through",
                               allFinite() && closest >= DRAPE_SPHERE_RADIUS - 0.01f && closest < DRAPE_SPHERE_RADIUS + 0.1f && highest < DRAPE_SPHERE_CENTER.y + 1.2f,
                               F("closest node %.3f m from centre (sphere radius %.2f), highest y %.2f", closest, DRAPE_SPHERE_RADIUS, highest));
                        Adv::ReadSoftBody(Find(ctx, "Rope"), x);
                        Expect("rope: 27 nodes, first one pinned in place", allFinite() && x.size() == 27 && x[0].w == 0.0f &&
                               glm::length(glm::vec3(x[0]) - glm::vec3(-0.5f, 4.2f, -1.0f)) < 1e-4f,
                               F("%.0f nodes, last node y %.2f", double(x.size()), x.empty() ? 0.0 : x.back().y));
                        Adv::ReadSoftBody(Find(ctx, "Jelly"), x);
                        float lo = 1e9f, hi = -1e9f;
                        for (const glm::vec4& p : x) { lo = std::min(lo, p.y); hi = std::max(hi, p.y); }
                        Expect("soft box landed and kept its shape", allFinite() && lo > -0.01f && lo < 0.1f && hi - lo > 0.7f && hi - lo < 0.9f,
                               F("bottom y %.3f, height %.3f m (rest 0.8)", lo, hi - lo));

                        // rigid bodies in fields
                        double worst = 0.0, minSweep = 1e9;
                        for (int i = 0; i < 3; ++i) {
                            const glm::vec3 d = Find(ctx, "Moon_" + std::to_string(i)).WorldPosition() - PLANET_CENTER;
                            worst = std::max(worst, std::abs(double(glm::length(d)) / MOON_ORBIT - 1.0));
                            const double start = i * glm::two_pi<double>() / 3.0;
                            double sweep = std::abs(std::remainder(start - std::atan2(double(d.z), double(d.x)), glm::two_pi<double>()));
                            minSweep = std::min(minSweep, sweep);
                        }
                        Expect("moons orbit the planetary gravity field", worst < 0.05 && minSweep > 0.5,
                               F("max radius deviation %.2f%%, smallest swept angle %.2f rad", worst * 100.0, minSweep));
                        glm::vec3 centroid(0.0f);
                        for (int i = 0; i < 6; ++i) centroid += Find(ctx, "Cluster_" + std::to_string(i)).WorldPosition() / 6.0f;
                        double spread = 0.0;
                        for (int i = 0; i < 6; ++i) spread += glm::length(Find(ctx, "Cluster_" + std::to_string(i)).WorldPosition() - centroid) / 6.0;
                        Expect("n-body cluster contracts under mutual gravity", spread < 1.55, F("mean distance to centroid %.3f m (started at 1.6)", spread));
                        m_scratch[0] = float(StepSimTime(ctx));
                        m_phase = 3;
                        return false;
                    }
                    case 3: {
                        if (StepSimTime(ctx) - m_scratch[0] < 4.0 / 60.0 - 1e-6) return false;   // 4 fixed steps (same on every tier / frame rate)
                        std::vector<glm::vec4> after;
                        Adv::ReadParticles(Find(ctx, "Particles"), after);
                        size_t inside = 0, ccw = 0;
                        for (size_t i = 0; i < std::min(after.size(), snapshot->size()); ++i) {
                            const glm::vec3 r = glm::vec3((*snapshot)[i]) - VORTEX_CENTER;
                            if (r.x * r.x + r.z * r.z > 2.5f * 2.5f || r.x * r.x + r.z * r.z < 0.04f) continue;
                            ++inside;
                            ccw += glm::cross(r, glm::vec3(after[i]) - glm::vec3((*snapshot)[i])).y > 0.0f;
                        }
                        Expect("vortex field swirls the particles counter-clockwise", inside > 100 && ccw > 0.8 * inside,
                               F("%.0f of %.0f particles near the axis turn counter-clockwise", double(ccw), double(inside)));
                        Screenshot("09_fieldlab");
                        RequestCapture();
                        m_phase = 4;
                        return false;
                    }
                    default: {
                        if (!m_captureReady) return false;
                        const size_t particles = CountId(m_capture, PixelId(Find(ctx, "Particles")));
                        const size_t curtain = CountId(m_capture, PixelId(Find(ctx, "Curtain")));
                        const size_t drape = CountId(m_capture, PixelId(Find(ctx, "Drape")));
                        const size_t sdf = CountId(m_capture, PixelId(Find(ctx, "SdfSculpture")));
                        Expect("pixels: particles, both cloths and the SDF sculpture are visible",
                               particles > 200 && curtain > 200 && drape > 200 && sdf > 200,
                               F("particles %.0f px, curtain %.0f px, drape %.0f px, ", double(particles), double(curtain), double(drape)) + F("SDF %.0f px", double(sdf)));
                        return true;
                    }
                }
            } });
        }

        // 10c. GPU kernels against their CPU references (synchronous readbacks).
        m_steps.push_back({ "FieldLab: GPU kernels vs CPU references", [this](GameContext& ctx) {
            namespace Adv = Physics::Advanced;
            if (Render::GetRendererInfo().compatTier) {   // no GPU kernels: the CPU backend's field sampling vs FieldMath
                std::vector<glm::vec4> samples;
                const bool ok = Adv::SampleFields(glm::vec3(-4, 0.5f, -4), glm::vec3(4, 3, 4), glm::ivec3(8, 3, 8), samples);
                Expect("4.1 tier: CPU field sampling available (GPU kernel checks skipped)", ok && samples.size() == 192, F("%.0f samples", double(samples.size())));
                return true;
            }
            // spatial hash: exact neighbour counts on a 1/64 m lattice (exactly representable)
            std::mt19937 rng(5);
            std::uniform_int_distribution<int> q(0, 4 * 64 - 1);
            std::vector<glm::vec3> p(8000);
            for (glm::vec3& v : p) v = glm::vec3(float(q(rng)), float(q(rng)), float(q(rng))) / 64.0f;
            std::vector<uint32_t> counts;
            const bool hashOk = Adv::GpuNeighbourCounts(p, 0.25f, 16384, 0.25f, counts) && counts.size() == p.size();
            size_t mismatches = 0, pairs = 0;
            for (size_t i = 0; hashOk && i < p.size(); ++i) {
                uint32_t n = 0;
                for (size_t j = 0; j < p.size(); ++j) {
                    const glm::vec3 d = p[i] - p[j];
                    n += (i != j && d.x * d.x + d.y * d.y + d.z * d.z < 0.25f * 0.25f);
                }
                mismatches += n != counts[i];
                pairs += n;
            }
            Expect("GPU spatial hash: neighbour counts equal brute force exactly", hashOk && mismatches == 0 && pairs > 0,
                   F("%.0f points, %.0f neighbour pairs, %.0f mismatches", double(p.size()), double(pairs), double(mismatches)));

            // XPBD: GPU solver vs CPU reference on the same model
            auto compare = [&](Physics::Xpbd::Model model, int substeps, int steps, bool fused = true) {
                Physics::Xpbd::Colorize(model);
                Physics::Xpbd::Model gpu = model;
                const Physics::Xpbd::Collider ground;
                Physics::Xpbd::StepParams sp;
                sp.substeps = substeps;
                sp.colliders = &ground;
                sp.colliderCount = 1;
                for (int i = 0; i < steps; ++i) Physics::Xpbd::Step(model, sp);
                if (!Adv::GpuStepModel(gpu, sp.dt, substeps, steps, true, fused)) return std::pair<double, Physics::Xpbd::Model>(1e9, gpu);
                double diff = 0.0;
                for (size_t i = 0; i < model.x.size(); ++i) {
                    const double d = glm::length(glm::vec3(model.x[i]) - glm::vec3(gpu.x[i]));
                    diff = std::isfinite(d) ? std::max(diff, d) : 1e9;
                }
                return std::pair<double, Physics::Xpbd::Model>(diff, gpu);
            };
            const auto [ropeDiff, rope] = compare(Physics::Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(2, 3, 0), 16, 1.0f, 0.0f, true), 10, 60);
            const auto [clothDiff, cloth] = compare(Physics::Xpbd::MakeCloth(glm::vec3(-1, 3, 0), glm::vec3(2, 0, 0), glm::vec3(0, -2, 0), 12, 12, 1.0f, 0.0f, 1e-2f, true), 10, 60);
            const auto [boxDiff, box] = compare(Physics::Xpbd::MakeSoftBox(glm::vec3(0, 1.5f, 0), glm::vec3(0.8f, 0.6f, 1.0f), 3, 2.0f, 1e-4f, 0.0f), 16, 180);
            Expect("GPU XPBD (fused single-workgroup solver) matches the CPU solver", ropeDiff < 0.01 && clothDiff < 0.01 && boxDiff < 0.01,
                   F("max node difference: rope %.2e m, cloth %.2e m, soft box %.2e m", ropeDiff, clothDiff, boxDiff));
            const double ropePasses = compare(Physics::Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(2, 3, 0), 16, 1.0f, 0.0f, true), 10, 60, false).first;
            const double clothPasses = compare(Physics::Xpbd::MakeCloth(glm::vec3(-1, 3, 0), glm::vec3(2, 0, 0), glm::vec3(0, -2, 0), 12, 12, 1.0f, 0.0f, 1e-2f, true), 10, 60, false).first;
            const double boxPasses = compare(Physics::Xpbd::MakeSoftBox(glm::vec3(0, 1.5f, 0), glm::vec3(0.8f, 0.6f, 1.0f), 3, 2.0f, 1e-4f, 0.0f), 16, 180, false).first;
            Expect("GPU XPBD (multi-pass solver for large bodies) matches the CPU solver", ropePasses < 0.01 && clothPasses < 0.01 && boxPasses < 0.01,
                   F("max node difference: rope %.2e m, cloth %.2e m, soft box %.2e m", ropePasses, clothPasses, boxPasses));
            Expect("GPU XPBD constraints stable (stretch, volume)",
                   Physics::Xpbd::MaxRelativeStretch(rope) < 0.02f && Physics::Xpbd::MaxRelativeStretch(cloth) < 0.05f &&
                   std::abs(Physics::Xpbd::TotalVolume(box) / 0.48f - 1.0f) < 0.05f,
                   F("rope stretch %.3f%%, cloth stretch %.3f%%, soft-box volume %.2f%% of rest", Physics::Xpbd::MaxRelativeStretch(rope) * 100.0,
                     Physics::Xpbd::MaxRelativeStretch(cloth) * 100.0, Physics::Xpbd::TotalVolume(box) / 0.48 * 100.0));
            Physics::Xpbd::Model link = Physics::Xpbd::MakeRope(glm::vec3(0, 3, 0), glm::vec3(0, 2, 0), 1, 2.0f, 1e-3f, true);
            Physics::Xpbd::Colorize(link);
            link.damping = 2.0f;
            Adv::GpuStepModel(link, 1.0f / 60.0f, 10, 600, false);
            const double stretch = glm::length(glm::vec3(link.x[1]) - glm::vec3(link.x[0])) - 1.0;
            const double expected = 1e-3 * 1.0 * 9.81;   // alpha * m * g, free node carries half of 2 kg
            Expect("GPU XPBD: compliant link settles at alpha*m*g", std::abs(stretch - expected) < 0.02 * expected,
                   F("stretch %.5f m, analytic %.5f m", stretch, expected));

            // n-body: tiled shared-memory kernel vs O(n^2) reference
            std::uniform_real_distribution<float> U(-4.0f, 4.0f), M(0.5f, 3.0f);
            std::vector<glm::vec3> bodies(2048), gpuAcc, cpuAcc(2048);
            std::vector<float> masses(2048);
            for (size_t i = 0; i < bodies.size(); ++i) { bodies[i] = glm::vec3(U(rng), U(rng), U(rng)); masses[i] = M(rng); }
            const bool nbodyOk = Adv::GpuNBody(bodies, masses, 0.5f, 0.05f, gpuAcc) && gpuAcc.size() == bodies.size();
            Physics::FieldMath::NBodyAccelerations(bodies.data(), masses.data(), bodies.size(), 0.5f, 0.05f, cpuAcc.data());
            double maxErr = 0.0, maxMag = 0.0;
            for (size_t i = 0; nbodyOk && i < bodies.size(); ++i) {
                maxErr = std::max(maxErr, double(glm::length(gpuAcc[i] - cpuAcc[i])));
                maxMag = std::max(maxMag, double(glm::length(cpuAcc[i])));
            }
            Expect("GPU n-body matches the CPU reference", nbodyOk && maxErr <= 1e-4 * maxMag,
                   F("2048 bodies: max error %.3e of max |a| %.3e", maxErr, maxMag));

            // public compute abstraction: a user kernel through IComputeBackend (OpenGL implementation)
            {
                Compute::IComputeBackend* gpu = Compute::GetBackend(Compute::BackendType::OpenGL);
                const Compute::DeviceInfo info = gpu ? gpu->Info() : Compute::DeviceInfo{};
                const std::string source =
                    "#version 460 core\n"
                    "layout(local_size_x = 64) in;\n"
                    "layout(std430, binding = 4) readonly buffer X { float x[]; };\n"
                    "layout(std430, binding = 5) buffer Y { float y[]; };\n"
                    "uniform float u_a;\n"
                    "uniform uint u_n;\n"
                    "void main() { uint i = gl_GlobalInvocationID.x; if (i < u_n) y[i] = u_a * x[i] + y[i]; }\n";
                const uint32_t n = 10000;
                std::vector<float> xs(n), ys(n, 1.0f);
                for (uint32_t i = 0; i < n; ++i) xs[i] = 0.5f * float(i);
                uint32_t wrong = n;
                if (gpu) {
                    const Compute::BufferId bx = gpu->CreateBuffer(n * sizeof(float)), by = gpu->CreateBuffer(n * sizeof(float));
                    gpu->Upload(bx, xs.data(), n * sizeof(float));
                    gpu->Upload(by, ys.data(), n * sizeof(float));
                    const Compute::KernelId k = gpu->CreateKernel("saxpy", source);
                    gpu->SetBuffer(k, 4, bx);
                    gpu->SetBuffer(k, 5, by);
                    gpu->SetUniform(k, "u_a", 3.0f);
                    gpu->SetUniform(k, "u_n", n);
                    gpu->Dispatch(k, (n + 63) / 64);
                    gpu->Barrier();
                    gpu->Download(by, ys.data(), n * sizeof(float));
                    wrong = 0;
                    for (uint32_t i = 0; i < n; ++i) wrong += ys[i] != 1.5f * float(i) + 1.0f;   // exact in float
                    gpu->DestroyBuffer(bx);
                    gpu->DestroyBuffer(by);
                }
                const std::vector<Compute::DeviceInfo> all = Compute::EnumerateBackends();
                Expect("compute backend API: user kernel runs on the OpenGL backend (exact saxpy)",
                       gpu && info.available && wrong == 0 && info.maxWorkGroupInvocations >= 1024,
                       std::string(info.device) + F(": %.0f wrong of 10000, %.0f invocations/group, %.0f storage blocks/kernel", wrong, info.maxWorkGroupInvocations, info.maxStorageBlocksPerKernel));
                Expect("OpenCL / SYCL listed as unavailable hooks, CPU backend available", all.size() == 4 && all[0].available && !all[1].available && !all[2].available && all[3].available,
                       all.size() == 4 ? all[1].note + "; " + all[3].device : std::string("?"));
            }

            // force-field sampling (the arrow visualiser's data) vs FieldMath
            using namespace Lab::FieldLayout;
            std::vector<glm::vec4> samples;
            const bool fieldsOk = Adv::SampleFields(FIELD_BOUNDS_MIN, FIELD_BOUNDS_MAX, FIELD_RESOLUTION, samples) &&
                                  samples.size() == size_t(FIELD_RESOLUTION.x * FIELD_RESOLUTION.y * FIELD_RESOLUTION.z);
            double fieldErr = 0.0, fieldMag = 0.0;
            for (size_t i = 0; fieldsOk && i < samples.size(); ++i) {
                const glm::ivec3 c(int(i) % FIELD_RESOLUTION.x, (int(i) / FIELD_RESOLUTION.x) % FIELD_RESOLUTION.y, int(i) / (FIELD_RESOLUTION.x * FIELD_RESOLUTION.y));
                const glm::vec3 point = glm::mix(FIELD_BOUNDS_MIN, FIELD_BOUNDS_MAX, (glm::vec3(c) + 0.5f) / glm::vec3(FIELD_RESOLUTION));
                glm::vec3 a(0.0f);
                ctx.CurrentScene()->Each<Physics::ForceField>([&](Entity e, Physics::ForceField& f) {
                    a += Physics::FieldMath::Acceleration(f, e.WorldPosition(), glm::normalize(e.WorldRotation() * f.axis), point);
                });
                fieldErr = std::max(fieldErr, double(glm::length(glm::vec3(samples[i]) - a)));
                fieldMag = std::max(fieldMag, double(glm::length(a)));
            }
            Expect("GPU field sampling matches FieldMath", fieldsOk && fieldMag > 1.0 && fieldErr <= 1e-4 * fieldMag,
                   F("%.0f samples: max error %.3e of max |a| %.3e", double(samples.size()), fieldErr, fieldMag));
            return true;
        } });

        // 10d. SDF raymarcher: a sphere's silhouette area matches the analytic projection.
        m_steps.push_back({ "FieldLab: SDF raymarch silhouette", [this](GameContext& ctx) {
            const Entity cam = Find(ctx, "MainCamera");
            const float R = 0.5f, distance = 4.0f;
            switch (m_phase) {
                case 0: {
                    const glm::vec3 eye = cam.WorldPosition(), forward = cam.WorldRotation() * glm::vec3(0, 0, -1);
                    for (int k = 0; k < 3; ++k) { m_scratch[k] = eye[k]; m_scratch[3 + k] = forward[k]; }
                    Render::SdfVisualization sdf = Render::GetSdfVisualization();
                    sdf.primitives.clear();
                    Render::SdfPrimitive sphere;
                    sphere.shape = Render::SdfShape::Sphere;
                    sphere.position = eye + forward * distance;
                    sphere.params = glm::vec4(R, 0, 0, 0);
                    sdf.primitives.push_back(sphere);
                    sdf.boundsMin = sphere.position - glm::vec3(R + 0.05f);
                    sdf.boundsMax = sphere.position + glm::vec3(R + 0.05f);
                    sdf.showSlice = false;
                    Render::SetSdfVisualization(sdf);
                    m_scratch[6] = float(m_stepFrames);
                    m_phase = 1;
                    return false;
                }
                case 1:
                    if (m_stepFrames - m_scratch[6] < 3) return false;
                    Screenshot("10_sdf_sphere");
                    RequestCapture();
                    m_phase = 2;
                    return false;
                default: {
                    if (!m_captureReady) return false;
                    const glm::vec3 eye(m_scratch[0], m_scratch[1], m_scratch[2]);
                    const float fov = cam.Get<Camera>().verticalFov;
                    const double f = 0.5 * m_capture.height / std::tan(0.5 * fov);
                    const double r = f * R / std::sqrt(double(distance) * distance - double(R) * R);
                    const double expected = glm::pi<double>() * r * r;
                    const double measured = double(CountId(m_capture, PixelId(Find(ctx, "SdfSculpture"))));
                    const double moved = glm::length(cam.WorldPosition() - eye);
                    Expect("SDF sphere silhouette area matches the analytic projection",
                           moved < 1e-3 && std::abs(measured - expected) <= 0.01 * expected + glm::pi<double>() * r,
                           F("%.0f px measured, %.1f px expected (radius %.2f px), ", measured, expected, r) +
                           F("camera moved %.4f m, capture %.0fx%.0f", moved, m_capture.width, m_capture.height) + F(", fov %.3f", fov));
                    Lab::FieldLabScene::ApplySdfSculpture(*ctx.CurrentScene());
                    return true;
                }
            }
        } });

        // 11. Round trip back: resource counts identical to the first load.
        // 11. P8: studio UI, tiers, CPU compute backend.
        m_steps.push_back({ "P8: studio UI, renderer tier, CPU compute backend", [this](GameContext&) {
            if (m_stepFrames < 3) return false;
            const Render::RendererInfo info = Render::GetRendererInfo();
            Expect("Dear ImGui studio panels rendered", UI::IsActive() && UI::LastDrawVertexCount() > 100, F("%.0f UI vertices last frame", double(UI::LastDrawVertexCount())));
            Expect("renderer tier reported", info.contextVersion >= 41 && info.gpuCompute == !info.compatTier,
                   F("OpenGL %.0f.%.0f, ", info.contextVersion / 10, info.contextVersion % 10) + (info.compatTier ? "4.1 tier" : "4.6 tier"));
            Compute::RegisterCpuKernel("saxpy", 64, [](const Compute::CpuInvocation& inv) {
                const uint32_t i = inv.globalId[0];
                if (i >= inv.U("u_n")) return;
                const float* x = inv.Buffer<float>(4);
                float* y = inv.Buffer<float>(5);
                y[i] = inv.F("u_a") * x[i] + y[i];
            });
            Compute::IComputeBackend* cpu = Compute::GetBackend(Compute::BackendType::CPU);
            const uint32_t n = 100000;
            std::vector<float> xs(n), ys(n, 1.0f);
            for (uint32_t i = 0; i < n; ++i) xs[i] = 0.5f * float(i % 4096);
            uint32_t wrong = n;
            if (cpu) {
                const Compute::BufferId bx = cpu->CreateBuffer(n * 4), by = cpu->CreateBuffer(n * 4);
                cpu->Upload(bx, xs.data(), n * 4);
                cpu->Upload(by, ys.data(), n * 4);
                const Compute::KernelId k = cpu->CreateKernel("saxpy", "");
                cpu->SetBuffer(k, 4, bx);
                cpu->SetBuffer(k, 5, by);
                cpu->SetUniform(k, "u_a", 3.0f);
                cpu->SetUniform(k, "u_n", n);
                cpu->Dispatch(k, (n + 63) / 64);
                cpu->Download(by, ys.data(), n * 4);
                wrong = 0;
                for (uint32_t i = 0; i < n; ++i) wrong += ys[i] != 1.5f * float(i % 4096) + 1.0f;
            }
            Expect("CPU compute backend: registered kernel on the worker pool (exact saxpy)", cpu && wrong == 0,
                   cpu ? cpu->Info().device + F(": %.0f wrong of %.0f", double(wrong), double(n)) : std::string("no CPU backend"));
            return true;
        } });

        // 12. P8: FSR 1.0 (EASU + RCAS) on the GPU vs the CPU reference, quality vs bilinear, live render scale.
        m_steps.push_back({ "P8: FSR 1.0 upscaling", [this](GameContext&) {
            Render::RenderSettings& rs = Render::Settings();
            switch (m_phase) {
                case 0: RequestCapture(); m_phase = 1; return false;
                case 1: {
                    if (!m_captureReady) return false;
                    // ground truth: the native frame; input: its 2/3 box-downsample
                    const int W = m_capture.width - m_capture.width % 3, H = m_capture.height - m_capture.height % 3;
                    const int w = W * 2 / 3, h = H * 2 / 3;
                    std::vector<uint8_t> truth(size_t(W) * H * 4), low(size_t(w) * h * 4);
                    for (int y = 0; y < H; ++y) std::copy_n(&m_capture.rgba[size_t(y) * m_capture.width * 4], size_t(W) * 4, &truth[size_t(y) * W * 4]);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            for (int c = 0; c < 4; ++c) {
                                const float sx = (x + 0.5f) * 1.5f - 0.5f, sy = (y + 0.5f) * 1.5f - 0.5f;   // bilinear sample of the area centre
                                const int x0 = int(sx), y0 = int(sy);
                                const float fx = sx - x0, fy = sy - y0;
                                auto T = [&](int xx, int yy) { return float(truth[(size_t(std::min(yy, H - 1)) * W + std::min(xx, W - 1)) * 4 + c]); };
                                const float v = (T(x0, y0) * (1 - fx) + T(x0 + 1, y0) * fx) * (1 - fy) + (T(x0, y0 + 1) * (1 - fx) + T(x0 + 1, y0 + 1) * fx) * fy;
                                low[(size_t(y) * w + x) * 4 + c] = uint8_t(std::lround(v));
                            }
                    std::vector<uint8_t> gpuFsr, gpuBilinear, cpuEasu, cpuFsr;
                    const bool ok = Render::UpscaleImage(low, w, h, W, H, Render::Upscaler::FSR1, 0.25f, gpuFsr) &&
                                    Render::UpscaleImage(low, w, h, W, H, Render::Upscaler::Bilinear, 0.0f, gpuBilinear);
                    Render::Fsr::Easu(low, w, h, W, H, cpuEasu);
                    Render::Fsr::Rcas(cpuEasu, W, H, 0.25f, cpuFsr);
                    int maxDiff = 0;
                    size_t over1 = 0;
                    for (size_t i = 0; ok && i < cpuFsr.size(); ++i) {
                        if (i % 4 == 3) continue;
                        const int d = std::abs(int(gpuFsr[i]) - int(cpuFsr[i]));
                        maxDiff = std::max(maxDiff, d);
                        over1 += d > 1;
                    }
                    Expect("GPU EASU + RCAS matches the CPU reference", ok && maxDiff <= 6 && over1 * 100 <= cpuFsr.size(),
                           F("%.0fx%.0f -> ", w, h) + F("%.0fx%.0f: max channel difference %.0f", W, H, maxDiff) + F(", %.0f values > 1", double(over1)));
                    const double psnrFsr = ok ? Render::Fsr::Psnr(gpuFsr, truth) : 0.0, psnrBilinear = ok ? Render::Fsr::Psnr(gpuBilinear, truth) : 0.0;
                    Expect("FSR reconstructs the native frame better than bilinear", ok && psnrFsr > psnrBilinear,
                           F("PSNR vs native: FSR %.2f dB, bilinear %.2f dB", psnrFsr, psnrBilinear));
                    m_scratch[5] = rs.renderScale;
                    rs.renderScale = 0.667f;
                    rs.upscaler = Render::Upscaler::FSR1;
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                }
                case 2:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    RequestCapture();
                    m_phase = 3;
                    return false;
                case 3: {
                    if (!m_captureReady) return false;
                    const RenderStats r = GetRenderStats();
                    const glm::ivec2 window = Render::GetFramebufferSize();
                    Expect("Low preset scale: scene at 2/3 resolution upscaled to the window", r.upscaled && r.outputWidth == window.x &&
                           std::abs(r.internalWidth - int(std::lround(window.x * 0.667f))) <= 1 && m_capture.width == window.x,
                           F("%.0fx%.0f -> ", r.internalWidth, r.internalHeight) + F("%.0fx%.0f", r.outputWidth, r.outputHeight));
                    Expect("no OpenGL errors with upscaling", Render::GetGLErrorCount() == 0, F("%.0f errors", double(Render::GetGLErrorCount())));
                    Screenshot("11_fsr_low_preset");
                    rs.renderScale = m_scratch[5];
                    return true;
                }
            }
            return false;
        } });

        // 13. P8: synthetic vision (aligned colour, linear depth, world normals, entity ids).
        m_steps.push_back({ "P8: synthetic vision buffers", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: Render::RequestVisionCapture(); RequestCapture(); m_phase = 1; return false;
                case 1: {
                    if (!m_captureReady) return false;
                    Render::VisionCapture v;
                    const bool ok = Render::GetVisionCapture(v) && v.width == m_capture.width && v.height == m_capture.height;
                    size_t labelled = 0, agree = 0, unitNormals = 0;
                    for (size_t p = 0; ok && p < v.entityIds.size(); ++p) {
                        if (v.entityIds[p] == 0) continue;
                        ++labelled;
                        agree += v.entityIds[p] == m_capture.entityIds[p];
                        unitNormals += std::abs(glm::length(v.normals[p]) - 1.0f) < 0.01f;
                    }
                    Expect("vision buffers aligned with the frame (ids agree with the colour pass)", ok && labelled > 1000 && agree >= labelled * 90 / 100,
                           F("%.0fx%.0f, ", v.width, v.height) + F("%.0f labelled pixels, %.2f%% agree", double(labelled), labelled ? 100.0 * agree / labelled : 0.0));
                    Expect("world normals are unit vectors", ok && unitNormals == labelled, F("%.0f of %.0f", double(unitNormals), double(labelled)));
                    // linear depth vs a physics raycast through the same pixel
                    const glm::mat4 invView = glm::inverse(v.view), invProj = glm::inverse(v.projection);
                    const glm::vec3 eye(invView[3]), forward = -glm::vec3(invView[2]);
                    int tested = 0;
                    double worst = 0.0;
                    for (int k = 0; ok && k < 4000 && tested < 40; ++k) {
                        const int x = (k * 37) % v.width, y = (k * 53) % v.height;
                        const size_t p = size_t(y) * v.width + x;
                        const uint32_t id = v.entityIds[p];
                        if (id == 0) continue;
                        const glm::vec4 ndc((x + 0.5f) / v.width * 2.0f - 1.0f, (y + 0.5f) / v.height * 2.0f - 1.0f, 1.0f, 1.0f);
                        glm::vec4 far = invView * (invProj * ndc);
                        const glm::vec3 dir = glm::normalize(glm::vec3(far) / far.w - eye);
                        Physics::QueryOptions options;
                        options.ignoreCharacter = ctx.player.GetCharacter();   // the camera may sit inside the player capsule
                        const auto hit = Physics::Raycast(eye, dir, 400.0f, options);
                        if (!hit) continue;
                        const Entity owner = ctx.CurrentScene()->FromBody(hit->body);
                        const bool sameEntity = owner && (owner.Id() + 1 == id || (owner.GetParent() && owner.GetParent().Id() + 1 == id));   // e.g. Ground + GroundCollider
                        if (!sameEntity) continue;
                        if (!owner.Has<Collider>() || owner.Get<Collider>().shape.shape != Physics::ShapeType::Box) continue;   // flat faces: mesh == collider
                        const double expected = hit->distance * glm::dot(dir, forward);
                        worst = std::max(worst, std::abs(double(v.linearDepth[p]) - expected));
                        ++tested;
                    }
                    Expect("linear depth equals the raycast distance along the view axis (box faces)", ok && tested >= 10 && worst < 0.01,
                           F("%.0f pixels, max error %.4f m", double(tested), worst));
                    return true;
                }
            }
            return false;
        } });

        // 14. P8: particle emitter + two-way coupling (Fountain), replay scrubbing, JSON round trip.
        m_steps.push_back({ "P8: emitter, two-way coupling, replay scrub, JSON round trip", [this](GameContext& ctx) {
            namespace Adv = Physics::Advanced;
            switch (m_phase) {
                case 0: ctx.scenes->Request("Fountain"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "Fountain") return false;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_scratch[1] = Find(ctx, "Crate0").WorldPosition().x;
                    m_phase = 2;
                    return false;
                case 2: {
                    if (StepSimTime(ctx) - m_scratch[0] < 3.0) return false;
                    const Adv::Stats s = Adv::GetStats();
                    const double t = StepSimTime(ctx) - m_scratch[0];
                    const double expectedAlive = std::min(900.0 * t, 900.0 * 5.0);   // rate x min(time, lifetime)
                    Expect("emitter spawns at its rate (pool of 6000, lifetime 5 s)", s.particleSystems == 1 && s.particles == 6000 &&
                           std::abs(double(s.particlesAlive) - expectedAlive) < 0.06 * expectedAlive + 20,
                           F("%.0f alive after %.2f s (expected ~%.0f)", double(s.particlesAlive), t, expectedAlive));
                    const Entity crate = Find(ctx, "Crate0");
                    Expect("particles push dynamic crates (two-way coupling)", s.coupledImpulsesApplied > 0 && std::abs(crate.WorldPosition().x - m_scratch[1]) > 0.02f,
                           F("%.0f impulses applied, Crate0 moved %.3f m", double(s.coupledImpulsesApplied), double(crate.WorldPosition().x - m_scratch[1])));
                    // replay: restore an older snapshot and check the scene matches it exactly
                    const Replay& replay = ctx.replay;
                    Expect("replay recorded every fixed step", replay.Enabled() && replay.Size() >= 150, F("%.0f snapshots", double(replay.Size())));
                    if (replay.Size() < 61) return true;
                    m_scratch[2] = float(replay.Size() - 60);
                    ctx.state = GameState::Paused;
                    ctx.replay.Restore(*ctx.CurrentScene(), size_t(m_scratch[2]));
                    m_scratch[3] = float(m_stepFrames);
                    m_phase = 3;
                    return false;
                }
                case 3: {
                    if (m_stepFrames - m_scratch[3] < 2) return false;
                    const ReplaySnapshot& snap = ctx.replay.At(size_t(m_scratch[2]));
                    const Entity crate = Find(ctx, "Crate0");
                    float worst = 0.0f;
                    for (const ReplayEntityState& st : snap.entities) {
                        Entity e = ctx.CurrentScene()->FromId(st.entity);
                        if (!e || !st.hasBody) continue;
                        worst = std::max(worst, glm::length(e.LocalTransform().position - st.position));
                        worst = std::max(worst, glm::length(Physics::GetPosition(e.GetBody()) - e.WorldPosition()));
                    }
                    Expect("scrubbed to an older step: bodies and transforms match the snapshot", crate && worst < 1e-4f,
                           F("snapshot %.0f (step %.0f): max deviation %.2e m", m_scratch[2], double(snap.step), worst));
                    ctx.state = GameState::Running;   // resume from the scrubbed step
                    m_scratch[3] = float(m_stepFrames);
                    m_phase = 4;
                    return false;
                }
                case 4: {
                    if (m_stepFrames - m_scratch[3] < 10) return false;
                    Expect("resuming discards the newer timeline", ctx.replay.Size() < size_t(m_scratch[2]) + 1 + 40 && !ctx.replay.Scrubbed(),
                           F("%.0f snapshots after resuming from %.0f", double(ctx.replay.Size()), m_scratch[2]));
                    // JSON round trip of the live scene
                    Scene& scene = *ctx.CurrentScene();
                    const std::string before = SceneSerializer::ToJson(scene);
                    const size_t entities = scene.EntityCount();
                    std::string error;
                    const bool loaded = SceneSerializer::FromJson(scene, before, &error);
                    const std::string after = SceneSerializer::ToJson(scene);
                    Expect("JSON scene round trip is lossless", loaded && after == before && scene.EntityCount() == entities,
                           F("%.0f entities, %.0f bytes", double(entities), double(before.size())) + (error.empty() ? "" : ", " + error));
                    m_scratch[3] = float(m_stepFrames);
                    m_phase = 5;
                    return false;
                }
                case 5: {
                    if (m_stepFrames - m_scratch[3] < 5) return false;
                    const RenderStats r = GetRenderStats();
                    Expect("reloaded scene rebuilt its render objects and bodies", r.meshObjects == CountRenderedEntities(ctx) && Adv::GetStats().particleSystems == 1,
                           F("%.0f render objects, %.0f MeshRenderer entities", r.meshObjects, double(CountRenderedEntities(ctx))));
                    Screenshot("12_fountain");
                    return true;
                }
            }
            return false;
        } });

        // ================================================================== P9
        // P9a. Retro studio skin: panels, LCD, LED meters, EQ sliders, transport bar, playlist; the
        // bitmap HUD telemetry is suppressed while the panels are open (and returns when hidden).
        m_steps.push_back({ "P9: retro studio skin, zero HUD overlap", [this](GameContext& ctx) {
            namespace Retro = UI::Retro;
            switch (m_phase) {
                case 0:
                    UI::SetPanelsVisible(true);
                    if (m_stepFrames < 3) return false;
                    m_backbufferRequested = true;
                    m_backbufferReady = false;
                    m_phase = 1;
                    return false;
                case 1: {
                    if (!m_backbufferReady) return false;
                    const Retro::FrameStats& s = Retro::LastFrameStats();
                    Expect("retro panels drawn (bevelled, gold title stripes)", s.panels >= 5, F("%.0f panels", s.panels));
                    Expect("LCD readout, LED meters and spectrum analyser", s.lcds >= 1 && s.meters >= 4 && s.litSegments > 0,
                           F("%.0f LCDs, %.0f meters, %.0f lit segments", s.lcds, s.meters, s.litSegments));
                    Expect("equaliser sliders, transport buttons, lesson playlist", s.eqSliders >= 6 && s.transportButtons >= 7 && s.playlistRows >= 12,
                           F("%.0f EQ sliders, %.0f transport buttons, %.0f playlist rows", s.eqSliders, s.transportButtons, s.playlistRows));
                    // the LCD on the presented back buffer: recessed black with phosphor-green digits
                    const Render::FrameCapture& b = m_backbuffer;
                    size_t black = 0, green = 0, total = 0;
                    const int x0 = int(s.lcdRect[0]) + 3, x1 = int(s.lcdRect[0] + s.lcdRect[2]) - 3;
                    const int y0 = int(s.lcdRect[1]) + 3, y1 = int(s.lcdRect[1] + s.lcdRect[3]) - 3;
                    for (int y = std::max(y0, 0); y < std::min(y1, b.height); ++y) {
                        for (int x = std::max(x0, 0); x < std::min(x1, b.width); ++x) {
                            const uint8_t* p = &b.rgba[(size_t(b.height - 1 - y) * b.width + x) * 4];
                            ++total;
                            black += p[0] < 24 && p[1] < 40 && p[2] < 24;
                            green += p[1] > 180 && p[0] < 110 && p[2] < 140;
                        }
                    }
                    const int cx = int(s.lcdRect[0] + s.lcdRect[2] * 0.5f), cy = int(s.lcdRect[1] + s.lcdRect[3] * 0.5f);
                    const uint8_t* cp = (cx >= 0 && cy >= 0 && cx < b.width && cy < b.height) ? &b.rgba[(size_t(b.height - 1 - cy) * b.width + cx) * 4] : nullptr;
                    Expect("LCD pixels on screen: black background, glowing green text", total > 500 && black > total / 2 && green > total / 100,
                           F("%.0f px: %.1f %% black, %.2f %% phosphor green", double(total), 100.0 * black / std::max<size_t>(total, 1), 100.0 * green / std::max<size_t>(total, 1))
                               + F(", rect (%.0f, %.0f, %.0f, ", s.lcdRect[0], s.lcdRect[1], s.lcdRect[2]) + F("%.0f) in %.0fx%.0f", s.lcdRect[3], b.width, b.height)
                               + (cp ? F(", centre (%.0f, %.0f, %.0f)", cp[0], cp[1], cp[2]) : std::string()));
                    { Assets::Image shot; shot.width = b.width; shot.height = b.height; shot.rgba.resize(b.rgba.size());
                      for (int y = 0; y < b.height; ++y) std::copy_n(&b.rgba[size_t(b.height - 1 - y) * b.width * 4], size_t(b.width) * 4, &shot.rgba[size_t(y) * b.width * 4]);
                      Assets::WritePng(m_runDirectory + "/p9_00_backbuffer_with_ui.png", shot); }
                    const RenderStats r = GetRenderStats();
                    Expect("no bitmap HUD text behind the panels", DebugOverlay::LastTelemetryLineCount() == 0 && r.overlayGlyphs <= 40,
                           F("%.0f telemetry lines, %.0f overlay glyphs (crosshair / target label only)", DebugOverlay::LastTelemetryLineCount(), r.overlayGlyphs));
                    Screenshot("p9_01_retro_studio");
                    UI::SetPanelsVisible(false);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                }
                default: {
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    const int lines = DebugOverlay::LastTelemetryLineCount();
                    Expect("HUD telemetry returns when the panels are hidden (Tab)", lines > 10 && ctx.overlayVisible, F("%.0f lines", lines));
                    UI::SetPanelsVisible(true);
                    return true;
                }
            }
        } });

        // P9b. glTF import, dual-quaternion skinning on the GPU-drawn mesh, spline camera path.
        m_steps.push_back({ "P9: glTF skinning and spline camera", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.scenes->Request("Animation"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "Animation") return false;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 2;
                    return false;
                case 2: {
                    if (StepSimTime(ctx) - m_scratch[0] < 1.0) return false;
                    const Animation::Stats a = Animation::GetStats();
                    Expect("three glTF walkers loaded, animated and skinned every frame", a.models == 3 && a.clipsPlaying == 3 && a.skinnedVertices == 3u * 1025u,
                           F("%.0f models, %.0f clips playing, %.0f vertices skinned in %.2f ms", a.models, a.clipsPlaying, a.skinnedVertices, a.skinningMs));
                    Entity dq = Find(ctx, "Walker DQ");
                    const SkinnedModel* m = dq.Valid() ? dq.TryGet<SkinnedModel>() : nullptr;
                    Expect("SkinnedModel loaded without errors (embedded texture registered)", m && m->loaded && m->error.empty() && Render::HasMaterial("gltf:models/walker.glb#material0"),
                           m ? m->error : std::string("no walker"));
                    RequestCapture();
                    m_phase = 3;
                    return false;
                }
                case 3: {
                    if (!m_captureReady) return false;
                    Entity body = Find(ctx, "Walker DQ/Body");
                    const size_t pixels = CountId(m_capture, PixelId(body));
                    const glm::dvec3 color = MeanColor(m_capture, PixelId(body));
                    Expect("skinned walker visible in the frame with its striped texture", pixels > 2000 && LumaStdDev(m_capture, PixelId(body)) > 12.0,
                           F("%.0f px, mean colour (%.0f, %.0f, ", double(pixels), color.x, color.y) + F("%.0f), luma std dev %.1f", color.z, LumaStdDev(m_capture, PixelId(body))));
                    Screenshot("p9_02_skinning");
                    Entity camera = Find(ctx, "MainCamera");
                    CameraPath* path = camera.Valid() ? camera.TryGet<CameraPath>() : nullptr;
                    Expect("Catmull-Rom camera path attached", path != nullptr, path ? F("%.0f points", double(path->points.size())) : std::string("none"));
                    if (path) { path->playing = true; path->time = 0.0f; }
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 4;
                    return false;
                }
                default: {
                    if (StepSimTime(ctx) - m_scratch[0] < 2.0) return false;
                    Entity camera = Find(ctx, "MainCamera");
                    CameraPath* path = camera.Valid() ? camera.TryGet<CameraPath>() : nullptr;
                    if (!path) return true;
                    const float u = path->time / path->duration;
                    const glm::vec3 expected = path->positionSpline.Evaluate(u - std::floor(u));
                    const glm::vec3 p = camera.WorldPosition();
                    const glm::vec3 forward = camera.WorldRotation() * glm::vec3(0, 0, -1);
                    const glm::vec3 toTarget = glm::normalize(glm::vec3(0.0f, 1.0f, -0.6f) - p);
                    Expect("camera rides the spline and looks at its target", glm::length(p - expected) < 0.05f && glm::dot(forward, toTarget) > 0.999f && path->positionSpline.Length() > 30.0f,
                           "at " + V(p) + F(" (u %.3f, error %.4f m), look dot %.5f", u, glm::length(p - expected), glm::dot(forward, toTarget)));
                    path->playing = false;
                    Screenshot("p9_03_camera_path");
                    return true;
                }
            }
        } });

        // P9c. Post-processing and VFX, each verified by an A/B frame of the same view.
        m_steps.push_back({ "P9: bloom, SSAO, trails, beam and decals", [this](GameContext& ctx) {
            Render::RenderSettings& rs = Render::Settings();
            auto meanLuma = [](const Render::FrameCapture& c) { double s = 0.0; for (size_t i = 0; i < c.entityIds.size(); ++i) s += Luma(c, i); return s / std::max<size_t>(c.entityIds.size(), 1); };
            switch (m_phase) {
                case 0:
                    m_savedSettings = rs;
                    ctx.scenes->Request("VFX");
                    m_phase = 1;
                    return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "VFX") return false;
                    Render::ApplyQualityPreset(Render::QualityPreset::High);
                    rs.renderScale = 1.0f;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 2;
                    return false;
                case 2: {
                    if (StepSimTime(ctx) - m_scratch[0] < 1.5) return false;
                    ctx.state = GameState::Paused;   // freeze the orbs for the A/B frames
                    const RenderStats r = GetRenderStats();
                    const VFX::Stats v = VFX::GetStats();
                    Expect("post stack active (High preset): bloom 6 mips, full-resolution SSAO", r.bloomMips == 6 && r.ssaoActive && r.ssaoWidth == r.internalWidth,
                           F("bloom %.0f mips, SSAO %.0fx%.0f", r.bloomMips, r.ssaoWidth, r.ssaoHeight) + F(" (scene %.0fx%.0f)", r.internalWidth, r.internalHeight));
                    Expect("VFX submitted: 3 trails, 3 orbit paths, a beam (2 strips), 4 decals", v.trails == 3 && v.orbits == 3 && v.beams == 1 && r.decalsDrawn == 4 && r.ribbonsDrawn == 8 && v.trailPoints > 30,
                           F("%.0f trails (%.0f points), %.0f orbits, %.0f beams", v.trails, v.trailPoints, v.orbits, v.beams) + F(", %.0f ribbons, %.0f decals", r.ribbonsDrawn, r.decalsDrawn)
                               + F(", frame dt %.3f s", ctx.stats.deltaTime));
                    Entity pylon = Find(ctx, "PylonA");
                    const Beam* beam = pylon.Valid() ? pylon.TryGet<Beam>() : nullptr;
                    const Entity crate = Find(ctx, "BeamCrate");
                    const float face = crate.Valid() ? crate.WorldPosition().x - 0.4f : 0.0f;
                    Expect("raycast beam stops on the crate in its path", beam && std::fabs(beam->lastEnd.x - face) < 0.15f,
                           beam ? F("beam end x %.3f, crate face x %.3f", beam->lastEnd.x, face) : std::string("no beam"));
                    RequestCapture();
                    m_phase = 3;
                    return false;
                }
                case 3:   // reference: everything on
                    if (!m_captureReady) return false;
                    m_reference = m_capture;
                    Screenshot("p9_04_vfx");
                    rs.bloom.enabled = false;
                    RequestCapture();
                    m_phase = 4;
                    return false;
                case 4: {
                    if (!m_captureReady) return false;
                    const double withBloom = meanLuma(m_reference), without = meanLuma(m_capture);
                    // glow spills onto the floor next to the neon sign (pixels that are not the sign itself)
                    const uint32_t neon = PixelId(Find(ctx, "Neon"));
                    double spill = 0.0;
                    size_t spillPixels = 0;
                    for (size_t i = 0; i < m_capture.entityIds.size(); ++i) {
                        if (m_capture.entityIds[i] == neon) continue;
                        const double d = Luma(m_reference, i) - Luma(m_capture, i);
                        if (d > 2.0) { spill += d; ++spillPixels; }
                    }
                    Expect("HDR bloom brightens the image around emissive sources", withBloom > without + 0.3 && spillPixels > 2000,
                           F("mean luma %.2f with vs %.2f without, %.0f px brightened", withBloom, without, double(spillPixels)));
                    rs.bloom.enabled = true;
                    rs.ssao.enabled = false;
                    RequestCapture();
                    m_phase = 5;
                    return false;
                }
                case 5: {
                    if (!m_captureReady) return false;
                    // AO darkens creases: the corner boxes / walls lose light, open floor stays (almost) the same
                    size_t darker = 0, lighter = 0;
                    double cornerDelta = 0.0;
                    size_t cornerPixels = 0;
                    // static corner geometry only (the ring decal pulses and the beam flickers between frames)
                    std::set<uint32_t> corner = { PixelId(Find(ctx, "WallBack")), PixelId(Find(ctx, "WallSide")) };
                    for (int k = 0; k < 4; ++k) corner.insert(PixelId(Find(ctx, "AOBox" + std::to_string(k))));
                    for (size_t i = 0; i < m_capture.entityIds.size(); ++i) {
                        if (!corner.count(m_capture.entityIds[i])) continue;
                        const double d = Luma(m_capture, i) - Luma(m_reference, i);   // without AO - with AO
                        darker += d > 3.0;
                        lighter += d < -3.0;
                        cornerDelta += d;
                        ++cornerPixels;
                    }
                    Expect("SSAO darkens the corner geometry (walls, stacked boxes)", darker > 1000 && lighter < darker / 20 && cornerPixels > 0 && cornerDelta / double(cornerPixels) > 1.0,
                           F("%.0f px darker with AO, %.0f lighter, corner mean %.2f luma", double(darker), double(lighter), cornerPixels ? cornerDelta / double(cornerPixels) : 0.0));
                    rs.ssao.enabled = true;
                    rs.decals = false;
                    RequestCapture();
                    m_phase = 6;
                    return false;
                }
                case 6: {
                    if (!m_captureReady) return false;
                    // scorch darkens, the glowing ring brightens the floor where the decals are
                    auto region = [&](const glm::vec3& world) {
                        Entity camera = Find(ctx, "MainCamera");
                        const Camera& cam = camera.Get<Camera>();
                        const float aspect = float(m_capture.width) / float(m_capture.height);
                        const glm::mat4 vp = glm::perspective(cam.verticalFov, aspect, cam.nearPlane, cam.farPlane) * glm::inverse(camera.WorldMatrix());
                        const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
                        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                        const int cx = int((ndc.x * 0.5f + 0.5f) * float(m_capture.width)), cy = int((ndc.y * 0.5f + 0.5f) * float(m_capture.height));
                        double delta = 0.0;
                        int n = 0;
                        for (int y = cy - 6; y <= cy + 6; ++y)
                            for (int x = cx - 6; x <= cx + 6; ++x) {
                                if (x < 0 || y < 0 || x >= m_capture.width || y >= m_capture.height) continue;
                                const size_t i = size_t(y) * m_capture.width + x;
                                delta += Luma(m_reference, i) - Luma(m_capture, i);   // with decal - without
                                ++n;
                            }
                        return n ? delta / n : 0.0;
                    };
                    const double scorch = region(glm::vec3(-3.0f, 0.0f, 2.5f)), ring = region(glm::vec3(0.936f, 0.0f, 2.5f)), paint = region(glm::vec3(3.0f, 0.0f, 2.5f));
                    Expect("projected decals: scorch darkens, glowing ring brightens, paint recolours the floor", scorch < -8.0 && ring > 4.0 && std::fabs(paint) > 4.0,
                           F("luma change: scorch %.1f, ring %.1f, paint %.1f", scorch, ring, paint));
                    rs.decals = true;
                    ctx.state = GameState::Running;
                    rs = m_savedSettings;
                    return true;
                }
            }
            return false;
        } });

        // P9d. Live shader hot reload from a watched copy of engine/shaders.
        m_steps.push_back({ "P9: live GLSL hot reload", [this](GameContext& ctx) {
            namespace fs = std::filesystem;
            const fs::path dir = fs::path(m_runDirectory) / "shaders_hot";
            const fs::path tonemap = dir / "tonemap.frag.glsl";
            auto writeTonemap = [&](const std::string& extra) {
                std::ofstream f(tonemap, std::ios::binary | std::ios::trunc);
                f << m_shaderOriginal << extra;
            };
            auto centre = [&](int channel) { return m_capture.rgba[(size_t(m_capture.height / 2) * m_capture.width + m_capture.width / 2) * 4 + channel]; };
            const Render::ShaderReloadStatus status = Render::GetShaderReloadStatus();
            switch (m_phase) {
                case 0: {
                    std::error_code ec;
                    fs::remove_all(dir, ec);
                    fs::create_directories(dir, ec);
                    fs::copy("engine/shaders", dir, fs::copy_options::overwrite_existing, ec);
                    std::ifstream in(tonemap, std::ios::binary);
                    std::stringstream text;
                    text << in.rdbuf();
                    m_shaderOriginal = text.str();
                    const bool enabled = !m_shaderOriginal.empty() && Render::EnableShaderHotReload(dir.string());
                    Expect("watching a copy of engine/shaders", enabled && Render::GetShaderReloadStatus().filesOverridden == 0,
                           dir.string() + F(" (%.0f files overridden: identical to the embedded sources)", Render::GetShaderReloadStatus().filesOverridden));
                    m_scratch[0] = float(status.reloads);
                    // edit: the tone mapper now paints everything magenta
                    writeTonemap("\n// hot-reload test\n#define main main_original\n");
                    {
                        std::ofstream f(tonemap, std::ios::binary | std::ios::trunc);
                        std::string edited = m_shaderOriginal;
                        const size_t end = edited.rfind('}');
                        edited.insert(end, "    o_color = vec4(1.0, 0.0, 1.0, 1.0);   // hot-reload test\n");
                        f << edited;
                    }
                    m_phase = 1;
                    return false;
                }
                case 1:
                    if (float(status.reloads) <= m_scratch[0]) return false;
                    RequestCapture();
                    m_phase = 2;
                    return false;
                case 2: {
                    if (!m_captureReady) return false;
                    Expect("edited tonemap.frag.glsl rebuilt live (frame turns magenta)", centre(0) > 240 && centre(1) < 15 && centre(2) > 240 && status.failures == 0,
                           F("%.0f reloads, %.0f programs rebuilt, centre pixel (%.0f, ", status.reloads, status.programsRebuilt, centre(0)) + F("%.0f, %.0f)", centre(1), centre(2)));
                    m_scratch[0] = float(status.failures);
                    std::ofstream f(tonemap, std::ios::binary | std::ios::trunc);
                    f << m_shaderOriginal << "\nthis is not GLSL;\n";
                    m_phase = 3;
                    return false;
                }
                case 3:
                    if (float(status.failures) <= m_scratch[0]) return false;
                    RequestCapture();
                    m_phase = 4;
                    return false;
                case 4: {
                    if (!m_captureReady) return false;
                    Expect("a broken edit is reported and the previous program keeps running", centre(0) > 240 && centre(1) < 15 && !status.lastError.empty(),
                           status.lastError + F(" (centre still %.0f, %.0f, ", centre(0), centre(1)) + F("%.0f)", centre(2)));
                    m_scratch[0] = float(status.reloads);
                    writeTonemap("");
                    m_phase = 5;
                    return false;
                }
                case 5:
                    if (float(status.reloads) <= m_scratch[0]) return false;
                    RequestCapture();
                    m_phase = 6;
                    return false;
                default: {
                    if (!m_captureReady) return false;
                    const bool magenta = centre(0) > 240 && centre(1) < 15 && centre(2) > 240;
                    Render::DisableShaderHotReload();
                    Expect("restored source renders normally again", !magenta && Render::GetShaderReloadStatus().filesOverridden == 0,
                           F("centre (%.0f, %.0f, %.0f)", centre(0), centre(1), centre(2)));
                    (void)ctx;
                    return true;
                }
            }
        }, 60.0 });

        // P9e. Terrain + foliage + navigation crowd + spatial audio.
        m_steps.push_back({ "P9: terrain, foliage, nav crowd, spatial audio", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.scenes->Request("Terrain"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "Terrain") return false;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_scratch[1] = 10.0f; m_scratch[2] = 0.0f; m_scratch[3] = 1e9f;
                    m_phase = 2;
                    return false;
                case 2: {
                    // track the drone's Doppler pitch range and the crowd spacing while it runs
                    Entity drone = Find(ctx, "Drone");
                    const AudioSource* a = drone.Valid() ? drone.TryGet<AudioSource>() : nullptr;
                    Audio::SpatialInfo info;
                    if (a && Audio::GetSpatialInfo(a->voice, info)) { m_scratch[1] = std::min(m_scratch[1], info.dopplerPitch); m_scratch[2] = std::max(m_scratch[2], info.dopplerPitch); }
                    if (StepSimTime(ctx) - m_scratch[0] > 1.0) m_scratch[3] = std::min(m_scratch[3], AI::GetStats().minSeparation);
                    if (StepSimTime(ctx) - m_scratch[0] < 12.0) return false;
                    const World::Stats w = World::GetStats();
                    Expect("terrain built: 129x129 height field, splat material, 4 instanced foliage batches", w.terrains == 1 && w.terrainTriangles == 128u * 128u * 2u && w.foliageBatches == 4 && w.foliageInstances > 800,
                           F("%.0f triangles, %.0f foliage instances in %.0f batches", w.terrainTriangles, w.foliageInstances, w.foliageBatches));
                    Entity terrain = Find(ctx, "Terrain");
                    float worst = 0.0f;
                    int hits = 0;
                    for (int i = 0; i < 24; ++i) {
                        const glm::vec3 p(std::cos(float(i)) * (3.0f + float(i)), 30.0f, std::sin(float(i)) * (3.0f + float(i)));
                        float h = 0.0f;
                        const auto hit = Physics::Raycast(p, glm::vec3(0, -1, 0), 60.0f);
                        if (!hit || !World::SampleTerrainHeight(terrain, p, h) || Find(ctx, "Terrain") != ctx.CurrentScene()->FromBody(hit->body)) continue;
                        worst = std::max(worst, std::fabs(hit->point.y - h));
                        ++hits;
                    }
                    Expect("height-field collision matches the rendered heights", hits >= 12 && worst < 0.03f, F("%.0f rays, max error %.4f m", double(hits), worst));
                    const AI::Stats nav = AI::GetStats();
                    const Lab::TerrainScene* scene = dynamic_cast<Lab::TerrainScene*>(ctx.CurrentScene());
                    Expect("navigation grid built from static geometry; crowd crosses and re-targets", nav.walkableCells > 1500 && nav.agents == 8 && scene && scene->crossings >= 8 && nav.pathFailures == 0,
                           F("%.0f walkable cells (%.1f ms), %.0f agents, %.0f arrivals", nav.walkableCells, nav.buildMs, nav.agents, scene ? double(scene->crossings) : 0.0)
                               + F(", %.0f paths, %.0f failures", nav.pathsPlanned, nav.pathFailures));
                    Expect("agents keep their distance (time-to-collision avoidance)", m_scratch[3] > -0.12f, F("closest gap %.3f m", m_scratch[3]));
                    Expect("spatial drone: panned, attenuated, Doppler-shifted voice", m_scratch[2] > 1.01f && m_scratch[1] < 0.99f,
                           F("pitch range %.3f .. %.3f", m_scratch[1], m_scratch[2]));
                    Screenshot("p9_05_terrain");
                    return true;
                }
            }
            return false;
        }, 30.0 });

        // P9f. Joints on the active backend, driven by Lua scripts; scripted interaction.
        m_steps.push_back({ "P9: joints and Lua scripting", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.scenes->Request("Joints"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "Joints") return false;
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_scratch[1] = 1e9f; m_scratch[2] = -1e9f; m_scratch[3] = 0.0f; m_scratch[4] = 1e9f; m_scratch[5] = -1e9f;
                    m_phase = 2;
                    return false;
                case 2: {
                    Entity lift = Find(ctx, "Elevator"), bob = Find(ctx, "SpringBob");
                    if (const Joint* j = lift.Valid() ? lift.TryGet<Joint>() : nullptr) {
                        const float p = Physics::GetJointState(j->runtime).position;
                        m_scratch[1] = std::min(m_scratch[1], p);
                        m_scratch[2] = std::max(m_scratch[2], p);
                    }
                    if (const Joint* j = bob.Valid() ? bob.TryGet<Joint>() : nullptr) {
                        const float p = Physics::GetJointState(j->runtime).position;
                        m_scratch[4] = std::min(m_scratch[4], p);
                        m_scratch[5] = std::max(m_scratch[5], p);
                    }
                    float worst = 0.0f;   // ball / hinge anchors must stay together
                    ctx.CurrentScene()->Each<Joint>([&](Entity, Joint& j) {
                        if (j.type != Physics::JointType::BallSocket && j.type != Physics::JointType::Hinge) return;
                        const Physics::JointState s = Physics::GetJointState(j.runtime);
                        worst = std::max(worst, glm::length(s.anchorA - s.anchorB));
                    });
                    m_scratch[3] = std::max(m_scratch[3], worst);
                    if (StepSimTime(ctx) - m_scratch[0] < 7.0) return false;
                    const Physics::Stats ps = Physics::GetStats();
                    Expect("9 joints created from Joint components", ps.joints == 9, F("%.0f joints on ", ps.joints) + Physics::BackendName());
                    Expect("ball / hinge anchors stay together under load", m_scratch[3] < 0.03f, F("max anchor separation %.4f m", m_scratch[3]));
                    Entity rotor = Find(ctx, "Windmill");
                    const Joint* motor = rotor.Valid() ? rotor.TryGet<Joint>() : nullptr;
                    const float speed = motor ? Physics::GetJointState(motor->runtime).speed : 0.0f;
                    Expect("motorised hinge spins at its target speed", std::fabs(std::fabs(speed) - 2.5f) < 0.25f, F("%.3f rad/s (target 2.5)", speed));
                    Expect("Lua drives the prismatic lift up and down between its limits", m_scratch[1] < 0.3f && m_scratch[2] > 1.9f && m_scratch[2] < 2.3f,
                           F("travel %.3f .. %.3f m (limits 0 .. 2.2)", m_scratch[1], m_scratch[2]));
                    Expect("spring-damper bob oscillates", m_scratch[5] - m_scratch[4] > 0.1f, F("length %.3f .. %.3f m", m_scratch[4], m_scratch[5]));
                    const Scripting::Stats s = Scripting::GetStats();
                    Expect("Lua scripts running without errors", s.scripts == 4 && s.running == 4 && s.errors == 0 && s.callbacks > 500,
                           F("%.0f scripts, %.0f running, %.0f errors, %.0f callbacks", s.scripts, s.running, s.errors, double(s.callbacks)) + (s.lastError.empty() ? "" : " last: " + s.lastError));
                    const Entity spinner = Find(ctx, "Spinner");
                    const float yaw = spinner.Valid() ? glm::degrees(glm::angle(spinner.WorldRotation())) : 0.0f;
                    Expect("file script (scripts/spinner.lua) rotates its kinematic body", yaw > 5.0f, F("rotated %.1f deg", yaw));
                    ctx.eventLog.clear();
                    ctx.input.InjectPress(Action::Interact);
                    m_scratch[0] = float(StepSimTime(ctx));
                    m_phase = 3;
                    return false;
                }
                default: {
                    bool hit = false;
                    for (const std::string& e : ctx.eventLog) hit |= e.find("bumper hit") != std::string::npos;
                    const bool spawned = Find(ctx, "Ball").Valid();
                    if (!hit && StepSimTime(ctx) - m_scratch[0] < 3.0) return false;
                    Expect("input -> Lua spawns a ball; its collision runs the bumper's on_collision", spawned && hit,
                           std::string(spawned ? "ball spawned" : "no ball") + (hit ? ", bumper hit event" : ", no hit event"));
                    Screenshot("p9_06_joints");
                    return true;
                }
            }
        }, 20.0 });

        m_steps.push_back({ "Scene transition back to Sandbox", [this](GameContext& ctx) {
            if (m_phase == 0) { ctx.scenes->Request("Sandbox"); m_phase = 1; return false; }
            if (ctx.scenes->CurrentName() != "Sandbox") return false;
            if (m_phase == 1) { m_scratch[0] = float(StepSimTime(ctx)); m_phase = 2; return false; }
            if (StepSimTime(ctx) - m_scratch[0] < 0.5) return false;
            Expect("entities equal first load", EntityCount(ctx) == m_baseline.entities, F("%.0f vs %.0f", double(EntityCount(ctx)), double(m_baseline.entities)));
            Expect("rigid dynamics equal first load", int(Physics::GetStats().dynamicBodies) == m_baseline.rigidDynamics, F("%.0f vs %.0f", int(Physics::GetStats().dynamicBodies), m_baseline.rigidDynamics));
            Expect("rigid statics equal first load", int(Physics::GetStats().staticBodies) == m_baseline.rigidStatics, F("%.0f vs %.0f", int(Physics::GetStats().staticBodies), m_baseline.rigidStatics));
            const RenderStats render = GetRenderStats();
            Expect("render objects equal first load", render.meshObjects == m_baseline.gameObjects, F("%.0f vs %.0f", render.meshObjects, double(m_baseline.gameObjects)));
            Expect("lights equal first load", render.lights == m_baseline.lights, F("%.0f vs %.0f", render.lights, double(m_baseline.lights)));
            Expect("character controllers equal first load", int(Physics::GetStats().characters) == m_baseline.characterControllers, F("%.0f", int(Physics::GetStats().characters)));
            const Physics::Advanced::Stats gpu = Physics::Advanced::GetStats();
            Expect("GPU particles and soft bodies released with FieldLab", gpu.particles == 0 && gpu.softBodies == 0 && gpu.particleSystems == 0,
                   F("%.0f particles, %.0f soft bodies", gpu.particles, gpu.softBodies));
            return true;
        } });

        // 12. Pause freezes simulation; resume continues it.
        m_steps.push_back({ "Pause and resume", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.input.InjectPress(Action::Pause); m_scratch[0] = float(m_stepFrames); m_phase = 1; return false;
                case 1:
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    Expect("Pause action enters Paused state", ctx.state == GameState::Paused, ToString(ctx.state));
                    m_scratch[1] = float(ctx.stats.simulationTime);
                    m_scratch[0] = float(m_stepFrames); m_phase = 2; return false;
                case 2:
                    if (m_stepFrames - m_scratch[0] < 15) return false;
                    Expect("no simulation while paused", std::abs(float(ctx.stats.simulationTime) - m_scratch[1]) < 1e-6f, F("sim time %.4f -> %.4f over 15 frames", m_scratch[1], ctx.stats.simulationTime));
                    Screenshot("09_paused");
                    ctx.input.InjectPress(Action::Pause);
                    m_scratch[0] = float(m_stepFrames); m_scratch[3] = 0.0f; m_phase = 3; return false;
                default:
                    // Measure from the frame the game actually resumed, in real time too: at high frame
                    // rates 6 frames can be shorter than one fixed step. Give up after 600 frames.
                    if (ctx.state != GameState::Running) { m_scratch[3] = 0.0f; if (m_stepFrames - m_scratch[0] < 600) return false; }
                    else if (m_scratch[3] == 0.0f) { m_scratch[3] = 1.0f; m_scratch[0] = float(m_stepFrames); m_wallMark = Time::NowSeconds(); }
                    if (m_stepFrames - m_scratch[0] < 600 && (m_stepFrames - m_scratch[0] < 6 || Time::NowSeconds() - m_wallMark < 0.1)) return false;
                    Expect("resume continues simulation", ctx.state == GameState::Running && ctx.stats.simulationTime > m_scratch[1] + 0.01,
                           std::string(ToString(ctx.state)) + F(", sim time %.4f -> %.4f", m_scratch[1], ctx.stats.simulationTime));
                    return true;
            }
        } });
    }
}
