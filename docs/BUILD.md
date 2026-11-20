# Building and Running Phisiks

Layout:
- `engine/`: the PhisiksCore library. `include/Phisiks` is the public API, `src` is private, and `shaders` holds GLSL that is embedded at build time.
- `apps/lab`: PhisiksLab.
- `tests/unit`: headless unit tests.
- `tests/runtime`: the runtime verification harness built into PhisiksLab.
- `assets/`: CC0 sample assets.
- `tools/`: assetgen, glprobe, stacksample and the CMake check scripts.

The `check_layering` target runs before the engine builds and fails the build if apps or tests bypass the public API.

## Requirements
- Windows 10/11 x64, Visual Studio 2026 (v145) C++ tools or Build Tools, CMake ≥ 3.25.
- GPU: OpenGL 4.6 core; the renderer needs nothing beyond it. Check with `Build/bin/Release/glprobe.exe`.
- Third-party libraries in `vendor/` (not committed; see `THIRD_PARTY_NOTICES.md`):

  | Folder | Contents |
  |---|---|
  | `glad/` | generated GL 4.6 loader: `include/`, `src/gl.c` |
  | `GLFW/` | `include/`, `lib/{release,debug}/glfw3.lib` |
  | `glm/glm/` | headers |
  | `entt/entt.hpp` | single header |
  | `stb_image/` | `stb_image.h`, `stb_image_write.h`, `stb_image.cpp` |
  | `physx/` | PhysX 5 `include/`, `lib/{release,debug}` |
  | `DLLs/` | the PhysX runtime DLLs |
- Shader programs are cached as driver binaries after the first run, in `%LOCALAPPDATA%\Phisiks\ShaderCache`. Override the location with the `PHISIKS_SHADER_CACHE_DIR` environment variable; delete the folder to force a cold compile.

## Build
```powershell
cmake --preset vs2026-x64
cmake --build --preset release          # or relwithdebinfo (symbols) / debug
ctest -C Release --test-dir Build/vs2026
```
From Git Bash, pass MSBuild switches with dashes (e.g. `-- -m`), because `/m` gets rewritten into a path.

## Outputs (`Build/bin/<Config>/`)
| File | What |
|---|---|
| `PhisiksLab.exe` | Interactive sample app on the Phisiks forward PBR renderer |
| `PhisiksUnitTests.exe` | Headless unit tests (also run by ctest) |
| `assetgen.exe [dir]` | Regenerates the CC0 sample assets (default `assets/`) |
| `glprobe.exe` | GPU capability check (`--all` lists extensions) |
| `stacksample.exe <pid>` | Symbolized stacks of a running process (hang diagnosis) |
| `*.dll` | PhysX runtime, copied after each build |

## Run (working directory: the project root, which contains `assets/`)
```powershell
.\Build\bin\Release\PhisiksLab.exe                              # interactive
.\Build\bin\Release\PhisiksLab.exe --scene PhysicsTower
.\Build\bin\Release\PhisiksLab.exe --autotest --run-dir Build/runs/mytest   # exit code = failed checks
```
Logs go to `<run-dir>/engine.log` (default `Build/runs/latest`). Crash dumps go to `<run-dir>/crash.dmp`.

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
- Deterministic runs use the null audio device.
- ctest `DeterministicReplay` runs the headless case twice and requires byte-identical CSV output.

## Assets
Runtime assets live in `assets/` (textures, audio, meshes):
- `tools/assetgen` generates them procedurally, and they are released under CC0 (`assets/LICENSE.md`).
- Regenerate them with `.\Build\bin\Release\assetgen.exe assets`.
- Textures decode on a background thread and stream into the renderer when ready.
- OBJ meshes and WAV clips load on first use.
