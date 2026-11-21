# Building and Running Phisiks

Layout:
- `engine/`: the PhisiksCore library. `include/Phisiks` is the public API, `src` is private, and `shaders` holds GLSL that is embedded at build time.
- `apps/lab`: PhisiksLab.
- `examples/`: the six P10 showcases (`NN_name/`, one executable each) and their shared framework `examples/common` (library `PhisiksShowcase`, also linked into PhisiksLab).
- `tests/unit`: headless unit tests.
- `tests/runtime`: the runtime verification harness built into PhisiksLab.
- `assets/`: CC0 sample assets.
- `tools/`: assetgen, glprobe, stacksample and the CMake check scripts.

The `check_layering` target runs before the engine builds and fails the build if apps or tests bypass the public API.

## Requirements
- **Windows** 10/11 x64: Visual Studio 2026 (v145) C++ tools or Build Tools, CMake ≥ 3.25.
- **Linux** (x86-64 / arm64): GCC 11+ or Clang 15+, CMake ≥ 3.25, Ninja, `libglfw3-dev` (or let CMake fetch GLFW 3.4), OpenGL development headers, ALSA / PulseAudio runtime.
- **macOS** 12+: Xcode command-line tools, CMake, Ninja, `brew install glfw` (or let CMake fetch it). Runs on the OpenGL 4.1 tier.
- GPU: OpenGL 4.6 core for the full tier (GPU culling and compute); any OpenGL 4.1 core GPU runs the 4.1 tier (CPU culling + CPU compute backend). `--gl41` forces the 4.1 tier anywhere. Check with `glprobe`.
- Backends (CMake cache): `PHISIKS_PHYSICS_BACKEND` = `PhysX` (Windows default) or `Native` (Linux / macOS default); `PHISIKS_AUDIO_BACKEND` = `WASAPI` (Windows default) or `miniaudio`.
- Third-party code in `vendor/` (not committed; see `THIRD_PARTY_NOTICES.md`). `python tools/fetch_deps.py` downloads every portable dependency at its pinned version (glm, EnTT, stb, glad (generated with glad2), Dear ImGui, ImGuizmo, miniaudio, nlohmann/json). Lua 5.4.7 is fetched from lua.org (SHA-256 checked) and built as the `PhisiksLua` static library. The Windows-only prebuilt binaries (PhysX, GLFW) are placed by hand:

  | Folder | Contents |
  |---|---|
  | `glad/` | generated GL 4.6 loader: `include/`, `src/gl.c` |
  | `GLFW/` | `include/`, `lib/{release,debug}/glfw3.lib` |
  | `glm/glm/` | headers |
  | `entt/entt.hpp` | single header |
  | `stb_image/` | `stb_image.h`, `stb_image_write.h`, `stb_image.cpp` |
  | `physx/` | PhysX 5 `include/`, `lib/{release,debug}` (Windows, PhysX backend) |
  | `DLLs/` | the PhysX runtime DLLs |
  | `imgui/`, `imguizmo/`, `miniaudio/`, `nlohmann/`, `lua/` | fetched by `tools/fetch_deps.py` |
- Shader programs are cached as driver binaries after the first run, in `%LOCALAPPDATA%\Phisiks\ShaderCache`. Override the location with the `PHISIKS_SHADER_CACHE_DIR` environment variable; delete the folder to force a cold compile.

## Build
```powershell
# Windows, PhysX + WASAPI
cmake --preset vs2026-x64
cmake --build --preset release          # or relwithdebinfo (symbols) / debug
ctest -C Release --test-dir Build/vs2026
# Windows, the portable backends used on Linux / macOS (native physics + miniaudio) -> Build/bin-portable
cmake --preset windows-portable
cmake --build --preset portable-release
```
```sh
# Linux (macOS: macos-release / macos-debug)
python3 tools/fetch_deps.py
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release            # add -DPHISIKS_TEST_SKIP_RENDER=ON at configure time without a display
```
CI (`.github/workflows/ci.yml`) builds and tests the portable configuration on Ubuntu 24.04, macOS 14 and Windows Server 2022.
From Git Bash, pass MSBuild switches with dashes (e.g. `-- -m`), because `/m` gets rewritten into a path.

