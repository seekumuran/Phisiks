# PHISIKS architecture

PHISIKS is a C++20 / OpenGL engine (`PhisiksCore`), an editor built on it (`PhisiksEditor`, `Phisiks`), and programs built on both: samples, showcases and example projects. This document describes the layering, the engine modules, the editor and its extension points, and the repository layout. Design rationale lives in `docs/ARCHITECTURE_DECISIONS.md`.

## Layers

```
Programs       Phisiks (editor) · PhisiksSamples · Showcase01..04 · ProjectMarina · OldMcDonald · PhisiksRuntimeTests
   │
Example libs   PhisiksSampleScenes (examples/samples) · PhisiksShowcase (examples/common + 0N_*)
   │           PhisiksProjectKit · ProjectMarinaScenes · OldMcDonaldScenes (projects/)
   │
Editor         PhisiksEditor (apps/editor/src): panels, viewport, scene files, launcher, WorkspaceExtension
   │
Engine         PhisiksCore: public API engine/include/Phisiks, private engine/src; PhisiksImGui, PhisiksLua
   │
Platform       OpenGL 4.6 / 4.1 · GLFW · PhysX 5 or native physics · WASAPI or miniaudio
```

Everything above the engine uses **only the public API** (`#include "Phisiks/..."`, glm, EnTT, Dear ImGui / ImGuizmo for UI). `tools/check_layering.cmake` runs before every build and fails it if an app, example, project or the runtime test includes an engine-private header or a backend library header (OpenGL loader, GLFW, PhysX). Unit tests may use engine-private headers (white-box), but not backend headers.

## Engine modules (`engine/include/Phisiks/<Module>`)

| Module | Responsibility | Key types / functions |
|---|---|---|
| `Runtime` | application loop, game interface, options | `Application`, `AppOptions`, `IGame` (`RegisterScenes`, `OnStart`, `OnUpdate`, `OnFixedUpdate`, `OnGui`, `OnQuitRequested`), `ITestHarness` |
| `Scene` | EnTT scenes, entities, components, hierarchy, JSON files | `Scene`, `Entity`, `Transform`, `MeshRenderer`, `Light`, `Camera`, `Collider`, `Rigidbody`, `SceneSerializer` |
| `Framework` | per-frame context, scene manager, camera controller, input actions, replay, debug overlay | `GameContext`, `SceneManager`, `CameraController`, `InputSystem`, `Replay` |
| `Render` | forward PBR renderer on OpenGL 4.6 / 4.1 | `Render::Settings()`, materials (`RegisterMaterial`), meshes (`RegisterMesh`), post passes, `CaptureFrame`, vision capture, quality presets, shader hot reload |
| `Physics` | rigid bodies, joints, character controller, raycasts; PhysX or native backend | `Physics::Init`, `AddImpulse`, `Raycast`, `JointState`, `Advanced::ParticleSystem / SoftBody`, `ForceField` |
| `Compute` | GPU (GL 4.6) / CPU compute backend for particles, XPBD, fields, culling | `IComputeBackend` |
| `Animation` | glTF 2.0 / GLB, linear and dual-quaternion skinning, splines, camera paths | `LoadGltf`, `SkinnedModel`, `CameraPath` |
| `World` | terrain, splat maps, foliage | `Terrain` |
| `AI` | navigation grid, A*, crowd steering | `NavigationSurface`, `NavAgent` |
| `Script` | sandboxed Lua 5.4 with hot reload | `Script`, `Scripting::Reload` |
| `Audio` | software mixer, 3D positional sound, Doppler | `AudioSource`, `Audio::GetSpatialInfo` |
| `VFX` | trails, ribbons, beams, decals | `Trail`, `Decal`, `Render::DrawRibbon` |
| `Assets` | file lookup (next to the executable, then the working directory, plus added roots), image / OBJ loading | `Assets::Resolve`, `AddSearchPath`, `LoadObj`, `LoadImageFile` |
| `Platform` | window, process / system info, paths | `ProcessMemoryBytes`, `ExecutableDirectory`, `UserDataDirectory`, `IsWritableDirectory` |
| `UI` | Dear ImGui layer and the console skin ("retro deck") | `UI::Init`, `UI::Scale`, `Retro::BeginDockedPanel`, `Lcd`, `LedMeter`, `EqSlider`, `TransportButton` |
| `Core`, `Math` | logging (with an in-memory ring for consoles), profiler, time, events; rotors, dual quaternions, integrators, SDFs | `Log::CopyRecent`, `Profiler` |

