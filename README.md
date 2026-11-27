# PHISIKS

**A general-purpose 3D engine.** PHISIKS is a C++20 / OpenGL engine with an editor, for building your own real-time 3D applications: tools, simulations, visualisations and games. You get its renderer, scene system, geometry, physics, compute and development infrastructure, and you write the application.

![platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-555) ![licence](https://img.shields.io/badge/licence-MIT-555)

Platform status for 1.1. What was verified, and how, is in [PHISIKS_RELEASE_LOG.md](PHISIKS_RELEASE_LOG.md); limitations are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

| Platform | Build | Tests | Status |
|---|---|---|---|
| Windows 10/11 x64 (MSVC) | local + CI | unit, layering, replay, showcases, runtime tests on the OpenGL 4.6 and 4.1 tiers (real GPU) | supported |
| Linux x64 (GCC) | CI (Ubuntu 24.04, 22.04) + release archive | unit, layering, replay; runtime tests on Mesa llvmpipe (software GL, 4.1 tier) | supported; hardware-GPU desktops not yet tested |
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
| **Editor** | the PHISIKS editor (below), also usable as a library (`PhisiksEditor`) by your own applications |

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

Tab hides every panel. **Help > Controls** lists the shortcuts. The UI scales with the monitor's DPI (override with `PHISIKS_UI_SCALE`).

The editor is honest about its scope. There is **no undo/redo** and no asset-import pipeline yet. Meshes are referenced by path (`assets/…`, or a folder added with `--assets`). The full list is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

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

## Building your own application on PHISIKS

An application implements `Phisiks::IGame`, registers its scenes and runs `Phisiks::Application`. To get the editor around it, use the `PhisiksEditor` library:

```cpp
#include "EditorApp.h"   // apps/editor/src

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

A scene that also derives from `Editor::WorkspaceExtension` gets its own panel and save / load hooks. [PHISIKS_ARCHITECTURE.md](PHISIKS_ARCHITECTURE.md) describes the modules, the public API boundary and the editor extension points.

## Examples and example projects

These are separate programs built on the engine. They are not part of the editor.

| Program | Where | What |
|---|---|---|
| `PhisiksSamples` | `examples/samples` | the engine sample scenes (physics, PBR, GPU compute, skinning, VFX, terrain and navigation, joints + Lua) in the editor, with a sample browser |
| `Showcase01_RoboticArm` .. `Showcase04_TurbineCad` | `examples/0N_*` | four standalone demos: a 6-DOF arm with IK, a pool with a floating ball and caustics, a ray-traced Schwarzschild black hole, a turbine assembly with exploded view |
| `ProjectMarina` | `projects/ProjectMarina` | example project: an editable membrane-distillation desalination prototype (schematic flow direction, no thermal / fluid simulation) |
| `OldMcDonald` | `projects/OldMcDonald` | example project: a greenhouse with a mapping drone and a soil rover (A* route and arm IK computed; flight, mapping and measurements scripted), plus standalone rover / drone CAD workspaces |

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
