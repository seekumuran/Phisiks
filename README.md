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
- **PhisiksLab studio:** Dear ImGui in an original dark retro hardware-console skin (obsidian / gunmetal panels with chiselled dark-steel bevels, File | View | Simulation | CAD | Help menu bar, etched control groups, black phosphor-green / amber LCD readouts, LED meters, gradient sliders, transport bar): lesson and showcase playlists, hierarchy, inspector with viewport gizmos, live tuning, CPU/GPU/RAM telemetry and the replay timeline.
- **Audio:** software mixer with WASAPI (Windows) or miniaudio (macOS, Linux) output; 3D voices with equal-power panning, inverse-square attenuation and Doppler shift.
- **Assets:** loaded on demand; the sample assets are procedurally generated and CC0.

## Build

```powershell
# Windows (PhysX + WASAPI; vendor/ as described in docs/BUILD.md)
cmake --preset vs2026-x64
cmake --build --preset release          # PhisiksCore, PhisiksLab, the four showcases, tests, tools
ctest --preset release                  # unit tests, layering, replay, the four showcase autotests
.\Build\bin\Release\PhisiksLab.exe
.\Build\bin\Release\Showcase01_RoboticArm.exe
```
```sh
# Linux / macOS (native physics + miniaudio)
python3 tools/fetch_deps.py
cmake --preset linux-release        # or macos-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/Showcase03_BlackHole
```
The executables find `assets/` next to themselves first, then in the working directory, so a development build runs from the repository root. Details: `docs/BUILD.md`.

## Download and run (no build tools)

Each version tag (`v1.0.0`, ...) publishes portable archives on the GitHub Release page (`.github/workflows/release.yml`): `Phisiks-<version>-win64.zip`, `Phisiks-<version>-linux-x86_64.tar.gz`, `Phisiks-<version>-macos.zip`. Extract one anywhere and start `PhisiksLab` or one of the four `Showcase0N_*` programs from any folder; `QUICKSTART.txt` inside lists the controls and options. To make the archive yourself: `cpack --config Build/vs2026/CPackConfig.cmake -C Release` -> `dist/Phisiks-1.0.0-win64.zip` (see `docs/BUILD.md`, "Release package").

## Showcases (`examples/`)

Four "hero centerpiece" executables on one shared framework (`examples/common`) and one dark 1990s engineering-console UI: menu bar (File | View | Simulation | CAD | Help), a telemetry panel (LCD FPS / frame time, CPU / GPU / RAM / culling LED meters, GPU and CPU particle counts, RAM against the 2.5 GB budget), the demo's control groups, a corner X / Y / Z orientation triad, five cinematic spline camera shots and a visualisation-mode switcher. The three engineering demos stand on a light-grey CAD studio cyclorama, the black hole on a deep-space backdrop. Hero meshes are generated by `tools/assetgen` (`assets/meshes/hero/*.obj`); the custom shaders are post passes in `assets/shaders/`. Every demo also appears in PhisiksLab's **SHOWCASE** playlist. They start at 1920x1080 on the Medium quality preset.

| # | Executable | Industry | What it shows |
|---|---|---|---|
| 01 | `Showcase01_RoboticArm` | Robotics | Beveled 6-DOF industrial arm (bolted base, cast orange housings, black servo drums, 3-axis wrist, parallel gripper); RGB coordinate frames at the base, J1-J6 and the tool with labels; dual-quaternion FK + damped-least-squares IK; a PTP / LIN pick-and-place program moving a physics part between two stands; glowing TCP trail; **Joint Jog** (J1-J6), **Cartesian Jog** (X / Y / Z, Euler Z-Y-X + quaternion readouts) and a joint-space **Timeline** (|<  <  >  ||  LIVE + scrubber) |
| 02 | `Showcase02_PoolFluid` | Real-time VFX | Open-top cyan-tiled pool with a buoyant white ball: 128 x 128 heightfield waves coupled two ways to the ball (displacement + buoyancy of the submerged cap), Snell refraction traced against the pool and the ball, Fresnel sky / ball reflections, Beer-Lambert tint, CPU-projected floor caustics with the ball's caustic shadow; click the water for ripples, DROP BALL, **Shader Passes** breakdown slider (Wireframe -> Depth -> Normals -> Caustics & Refraction, fractions wipe) |
| 03 | `Showcase03_BlackHole` | Science | Schwarzschild black hole ray-traced per pixel (photon orbits from the Binet equation, RK4): black shadow, Einstein photon ring, lensed starfield, the far side of the disk bent over and under the shadow; 16,000 disk particles in the Paczynski-Wiita potential on Euler / velocity Verlet / RK4 (or all three in sectors: split-test), Doppler shift + beaming + gravitational redshift (`doppler.glsl`, hot-reloadable); **Black Hole Parameters** (mass, inclination, Doppler, beaming, glow, time scale) |
| 04 | `Showcase04_TurbineCad` | CAD / engineering | Axial-centrifugal turbine assembly (20 parts: bolted flanges with hex bolts, intake bell-mouth, three axial rotor stages, magenta stator rings, gold centrifugal impeller with splitters, volute scroll, bearing housing, nozzle ring, turbine wheel, exhaust cone); **CAD Assembly** exploded-view slider along the shaft, ghosted see-through casing with a wireframe overlay, CAD ink edges, rotor speed, bill of materials, F3 4-way orthographic blueprints |

