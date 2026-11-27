#include "Phisiks/Runtime/Application.h"
#include "Phisiks/Core/Events.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Core/Profiler.h"
#include "Phisiks/Core/Time.h"
#include "Phisiks/Framework/DebugOverlay.h"
#include "Phisiks/Platform/Window.h"
#include "Phisiks/Platform/System.h"
#include "Phisiks/Core/InputState.h"
#include "Phisiks/Physics/Physics.h"
#include "core/CrashHandler.h"
#include "render/RendererInternal.h"
#include "runtime/Presentation.h"
#include "runtime/Recorder.h"
#include "compute/AdvancedWorld.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Audio/Audio.h"
#include "Phisiks/UI/ImGuiLayer.h"
#include "Phisiks/AI/Navigation.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/VFX/VFX.h"
#include "Phisiks/World/Terrain.h"

namespace Phisiks {
    namespace {
        float ZoneMs(const char* name) {
            const ProfileZoneStats* zone = Profiler::Get().GetZone(name);
            return zone ? float(zone->lastMs) : 0.0f;
        }
    }

    Application::Application() = default;
    Application::~Application() {
        if (m_initialized && !m_shutDown) Shutdown();
    }

    bool Application::Init(const AppOptions& options, IGame& game, std::unique_ptr<ITestHarness> harness) {
        m_options = options;
        m_game = &game;
        m_harness = std::move(harness);
        // Logs and crash dumps go to the run directory (relative to the working directory). A read-only
        // install folder falls back to the per-user data folder, so they are never lost silently.
        std::string fallbackNote;
        if (!Platform::IsWritableDirectory(m_options.runDirectory)) {
            const std::string user = Platform::UserDataDirectory();
            const std::string fallback = user.empty() ? std::string() : user + "/runs/latest";
            if (!fallback.empty() && Platform::IsWritableDirectory(fallback)) {
                fallbackNote = m_options.runDirectory;
                m_options.runDirectory = fallback;
            }
        }
        Log::Init(m_options.runDirectory + "/engine.log");
        CrashHandler::Install(m_options.runDirectory);
        if (!fallbackNote.empty()) Log::Warning() << "Run directory '" << fallbackNote << "' is not writable; logs and crash dumps go to " << m_options.runDirectory;
        Log::Info() << "Application::Init (scene '" << options.initialScene << "'" << (m_harness ? ", test harness" : "") << ")";

        SetState(GameState::Booting);
        Time::Init();
        Time::SetDeterministic(options.deterministic, options.stepsPerFrame);
        if (!options.headless) {
            Platform::WindowDesc window;
            window.title = options.windowTitle + (m_harness ? " [AUTOTEST]" : "");
            window.fullscreen = options.fullscreen;
            window.width = options.windowWidth;
            window.height = options.windowHeight;
            window.vsync = options.vsync;
            window.maxGLVersion = options.maxGLVersion;
            if (!Platform::CreateMainWindow(window)) {
                Log::Error() << "Window / OpenGL context creation failed; aborting";
                return false;
            }
            if (!Render::Internal::Init()) {
                Log::Error() << "Renderer initialization failed; aborting";
                return false;
            }
            m_rendererReady = true;
            if (options.enableUI && !UI::Init()) Log::Warning() << "UI unavailable; continuing without the editor panels";
            if (!options.shaderHotReloadDirectory.empty()) Render::EnableShaderHotReload(options.shaderHotReloadDirectory);
        }
        if (!Physics::Init()) {
            Log::Error() << "Physics initialization failed; aborting";
            return false;
        }
        // Deterministic and headless runs use the null audio device: voices advance with simulated time.
        Audio::InitOptions audio;
        audio.useDevice = !options.headless && !options.deterministic;
        Audio::Init(audio);
        Presentation::Install(m_rendererReady);
        if (!m_rendererReady) Advanced::Init();   // headless: particles / XPBD on the CPU backend
        m_ctx.replay.SetCapacity(options.replayCapacity);
        m_ctx.replay.SetEnabled(options.recordReplay);
        if (!options.exportDirectory.empty()) {
            m_recorder = std::make_unique<Recorder>();
            m_recorder->Begin(options.exportDirectory, options.exportFrames, options.exportData, options.exportEvery, options.exportVision);
        }
        if (Platform::HasWindow() && !options.hiddenWindow) Platform::Show();
        Log::Info() << "Engine initialized (" << (options.headless ? "headless" : options.hiddenWindow ? "hidden window" : "window")
                    << (options.deterministic ? ", deterministic " + std::to_string(options.stepsPerFrame) + " step(s)/frame" : std::string())
                    << ", renderer " << (m_rendererReady ? "on" : "off") << ", physics, audio)";

        m_ctx.scenes = &m_scenes;
        m_game->RegisterScenes(m_scenes);
        if (!m_scenes.Has(options.initialScene)) {
            Log::Error() << "Unknown initial scene '" << options.initialScene << "'";
            return false;
        }

        if (m_harness) m_ctx.input.SetHardwareEnabled(false);   // stray keystrokes must not affect a test

        SetState(GameState::Loading);
        m_initialized = true;
        return true;
    }