### Frame

`Application::Run`, per frame:

1. Time and window events, input latch, audio.
2. Scene transitions at a safe point (`SceneManager`).
3. While running: `IGame::OnUpdate`, scene `Update`, Lua.
4. Fixed steps: `OnFixedUpdate`, scene `FixedUpdate`, then the physics step.
5. Animation and presentation sync.
6. Render all cameras: each camera has a normalised viewport, so one window can show several views.
7. `IGame::OnGui`: ImGui, drawn after the 3D frame and never part of captures.
8. Swap.

A user quit (window close, Q while paused) first asks `IGame::OnQuitRequested`. Automated runs (`--max-frames`, test harness) quit directly.

### Feature tiers

At startup the renderer chooses **4.6** (SSBOs, GPU culling, Hi-Z occlusion, GPU compute) or **4.1** (texture-buffer storage, CPU culling, CPU compute on a thread pool). Shaders are written once: a preprocessor rewrites `#version 460` and `layout(binding = N)` for 4.1. `--gl41` forces the 4.1 tier on any GPU, which is how the macOS path is tested on Windows and Linux. macOS always runs the 4.1 tier.

### Scene files

`SceneSerializer` writes `phisiks-scene` JSON, version 1: entities with parent indices, transforms, built-in components, and the procedural materials they use. Loading parses and validates the **whole** file before the open scene is touched; a damaged or newer-version file fails with an error and changes nothing. Saving writes `<file>.tmp` and renames it over the target. Not stored: joints, scripts, navigation, terrain, decals, trails, skinned models, camera paths.

### Command-line simulation runs and export

Every editor-based program shares these options (`--help`):

```sh
# Deterministic and headless (no window, GPU or audio device): exactly one 1/60 s step per frame, CSV of every dynamic body
PhisiksSamples --scene PhysicsTower --headless --deterministic --max-frames 600 --export-dir out/sim --export-data
# Rendered offscreen (hidden window), a PNG every 10th frame
PhisiksSamples --scene PhysicsTower --hidden --deterministic --max-frames 300 --export-dir out/frames --export-frames --export-every 10
```

- `--steps-per-frame <n>` advances n fixed steps per frame. Deterministic runs use the null audio device. Replays are deterministic for the same build on the same machine.
- `--export-vision` writes, every exported frame, to `<export-dir>/vision/`. All passes are at the scene resolution and pixel-aligned:
  - `frame_N_rgb.png`;
  - `frame_N_depth.pfm`: float32 linear depth in metres along the view axis, 0 = background;
  - `frame_N_normal.png`: world normal × 0.5 + 0.5;
  - `frame_N_id.png`: `Entity::Id() + 1` as little-endian RGBA;
  - `cameras.csv`: intrinsics and the view matrix per frame;
  - `entities.csv`: id → name.
- ctest `DeterministicReplay` runs the headless case twice and requires byte-identical CSV.

## The editor (`apps/editor/src`, library `PhisiksEditor`)

| File | Contents |
|---|---|
| `Editor.h` | the public editor API (below) |
| `Editor.cpp` | layout (menu bar, tool bar, docked panels with splitters, inset viewport), File / Edit / Create / View / Simulation / Help menus, shortcuts, scene files (`SaveScene`, `LoadScene`, `ResolveScenePath`), unsaved-changes prompt, file dialog |
| `EditorPanels.cpp` | scene hierarchy (search, drag-to-parent, context menu), inspector (all components, add component, Euler cache), settings, output dock (console, telemetry, replay timeline), delete confirmation, Create menu |
| `EditorViewport.cpp` | fitting the main camera's viewport to the centre area, the 4-way view, gizmo (ImGuizmo, in the camera's viewport rect and aspect), picking through the renderer's entity-id buffer, frame selection |
| `EditorScene.cpp` | the empty document (`RegisterEmptyScene`): ground grid with a collider, sun, orbit camera |
| `EditorApp.h/.cpp` | the shared launcher: command line (`ParseCommandLine`), `Host` (an `IGame` that hosts the editor), `Run`, `AddAssetDirectory` |
| `Main.cpp` | the `Phisiks` executable |

### Extension points