## Outputs (`Build/bin/<Config>/`)
| File | What |
|---|---|
| `PhisiksLab.exe` | Interactive sample app on the Phisiks forward PBR renderer |
| `Showcase01_StuntCar.exe` .. `Showcase06_BlackHole.exe` | The six showcases (see `README.md`); `--autotest` runs their runtime verification |
| `PhisiksUnitTests.exe` | Headless unit tests (also run by ctest) |
| `assetgen.exe [dir]` | Regenerates the CC0 sample assets (default `assets/`) |
| `glprobe.exe` | GPU capability check (`--all` lists extensions) |
| `stacksample.exe <pid>` | Symbolized stacks of a running process (hang diagnosis) |
| `*.dll` | PhysX runtime, copied after each build |

## Run (working directory: the project root, which contains `assets/`)
```powershell
.\Build\bin\Release\PhisiksLab.exe                              # interactive
.\Build\bin\Release\PhisiksLab.exe --scene PhysicsTower
.\Build\bin\Release\PhisiksLab.exe --scene FieldLab        # GPU particles, XPBD cloth/rope/soft body, force fields, SDF
.\Build\bin\Release\PhisiksLab.exe --autotest --run-dir Build/runs/mytest   # exit code = failed checks
```
Logs go to `<run-dir>/engine.log` (default `Build/runs/latest`). Crash dumps go to `<run-dir>/crash.dmp`.

Showcase scenes (also as standalone executables): `StuntCar`, `FluidHarbor`, `Robotics`, `Villa`, `Vision`, `BlackHole` (PhisiksLab: SHOWCASE playlist, or `--scene <name>`).

Scenes: `Sandbox`, `PhysicsTower`, `FieldLab`, the lesson scenes `Rotors`, `Orbits`, `PBR`, `Fountain`, and the P9 scenes `Animation` (glTF skinning, camera path), `VFX` (bloom, SSAO, trails, beam, decals), `Terrain` (terrain, foliage, nav crowd, spatial audio) and `Joints` (joints + Lua) (`N` cycles).

Studio options: `--no-studio` disables the ImGui panels, `--quality low|medium|high` applies a scalability preset, `--gl41` forces the OpenGL 4.1 tier, `--hot-reload [dir]` watches GLSL sources (default `engine/shaders`) and rebuilds changed programs while running. `Tab` hides / shows the panels; the bitmap HUD telemetry only appears while they are hidden.

## PhisiksLab studio (Dear ImGui)
| Panel | What |
|---|---|
| Menu bar | `=== PHISIKS LAB ===`; Scene: save / load JSON (path field), reload; Lessons; View: hide panels (Tab) |
| PHISIKS LAB (classroom) | lesson playlist (12 lessons, black list box, phosphor-green rows, gold selection) and the current lesson's caption; "SIM EQUALISER" vertical gradient sliders (gravity, friction, solver substeps, exposure, render scale, bloom); "RENDER DECK": quality preset, upscaler, RCAS sharpness, SSAO radius, LED toggles (culling, Hi-Z, shadows, fog, bloom, SSAO, decals, physics debug, camera fly-through), shader reload (LED: watching / failed) |
| Scene hierarchy | entity tree in a playlist box with right-aligned descendant counts; click to select (or click an object in the viewport) |
| Inspector | name, Transform (+ translate / rotate / scale gizmo, world / local), mesh renderer + live material editing (incl. splat layers), light, collider, rigidbody, joint (state, motor), skinned model, camera path, nav agent, Lua script (errors, reload), audio source (L/R LED meters, distance, Doppler), terrain, decal, trail, force field, particle system, soft body |
| Telemetry | LCD sim clock + FPS / step / state; CPU / GPU / PHYS / RAM LED bar meters; frame-time spectrum analyser; OS, OpenGL tier, physics backend, resolution, culling, bodies, joints |
| Replay timeline | transport buttons (step back, play, pause, stop = rewind to the first step, step forward, record, eject = clear), scrub slider over the replay ring (30 s at 60 Hz); playing after a scrub continues from that step | The autotest visits all three and runs the GPU compute checks (culling, Hi-Z, spatial hash, XPBD, N-body, fields, SDF) on the machine's GPU; it needs OpenGL 4.6 with compute shaders and at least 16 shader-storage bindings.