    void Application::SetCursor(Platform::CursorMode mode) {
        if (Platform::HasWindow() && !m_options.hiddenWindow) Platform::SetCursorMode(mode);
    }

    void Application::SetState(GameState state) {
        if (m_ctx.state == state) return;
        Log::Info() << "Game state: " << ToString(m_ctx.state) << " -> " << ToString(state);
        m_ctx.state = state;
    }

    int Application::Run() {
        if (!m_initialized) return 2;

        while (true) {
            if (m_ctx.quitRequested) break;
            // A user quit (window close button, Q while paused) asks the game first.
            const bool windowClose = Platform::HasWindow() && Platform::ShouldClose();
            if (windowClose && Platform::CloseWasRequestedByApp()) break;
            if (windowClose || m_userQuit) {
                if (m_harness || m_game->OnQuitRequested(m_ctx)) break;
                if (windowClose) Platform::CancelClose();
                m_userQuit = false;
            }
            Profiler::Get().BeginFrame();
            if (m_rendererReady) GpuTimer::Get().BeginFrame();   // collects GPU results that are ready; never waits
            {
                PHISIKS_PROFILE_SCOPE("Frame");
                // Time, window events, input latch, audio.
                Time::Update();
                if (Platform::HasWindow()) Platform::PollEvents();
                GetInputState().NewFrame();
                Audio::Update(Time::DeltaTime());
                GlobalEvents().DispatchQueued();

                if (m_ctx.state == GameState::Loading) {
                    // Assets stream in on demand, so the game starts on the first frame.
                    SetState(GameState::Running);
                    while (Time::ConsumeFixedStep()) {}   // startup time is not simulated: game frame N = fixed step N
                    m_game->OnStart(m_ctx);
                    m_scenes.Request(m_options.initialScene);
                    SetCursor(m_options.captureCursor ? Platform::CursorMode::Captured : Platform::CursorMode::Normal);
                    if (m_rendererReady) Render::Internal::RenderFrame();
                }
                else {
                    RunGameFrame();
                }

                if (Platform::HasWindow()) Platform::SwapBuffers();
            }
            Profiler::Get().EndFrame();

            m_ctx.stats.frameIndex++;
            m_ctx.stats.cpuFrameMs = ZoneMs("Frame");
            m_ctx.stats.cpuUpdateMs = ZoneMs("Update");
            m_ctx.stats.cpuPhysicsMs = ZoneMs("Physics");
            m_ctx.stats.cpuRenderMs = ZoneMs("Render");
            m_ctx.stats.gpuFrameMs = m_rendererReady ? float(GpuTimer::Get().LastMs("Frame")) : 0.0f;

            if (m_options.maxFrames > 0 && m_ctx.stats.frameIndex >= m_options.maxFrames) {
                if (m_harness) Log::Error() << "Reached --max-frames " << m_options.maxFrames << " before the test finished; quitting";
                else Log::Info() << "Reached --max-frames " << m_options.maxFrames << "; quitting";
                m_ctx.quitRequested = true;
            }
            if (m_harness && m_harness->Finished()) m_ctx.quitRequested = true;
        }

        if (Platform::HasWindow() && Platform::ShouldClose() && !Platform::CloseWasRequestedByApp()) Log::Info() << "Window closed";
        const int exitCode = m_harness ? m_harness->ExitCode() : 0;
        Shutdown();
        return exitCode;
    }

