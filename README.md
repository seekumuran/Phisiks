# Phisiks

A general-purpose 3D simulation, visualisation and game engine in C++20 for Windows, Linux and macOS (OpenGL 4.6, with an OpenGL 4.1 tier for macOS and older GPUs). **Core engine feature freeze: P10 (`p10-showcase`).**

- **Scenes:** EnTT entity-component scenes with transform hierarchies.
- **Physics:** rigid bodies, colliders (incl. height fields), joints (hinge, ball-and-socket, prismatic, spring-damper; limits and motors), queries and a character controller behind a small `Phisiks::Physics` API, with two backends: NVIDIA PhysX 5 (Windows default) and the built-in native solver (macOS, Linux).
- **Renderer:** forward PBR with GPU-driven indirect drawing (compute frustum + Hi-Z occlusion culling), directional/spot/point shadows and analytic fog, plus a persistent shader binary cache, wireframe mode and non-blocking GPU timers per pass.
- **Compute:** `Phisiks::Compute::IComputeBackend` with an OpenGL 4.6 compute backend and a multi-threaded CPU backend (OpenCL/SYCL hooks); GPU spatial hash and particles, with the same systems on the CPU worker pool on the 4.1 tier and headless.
- **Upscaling and post FX:** FSR 1.0-style EASU + RCAS spatial upscaling (clean-room), HDR bloom (mip chain), SSAO, projected decals, a 4-pass screen-space fluid renderer, ribbon trails / beams / orbit paths, live GLSL hot reload, and Low / Medium / High quality presets for integrated GPUs.
- **Animation:** glTF 2.0 / GLB import (meshes, skins, clips, PBR base colour textures), dual-quaternion skinning, Catmull-Rom / Bezier camera paths.
- **World:** fBm or image heightmap terrain with 4-layer splatmaps and height-field collision, GPU-instanced foliage scatter.
- **AI:** navigation grid built from static physics geometry, A* with string pulling, crowd steering (separation + time-to-collision avoidance).
- **Scripting:** embedded, sandboxed Lua 5.4 (`Script` component: entities, physics, joints, input, scene events, file hot reload).
- **Advanced physics:** XPBD rope, cloth and soft bodies colliding with the scene; particle emitters and box / disk (orbital) / fill spawns; two-way coupling (particles and soft bodies push rigid bodies); force fields (planetary gravity, vortex, N-body); relativistic Doppler particle shading.
- **Visualisers:** raymarched SDF / smooth-CSG shapes and vector-field arrows.
- **Cameras:** multiple viewports and orthographic cameras, offscreen targets (optionally as live entity-id segmentation masks), a CAD-style orbit camera.
- **Simulation:** deterministic stepped simulation (headless or offscreen) with PNG frame and CSV state export, synthetic-vision export (aligned RGB, linear depth, world normals, entity-id masks, camera CSV), a replay ring buffer with timeline scrubbing, and JSON scene files.
- **PhisiksLab studio:** Dear ImGui in an original "retro deck" skin (bevelled slate panels, LCD readouts, LED meters, equaliser sliders, transport bar): lesson and showcase playlists, hierarchy, inspector with viewport gizmos, live tuning, CPU/GPU/RAM telemetry and the replay timeline.
- **Audio:** software mixer with WASAPI (Windows) or miniaudio (macOS, Linux) output; 3D voices with equal-power panning, inverse-square attenuation and Doppler shift.
- **Assets:** loaded on demand; the sample assets are procedurally generated and CC0.

## Build

```powershell
# Windows (PhysX + WASAPI; vendor/ as described in docs/BUILD.md)
cmake --preset vs2026-x64
cmake --build --preset release          # PhisiksCore, PhisiksLab, the six showcases, tests, tools
ctest --preset release                  # unit tests, layering, replay, the six showcase autotests, vision export
.\Build\bin\Release\PhisiksLab.exe
.\Build\bin\Release\Showcase01_StuntCar.exe
```
```sh
# Linux / macOS (native physics + miniaudio)
python3 tools/fetch_deps.py
cmake --preset linux-release        # or macos-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/Showcase06_BlackHole
```
Run every executable from the repository root (it contains `assets/` and `engine/shaders/`). Details: `docs/BUILD.md`.

## Showcases (`examples/`)

