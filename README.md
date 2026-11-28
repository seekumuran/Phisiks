# PHISIKS

**PHISIKS is a modular, resource-aware 3D engineering visualisation and simulation engine with an integrated editor, CAD modelling capabilities, a reusable C++ SDK and controlled external-agent integration.** It is written in C++20 on OpenGL and runs on modest hardware, including integrated GPUs. It needs no cloud service and contains no AI.

Its strengths:

- **Two OpenGL feature tiers:** 4.6 (GPU culling and compute) and 4.1 (CPU culling and CPU compute), running the same scenes.
- **Integrated-GPU compatibility:** developed and tested on Intel UHD Graphics; Low / Medium quality presets and FSR 1 upscaling.
- **Headless and deterministic simulation:** fixed-step runs without a window, reproducible with the same build on the same machine.
- **Data and synthetic-vision export:** CSV telemetry, PNG frames, and RGB / depth / normals / entity-id images.
- **Physics and compute with CPU fallback:** PhysX or a built-in solver; particles, cloth, rope, soft bodies and force fields on the GPU or on CPU threads.
- **An extensible editor** with undo / redo for every edit, separate edit and play modes, and a project asset pipeline with stable asset ids.
- **Parametric CAD modelling** on an exact B-rep kernel (Open CASCADE): constrained sketches, features driven by parameters, STEP / IGES exchange ([docs/CAD.md](docs/CAD.md)).
- **An installable SDK:** `find_package(PHISIKS)` from your own CMake project ([SDK.md](SDK.md)).
- **PAI, the PHISIKS Agent Interface:** a local, permissioned JSON-RPC interface for external tools and agents, off by default ([docs/PAI.md](docs/PAI.md)).

PHISIKS is **not**:

- a replacement for a commercial CAD suite: no assemblies with mates, sweeps, lofts, shells, drawings or tolerancing (see [docs/CAD.md](docs/CAD.md));
- a validated engineering-physics solver: its physics is real-time and game-grade;
- an AI system: PAI lets an external program you start drive the editor; PHISIKS itself runs no model and calls no service.

![platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-555) ![licence](https://img.shields.io/badge/licence-MIT-555)

Platform status for 1.1. What was verified, and how, is in [RELEASE_NOTES.md](RELEASE_NOTES.md); limitations are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

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
| **Geometry and assets** | OBJ and STL meshes, glTF 2.0 / GLB with skinning, PNG / JPG / TGA / BMP textures (decoded on a worker thread), WAV audio; a project asset database with stable ids, `.meta` sidecars, import, rename / move tracking and missing-asset detection |
| **CAD** (`PhisiksCad`) | sketches with geometric and dimensional constraints and a numerical solver; a parametric feature history (extrude, revolve, primitives, fillet, chamfer, booleans, patterns, mirror, import); exact B-rep geometry, measurement and STEP / IGES / STL / BREP exchange through Open CASCADE |
| **Physics** | rigid bodies, box / sphere / capsule / convex / triangle-mesh / height-field colliders, hinge / ball-and-socket / prismatic / spring-damper joints, a character controller, raycasts. Backends: NVIDIA PhysX 5 (Windows) or a built-in solver (all platforms) |
| **Compute** | GPU particles with a spatial hash, XPBD cloth / rope / soft bodies, force fields (uniform, radial, vortex, N-body), an SDF visualiser; on OpenGL 4.6 compute or a multi-threaded CPU backend |
| **World and AI** | terrain with splat maps, instanced foliage, navigation grids, A* path planning, crowd steering |
| **Scripting and audio** | sandboxed Lua 5.4 with hot reload; a software mixer with 3D positional sound and Doppler |
| **Simulation infrastructure** | fixed-step simulation, deterministic headless runs, CSV / PNG export, synthetic vision export (RGB, depth, normals, entity ids), a replay ring buffer with timeline scrubbing |
| **Editor** | the PHISIKS editor (below), also a library (`Phisiks::Editor`) for your own applications |
| **Agent interface** (`PhisiksPAI`) | JSON-RPC 2.0 over the editor's stdin / stdout with read / write / simulate permissions |

Two feature tiers: **OpenGL 4.6** (GPU culling and compute) and **OpenGL 4.1** (CPU culling and CPU compute: older GPUs and macOS). `--gl41` forces the 4.1 tier anywhere.

## The editor

`Phisiks` opens an empty scene in the editor:

- **Scene** (left): the hierarchy, with search, drag-to-parent, a context menu and descendant counts.
- **Inspector** (right): name, transform, mesh and material, light, collider, rigidbody, joints, animation, navigation, scripts, audio, VFX and compute components; add components.
- **Viewport** (centre): orbit / pan / zoom, selection by picking, move / rotate / scale gizmo (world or local), frame selection, and a 4-way perspective + orthographic view.
- **Settings** (left): physics (gravity, substeps, friction) and rendering (quality preset, upscaler, exposure, bloom, SSAO, culling, shadows, fog, shader hot reload).
- **Output** (bottom): console (filterable engine log), telemetry (frame time, GPU / physics time, memory, renderer info), the replay timeline and the **assets** browser (project folders, search, type filter, metadata, import, missing assets).
- **File:** New, Open, Save, Save as, Reopen, Import asset, with an unsaved-changes prompt.
- **Edit:** undo / redo with the action's name; a continuous interaction (a gizmo drag, a dragged value, a sketch point drag) is one step.
- **Create:** empty, cube, sphere, plane, dynamic rigid cube, lights, **CAD part** (parametric block or empty).
- **Simulation:** play, pause, step one frame, stop. **Play** captures the authored scene; **Stop** restores it (entities, components, selection). Editing and saving are disabled while playing.
- **CAD:** select a CAD part to edit its parameters, features and sketches in the inspector and the viewport ([docs/CAD.md](docs/CAD.md)).

Scene files store every component (format 2; version-1 files are migrated). Components holding C++ callbacks can't be stored; File > Save lists them with their counts and writes nothing until you confirm or cancel.

Tab hides every panel. **Help > Controls** lists the shortcuts. The UI scales with the monitor's DPI (override with `PHISIKS_UI_SCALE`).

What undo covers, what play mode preserves and the editor's limits are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

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

Install the SDK and use it from any CMake project ([SDK.md](SDK.md) has the details):

```sh
cmake --install Build/vs2026 --config Release --component sdk --prefix C:/sdk/phisiks
```

```cmake
find_package(PHISIKS 1.1 CONFIG REQUIRED)               # -DCMAKE_PREFIX_PATH=C:/sdk/phisiks
target_link_libraries(MyApp PRIVATE Phisiks::Editor)    # or Phisiks::Core, Phisiks::Cad, Phisiks::PAI
phisiks_copy_runtime(MyApp)                             # Windows: DLLs next to the program
```

- **Public API:** the headers under `engine/include/Phisiks/` plus `Editor.h`, `EditorApp.h` and `Pai.h`. The layering check (`tools/check_layering.cmake`, ctest `Layering`) fails the build if a public header, the editor, PAI or an example project includes anything else.
- **Version:** `Phisiks/Version.h` (`PHISIKS_VERSION_MAJOR / MINOR / PATCH`, `PHISIKS_VERSION_STRING`, `PHISIKS_VERSION`), generated from `project(Phisiks VERSION ...)`.
- **Third-party types in the API:** GLM 1.0.1, EnTT 3.11.1 and Dear ImGui 1.93.0. Every other dependency (GLFW, glad, stb, nlohmann/json, miniaudio, Lua, PhysX, Open CASCADE) is private.
- **Verified:** ctest `SdkConsumer` installs the SDK, copies `examples/sdk_consumer` out of the source tree, builds it against the installed package and runs it (a headless simulation, a parametric CAD part, an asset database).

An application implements `Phisiks::IGame` and runs `Phisiks::Application`, or puts the editor around its scenes with `Editor::Run`; `PhisiksSamples` (`examples/samples`) and `examples/sdk_consumer` are built that way. [PHISIKS_ARCHITECTURE.md](PHISIKS_ARCHITECTURE.md) describes the modules and extension points.

## Assets

| Kind | Formats | In the editor |
|---|---|---|
| Static meshes | **OBJ**, **STL** (binary / ASCII), built-in primitives | import, add to the scene, assign to a mesh renderer |
| Animated / skinned models | **glTF 2.0** / **GLB** (`SkinnedModel`) | import, add, assign |
| Images | **PNG, JPG, TGA, BMP** | import, assign as an entity material's texture |
| Audio | **WAV** | import, assign to an audio source |
| CAD | **STEP**, **IGES** (exact geometry), `.phcad` (PHISIKS parametric part) | add or assign as a CAD part (an Import feature keeps the exact geometry); CAD parts export STEP / IGES / STL / BREP / `.phcad` |
| Scenes, scripts | `phisiks-scene` JSON, Lua | File > Open / Save; assign scripts |

A **project** is a folder with `assets/` and `scenes/` (`--project <dir>` or the assets browser). Every asset gets a `.meta` file with a stable id; scenes refer to assets as `asset:<id>/<name>`, so renaming or moving files (in the browser or outside PHISIKS) keeps references working, duplicate imports return the existing asset, missing files are reported, and changed files are reimported. Plain relative paths (`meshes/arm.obj`) still resolve through the search paths: `--assets <dir>`, `assets/` next to the executable, `assets/` in the working directory.

STL is triangles only, in both the mesh and the CAD pipeline: an STL is never an editable parametric model. Limits are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Examples

These are separate programs built on the engine. They are not part of the editor.

| Program | Where | What |
|---|---|---|
| `PhisiksSamples` | `examples/samples` | the engine sample scenes (physics, PBR, GPU compute, skinning, VFX, terrain and navigation, joints + Lua) in the editor, with a sample browser |
| `Showcase01_RoboticArm` .. `Showcase04_TurbineCad` | `examples/0N_*` | four standalone demos: a 6-DOF arm with IK, a pool with a floating ball and caustics, a ray-traced Schwarzschild black hole, a turbine assembly with exploded view |
| `sdk_consumer` | `examples/sdk_consumer` | a minimal application built out of tree against the installed SDK (simulation, assets, a parametric CAD part) |
| `pai_client.py` | `examples/pai_client` | an example agent-interface client: starts the editor, edits and simulates a scene over PAI |

## Documentation

| Document | Contents |
|---|---|
| [BUILD_WINDOWS.md](BUILD_WINDOWS.md), [BUILD_LINUX.md](BUILD_LINUX.md), [BUILD_MACOS.md](BUILD_MACOS.md) | toolchains, dependencies, configure / build / test / package commands, tested results |
| [PHISIKS_ARCHITECTURE.md](PHISIKS_ARCHITECTURE.md) | engine modules, targets, the editor and its extension points, repository layout |
| [SDK.md](SDK.md) | installing and using the SDK, public API, ownership and errors, versions, redistribution |
| [docs/CAD.md](docs/CAD.md) | the CAD workspace, kernel decision and licence, features, file format, limits |
| [docs/PAI.md](docs/PAI.md) | the agent interface: protocol, methods, permissions, security model |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | limitations and known problems, per area and platform |
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | what is new in 1.1.0, how it was verified, what was not verified |
| [BUILDING.md](BUILDING.md) | building, testing, installing the SDK and packaging, on every platform |
| `packaging/QUICKSTART.txt` | the release package's quick start (controls, programs) |
| `docs/` | architecture decisions (pre-1.1 names), the clean-room record, CAD and PAI guides |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | third-party libraries and their licences |

MIT licensed. See [LICENSE](LICENSE).