    void Application::HandleGlobalActions() {
        InputSystem& input = m_ctx.input;
        if (input.Pressed(Action::Pause)) {
            if (m_ctx.state == GameState::Running) {
                SetState(GameState::Paused);
                SetCursor(Platform::CursorMode::Normal);
            }
            else if (m_ctx.state == GameState::Paused) {
                SetState(GameState::Running);
                SetCursor(m_ctx.camera.GetMode() == CameraMode::Orbit ? Platform::CursorMode::Normal : Platform::CursorMode::Captured);
            }
        }
        if (m_ctx.state == GameState::Paused && input.Pressed(Action::Quit)) {
            Log::Info() << "Quit requested from pause";
            m_userQuit = true;
        }
        if (input.Pressed(Action::ToggleOverlay)) m_ctx.overlayVisible = !m_ctx.overlayVisible;
        if (input.Pressed(Action::TogglePhysicsDebug)) m_ctx.physicsDebugDraw = !m_ctx.physicsDebugDraw;
        if (m_ctx.state == GameState::Running) {
            if (m_options.sceneHotkeys && input.Pressed(Action::ReloadScene)) m_scenes.RequestReload();
            if (m_options.sceneHotkeys && input.Pressed(Action::NextScene)) m_scenes.RequestNext();
            if (input.Pressed(Action::CycleCamera)) {
                m_ctx.camera.CycleMode();
                SetCursor(Platform::CursorMode::Captured);
                m_ctx.PushEvent(std::string("Camera: ") + ToString(m_ctx.camera.GetMode()));
            }
            if (input.Pressed(Action::ToggleOrbitCamera)) {
                if (m_ctx.camera.GetMode() == CameraMode::Orbit) {
                    m_ctx.camera.SetMode(CameraMode::FirstPerson);
                    SetCursor(Platform::CursorMode::Captured);
                }
                else {
                    // Orbit around the point 8 m in front of the current view.
                    const glm::vec3 target = m_ctx.camera.GetPosition() + m_ctx.camera.GetForward() * 8.0f;
                    m_ctx.camera.SetOrbit(target, 8.0f, m_ctx.camera.GetYaw(), m_ctx.camera.GetPitch());
                    SetCursor(Platform::CursorMode::Normal);
                }
                m_ctx.PushEvent(std::string("Camera: ") + ToString(m_ctx.camera.GetMode()));
            }
        }
    }