Six standalone executables built on one shared framework (`examples/common`): a RetroDeck HUD (FPS, frame time, CPU / GPU ms, GPU and CPU particle counts, RAM against a 2.5 GB budget, culling savings), five cinematic spline camera shots, and a visualisation-mode switcher. Every demo also appears in PhisiksLab's **SHOWCASE** playlist. They start at 1920x1080 on the Medium quality preset.

| # | Executable | Industry | What it shows |
|---|---|---|---|
| 01 | `Showcase01_StuntCar` | Game development | Driveable car on raycast spring-damper suspension, Lua boost pads (`on_trigger`), a kicker ramp into six-storey crate / glass towers that shatter on impact, HDR spark ribbons and bloom, neon tyre trails and skid decals, guard bots patrolling on the navigation grid, spatial engine and siren audio with Doppler |
| 02 | `Showcase02_FluidHarbor` | Real-time VFX | ~11k-particle harbor basin + waterfall rendered by the 4-pass screen-space fluid pipeline (sphere depth -> narrow-range bilateral blur -> normals -> Beer-Lambert absorption, Fresnel, foam), buoyant boat and crates (Archimedes from a particle-derived surface, particles displaced by the hulls), a whirlpool force field, and the breakdown wipe slider (raw particles -> depth -> normals -> water) |
| 03 | `Showcase03_Robotics` | Robotics & mechanics | 6-DOF arm (5 motorised hinges + prismatic gripper slide) driven by dual-quaternion damped-least-squares IK with whole-arm SDF obstacle avoidance; it loads a crate onto a 6-wheel spring-damper rover that drives a loop over fBm terrain; joint angle / holding-torque readouts and replay timeline scrubbing |
| 04 | `Showcase04_Villa` | Architecture & digital twin | Glass-and-concrete cliffside villa with an infinity pool, instanced landscaping (pines, bushes, grass, rocks), a time-of-day slider (sun, sky, exposure; interior lights and garden spots at dusk) and the F3 4-way CAD blueprint view |
| 05 | `Showcase05_Vision` | AI & computer vision | Signalised intersection with traffic and pedestrians, a 360-degree 16 x 720 LiDAR at 5 Hz (labelled physics raycasts), split-screen camera / live instance-segmentation mask, and batch export (`--headless --export-vision`) |
| 06 | `Showcase06_BlackHole` | Science & maths | 52,000 Doppler-shaded plasma particles on Keplerian orbits (hot-reloadable `engine/shaders/doppler.glsl`), helical polar jets, and an Euler / velocity Verlet / RK4 split-test on precessing pseudo-Newtonian orbits |

**Common controls (all showcases):**

| Key | Action |
|---|---|
| `1` .. `5` | cinematic camera shots (spline paths); `0` back to the free camera |
| `M` | cycle the view: Final Render -> Wireframe -> 3D SDF & Vector Fields -> Raw Compute Particles -> CAD 4-way |
| `F3` | CAD 4-way view (perspective + orthographic top / front / side, blueprint grid) |
| `Tab` | hide / show the panels (in PhisiksLab) |
| RMB / MMB / wheel | orbit / pan / zoom (orbit camera) |
| `Esc` | pause (then `Q` quits) |
| StuntCar | `W`/`S` throttle, `A`/`D` steer, `Space` brake, `Shift` nitro, `E` reset the car, `C` chase camera |

**Options:** `--autotest [--quick]` (runtime verification, report in `Build/runs/<scene>/showcase_report.txt`), `--quality low|medium|high`, `--gl41`, `--width W --height H`, `--fullscreen`, `--hidden`, `--hot-reload [dir]`, `--export-dir <dir> --export-vision|--export-frames|--export-data [--export-every N]`, `--headless` (with GPU exports it renders into a hidden window).

```powershell
# Synthetic-vision batch export of the LiDAR proving ground
.\Build\bin\Release\Showcase05_Vision.exe --headless --export-vision --export-dir Build/vision --export-every 30 --max-frames 600
```

## Docs

| Doc | What |
|---|---|
| `docs/BUILD.md` | requirements, `vendor/` layout, outputs, controls, simulation/export flags |
| `docs/ROADMAP.md` | completed phases, the feature freeze, open follow-ups |
| `docs/ARCHITECTURE_DECISIONS.md` | key design decisions |
| `docs/PHASE_LOG.md` | phase-by-phase history with measurements |
| `docs/KNOWN_ISSUES.md` | known limitations |
| `docs/CLEAN_ROOM.md` | provenance: all code original |
| `THIRD_PARTY_NOTICES.md` | third-party libraries and their licences |

MIT licensed, see `LICENSE`.