- **`Editor::AppConfig`**, passed to `Editor::Configure`:
  - `menus`: extra menus, e.g. *Samples*, *Workspace*;
  - `sidePanel` + `sidePanelTitle`: the left panel for scenes that are not workspaces (default: settings);
  - `insetViewport`: fit the 3D view between the panels, or render under them;
  - `emptyScene`: the document File > New opens;
  - `title`.
- **`Editor::WorkspaceExtension`**, derived by a `Scene`:
  - `WorkspaceTitle`, `DrawWorkspacePanel`: the left panel below the hierarchy;
  - `DrawViewportOverlay`;
  - `DefaultScenePath`: the file that File > Save / Reopen use;
  - `SupportsReplay`;
  - `OnBeforeSave` / `OnAfterSave` / `OnAfterLoad`. Every save / load path (File menu, workspace buttons, reopen) goes through these hooks, so state that is not part of the file is kept consistent. The CAD workspaces use them to collapse exploded views before writing.
- **Editor services** for apps and workspaces:
  - selection: `SelectedEntity`, `Select`;
  - scene files: `SaveScene`, `LoadScene`, `ResolveScenePath`;
  - guarded navigation that asks about unsaved changes: `RequestScene`, `RequestReload`;
  - `MarkEdited` for edits the editor cannot see;
  - layout: `ViewportRect`, `RowButtonWidth`;
  - `SetQuadView`, `DrawSettingsPanel`, `ShowOutputTab`.

### Paths

Scene files, screenshots and the run directory follow one rule. Files are found **next to the executable first** (a release package), then in the **working directory** (a repository checkout). When that folder is not writable, a per-user copy is used: `Platform::UserDataDirectory()`, that is `%LOCALAPPDATA%\Phisiks`, `~/Library/Application Support/Phisiks` or `~/.local/share/phisiks`. Assets resolve the same way, plus any roots added with `Assets::AddSearchPath` / `--assets`.

## Programs on top of the editor

| Program | Source | Notes |
|---|---|---|
| `Phisiks` | `apps/editor/src/Main.cpp` | the editor, empty document |
| `PhisiksSamples` | `examples/samples` | the sample scenes; `SampleBrowser` supplies the side panel and the Samples menu |
| `Showcase01_RoboticArm` .. `04_TurbineCad` | `examples/0N_*`, framework `examples/common` (`PhisiksShowcase`) | standalone; their own console HUD, not the editor |
| `ProjectMarina`, `OldMcDonald` | `projects/<name>/src` + `projects/common` | separate applications. Their scenes derive from `WorkspaceExtension`; meshes in `projects/assets` (added with `Editor::AddAssetDirectory`) |
| `PhisiksRuntimeTests` | `tests/runtime` | hosts every scene above, with the `AutoTest` harness |

## Verification

| Layer | How |
|---|---|
| Unit | `PhisiksUnitTests` (`tests/unit`): math, scene, serializer, physics on both backends, compute (GPU kernels vs CPU references), renderer helpers, audio, assets. Headless |
| Structure | `Layering` (`tools/check_layering.cmake`) |
| Determinism | `DeterministicReplay`: two headless runs, byte-identical CSV; a hidden run exports PNGs |
| Runtime | `PhisiksRuntimeTests --autotest`, on both tiers: the real loop, renderer, physics and audio. It drives only through injected input and API calls, measures every check (entity-id pixels, captured frames, physics state) and writes `autotest_report.txt`. Covers the editor (console skin, inset viewport, picking, scene-file safety, unsaved changes), the samples, the showcases inside the host, and both example projects |
| Showcases | each `ShowcaseNN --autotest --quick --hidden` |

## Repository layout

```
apps/editor/        the editor (library PhisiksEditor + the Phisiks executable)
engine/             PhisiksCore: include/Phisiks (public API), src (private), shaders (GLSL, embedded)
examples/           samples/ (PhisiksSamples), common/ + 0N_*/ (the four showcases)
projects/           example projects: common/, ProjectMarina/, OldMcDonald/, assets/, tools/
tests/              unit/ (PhisiksUnitTests), runtime/ (PhisiksRuntimeTests)
tools/              assetgen, glprobe, stacksample, fetch_deps.py, CMake check scripts, ci_report.sh
assets/             engine / example assets (CC0, generated by assetgen)
packaging/          files only in release archives (QUICKSTART.txt)
docs/               design decisions, history (pre-1.1 names), the 1.1 release audit, clean-room record
vendor/             third-party code, not committed (tools/fetch_deps.py)
```