    void Application::RunGameFrame() {
        const float dt = Time::DeltaTime();
        m_ctx.stats.deltaTime = dt;
        const bool running = m_ctx.state == GameState::Running;
        Scene* scene = nullptr;

        {
            PHISIKS_PROFILE_SCOPE("Update");
            // 1. Input (tests inject synthetic input before it is sampled)
            if (m_harness) m_harness->BeforeInput(m_ctx);
            // The editor UI owns the mouse / keyboard while it is hovered or typed into.
            if (!m_harness && UI::IsActive()) m_ctx.input.SetHardwareEnabled(!UI::WantsMouse() && !UI::WantsKeyboard());
            m_ctx.input.Update(Platform::HasWindow() && Platform::HasFocus());
            HandleGlobalActions();

            // 2. Scene transitions at a safe point
            if (m_scenes.ApplyPending(m_ctx)) {
                Advanced::Clear();   // GPU particles / soft bodies of the previous scene
                GlobalEvents().Enqueue(SceneLoadedEvent{ m_scenes.CurrentName() });
            }
            scene = m_scenes.Current();
            if (scene) World::UpdateScene(*scene);       // terrain meshes / colliders / foliage for new components

            // 3. Variable-rate gameplay
            if (running) {
                m_game->OnUpdate(m_ctx, dt);
                Gameplay::UpdatePlayerActions(m_ctx);
                if (scene) scene->Update(m_ctx, dt);
                if (scene) Scripting::Update(*scene, m_ctx, dt);   // Lua on_start / on_update
            }
            if (scene) Animation::UpdateScene(*scene, running ? dt : 0.0f);   // clips + dual-quaternion skinning
            m_ctx.player.CaptureInput(m_ctx.input, running && m_ctx.camera.GetMode() != CameraMode::FreeFly && m_ctx.camera.GetMode() != CameraMode::Orbit);
        }

        {
            PHISIKS_PROFILE_SCOPE("Physics");
            // 5. Fixed-rate gameplay interleaved with physics (Phisiks::Time owns the accumulator).
            //    Collision/trigger events of this frame's steps stay readable until the next frame.
            Physics::ClearEvents();
            if (scene) scene->SyncPhysicsBodies();       // bodies for new/changed Collider+Rigidbody components
            int steps = 0;
            const float fixedDt = Time::FixedDeltaTime();
            if (m_rendererReady) GpuTimer::Get().Begin("Simulation");   // GPU particles / XPBD of this frame's steps
            while (Time::ConsumeFixedStep()) {
                if (!running) continue;                  // paused: discard accumulated time
                m_game->OnFixedUpdate(m_ctx, fixedDt);
                if (scene) {
                    scene->FixedUpdate(m_ctx, fixedDt);
                    Scripting::FixedUpdate(*scene, m_ctx, fixedDt);
                    AI::UpdateScene(*scene, fixedDt);    // navigation grid + crowd steering
                    scene->SyncPhysicsBodies();          // also pushes kinematic targets for this step
                }
                m_ctx.player.FixedUpdate(m_ctx.camera.GetYaw(), fixedDt);
                if (scene) scene->ApplyForceFields(fixedDt);      // force fields -> rigid bodies
                Physics::Step(fixedDt);
                if (scene && Advanced::Available()) Advanced::Step(*scene, fixedDt);   // particles + XPBD (GPU or CPU backend)
                ++steps;
                if (scene && m_ctx.replay.Enabled()) {
                    scene->PullPhysicsPoses();
                    m_ctx.replay.Record(*scene, m_ctx.stats.totalFixedSteps + steps, m_ctx.stats.simulationTime + steps * double(fixedDt), &m_ctx.input, &m_ctx.camera);
                }
            }
            if (m_rendererReady) GpuTimer::Get().End("Simulation");
            if (scene) scene->PullPhysicsPoses();         // dynamic bodies -> transforms
            if (scene && steps > 0) Scripting::DispatchPhysicsEvents(*scene, m_ctx);   // on_collision / on_trigger
            m_ctx.stats.fixedStepsThisFrame = steps;
            m_ctx.stats.totalFixedSteps += steps;
            m_ctx.stats.simulationTime += steps * double(fixedDt);
        }

        {
            PHISIKS_PROFILE_SCOPE("Update");
            // 6. Post-physics: camera, player body, presentation (renderers/lights/camera/audio)
            m_ctx.camera.Update(m_ctx.input, &m_ctx.player, dt, running);
            if (scene) m_ctx.camera.ApplyToScene(*scene);
            if (scene) Animation::UpdateCameraPaths(*scene, running ? dt : 0.0f);   // spline camera sequencer overrides the controller
            Gameplay::UpdatePlayerBody(m_ctx);
            if (m_harness) m_harness->AfterSimulation(m_ctx);
            if (scene) {
                scene->FlushDestroyed();                 // deferred destruction (releases bodies/render objects)
                scene->SyncPresentation();
            }
        }

        {
            PHISIKS_PROFILE_SCOPE("Render");
            // 7. Debug drawing + overlay, then render (GPU-timed without stalling)
            if (scene && m_rendererReady) VFX::UpdateScene(*scene, running ? dt : 0.0f);   // trails / beams / orbits / decals
            if (m_ctx.physicsDebugDraw) DebugOverlay::DrawPhysicsDebug(m_ctx);
            DebugOverlay::Draw(m_ctx);
            if (scene) scene->DrawUI(m_ctx);
            if (m_rendererReady) {
                if (m_recorder && m_recorder->WantsVision(m_ctx.stats.frameIndex)) Render::RequestVisionCapture();
                GpuTimer::Get().Begin("Frame");
                Render::Internal::RenderFrame();
                GpuTimer::Get().End("Frame");
            }
            if (m_recorder) m_recorder->Capture(scene, m_ctx.stats.frameIndex, m_ctx.stats.totalFixedSteps, m_ctx.stats.simulationTime, m_rendererReady);
            if (UI::IsActive()) {   // editor panels on top of the presented frame (not part of captures)
                UI::BeginFrame();
                m_game->OnGui(m_ctx);
                if (m_rendererReady) GpuTimer::Get().Begin("UI");
                UI::EndFrame();
                if (m_rendererReady) GpuTimer::Get().End("UI");
            }
            if (m_harness) m_harness->AfterRender(m_ctx);
        }
    }