## PhisiksLab controls
| Input | Action |
|---|---|
| WASD / arrows | move |
| Shift | sprint |
| Space | jump |
| Mouse | look |
| E | interact |
| LMB | push / damage |
| V | cycle camera: first person, third person, free-fly (Space/Ctrl for up/down) |
| **F4** | orbit camera (RMB drag orbits, MMB drag pans, wheel zooms) |
| **F3** | quad view: perspective + orthographic top/front/side |
| F1 | overlay |
| F2 | physics debug draw |
| R | reload scene |
| N | next scene |
| Esc | pause (Q quits while paused) |

Closing the window shuts down cleanly.

## Simulation runs and export
```powershell
# Deterministic, headless (no window/GPU/audio device): exactly one 1/60 s step per frame, CSV of every dynamic body
.\Build\bin\Release\PhisiksLab.exe --scene PhysicsTower --headless --deterministic --max-frames 600 --export-dir Build/runs/sim --export-data
# Rendered offscreen (hidden window), PNG every 10th frame
.\Build\bin\Release\PhisiksLab.exe --scene PhysicsTower --hidden --deterministic --max-frames 300 --export-dir Build/runs/frames --export-frames --export-every 10
```
- `--steps-per-frame <n>` advances n fixed steps per frame.
- `--export-vision` writes the synthetic-vision passes every exported frame to `<export-dir>/vision/`: `frame_N_rgb.png`, `frame_N_depth.pfm` (float32 linear depth in metres along the view axis, 0 = background), `frame_N_normal.png` (world normal × 0.5 + 0.5), `frame_N_id.png` (Entity::Id() + 1 as little-endian RGBA bytes), plus `cameras.csv` (intrinsics fx, fy, cx, cy, near, far and the view matrix per frame) and `entities.csv` (id → name). All passes are at the scene resolution and pixel-aligned.
- Deterministic runs use the null audio device.
- ctest `DeterministicReplay` runs the headless case twice and requires byte-identical CSV output.

## Assets
Runtime assets live in `assets/` (textures, audio, meshes):
- `tools/assetgen` generates them procedurally, and they are released under CC0 (`assets/LICENSE.md`).
- Regenerate them with `.\Build\bin\Release\assetgen.exe assets`.
- Textures decode on a background thread and stream into the renderer when ready.
- OBJ meshes and WAV clips load on first use.

## Showcases
```powershell
.\Build\bin\Release\Showcase02_FluidHarbor.exe                       # 1920x1080, Medium preset
.\Build\bin\Release\Showcase03_Robotics.exe --quality low --gl41     # the macOS / old-iGPU path on any GPU
.\Build\bin\Release\Showcase06_BlackHole.exe --hot-reload             # edit engine/shaders/doppler.glsl while it runs
.\Build\bin\Release\Showcase05_Vision.exe --headless --export-vision --export-dir Build/vision --export-every 30 --max-frames 600
.\Build\bin\Release\Showcase01_StuntCar.exe --autotest                # report: Build/runs/StuntCar/showcase_report.txt
```
`--autotest` runs the demo's own steps, the five camera shots, the five view modes, a 300-frame frame-budget measurement (average must stay under 16.7 ms; `--quick` skips it) and the budgets (0 OpenGL errors, working set under 2.5 GB). ctest runs every showcase with `--quick --hidden` plus the vision export check (`tools/test_vision_export.cmake`).

Vision export layout (`<export-dir>/vision/`): `frame_NNNNNN_rgb.png`, `_depth.pfm` (linear metres), `_normal.png` (world normals), `_id.png` (entity id + 1, little-endian RGBA), `cameras.csv` (intrinsics + view matrix per frame), `entities.csv` (id, name), `classes.csv` (id, name, class) and `lidar_sweep_NNNN.csv` (x, y, z, distance, channel, azimuth step, entity id, class).