## Controls cheat sheet

The same sheet ships as `QUICKSTART.txt` in the release archive. Keyboard and mouse go to the 3D view only while the pointer is not over a console panel (and no text field is active). Showcases start in the orbit camera, PhisiksLab in first person; the camera does not move while paused.

**Camera (all programs)**

| Input | Action |
|---|---|
| RMB drag / MMB drag / wheel | orbit / pan / zoom (orbit camera) |
| `F4` | toggle orbit camera <-> first person (the orbit centres 8 m in front of the view) |
| `V` | cycle camera: first person -> third person -> free fly |
| Mouse | look (first / third person, free fly; cursor captured) |
| `WASD` / arrows, `Shift`, `Space` | move, sprint, jump (free fly: `Space` up, `Ctrl` down) |

**Camera presets and view modes (showcases, also inside PhisiksLab's SHOWCASE scenes)**

| Key | Action |
|---|---|
| `1` .. `5` | cinematic camera shots along spline paths (panel group *Camera Shots*; hover a button for the shot name) |
| `0` | stop the shot, back to the free camera (*FREE* button) |
| `M` | cycle the view mode: FINAL RENDER -> WIREFRAME -> SDF + FIELDS -> RAW PARTICLES -> CAD 4-WAY (*View Mode* buttons) |
| `F3` | CAD 4-way view on / off: perspective + orthographic top / front / side, blueprint grid, CAD ink (in PhisiksLab scenes: the quad view) |

**Panels and general**

| Key | Action |
|---|---|
| `Tab` | hide / show the console panels |
| `F1` / `F2` | overlay / physics debug draw |
| `Esc` | pause / resume; `Q` while paused quits |
| PhisiksLab only | `R` reload scene, `N` next scene, `E` interact, LMB push / damage |

**Replay timeline transport buttons**

| Where | Buttons (left to right) |
|---|---|
| PhisiksLab *Replay timeline* panel (30 s ring at 60 Hz) | step back; play (continues from the scrubbed step); pause; stop (rewind to the first recorded step); step forward; record on / off; eject (clear the timeline); scrub slider (pauses at that step) |
| RoboticArm *Timeline Replay* group (60 s joint-space recording) | `\|<` first recorded step; eject glyph = step back 0.25 s; play the recording; pause; *LIVE* (back to the live arm); scrub slider |

**Per-demo controls (right-hand console panel)**

| Demo | Controls |
|---|---|
| 01 RoboticArm | *Program*: RUN PROGRAM (PTP / LIN pick-and-place cycle), HOME, OPEN / CLOSE gripper. *Joint Jog*: J1 .. J6 sliders in degrees (switches to manual). *Cartesian Jog*: X / Y / Z tool-point sliders in mm (IK), readouts XYZ, Euler Z-Y-X, quaternion, IK residual. *Timeline Replay*: above. *Display*: FRAMES, TRAIL, CAD INK |
| 02 PoolFluid | Left-click the water: ripple. *Shader Passes*: BREAKDOWN slider 0..3 + WIRE / DEPTH / NORMALS / FINAL buttons. *Optics*: CAUSTICS, REFRACTION. *Interaction*: DROP BALL, RAIN on / off, CALM (flatten the water) |
| 03 BlackHole | *Black Hole Parameters*: MASS, INCLINATION, DOPPLER, BEAMING, DISK GLOW, TIME SCALE. *Integrator Split-Test*: EULER / VERLET / RK4, SPLIT (each third of the disk on its own integrator), RESET DISK, energy-drift readouts. *Lens*: STARS, PHOTON RING |
| 04 TurbineCad | *CAD Assembly*: EXPLODE slider (along the shaft), AUTO (animate), ASSEMBLE, EXPLODE ALL, CASING OPACITY, CASING WIRE, CAD INK + INK. *Rotor*: ROTOR RPM. *Bill of Materials*: the 20 parts with their exploded offsets. `F3`: 4-way orthographic blueprints |

**Showcase options:** `--autotest [--quick]` (runtime verification, report in `Build/runs/<scene>/showcase_report.txt`), `--quality low|medium|high`, `--gl41`, `--width W --height H`, `--fullscreen`, `--hidden`, `--hot-reload [dir]` (also rebuilds the post-pass shaders in `assets/shaders/` when they change), `--export-dir <dir> --export-frames|--export-data [--export-every N]`, `--headless`.

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