    void Application::Shutdown() {
        if (m_shutDown) return;
        m_shutDown = true;
        if (!m_initialized) {   // Init failed part-way: release what exists
            Log::Error() << "Shutdown after failed Init";
            Presentation::Uninstall();
            Audio::Shutdown();
            Assets::Shutdown();
            if (m_rendererReady) Render::Internal::Shutdown();
            if (Physics::IsInitialized()) Physics::Shutdown();
            if (Platform::HasWindow()) Platform::DestroyMainWindow();
            Log::Shutdown();
            return;
        }
        SetState(GameState::ShuttingDown);
        if (m_game) m_game->OnShutdown(m_ctx);
        Log::Info() << "Shutdown: unloading scene '" << m_scenes.CurrentName() << "' ("
                    << (m_scenes.Current() ? m_scenes.Current()->EntityCount() : 0) << " entities, " << Physics::GetStats().dynamicBodies << " dynamic bodies)";
        m_scenes.UnloadCurrent(m_ctx);
        Advanced::Clear();
        const Physics::Stats physicsStats = Physics::GetStats();
        Log::Info() << "Shutdown: after unload " << (m_scenes.Current() ? m_scenes.Current()->EntityCount() : 0) << " entities, "
                    << physicsStats.dynamicBodies << " dynamic, " << physicsStats.staticBodies << " static, "
                    << physicsStats.kinematicBodies << " kinematic bodies, " << physicsStats.characters << " characters";
        if (m_rendererReady) {
            Log::Info() << "GPU timer: " << GpuTimer::Get().SamplesCollected() << " samples collected, "
                        << GpuTimer::Get().SamplesDropped() << " dropped (never waited on the GPU)";
        }
        if (m_recorder) m_recorder->End();
        if (m_harness) {
            m_harness->AfterUnload(m_ctx);
            m_harness->WriteReport();
        }
        Scripting::Shutdown();
        Presentation::Uninstall();
        Audio::Shutdown();
        Assets::Shutdown();
        UI::Shutdown();
        if (!m_rendererReady) Advanced::Shutdown();
        if (m_rendererReady) {
            GpuTimer::Get().Shutdown();
            Render::Internal::Shutdown();
        }
        Physics::Shutdown();
        if (Platform::HasWindow()) Platform::DestroyMainWindow();
        Log::Info() << "Shutdown complete";
        Log::Shutdown();
    }
}
