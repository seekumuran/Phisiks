# PHISIKS

**PHISIKS is a resource-aware C++20 3D visualisation and simulation engine with an integrated editor.** It is built on OpenGL and written to run on modest hardware, including integrated GPUs. You get its renderer, scene system, geometry, physics, compute and development infrastructure, and you write the application.

Its strengths:

- **Two OpenGL feature tiers:** 4.6 (GPU culling and compute) and 4.1 (CPU culling and CPU compute), running the same scenes.
- **Integrated-GPU compatibility:** developed and tested on Intel UHD Graphics; Low / Medium quality presets and FSR 1 upscaling.
- **Headless and deterministic simulation:** fixed-step runs without a window, reproducible with the same build on the same machine.
- **Data and synthetic-vision export:** CSV telemetry, PNG frames, and RGB / depth / normals / entity-id images.
- **Physics and compute with CPU fallback:** PhysX or a built-in solver; particles, cloth, rope, soft bodies and force fields on the GPU or on CPU threads.
- **An extensible editor:** hierarchy, inspector, gizmos and console, with workspace hooks for application-specific tools.

PHISIKS is **not**:

- a CAD modeller: its CAD workspaces present and explode assemblies; they don't create or dimension geometry;
- an installable SDK package: see [Building on PHISIKS](#building-on-phisiks);
- a validated engineering-physics solver: its physics is real-time and game-grade;
- an AI-agent platform.

![platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-555) ![licence](https://img.shields.io/badge/licence-MIT-555)

Platform status for 1.1. What was verified, and how, is in [PHISIKS_RELEASE_LOG.md](PHISIKS_RELEASE_LOG.md); limitations are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

| Platform | Build | Tests | Status |
|---|---|---|---|
| Windows 10/11 x64 (MSVC) | local + CI | unit, layering, replay, showcases, runtime tests on the OpenGL 4.6 and 4.1 tiers (real GPU) | supported |
| Linux x64 (GCC) | CI (Ubuntu 24.04, 22.04) + release archive | unit, layering, replay; runtime tests on Mesa llvmpipe (software GL, 4.1 tier) | supported; rendering verified on software GL only, no hardware-GPU desktop tested |
| macOS arm64 (Apple Clang) | CI (macOS 15) + release archive | unit, layering, replay. **Rendering untested:** no OpenGL-capable macOS runner or Mac was available | experimental (OpenGL 4.1, deprecated by Apple) |

## What the engine provides

| Module | What it does |
|---|---|
| **Scene** | EnTT entity-component scenes with transform hierarchies; JSON scene files (`phisiks-scene`), loaded with validation and saved atomically |
| **Rendering** | forward PBR, shadows, fog, GPU frustum + Hi-Z occlusion culling, MSAA, FSR 1 upscaling, bloom, SSAO, decals, trails, a screen-space fluid pass, CAD edge ink, multiple viewports and offscreen cameras, a binary shader cache, live GLSL hot reload, GPU timers |
| **Geometry and assets** | OBJ meshes, glTF 2.0 / GLB with skinning, PNG / JPG / TGA / BMP textures (decoded on a worker thread), WAV audio; asset roots next to the executable or in the working directory |
| **Physics** | rigid bodies, box / sphere / capsule / convex / triangle-mesh / height-field colliders, hinge / ball-and-socket / prismatic / spring-damper joints, a character controller, raycasts. Backends: NVIDIA PhysX 5 (Windows) or a built-in solver (all platforms) |
| **Compute** | GPU particles with a spatial hash, XPBD cloth / rope / soft bodies, force fields (uniform, radial, vortex, N-body), an SDF visualiser; on OpenGL 4.6 compute or a multi-threaded CPU backend |
| **World and AI** | terrain with splat maps, instanced foliage, navigation grids, A* path planning, crowd steering |
| **Scripting and audio** | sandboxed Lua 5.4 with hot reload; a software mixer with 3D positional sound and Doppler |
| **Simulation infrastructure** | fixed-step simulation, deterministic headless runs, CSV / PNG export, synthetic vision export (RGB, depth, normals, entity ids), a replay ring buffer with timeline scrubbing |
| **Editor** | the PHISIKS editor (below), also an in-tree library (`PhisiksEditor`) for your own applications |

Two feature tiers: **OpenGL 4.6** (GPU culling and compute) and **OpenGL 4.1** (CPU culling and CPU compute: older GPUs and macOS). `--gl41` forces the 4.1 tier anywhere.

## The editor

`Phisiks` opens an empty scene in the editor:

- **Scene** (left): the hierarchy, with search, drag-to-parent, a context menu and descendant counts.
- **Inspector** (right): name, transform, mesh and material, light, collider, rigidbody, joints, animation, navigation, scripts, audio, VFX and compute components; add components.
- **Viewport** (centre): orbit / pan / zoom, selection by picking, move / rotate / scale gizmo (world or local), frame selection, and a 4-way perspective + orthographic view.
- **Settings** (left): physics (gravity, substeps, friction) and rendering (quality preset, upscaler, exposure, bloom, SSAO, culling, shadows, fog, shader hot reload).
- **Output** (bottom): console (filterable engine log), telemetry (frame time, GPU / physics time, memory, renderer info) and the replay timeline.
- **File:** New, Open, Save, Save as, Reopen, with an unsaved-changes prompt.
- **Create:** empty, cube, sphere, plane, dynamic rigid cube, directional / point / spot light.
- **Simulation:** play, pause, step one frame, reload.

Saving never drops data silently. Scene files don't store some components (joints, scripts, terrain, decals and others; see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). If the scene has any, File > Save lists them with their counts and writes nothing until you confirm or cancel.

Tab hides every panel. **Help > Controls** lists the shortcuts. The UI scales with the monitor's DPI (override with `PHISIKS_UI_SCALE`).

The editor is honest about its scope:
- There is **no undo/redo** and no asset-import pipeline.
- Play runs the scene you're editing, so there's no separate play mode.
- Meshes are referenced by path (`assets/…`, or a folder added with `--assets`).

The full list is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Build

| Platform | Guide |
|---|---|
| Windows (Visual Studio 2022 / 2026) | [BUILD_WINDOWS.md](BUILD_WINDOWS.md) |
| Linux (GCC 11+ / Clang 15+) | [BUILD_LINUX.md](BUILD_LINUX.md) |
| macOS (Apple Clang, experimental) | [BUILD_MACOS.md](BUILD_MACOS.md) |

The short version, on Linux:

```sh
python3 tools/fetch_deps.py
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/Phisiks
```

Run development builds from the repository root: executables look for `assets/` next to themselves first, then in the working directory.

### Release packages

The GitHub release contains exactly three archives, built by the `release` workflow (`.github/workflows/release.yml`) on GitHub's runners with released toolchains and the built-in physics backend:

| Archive | Built on |
|---|---|
| `Phisiks-<version>-win64.zip` | Windows Server 2022, Visual Studio 2022 |
| `Phisiks-<version>-linux-x86_64.tar.gz` | Ubuntu 22.04, GCC 12 |
| `Phisiks-<version>-macos-arm64.zip` | macOS 15, Apple Clang |

How to check where an archive came from:
- Each archive contains `BUILD_INFO.txt`, with the commit, the CI run URL, the compiler, the CMake version and the backends. The workflow refuses to upload an archive whose `BUILD_INFO.txt` doesn't name that run and commit, or whose toolchain isn't a released one.
- `SHA256SUMS.txt` lists the archives' checksums.

No locally built package is attached. The PhysX build (Windows) is for building from source: see [BUILD_WINDOWS.md](BUILD_WINDOWS.md).

## Building on PHISIKS

PHISIKS 1.1 is used **in-tree**: you add your application to this repository's CMake project and build it with the engine. **This release has no installable SDK package.** No headers or libraries are installed, and there is no `find_package(Phisiks)` configuration.

- **Public API:** the headers under `engine/include/Phisiks/` (library `PhisiksCore`). The layering check (`tools/check_layering.cmake`, ctest `Layering`) fails the build if a public header, the editor or an example project includes anything else. The editor and both example projects are built only against this API.
- **Version:** `#include "Phisiks/Version.h"` gives `PHISIKS_VERSION_MAJOR`, `PHISIKS_VERSION_MINOR`, `PHISIKS_VERSION_PATCH`, `PHISIKS_VERSION_STRING`, and `PHISIKS_VERSION` (major × 10000 + minor × 100 + patch). It's generated from `project(Phisiks VERSION ...)` in `CMakeLists.txt`.
- **Editor library:** `PhisiksEditor` is a reusable in-tree library. Its headers are `Editor.h` and `EditorApp.h` in `apps/editor/src`. `Editor::WorkspaceExtension` gives a scene its own panel and save/load hooks.
- **Third-party types in the API:** these libraries appear in public headers, so their pinned versions are part of the API.

  | Library | Version | Where it appears |
  |---|---|---|
  | GLM | 1.0.1 | Vectors, matrices and quaternions throughout |
  | EnTT | 3.11.1 | `Scene::Registry()` (`Phisiks/Scene/Scene.h`) |
  | Dear ImGui | 1.93.0 | `Phisiks/UI/ImGuiLayer.h`, `Phisiks/UI/RetroDeck.h`, editor panels (linked through `PhisiksImGui`) |

  Every other dependency (GLFW, glad, stb, nlohmann/json, miniaudio, Lua, PhysX) is private to `PhisiksCore`. `tools/fetch_deps.py` downloads the pinned versions, and `THIRD_PARTY_NOTICES.md` lists them all.

An application implements `Phisiks::IGame`, registers its scenes and runs `Phisiks::Application`. To put the editor around it, use `PhisiksEditor`:

1. Add an executable to `CMakeLists.txt` the way `projects/` does: `target_link_libraries(MyApp PRIVATE PhisiksEditor)`.
2. Write a `main` like this one:

```cpp
#include "EditorApp.h"   // apps/editor/src (PhisiksEditor)

class MyScene : public Phisiks::Scene { /* OnEnter: create entities; Update: your logic */ };

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "My application";
    launch.app.initialScene = "Main";
    if (int code = Editor::ParseCommandLine(argc, argv, "MyApp", launch)) return code == -1 ? 0 : code;
    Editor::Configure(Editor::AppConfig{});
    Editor::Host host;
    host.registerScenes = [](Phisiks::SceneManager& s) { s.Register("Main", [] { return std::make_unique<MyScene>(); }); };
    return Editor::Run(launch, host);
}
```

[PHISIKS_ARCHITECTURE.md](PHISIKS_ARCHITECTURE.md) describes the modules, the public API boundary and the editor extension points. `projects/OldMcDonald` and `projects/ProjectMarina` are complete applications built this way.

## Assets

| Kind | Formats the runtime loads | What the editor offers |
|---|---|---|
| Static meshes | Wavefront **OBJ** (`Assets::LoadObj`), plus built-in primitives (cube, sphere, plane, ...) | The inspector's mesh list: every `.obj` under the asset folders, with a rescan entry. Assign one to a mesh renderer. |
| Animated / skinned models | **glTF 2.0** (`.gltf` with `.bin` or data URIs) and **GLB**, through the `SkinnedModel` component | Shown in the inspector when present. **It can't be added from the editor** or stored in scene files: create it in code. |
| Images | **PNG, JPG, TGA, BMP** (stb_image), decoded on a worker thread | None. Editor materials are named procedural materials (colour, roughness, metallic, emissive, patterns). Textures are assigned in code (`Render::MaterialDesc`). |
| Audio | **WAV** only (`AudioSource` clips) | The inspector shows the clip and whether it's playing. Clips are set in code or in scene files. |
| Scenes | `phisiks-scene` JSON (`SceneSerializer`) | File > New / Open / Save / Save as |

**There is no asset-import pipeline.** Files aren't imported, converted, cached or given IDs. They are referenced by path and loaded the first time they're used.

**Search paths:** a relative asset path such as `meshes/hero/arm.obj` is looked up in this order, and the first match wins:

1. folders added with `--assets <dir>` (or `Assets::AddSearchPath`);
2. `assets/` next to the executable (an extracted release package);
3. `assets/` in the working directory (a development build run from the repository root);
4. the working directory itself.

Absolute paths are used as they are. Scene files store the relative paths you assigned, so a scene stays portable as long as its meshes stay under one of these roots. A missing file is reported in the console and the mesh is skipped.

**Limits:**
- Meshes and textures don't hot-reload; shaders and Lua scripts do.
- glTF uses only the base-colour texture, and doesn't support sparse accessors, morph targets or KHR extensions.

The full list is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Examples and example projects

These are separate programs built on the engine. They are not part of the editor.

| Program | Where | What |
|---|---|---|
| `PhisiksSamples` | `examples/samples` | the engine sample scenes (physics, PBR, GPU compute, skinning, VFX, terrain and navigation, joints + Lua) in the editor, with a sample browser |
| `Showcase01_RoboticArm` .. `Showcase04_TurbineCad` | `examples/0N_*` | four standalone demos: a 6-DOF arm with IK, a pool with a floating ball and caustics, a ray-traced Schwarzschild black hole, a turbine assembly with exploded view |
| `ProjectMarina` | `projects/ProjectMarina` | example project: an editable membrane-distillation desalination prototype (schematic flow direction, no thermal / fluid simulation) |
| `OldMcDonald` | `projects/OldMcDonald` | example project: a greenhouse with a mapping drone and a soil rover (A* route and arm IK computed; flight, mapping and measurements scripted), plus standalone rover and drone assembly workspaces (orthographic views, exploded view, subsystem visibility; no geometry editing) |

See [projects/README.md](projects/README.md).

## Documentation

| Document | Contents |
|---|---|
| [BUILD_WINDOWS.md](BUILD_WINDOWS.md), [BUILD_LINUX.md](BUILD_LINUX.md), [BUILD_MACOS.md](BUILD_MACOS.md) | toolchains, dependencies, configure / build / test / package commands, tested results |
| [PHISIKS_ARCHITECTURE.md](PHISIKS_ARCHITECTURE.md) | engine modules, targets, the editor and its extension points, repository layout |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | limitations and known problems, per area and platform |
| [PHISIKS_RELEASE_LOG.md](PHISIKS_RELEASE_LOG.md) | what changed in 1.1, verification results |
| `packaging/QUICKSTART.txt` | the release package's quick start (controls, programs) |
| `docs/` | design decisions, development history (`PHASE_LOG.md`, pre-1.1 names), the 1.1 release audit, clean-room record |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | third-party libraries and their licences |

MIT licensed. See [LICENSE](LICENSE).
