#include "AutoTest.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Time.h"
#include "SandboxScenes.h"

#include "Phisiks/Physics/Physics.h"
#include "Phisiks/Framework/GameplayComponents.h"
#include "Phisiks/Render/RenderStats.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Audio/Audio.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
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
            Expect("shader programs built (cache or compile), none failed",
                   cache.programs == 7 && cache.failures == 0 && cache.loadedFromCache + cache.compiled == cache.programs && cache.written == cache.compiled,
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

        // 11. Round trip back: resource counts identical to the first load.
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
                    Expect("no simulation while paused", std::abs(ctx.stats.simulationTime - m_scratch[1]) < 1e-6, F("sim time %.4f -> %.4f over 15 frames", m_scratch[1], ctx.stats.simulationTime));
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
