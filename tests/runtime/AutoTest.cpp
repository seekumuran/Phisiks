#include "AutoTest.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "SandboxScenes.h"
#include "FieldLab.h"
#include "MarinaScene.h"
#include "OldMcDonaldScene.h"
#include "RoverCadScene.h"
#include "DroneCadScene.h"
#include "CadWorkspace.h"
#include "Editor.h"
#include "ProjectKit.h"
#include "SceneBuilder.h"
#include "P9Scenes.h"
#include "Showcase.h"
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
#include <imgui.h>

#include <filesystem>

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

        using namespace Samples::Layout;
        using Samples::PhysicsTowerScene;

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
        // Frame cap scales with the step's sim-time budget: fast frames (e.g. the OpenGL 4.1 tier at
        // ~10-20 ms) must not end a long step (the Old-McDonald mission needs ~45 s of simulation).
        if (!done && (StepSimTime(ctx) > step.timeoutSimSeconds || m_stepFrames > std::max(2000.0, step.timeoutSimSeconds * 100.0))) {
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
        out << "PHISIKS runtime tests - automated runtime verification report\n";
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
                   cache.programs == (compat ? 29u : 46u) && cache.failures == 0 && cache.loadedFromCache + cache.compiled == cache.programs && cache.written == cache.compiled,
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
                case 0: Samples::SetQuadView(scene, true); m_scratch[0] = float(m_stepFrames); m_phase = 1; return false;
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
                    Samples::SetQuadView(scene, false);
                    m_scratch[0] = float(m_stepFrames); m_phase = 3; return false;
                }
                default:
                    if (m_stepFrames - m_scratch[0] < 2) return false;
                    Expect("single view restored", GetRenderStats().views == 1 && !Samples::IsQuadView(scene), F("%.0f views", GetRenderStats().views));
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
                    Samples::SceneBuilder::AddStaticBox(scene, "OcclusionProbe", glm::vec3(0.0f, 0.4f, WALL_CENTER.z - 3.0f), glm::vec3(0.8f), "Sandbox/Ground");
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
                using namespace Samples::FieldLayout;
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
            using namespace Samples::FieldLayout;
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
                    Samples::FieldLabScene::ApplySdfSculpture(*ctx.CurrentScene());
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
                    m_scratch[4] = float(replay.At(replay.Size() - 1).step);   // newest step before scrubbing back
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
                    m_scratch[5] = float(ctx.stats.totalFixedSteps);
                    m_phase = 4;
                    return false;
                }
                case 4: {
                    if (m_stepFrames - m_scratch[3] < 10) return false;
                    {
                        // Counted in fixed steps, not frames: a slow renderer (llvmpipe) runs several steps per frame.
                        // Kept timeline: the snapshot after the restored one would be the old next step; discarded: it is
                        // a step recorded after resuming, newer than everything that was dropped.
                        const size_t base = size_t(m_scratch[2]);
                        const double stepsSince = double(ctx.stats.totalFixedSteps) - double(m_scratch[5]);
                        const double expected = double(base) + 1.0 + stepsSince;
                        const bool fresh = ctx.replay.Size() > base + 1 && double(ctx.replay.At(base + 1).step) > double(m_scratch[4]);
                        Expect("resuming discards the newer timeline", fresh && std::abs(double(ctx.replay.Size()) - expected) <= 1.0 && !ctx.replay.Scrubbed(),
                               F("%.0f snapshots after resuming from %.0f", double(ctx.replay.Size()), double(base)) + F(" (%.0f steps since; ", stepsSince) +
                               (fresh ? std::string("next snapshot is new)") : std::string("next snapshot is from the old timeline)")));
                    }
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
        // P9a. Editor skin (industrial console): docked panels, LCD, LED meters, EQ sliders, tool bar
        // transport, sample browser playlist; the bitmap HUD telemetry is suppressed while the panels
        // are open (and returns when hidden).
        m_steps.push_back({ "Editor: console skin, zero HUD overlap", [this](GameContext& ctx) {
            namespace Retro = UI::Retro;
            switch (m_phase) {
                case 0:
                    UI::SetPanelsVisible(true);
                    Editor::ShowOutputTab(Editor::OutputTab::Telemetry);
                    if (m_stepFrames < 3) return false;
                    m_backbufferRequested = true;
                    m_backbufferReady = false;
                    m_phase = 1;
                    return false;
                case 1: {
                    if (!m_backbufferReady) return false;
                    const Retro::FrameStats& s = Retro::LastFrameStats();
                    Expect("editor panels drawn (bevelled, amber title stripes): scene, side, inspector, output", s.panels >= 4, F("%.0f panels", s.panels));
                    Expect("LCD readout, LED meters and spectrum analyser", s.lcds >= 1 && s.meters >= 4 && s.litSegments > 0,
                           F("%.0f LCDs, %.0f meters, %.0f lit segments", s.lcds, s.meters, s.litSegments));
                    Expect("equaliser sliders, tool bar transport, sample browser", s.eqSliders >= 6 && s.transportButtons >= 4 && s.playlistRows >= 11,
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
                    Editor::ShowOutputTab(Editor::OutputTab::Console);
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
                    std::string source = Assets::ResolveDirectory("engine/shaders");   // repository
                    if (source.empty()) source = Assets::ResolveDirectory("shaders");    // release package
                    // `recursive`: for a directory source the standard copies its files only with recursive (or
                    // none); libstdc++ follows that strictly, so overwrite_existing alone copied nothing on Linux.
                    fs::copy(source, dir, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
                    std::ifstream in(tonemap, std::ios::binary);
                    std::stringstream text;
                    text << in.rdbuf();
                    m_shaderOriginal = text.str();
                    const bool enabled = !m_shaderOriginal.empty() && Render::EnableShaderHotReload(dir.string());
                    Expect("watching a copy of engine/shaders", enabled && Render::GetShaderReloadStatus().filesOverridden == 0,
                           dir.string() + F(" (%.0f files overridden: identical to the embedded sources)", Render::GetShaderReloadStatus().filesOverridden));
                    m_scratch[0] = float(status.reloads);
                    if (!enabled || m_shaderOriginal.rfind('}') == std::string::npos) return true;   // reported above; nothing to edit
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
                    const Samples::TerrainScene* scene = dynamic_cast<Samples::TerrainScene*>(ctx.CurrentScene());
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

        m_steps.push_back({ "P10: the four showcase scenes hosted in the editor application", [this](GameContext& ctx) {
            const auto& demos = Showcase::Demos();
            const size_t i = size_t(m_phase / 2);
            if (i >= demos.size()) {
                Expect("all four showcase scenes load, run and unload in the Lab", m_scratch[1] == float(demos.size()) && Render::GetGLErrorCount() == 0,
                       F("%.0f of %.0f showcases ran 40 frames, ", m_scratch[1], double(demos.size())) + F("%.0f GL errors", Render::GetGLErrorCount()));
                return true;
            }
            if (m_phase % 2 == 0) { if (i == 0) m_scratch[1] = 0.0f; ctx.scenes->Request(demos[i].scene); m_stepFrames = 0; m_scratch[0] = 0; ++m_phase; return false; }
            if (ctx.scenes->CurrentName() != demos[i].scene) return false;
            if (++m_scratch[0] < 40) return false;
            if (Showcase::Current(ctx) && ctx.stats.fixedStepsThisFrame >= 0) m_scratch[1] += 1.0f;
            Screenshot(std::string("p10_") + demos[i].scene);
            ++m_phase;
            return false;
        }, 120.0 });

        // Project Marina: the engineering scene is made of selectable, editable entities, every tube
        // and wire run is continuous, and the project survives a save to disk + reopen unchanged.
        m_steps.push_back({ "Project Marina: editable engineering scene, save and reopen", [this](GameContext& ctx) {
            auto* marina = dynamic_cast<Projects::MarinaScene*>(ctx.CurrentScene());
            auto writeBackbuffer = [&](const std::string& name) {   // what the window shows, studio UI included
                const Render::FrameCapture& b = m_backbuffer;
                Assets::Image shot;
                shot.width = b.width;
                shot.height = b.height;
                shot.rgba.resize(b.rgba.size());
                for (int y = 0; y < b.height; ++y) std::copy_n(&b.rgba[size_t(b.height - 1 - y) * b.width * 4], size_t(b.width) * 4, &shot.rgba[size_t(y) * b.width * 4]);
                if (Assets::WritePng(m_runDirectory + "/" + name + ".png", shot)) m_writtenScreenshots.push_back(m_runDirectory + "/" + name + ".png");
            };
            auto requestBackbuffer = [&] { m_backbufferRequested = true; m_backbufferReady = false; };
            auto worstGap = [&](size_t& runs) {
                float worst = 0.0f;
                runs = 0;
                for (const auto& route : marina->Routes()) { worst = std::max(worst, marina->RouteGap(route.group)); ++runs; }
                return worst;
            };
            switch (m_phase) {
                case 0: ctx.scenes->Request("ProjectMarina"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "ProjectMarina" || !marina) return false;
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 30) return false;
                    const char* subsystems[] = { "Project Marina", "0 Bench", "1 Computing Unit - SBC", "2 Cooling Circuit", "3 Electronics",
                                                 "4 Pump Assembly - P1", "5 Heat Exchanger - HX1", "6 Membrane Distillation - MD1",
                                                 "7 Seawater Reservoir - TK1", "8 Condensation System", "9 Sensor Wiring", "Process Tubing" };
                    int found = 0;
                    for (const char* name : subsystems) if (Find(ctx, name)) ++found;
                    Expect("all subsystem groups are in the hierarchy", found == int(std::size(subsystems)), F("%.0f of %.0f", found, double(std::size(subsystems))));
                    const RenderStats r = GetRenderStats();
                    Expect("every part mesh loaded into a render object", r.meshObjects == CountRenderedEntities(ctx) && r.meshObjects > 250,
                           F("%.0f render objects, %.0f MeshRenderer entities", r.meshObjects, double(CountRenderedEntities(ctx))));
                    size_t runs = 0;
                    const float gap = worstGap(runs);
                    Expect("tubing and wiring runs are continuous (piece to piece)", runs >= 20 && gap < 1e-4f, F("%.0f runs, worst gap %.2e m", double(runs), gap));
                    RequestCapture();
                    m_phase = 3;
                    return false;
                }
                case 3: {
                    if (!m_captureReady) return false;
                    const Entity root = Find(ctx, "Project Marina");
                    std::vector<uint32_t> ids(m_capture.entityIds);
                    std::sort(ids.begin(), ids.end());
                    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                    int parts = 0;
                    for (uint32_t id : ids) if (id && ctx.CurrentScene()->FromId(id - 1).IsDescendantOf(root)) ++parts;
                    Expect("parts are individually pickable (entity id buffer)", parts >= 60, F("%.0f distinct Marina parts in view", parts));
                    requestBackbuffer();
                    m_phase = 4;
                    return false;
                }
                case 4: {
                    if (!m_backbufferReady) return false;
                    writeBackbuffer("marina_studio");
                    Scene& scene = *ctx.CurrentScene();
                    const std::string path = m_runDirectory + "/project_marina.json";
                    const std::string before = SceneSerializer::ToJson(scene);
                    const size_t entities = scene.EntityCount();
                    std::string error;
                    const bool saved = SceneSerializer::Save(scene, path);
                    const bool loaded = saved && SceneSerializer::Load(scene, path, &error);
                    const std::string after = SceneSerializer::ToJson(scene);
                    Expect("project saves to disk and reopens unchanged", saved && loaded && after == before && scene.EntityCount() == entities,
                           F("%.0f entities, %.0f bytes", double(entities), double(before.size())) + (error.empty() ? "" : ", " + error));
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 5;
                    return false;
                }
                case 5: {
                    if (m_stepFrames - m_scratch[0] < 5) return false;
                    const RenderStats r = GetRenderStats();
                    size_t runs = 0;
                    const float gap = worstGap(runs);
                    Expect("reopened project rebuilt its parts and runs", r.meshObjects == CountRenderedEntities(ctx) && runs >= 20 && gap < 1e-4f,
                           F("%.0f render objects, worst run gap %.2e m", r.meshObjects, gap));
                    const Render::MaterialDesc* tube = Render::FindMaterial("Marina/ClearTube");
                    Expect("see-through materials keep their opacity after reopening", tube && tube->opacity < 0.5f, F("clear tubing opacity %.2f", tube ? tube->opacity : -1.0f));
                    UI::SetPanelsVisible(false);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 6;
                    return false;
                }
                case 6:
                    if (m_stepFrames - m_scratch[0] < 3) return false;
                    requestBackbuffer();
                    m_phase = 7;
                    return false;
                case 7:
                    if (!m_backbufferReady) return false;
                    writeBackbuffer("marina_presentation_mode");
                    Expect("no text labels, callouts or overlays over the scene (panels hidden)", UI::LastDrawVertexCount() == 0 && GetRenderStats().overlayGlyphs == 0,
                           F("%.0f UI vertices, ", double(UI::LastDrawVertexCount())) + F("%.0f bitmap glyphs", double(GetRenderStats().overlayGlyphs)));
                    UI::SetPanelsVisible(true);
                    return true;
            }
            return false;
        } });

        // Old-McDonald: a second engineering project on the same scene / studio systems. Every part
        // is a pickable entity, presets frame what they claim, edits survive save + reopen, and the
        // mission sequence runs in order (rover routed by the engine's A*, arm placed by IK).
        m_steps.push_back({ "Old-McDonald: agricultural robotics project, edit, save, mission", [this](GameContext& ctx) {
            auto* omd = dynamic_cast<Projects::OldMcDonaldScene*>(ctx.CurrentScene());
            using OMD = Projects::OldMcDonaldScene;
            auto writeBackbuffer = [&](const std::string& name) {
                const Render::FrameCapture& b = m_backbuffer;
                Assets::Image shot;
                shot.width = b.width;
                shot.height = b.height;
                shot.rgba.resize(b.rgba.size());
                for (int y = 0; y < b.height; ++y) std::copy_n(&b.rgba[size_t(b.height - 1 - y) * b.width * 4], size_t(b.width) * 4, &shot.rgba[size_t(y) * b.width * 4]);
                if (Assets::WritePng(m_runDirectory + "/" + name + ".png", shot)) m_writtenScreenshots.push_back(m_runDirectory + "/" + name + ".png");
            };
            auto requestBoth = [&] { m_backbufferRequested = true; m_backbufferReady = false; RequestCapture(); };
            auto partsInView = [&](const char* ancestor) {
                const Entity root = Find(ctx, ancestor);
                std::vector<uint32_t> ids(m_capture.entityIds);
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                int n = 0;
                for (uint32_t id : ids) if (id && root && ctx.CurrentScene()->FromId(id - 1).IsDescendantOf(root)) ++n;
                return n;
            };
            const int presets[] = { OMD::DroneClose, OMD::RoverClose, OMD::Overview, OMD::TopPlan, OMD::DroneExploded, OMD::RoverExploded };
            const char* shots[] = { "omd_drone_close", "omd_rover_close", "omd_overview", "omd_top_plan", "omd_drone_exploded", "omd_rover_exploded" };
            switch (m_phase) {
                case 0: ctx.scenes->Request("OldMcDonald"); m_phase = 1; return false;
                case 1:
                    if (ctx.scenes->CurrentName() != "OldMcDonald" || !omd) return false;
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                case 2: {
                    if (m_stepFrames - m_scratch[0] < 40) return false;
                    const char* groups[] = { "Old-McDonald", "1 Greenhouse", "2 Growing Beds", "3 Mapping Drone", "4 Soil Rover", "5 Sampling Grid", "6 Mission Route",
                                             "Sensor-positioning arm", "LiDAR", "Soil sensor head", "Flight controller - Pixhawk 2.4.8", "GPS - ReadyToSky", "Raspberry Pi Pico",
                                             "Soil moisture sensor", "Soil temperature sensor", "Soil pH sensor" };
                    int found = 0;
                    for (const char* g : groups) if (Find(ctx, g)) ++found;
                    Expect("project hierarchy: subsystems and named hardware present", found == int(std::size(groups)), F("%.0f of %.0f", found, double(std::size(groups))));
                    int samples = 0;
                    for (char row : { 'A', 'B', 'C', 'D' }) for (int c = 1; c <= 4; ++c) if (Find(ctx, std::string("SP-") + row + std::to_string(c))) ++samples;
                    Expect("fixed sampling grid: 16 individually named points", samples == 16, F("%.0f sampling points", samples));
                    const RenderStats r = GetRenderStats();
                    Expect("every part mesh loaded into a render object", r.meshObjects == CountRenderedEntities(ctx) && r.meshObjects > 300,
                           F("%.0f render objects, %.0f MeshRenderer entities", r.meshObjects, double(CountRenderedEntities(ctx))));
                    float worst = 0.0f;
                    int runs = 0;
                    for (const char* parent : { "Drone wiring", "Rover wiring", "ESC leads 1", "ESC leads 2", "ESC leads 3", "ESC leads 4" })
                        if (Entity p = Find(ctx, parent)) for (Entity run : p.GetChildren()) {
                            if (run.GetName().rfind("ESC leads", 0) == 0) continue;   // sub-groups of "Drone wiring", listed separately
                            worst = std::max(worst, Projects::ProjectKit::RunGap(*ctx.CurrentScene(), run.GetName()));
                            ++runs;
                        }
                    Expect("wiring runs are continuous (piece to piece)", runs >= 30 && worst < 1e-4f, F("%.0f runs, worst gap %.2e m", runs, worst));
                    AI::NavGrid* grid = AI::SceneGrid(*ctx.CurrentScene());
                    Expect("navigation grid built from the scene colliders; rover route planned by A*", grid && grid->Valid() && omd->RouteBuilt(),
                           grid ? F("%.0f walkable cells", double(grid->WalkableCells())) : std::string("no grid"));
                    {
                        // Scene format 2 stores NavigationSurface + NavAgent: nothing in the project is lost on save.
                        const size_t unstored = SceneSerializer::UnsavedComponents(*ctx.CurrentScene()).size();
                        const size_t lost = Editor::ComponentsLostOnSave(ctx).size();
                        Expect("scene format 2: the project's navigation components are stored (no save warning)", unstored == 0 && lost == 0,
                               F("%.0f unstored types, ", double(unstored)) + F("%.0f reported", double(lost)));
                    }
                    // the project as built (assembled, idle): kept as the run's reference scene file
                    Expect("project scene file written", SceneSerializer::Save(*ctx.CurrentScene(), m_runDirectory + "/old_mcdonald.scene.json"), m_runDirectory + "/old_mcdonald.scene.json");
                    requestBoth();
                    m_phase = 3;
                    return false;
                }
                case 3: {
                    if (!m_backbufferReady || !m_captureReady) return false;
                    writeBackbuffer("omd_presentation");
                    Expect("presentation view: parts individually pickable", partsInView("Old-McDonald") >= 80, F("%.0f distinct project parts in view", partsInView("Old-McDonald")));
                    m_scratch[1] = 0.0f;   // preset index
                    m_phase = 4;
                    return false;
                }
                case 4:
                    omd->ApplyCamera(ctx, presets[int(m_scratch[1])]);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 5;
                    return false;
                case 5:
                    if (m_stepFrames - m_scratch[0] < 90) return false;
                    requestBoth();
                    m_phase = 6;
                    return false;
                case 6: {
                    if (!m_backbufferReady || !m_captureReady) return false;
                    const int i = int(m_scratch[1]);
                    writeBackbuffer(shots[i]);
                    const int preset = presets[i];
                    if (preset == OMD::DroneClose) Expect("drone close-up: drone components individually pickable", partsInView("3 Mapping Drone") >= 15, F("%.0f drone parts", partsInView("3 Mapping Drone")));
                    if (preset == OMD::RoverClose) Expect("rover close-up: rover components individually pickable", partsInView("4 Soil Rover") >= 20, F("%.0f rover parts", partsInView("4 Soil Rover")));
                    if (preset == OMD::Overview) Expect("overview: greenhouse, beds, drone and rover in view", partsInView("1 Greenhouse") > 0 && partsInView("2 Growing Beds") > 20 && partsInView("3 Mapping Drone") > 0 && partsInView("4 Soil Rover") > 0,
                                                       F("%.0f bed parts", partsInView("2 Growing Beds")));
                    if (preset == OMD::TopPlan) {
                        const Entity cam = Find(ctx, "MainCamera");
                        // The disc marks each point; the 8 mm stake can fall below a pixel at low render resolutions.
                        int discs = 0;
                        {
                            std::vector<uint32_t> ids(m_capture.entityIds);
                            std::sort(ids.begin(), ids.end());
                            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                            const Entity grid = Find(ctx, "5 Sampling Grid");
                            for (uint32_t id : ids) {
                                if (!id || !grid) continue;
                                const Entity e = ctx.CurrentScene()->FromId(id - 1);
                                const std::string& n = e.GetName();
                                if (e.IsDescendantOf(grid) && n.size() > 5 && n.compare(n.size() - 5, 5, " disc") == 0) ++discs;
                            }
                        }
                        Expect("top plan: orthographic, all 16 sampling points visible", cam && cam.Get<Camera>().projection == Projection::Orthographic && discs == 16,
                               F("%.0f of 16 sampling discs in view", discs) + F(" (%.0f grid parts incl. stakes)", partsInView("5 Sampling Grid")));
                    }
                    if (preset == OMD::DroneExploded) {
                        Entity fc = Find(ctx, "Flight controller - Pixhawk 2.4.8"), bat = Find(ctx, "Battery - LiPo");
                        Expect("exploded drone: flight controller up, battery down", omd->Explode(OMD::DroneAssembly) > 0.99f && fc.LocalTransform().position.y > 0.12f && bat.LocalTransform().position.y < -0.19f,
                               F("explode %.2f, FC +%.3f m, battery %.3f m", omd->Explode(OMD::DroneAssembly), fc.LocalTransform().position.y, bat.LocalTransform().position.y));
                    }
                    if (preset == OMD::RoverExploded) {
                        Entity lidar = Find(ctx, "LiDAR");
                        Expect("exploded rover: assembly separated, drone re-assembled", omd->Explode(OMD::RoverAssembly) > 0.99f && omd->Explode(OMD::DroneAssembly) < 0.01f && lidar.LocalTransform().position.y > 0.4f,
                               F("rover explode %.2f, drone explode %.2f", omd->Explode(OMD::RoverAssembly), omd->Explode(OMD::DroneAssembly)) + F(", LiDAR at %.3f m", lidar.LocalTransform().position.y));
                    }
                    m_scratch[1] += 1.0f;
                    m_phase = int(m_scratch[1]) < int(std::size(presets)) ? 4 : 7;
                    return false;
                }
                case 7: omd->ApplyCamera(ctx, OMD::Presentation); m_scratch[0] = float(m_stepFrames); m_phase = 8; return false;
                case 8:
                    if (m_stepFrames - m_scratch[0] < 90) return false;
                    Projects::ProjectKit::SetVisible(Find(ctx, "3 Mapping Drone"), false);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 9;
                    return false;
                case 9: if (m_stepFrames - m_scratch[0] < 3) return false; RequestCapture(); m_phase = 10; return false;
                case 10: {
                    if (!m_captureReady) return false;
                    const int droneParts = partsInView("3 Mapping Drone");
                    Projects::ProjectKit::SetVisible(Find(ctx, "3 Mapping Drone"), true);
                    int toggled = 0;
                    for (const char* g : { "1 Greenhouse", "2 Growing Beds", "3 Mapping Drone", "4 Soil Rover", "Sensor-positioning arm", "LiDAR", "Soil sensor head", "5 Sampling Grid", "6 Mission Route" }) {
                        Entity e = Find(ctx, g);
                        Projects::ProjectKit::SetVisible(e, false);
                        const bool hidden = !Projects::ProjectKit::AnyVisible(e);
                        Projects::ProjectKit::SetVisible(e, true);
                        if (hidden && Projects::ProjectKit::AnyVisible(e)) ++toggled;
                    }
                    Expect("subsystem visibility: hidden drone leaves the image; 9 subsystems toggle", droneParts == 0 && toggled == 9, F("%.0f drone parts while hidden, ", droneParts) + F("%.0f of 9 toggled", toggled));
                    m_phase = 11;
                    return false;
                }
                case 11: {   // edit a transform, a scale and a material, save, reopen
                    Scene& scene = *ctx.CurrentScene();
                    Find(ctx, "Flight controller - Pixhawk 2.4.8").LocalTransform().position += glm::vec3(0.0f, 0.01f, 0.0f);
                    Find(ctx, "Dock marker post").LocalTransform().scale = glm::vec3(1.25f);
                    Render::MaterialDesc red = *Render::FindMaterial("OMD/MotorRed");
                    red.baseColor = glm::vec3(0.1f, 0.3f, 0.7f);
                    Render::RegisterMaterial("OMD/MotorRed", red);
                    const std::string path = m_runDirectory + "/old_mcdonald_edit_test.scene.json";
                    const std::string before = SceneSerializer::ToJson(scene);
                    const size_t entities = scene.EntityCount();
                    std::string error;
                    const bool saved = SceneSerializer::Save(scene, path);
                    Render::RegisterMaterial("OMD/MotorRed", Render::MaterialDesc());   // wiped: must come back from the file
                    const bool loaded = saved && SceneSerializer::Load(scene, path, &error);
                    const std::string after = SceneSerializer::ToJson(scene);
                    const Render::MaterialDesc* arm = Render::FindMaterial("OMD/MotorRed");
                    const Render::MaterialDesc* glass = Render::FindMaterial("OMD/Glass");
                    Entity fc = Find(ctx, "Flight controller - Pixhawk 2.4.8"), post = Find(ctx, "Dock marker post");
                    Expect("edits survive save + reopen (transform, scale, material, glazing opacity)",
                           loaded && after == before && scene.EntityCount() == entities && std::abs(fc.LocalTransform().position.y - 0.01f) < 1e-5f &&
                               std::abs(post.LocalTransform().scale.x - 1.25f) < 1e-5f && arm && std::abs(arm->baseColor.z - 0.7f) < 1e-5f && glass && glass->opacity < 0.2f,
                           F("%.0f entities, %.0f bytes", double(entities), double(before.size())) + (error.empty() ? "" : ", " + error));
                    Find(ctx, "Flight controller - Pixhawk 2.4.8").LocalTransform().position -= glm::vec3(0.0f, 0.01f, 0.0f);
                    red.baseColor = glm::vec3(0.70f, 0.06f, 0.05f);
                    Render::RegisterMaterial("OMD/MotorRed", red);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 12;
                    return false;
                }
                case 12: {
                    if (m_stepFrames - m_scratch[0] < 10) return false;
                    AI::NavGrid* grid = AI::SceneGrid(*ctx.CurrentScene());
                    const Entity rover = Find(ctx, "4 Soil Rover");
                    const RenderStats r = GetRenderStats();
                    Expect("reopened project: runtime navigation re-attached, parts rebuilt", rover.Has<NavAgent>() && grid && grid->Valid() && r.meshObjects == CountRenderedEntities(ctx),
                           F("%.0f render objects", r.meshObjects) + (grid && grid->Valid() ? ", grid valid" : ", no grid"));
                    return true;
                }
            }
            return false;
        }, 120.0 });

        m_steps.push_back({ "Old-McDonald: mission playback (sequential drone then rover, A* route, IK arm)", [this](GameContext& ctx) {
            auto* omd = dynamic_cast<Projects::OldMcDonaldScene*>(ctx.CurrentScene());
            using OMD = Projects::OldMcDonaldScene;
            if (!omd) { Expect("Old-McDonald scene active", false, "not loaded"); return true; }
            auto writeBackbuffer = [&](const std::string& name) {
                const Render::FrameCapture& b = m_backbuffer;
                Assets::Image shot;
                shot.width = b.width;
                shot.height = b.height;
                shot.rgba.resize(b.rgba.size());
                for (int y = 0; y < b.height; ++y) std::copy_n(&b.rgba[size_t(b.height - 1 - y) * b.width * 4], size_t(b.width) * 4, &shot.rgba[size_t(y) * b.width * 4]);
                if (Assets::WritePng(m_runDirectory + "/" + name + ".png", shot)) m_writtenScreenshots.push_back(m_runDirectory + "/" + name + ".png");
            };
            switch (m_phase) {
                case 0:
                    omd->missionSpeed = 6.0f;
                    omd->ApplyCamera(ctx, OMD::MissionView);   // the Mission preset starts playback
                    m_scratch[2] = 0.0f;   // screenshot flags
                    m_phase = 13;
                    return false;
                case 13: {
                    if (m_backbufferReady) {
                        writeBackbuffer(int(m_scratch[2]) == 1 ? "omd_mission_survey" : "omd_mission_arm");
                        m_backbufferReady = false;
                    }
                    if (omd->Stage() == 2 && m_scratch[2] < 1.0f && omd->CropsMapped() > 10) { m_scratch[2] = 1.0f; m_backbufferRequested = true; }
                    if (omd->Stage() == 8 && m_scratch[2] < 2.0f && omd->SamplesTaken() >= 5) { m_scratch[2] = 2.0f; m_backbufferRequested = true; }
                    if (omd->GetPhase() != OMD::Phase::Docked) return false;
                    int stages = 0;
                    for (int s = 1; s <= 10; ++s) if ((omd->StagesSeen() >> s) & 1) ++stages;
                    const glm::vec3 rover = Find(ctx, "4 Soil Rover").WorldPosition(), drone = Find(ctx, "3 Mapping Drone").WorldPosition();
                    Expect("mission: all 9 stages run in order to completion", stages == 10, F("%.0f of 10 stage states seen", stages));
                    Expect("mission: drone mapped every crop location, then landed before the rover left", omd->CropsMapped() == 48 && omd->DroneLandedBeforeRover() && drone.y < 0.05f,
                           F("%.0f of 48 crops mapped, drone at y %.3f", omd->CropsMapped(), drone.y));
                    Expect("mission: rover sampled all 16 points via A* paths and returned to the dock", omd->SamplesTaken() == 16 && omd->PathFailures() == 0 && glm::length(glm::vec2(rover.x + 2.5f, rover.z + 0.7f)) < 0.1f,
                           F("%.0f samples, %.0f path failures, ", omd->SamplesTaken(), omd->PathFailures()) + F("dock offset %.3f m", glm::length(glm::vec2(rover.x + 2.5f, rover.z + 0.7f))));
                    UI::SetPanelsVisible(false);
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 14;
                    return false;
                }
                case 14: if (m_stepFrames - m_scratch[0] < 3) return false; m_backbufferRequested = true; m_backbufferReady = false; m_phase = 15; return false;
                case 15:
                    if (!m_backbufferReady) return false;
                    writeBackbuffer("omd_presentation_mode");
                    Expect("no text labels, callouts or overlays over the scene (panels hidden)", UI::LastDrawVertexCount() == 0 && GetRenderStats().overlayGlyphs == 0,
                           F("%.0f UI vertices, ", double(UI::LastDrawVertexCount())) + F("%.0f bitmap glyphs", double(GetRenderStats().overlayGlyphs)));
                    UI::SetPanelsVisible(true);
                    return true;
            }
            return false;
        }, 600.0 });

        // Old-McDonald standalone CAD workspaces (rover, drone; Projects::CadWorkspace): the same assembly as the
        // mission scene, isolated in a neutral studio; seven views, per-part picking, selection, explode,
        // hide / show, transform + material edit, save + reopen. One parametrised step per workspace.
        struct CadCase {
            std::string title, scene, root, label, shotPrefix;
            std::vector<std::string> named;          // groups / parts that must exist (shared hierarchy)
            std::vector<std::string> wiringGroups;   // direct children are runs ("ESC leads" sub-groups skipped)
            int minParts = 0, minRuns = 0, minViewParts = 0, minBottomParts = 0;
            std::string hideGroup, hidePart, editPart, editMaterial, exploded, collapsed;
            std::function<bool(GameContext&, std::string&)> explodedOk, collapsedOk;
        };
        auto cadStep = [this](CadCase c) {
            return [this, c](GameContext& ctx) {
                auto* cad = dynamic_cast<Projects::CadWorkspace*>(ctx.CurrentScene());
                using CW = Projects::CadWorkspace;
                auto writeBackbuffer = [&](const std::string& name) {
                    const Render::FrameCapture& b = m_backbuffer;
                    Assets::Image shot;
                    shot.width = b.width;
                    shot.height = b.height;
                    shot.rgba.resize(b.rgba.size());
                    for (int y = 0; y < b.height; ++y) std::copy_n(&b.rgba[size_t(b.height - 1 - y) * b.width * 4], size_t(b.width) * 4, &shot.rgba[size_t(y) * b.width * 4]);
                    if (Assets::WritePng(m_runDirectory + "/" + name + ".png", shot)) m_writtenScreenshots.push_back(m_runDirectory + "/" + name + ".png");
                };
                auto requestBoth = [&] { m_backbufferRequested = true; m_backbufferReady = false; RequestCapture(); };
                auto partsInView = [&](const std::string& ancestor) {
                    const Entity root = Find(ctx, ancestor);
                    std::vector<uint32_t> ids(m_capture.entityIds);
                    std::sort(ids.begin(), ids.end());
                    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                    int n = 0;
                    for (uint32_t id : ids) {
                        if (!id || !root) continue;
                        const Entity e = ctx.CurrentScene()->FromId(id - 1);
                        if (e == root || e.IsDescendantOf(root)) ++n;
                    }
                    return n;
                };
                const char* views[CW::ViewCount] = { "perspective", "front", "rear", "left", "right", "top", "bottom" };
                const std::string L = c.label;
                switch (m_phase) {
                    case 0: ctx.scenes->Request(c.scene); m_phase = 1; return false;
                    case 1:
                        if (ctx.scenes->CurrentName() != c.scene || !cad) return false;
                        m_scratch[0] = float(m_stepFrames);
                        m_phase = 2;
                        return false;
                    case 2: {
                        if (m_stepFrames - m_scratch[0] < 40) return false;
                        Scene& scene = *ctx.CurrentScene();
                        // isolation: nothing from the greenhouse scene or the other vehicle; every rendered part is the assembly or the ground grid
                        int foreign = 0;
                        for (const char* g : { "Old-McDonald", "1 Greenhouse", "2 Growing Beds", "3 Mapping Drone", "4 Soil Rover", "5 Sampling Grid", "6 Mission Route",
                                               "Rover dock", "Drone landing pad", "Floor slab", "Old-McDonald Soil Rover", "Old-McDonald Mapping Drone" })
                            if (c.root != g && Find(ctx, g)) ++foreign;
                        const Entity root = Find(ctx, c.root), ground = Find(ctx, "Ground grid");
                        int parts = 0, other = 0;
                        scene.Each<MeshRenderer>([&](Entity e, MeshRenderer&) { if (root && (e == root || e.IsDescendantOf(root))) ++parts; else if (!(e == ground)) ++other; });
                        Expect(L + " CAD: assembly isolated (no greenhouse, beds, dock, pad, other vehicle, route)", foreign == 0 && other == 0 && parts >= c.minParts,
                               F("%.0f assembly parts, ", parts) + F("%.0f unrelated scene objects, ", other) + F("%.0f foreign groups", foreign));
                        int found = 0;
                        for (const std::string& g : c.named) if (Find(ctx, g)) ++found;
                        Expect(L + " CAD: same assembly hierarchy as the mission scene", found == int(c.named.size()), F("%.0f of %.0f named groups / parts", found, double(c.named.size())));
                        const RenderStats r = GetRenderStats();
                        Expect(L + " CAD: every part mesh loaded into a render object", r.meshObjects == CountRenderedEntities(ctx), F("%.0f render objects", r.meshObjects));
                        float worst = 0.0f;
                        int runs = 0;
                        for (const std::string& g : c.wiringGroups)
                            if (Entity w = Find(ctx, g)) for (Entity run : w.GetChildren()) {
                                if (run.GetName().rfind("ESC leads", 0) == 0) continue;   // sub-group, listed separately
                                worst = std::max(worst, Projects::ProjectKit::RunGap(scene, run.GetName()));
                                ++runs;
                            }
                        Expect(L + " CAD: wiring runs continuous", runs >= c.minRuns && worst < 1e-4f, F("%.0f runs, worst gap %.2e m", runs, worst));
                        // selection (studio picking / inspector target) of a single component
                        Entity part = Find(ctx, c.editPart);
                        if (part) Editor::Select(part.Id() + 1);
                        Expect(L + " CAD: component selectable for the inspector", part && Editor::SelectedEntity() == part.Id() + 1 && part.Has<MeshRenderer>(), c.editPart);
                        Editor::Select(0);
                        const std::string file = m_runDirectory + "/" + c.shotPrefix + ".scene.json";
                        Expect(L + " CAD scene file written", SceneSerializer::Save(scene, file), file);
                        m_scratch[1] = 0.0f;   // view index
                        m_phase = 3;
                        return false;
                    }
                    case 3:
                        cad->ApplyView(ctx, int(m_scratch[1]));
                        m_scratch[0] = float(m_stepFrames);
                        m_phase = 4;
                        return false;
                    case 4:
                        if (m_stepFrames - m_scratch[0] < 30) return false;
                        requestBoth();
                        m_phase = 5;
                        return false;
                    case 5: {
                        if (!m_backbufferReady || !m_captureReady) return false;
                        const int v = int(m_scratch[1]);
                        writeBackbuffer(c.shotPrefix + "_" + views[v]);
                        const Entity cam = Find(ctx, "MainCamera");
                        const bool ortho = cam && cam.Get<Camera>().projection == Projection::Orthographic;
                        const int parts = partsInView(c.root);
                        const int need = v == CW::Bottom ? c.minBottomParts : c.minViewParts;   // from below the lowest parts hide the rest
                        Expect(L + " CAD view " + views[v] + ": components individually pickable", parts >= need && ortho == (v != CW::Perspective) && cad->CurrentView() == v,
                               F("%.0f distinct parts in view, ", parts) + (ortho ? "orthographic" : "perspective"));
                        m_scratch[1] += 1.0f;
                        m_phase = int(m_scratch[1]) < CW::ViewCount ? 3 : 6;
                        return false;
                    }
                    case 6:
                        cad->SetExplodeTarget(1.0f);
                        cad->ApplyView(ctx, CW::Perspective);
                        m_scratch[0] = float(m_stepFrames);
                        m_phase = 7;
                        return false;
                    case 7:
                        if (cad->Explode() < 0.999f || m_stepFrames - m_scratch[0] < 20) return false;
                        requestBoth();
                        m_phase = 8;
                        return false;
                    case 8: {
                        if (!m_backbufferReady || !m_captureReady) return false;
                        writeBackbuffer(c.shotPrefix + "_exploded");
                        std::string detail;
                        const bool ok = c.explodedOk(ctx, detail);
                        Expect(L + " CAD exploded view: " + c.exploded, ok, detail + F(", %.0f parts in view", partsInView(c.root)));
                        {   // Release audit B4: every save / load path keeps the explode state consistent.
                            const std::string file = m_runDirectory + "/" + c.shotPrefix + "_saved_exploded.scene.json";
                            std::string status, d1, d2, d3, d4;
                            const bool saved = Editor::SaveScene(ctx, file, &status);           // File > Save while exploded
                            const bool stillExploded = cad->Explode() > 0.999f && c.explodedOk(ctx, d1);
                            cad->SetExplodeNow(1.0f);
                            const bool loaded = Editor::LoadScene(ctx, file, &status);          // File > Open while exploded
                            const bool fileAssembled = loaded && cad->Explode() < 1e-6f && c.collapsedOk(ctx, d2);
                            cad->SetExplodeNow(1.0f);
                            const bool reExplodes = c.explodedOk(ctx, d3);
                            cad->SetExplodeNow(0.0f);
                            const bool reCollapses = c.collapsedOk(ctx, d4);
                            Expect(L + " CAD: File save / open while exploded keep the assembly intact (audit B4)", saved && stillExploded && fileAssembled && reExplodes && reCollapses,
                                   std::string(saved ? "saved assembled" : "save failed") + "; reopened: " + d2 + "; exploded again: " + d3 + "; collapsed: " + d4);
                            Expect(L + " CAD: replay timeline disabled in the workspace (audit B4)", !ctx.replay.Enabled() && ctx.replay.Size() == 0,
                                   F("recording %.0f, ", ctx.replay.Enabled() ? 1.0 : 0.0) + F("%.0f steps", double(ctx.replay.Size())));
                            cad->SetExplodeNow(1.0f);
                        }
                        cad->SetExplodeNow(0.0f);
                        std::string back;
                        const bool collapsed = c.collapsedOk(ctx, back);
                        Expect(L + " CAD: " + c.collapsed, collapsed, back);
                        // hide a subsystem and one individual component
                        Projects::ProjectKit::SetVisible(Find(ctx, c.hideGroup), false);
                        Projects::ProjectKit::SetVisible(Find(ctx, c.hidePart), false);
                        cad->ApplyView(ctx, CW::Perspective);
                        m_scratch[0] = float(m_stepFrames);
                        m_phase = 9;
                        return false;
                    }
                    case 9: if (m_stepFrames - m_scratch[0] < 5) return false; RequestCapture(); m_phase = 10; return false;
                    case 10: {
                        if (!m_captureReady) return false;
                        const int group = partsInView(c.hideGroup), single = partsInView(c.hidePart), rest = partsInView(c.root);
                        Projects::ProjectKit::SetVisible(Find(ctx, c.hideGroup), true);
                        Projects::ProjectKit::SetVisible(Find(ctx, c.hidePart), true);
                        Expect(L + " CAD: hidden subsystem and hidden component leave the image, the rest stays", group == 0 && single == 0 && rest >= c.minViewParts &&
                               Projects::ProjectKit::AnyVisible(Find(ctx, c.hideGroup)) && Projects::ProjectKit::AnyVisible(Find(ctx, c.hidePart)),
                               c.hideGroup + F(" %.0f, ", group) + c.hidePart + F(" %.0f, ", single) + F("other parts %.0f", rest));
                        m_phase = 11;
                        return false;
                    }
                    case 11: {   // edit a transform and a material, save, reopen
                        Scene& scene = *ctx.CurrentScene();
                        Find(ctx, c.editPart).LocalTransform().position += glm::vec3(0.0f, 0.02f, 0.0f);
                        Render::MaterialDesc mat = *Render::FindMaterial(c.editMaterial);
                        const glm::vec3 original = mat.baseColor;
                        mat.baseColor = glm::vec3(0.9f, 0.3f, 0.1f);
                        Render::RegisterMaterial(c.editMaterial, mat);
                        const std::string path = m_runDirectory + "/" + c.shotPrefix + "_edit_test.scene.json";
                        const std::string before = SceneSerializer::ToJson(scene);
                        const size_t entities = scene.EntityCount();
                        std::string error;
                        const bool saved = SceneSerializer::Save(scene, path);
                        Render::RegisterMaterial(c.editMaterial, Render::MaterialDesc());   // wiped: must come back from the file
                        const bool loaded = saved && SceneSerializer::Load(scene, path, &error);
                        const std::string after = SceneSerializer::ToJson(scene);
                        const Render::MaterialDesc* m = Render::FindMaterial(c.editMaterial);
                        Entity part = Find(ctx, c.editPart);
                        Expect(L + " CAD: edits survive save + reopen (" + c.editPart + " transform, " + c.editMaterial + ")",
                               loaded && after == before && scene.EntityCount() == entities && part && std::abs(part.LocalTransform().position.y - 0.02f) < 1e-5f && m && std::abs(m->baseColor.y - 0.3f) < 1e-5f,
                               F("%.0f entities, %.0f bytes", double(entities), double(before.size())) + (error.empty() ? "" : ", " + error));
                        Find(ctx, c.editPart).LocalTransform().position -= glm::vec3(0.0f, 0.02f, 0.0f);
                        mat.baseColor = original;
                        Render::RegisterMaterial(c.editMaterial, mat);
                        m_scratch[0] = float(m_stepFrames);
                        m_phase = 12;
                        return false;
                    }
                    case 12: {
                        if (m_stepFrames - m_scratch[0] < 10) return false;
                        const RenderStats r = GetRenderStats();
                        Expect(L + " CAD reopened: parts rebuilt", r.meshObjects == CountRenderedEntities(ctx) && Find(ctx, c.root), F("%.0f render objects", r.meshObjects));
                        return true;
                    }
                }
                return false;
            };
        };

        {
            CadCase rover;
            rover.title = "Rover CAD: standalone rover workspace, views, explode, visibility, save";
            rover.scene = "RoverCAD"; rover.root = Projects::RoverCadScene::ROOT; rover.label = "rover"; rover.shotPrefix = "rcad";
            rover.named = { "Rover body", "Chassis", "Bodywork", "Drive system", "Electronics", "LiDAR", "Sensor-positioning arm", "End effector",
                            "Soil sensor head", "Rover wiring", "Raspberry Pi Pico", "J1 Base yaw", "J4 Wrist", "Wheel FL", "Wheel RR", "Fender L", "Fender R" };
            rover.wiringGroups = { "Rover wiring" };
            rover.minParts = 90; rover.minRuns = 8; rover.minViewParts = 25; rover.minBottomParts = 10;   // from below the chassis box hides everything but the underside
            rover.hideGroup = "Bodywork"; rover.hidePart = "Gripper"; rover.editPart = "Fender R"; rover.editMaterial = "OMD/RoverYellow";
            rover.exploded = "LiDAR up, wheels out, arm lifted, wiring hidden";
            rover.explodedOk = [this](GameContext& ctx, std::string& d) {
                Entity lidar = Find(ctx, "LiDAR"), wheel = Find(ctx, "Wheel FR"), arm = Find(ctx, "Sensor-positioning arm"), wiring = Find(ctx, "Rover wiring");
                d = F("LiDAR y %.3f, ", lidar.LocalTransform().position.y) + F("wheel FR z %.3f", wheel.LocalTransform().position.z);
                return lidar.LocalTransform().position.y > 0.4f && wheel.LocalTransform().position.z > 0.4f && arm.LocalTransform().position.y > 0.4f && !Projects::ProjectKit::AnyVisible(wiring);
            };
            rover.collapsed = "collapses back to the assembled layout";
            rover.collapsedOk = [this](GameContext& ctx, std::string& d) {
                Entity lidar = Find(ctx, "LiDAR");
                d = F("LiDAR y %.4f", lidar.LocalTransform().position.y);
                return std::abs(lidar.LocalTransform().position.y - 0.168f) < 1e-4f && Projects::ProjectKit::AnyVisible(Find(ctx, "Rover wiring"));
            };
            m_steps.push_back({ rover.title, cadStep(rover), 120.0 });
        }
        {
            CadCase drone;
            drone.title = "Drone CAD: standalone drone workspace, views, explode, visibility, save";
            drone.scene = "DroneCAD"; drone.root = Projects::DroneCadScene::ROOT; drone.label = "drone"; drone.shotPrefix = "dcad";
            drone.named = { "Frame (F450 class)", "Propulsion", "Propellers", "Electronics", "Drone wiring", "Battery - LiPo", "GPS - ReadyToSky", "Landing gear",
                            "Flight controller - Pixhawk 2.4.8", "Pixhawk 2.4.8 case", "GPS puck", "Top plate", "Arm beam 1", "Arm beam 4", "Motor 1", "Motor 4",
                            "Propeller 1", "Propeller 4", "ESC 1", "ESC leads 4", "Motor mount 2", "Cable tie 3", "Prop nut 2" };
            drone.wiringGroups = { "Drone wiring", "ESC leads 1", "ESC leads 2", "ESC leads 3", "ESC leads 4" };
            drone.minParts = 60; drone.minRuns = 23; drone.minViewParts = 25; drone.minBottomParts = 15;
            drone.hideGroup = "Propellers"; drone.hidePart = "GPS puck"; drone.editPart = "Top plate"; drone.editMaterial = "OMD/MotorRed";
            drone.exploded = "FC up, battery and gear down, arms out, assembly lifted clear of the ground, wiring hidden";
            drone.explodedOk = [this](GameContext& ctx, std::string& d) {
                Entity fc = Find(ctx, "Flight controller - Pixhawk 2.4.8"), bat = Find(ctx, "Battery - LiPo"), arm = Find(ctx, "Arm beam 1"),
                       root = Find(ctx, Projects::DroneCadScene::ROOT), gear = Find(ctx, "Landing gear");
                const float lowest = std::min(bat.WorldPosition().y + 0.068f - 0.0175f, gear.WorldPosition().y);   // pack underside, skid underside
                d = F("FC +%.3f, ", fc.LocalTransform().position.y) + F("battery %.3f, ", bat.LocalTransform().position.y) +
                    F("arm 1 out %.3f, ", glm::length(arm.LocalTransform().position)) + F("lowest part y %.3f", lowest);
                return fc.LocalTransform().position.y > 0.12f && bat.LocalTransform().position.y < -0.19f && glm::length(arm.LocalTransform().position) > 0.10f &&
                       root.LocalTransform().position.y > 0.19f && lowest > 0.0f && !Projects::ProjectKit::AnyVisible(Find(ctx, "Drone wiring"));
            };
            drone.collapsed = "collapses back to the assembled layout (skids on the ground)";
            drone.collapsedOk = [this](GameContext& ctx, std::string& d) {
                Entity root = Find(ctx, Projects::DroneCadScene::ROOT), bat = Find(ctx, "Battery - LiPo");
                d = F("root y %.4f, ", root.LocalTransform().position.y) + F("battery %.4f", bat.LocalTransform().position.y);
                return std::abs(root.LocalTransform().position.y) < 1e-4f && std::abs(bat.LocalTransform().position.y) < 1e-4f && Projects::ProjectKit::AnyVisible(Find(ctx, "Drone wiring"));
            };
            m_steps.push_back({ drone.title, cadStep(drone), 120.0 });
        }

        // Editor document safety (release audit B5, H1, H3): a damaged or newer scene file never
        // touches the open scene, saves are atomic, workspace scene files resolve without depending on
        // the working directory, edits are tracked as unsaved changes.
        m_steps.push_back({ "Editor: scene file safety and unsaved changes", [this](GameContext& ctx) {
            Scene& scene = *ctx.CurrentScene();
            const size_t entities = scene.EntityCount();
            const std::string rootName = scene.Name() == "DroneCAD" ? Projects::DroneCadScene::ROOT : std::string();
            std::string error1, error2;
            const std::string damaged = R"({"format":"phisiks-scene","version":1,"name":"x","entities":[
                {"name":"A","parent":-1,"transform":{"position":[0,0,0]}},
                {"name":"B","parent":-1,"transform":{"position":[1,2]}}],"materials":{}})";
            const bool damagedLoaded = SceneSerializer::FromJson(scene, damaged, &error1);
            const size_t afterDamaged = scene.EntityCount();
            const std::string newer = R"({"format":"phisiks-scene","version":99,"name":"x","entities":[]})";
            const bool newerLoaded = SceneSerializer::FromJson(scene, newer, &error2);
            const size_t afterNewer = scene.EntityCount();
            Expect("a damaged scene file is rejected and the open scene is untouched (audit B5)",
                   !damagedLoaded && afterDamaged == entities && (rootName.empty() || Find(ctx, rootName)), F("%.0f -> ", double(entities)) + F("%.0f entities; ", double(afterDamaged)) + error1);
            Expect("a newer scene format version is rejected (audit B5)", !newerLoaded && afterNewer == entities, error2);

            const std::string file = m_runDirectory + "/editor_atomic.scene.json";
            std::string status;
            Editor::MarkEdited();
            const bool dirty = Editor::HasUnsavedChanges(ctx);
            const bool saved = Editor::SaveScene(ctx, file, &status);
            std::error_code ec;
            const bool noTemp = !std::filesystem::exists(file + ".tmp", ec) && std::filesystem::exists(file, ec);
            Expect("save is atomic (no temporary file left) and clears the unsaved-changes flag (audit H1)", saved && noTemp && dirty && !Editor::HasUnsavedChanges(ctx), status);

            const std::string resolved = Editor::ResolveScenePath("projects/OldMcDonald/scenes/drone_cad.scene.json");
            Expect("workspace scene files resolve to an existing absolute path (audit H3)",
                   std::filesystem::path(resolved).is_absolute() && std::filesystem::exists(resolved, ec), resolved);
            return true;
        } });

        // Scene format 2 on the Joints sample: joints and Lua scripts now survive save + reopen. What a
        // save still cannot keep (C++ callbacks) is listed first: the editor refuses an unconfirmed
        // save and File > Save asks; cancel leaves the file untouched, confirm writes it without them.
        m_steps.push_back({ "Editor: scene format 2 and the save warning for C++ callbacks", [this](GameContext& ctx) {
            const std::string file = m_runDirectory + "/editor_lossy.scene.json";
            const std::string sentinel = "sentinel: a refused or cancelled save must not touch this file\n";
            auto fileText = [&] { std::ifstream in(file, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
            auto joints = [](Scene& s) { size_t n = 0; for (auto e : s.Registry().view<Joint>()) { (void)e; ++n; } return n; };
            auto scripts = [](Scene& s) { size_t n = 0; for (auto e : s.Registry().view<Script>()) { (void)e; ++n; } return n; };
            switch (m_phase) {
                case 0: ctx.scenes->Request("Joints"); m_phase = 1; return false;
                case 1: {
                    if (!ctx.CurrentScene() || ctx.CurrentScene()->Name() != "Joints") return false;
                    Scene& scene = *ctx.CurrentScene();
                    const std::string stored = m_runDirectory + "/joints_v2.scene.json";
                    std::string status, error;
                    const bool savedV2 = Editor::SaveScene(ctx, stored, &status);
                    Scene reopenedV2("Joints (format 2)");
                    const bool loadedV2 = SceneSerializer::Load(reopenedV2, stored, &error);
                    Expect("scene format 2: joints and scripts survive save + reopen with no warning",
                           savedV2 && loadedV2 && joints(scene) > 0 && joints(reopenedV2) == joints(scene) && scripts(reopenedV2) == scripts(scene) && Editor::ComponentsLostOnSave(ctx).empty(),
                           F("%.0f joints, ", double(joints(reopenedV2))) + F("%.0f scripts reopened; ", double(scripts(reopenedV2))) + status + error);
                    scene.CreateEntity("Callback target").Add<Health>().onDeath = [](GameContext&, Entity, Entity) {};
                    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << sentinel; }
                    m_scratch[0] = float(joints(scene));
                    size_t listed = 0;
                    std::string types;
                    for (const SceneSerializer::UnsavedComponent& c : Editor::ComponentsLostOnSave(ctx)) {
                        types += c.type + " x" + std::to_string(c.count) + " ";
                        if (c.type.rfind("Health.onDeath", 0) == 0) listed = c.count;
                    }
                    Expect("the editor lists what a save would drop (a C++ callback), with counts", listed == 1, types);
                    const bool saved = Editor::SaveScene(ctx, file, &status);
                    Expect("an unconfirmed save is refused and the file is untouched", !saved && fileText() == sentinel && status.find("Health.onDeath") != std::string::npos, status);
                    const Editor::SaveRequest request = Editor::RequestSave(ctx, file, &status);
                    Expect("File > Save asks first and writes nothing yet", request == Editor::SaveRequest::AwaitingConfirmation && Editor::SaveConfirmationPending() && fileText() == sentinel, status);
                    m_scratch[1] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                }
                case 2: {
                    if (m_stepFrames - m_scratch[1] < 3) return false;
                    Expect("the confirmation dialog is on screen", Editor::SaveConfirmationVisible(), "");
                    Editor::ResolveSaveConfirmation(ctx, false);
                    Expect("cancel: the file is untouched and the open scene keeps its components",
                           !Editor::SaveConfirmationPending() && fileText() == sentinel && joints(*ctx.CurrentScene()) == size_t(m_scratch[0]), F("%.0f joints", double(joints(*ctx.CurrentScene()))));
                    std::string status;
                    Editor::RequestSave(ctx, file, &status);
                    Editor::ResolveSaveConfirmation(ctx, true);
                    std::string error;
                    Scene reopened("Joints (reopened)");
                    const bool loaded = SceneSerializer::Load(reopened, file, &error);
                    Entity target = reopened.FindByName("Callback target");
                    Expect("confirm: the file is written without the callback; the open scene still has it",
                           !Editor::SaveConfirmationPending() && loaded && target && target.Has<Health>() && !target.Get<Health>().onDeath && joints(reopened) == size_t(m_scratch[0])
                           && reopened.EntityCount() == ctx.CurrentScene()->EntityCount() && bool(ctx.CurrentScene()->FindByName("Callback target").Get<Health>().onDeath),
                           F("%.0f entities reopened", double(reopened.EntityCount())) + F(" of %.0f; ", double(ctx.CurrentScene()->EntityCount())) + error);
                    return true;
                }
            }
            return false;
        } });

        // Editor viewport: the main camera is fitted to the centre area between the panels (the
        // runtime test otherwise renders the full window under the panels) and picking works in it.
        m_steps.push_back({ "Editor: empty document, inset viewport, picking", [this](GameContext& ctx) {
            switch (m_phase) {
                case 0: ctx.scenes->Request("Untitled"); m_phase = 1; return false;
                case 1: {
                    if (!ctx.CurrentScene() || ctx.CurrentScene()->Name() != "Untitled") return false;
                    Editor::AppConfig config = Editor::Config();
                    config.insetViewport = true;
                    Editor::Configure(config);
                    Entity cube = ctx.CurrentScene()->CreateEntity("Pick cube");
                    cube.Get<Transform>().position = ctx.camera.GetOrbitTarget();
                    cube.Add<MeshRenderer>(MeshRenderer{ "Cube", "" });
                    m_scratch[0] = float(m_stepFrames);
                    m_phase = 2;
                    return false;
                }
                case 2: if (m_stepFrames - m_scratch[0] < 6) return false; RequestCapture(); m_phase = 3; return false;
                case 3: {
                    if (!m_captureReady) return false;
                    Scene& scene = *ctx.CurrentScene();
                    const glm::vec4 rect = Editor::ViewportRect();
                    const Entity main = Find(ctx, "MainCamera");
                    const glm::vec4 vp = main ? main.Get<Camera>().viewport : glm::vec4(0.0f);
                    const ImVec2 d = ImGui::GetIO().DisplaySize;
                    const glm::vec4 expected(rect.x / d.x, 1.0f - (rect.y + rect.w) / d.y, rect.z / d.x, rect.w / d.y);
                    const bool fitted = rect.z > 100.0f && glm::all(glm::lessThan(glm::abs(vp - expected), glm::vec4(0.01f))) && vp.z < 0.99f;
                    Expect("main camera fitted to the viewport between the panels", fitted,
                           F("viewport (%.3f, %.3f, ", vp.x, vp.y) + F("%.3f, %.3f), ", vp.z, vp.w) + F("rect %.0f x %.0f px", rect.z, rect.w));
                    const Render::FrameCapture& c = m_capture;
                    const int px = int((rect.x + rect.z * 0.5f) / d.x * float(c.width)), py = int((1.0f - (rect.y + rect.w * 0.5f) / d.y) * float(c.height));
                    const uint32_t id = (px >= 0 && py >= 0 && px < c.width && py < c.height) ? c.entityIds[size_t(py) * c.width + px] : 0;
                    const Entity cube = Find(ctx, "Pick cube");
                    Expect("the object at the viewport centre is pickable", cube && id == PixelId(cube), F("id %.0f", double(id)));
                    Screenshot("editor_inset_viewport");
                    Editor::AppConfig config = Editor::Config();
                    config.insetViewport = false;
                    Editor::Configure(config);
                    (void)scene;
                    return true;
                }
            }
            return false;
        } });

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
