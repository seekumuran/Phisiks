# Phisiks Roadmap

Phisiks is a general-purpose 3D simulation, visualisation and game engine:
- C++20 on Windows, Linux and macOS; OpenGL 4.6 with an OpenGL 4.1 tier;
- EnTT scenes; NVIDIA PhysX 5 or the native rigid-body solver;
- the default look is a dark studio with a rainbow calibration-grid floor.

Every phase ends with a clean Release build, all unit and runtime checks green, a commit and a tag. Phase history and measurements are in `PHASE_LOG.md`.

## Feature freeze (P10, `p10-showcase`)
The core engine API (`engine/include/Phisiks`) is frozen as of P10: from here on, changes to the engine are bug fixes, performance work, platform bring-up (the first Linux / macOS CI runs) and the open follow-ups below; new features go into games, `examples/` and tools on top of the public API. Breaking API changes need an ADR.

## Done

| Phase | Tag | Delivered |
|---|---|---|
| P0 | `p0-skeleton` | CMake layout (`PhisiksCore`, app, tests), layering check |
| P1 | `p1-core` | Platform/window, input, time and fixed step, events, logging, crash handler, CPU and non-blocking GPU profiler, application loop |
| P2 / P2b | `p2-physics` | `Phisiks::Physics` (bodies, colliders, materials, layers, queries, events, character controller); `Phisiks::Math` (dual quaternions, rotors, integrators, SDF/CSG) |
| P3 | `p3-scene` | `Phisiks::Scene` / `Entity` (EnTT), transform hierarchy, components, physics and presentation sync |
| P4 | `p4-renderer` | Forward PBR renderer, indirect drawing, shadows, analytic fog, persistent shader binary cache with crash journal, pixel-level runtime checks |
| P5 | `p5-assets-audio` | `Phisiks::Audio` (WASAPI + mixer), on-demand `Phisiks::Assets`, CC0 asset generator, dark studio look, multi-viewport and orthographic cameras, orbit camera, deterministic headless simulation with frame/data export |
| P6 | `p6-clean-engine` | Repository contains only Phisiks code, CC0 assets and listed third-party libraries; strict layering/name check; MIT licence |
| P7 / P7b | `p7-compute` | `Phisiks::Compute` (OpenGL 4.6 compute + SSBOs, `IComputeBackend` with OpenCL/SYCL hooks); persistent-mapped ring buffer; GPU frustum + Hi-Z occlusion culling writing the indirect commands; GPU spatial hash and particles; `Phisiks::Physics::Advanced` GPU XPBD (rope, cloth, soft bodies) with scene colliders; force fields (uniform, planetary, vortex, N-body) on rigid bodies, particles and XPBD; raymarched SDF/CSG and vector-field arrow visualisers; FieldLab scene |
| P9 | `p9-3d-vfx` | Retro deck studio skin (zero HUD overlap); glTF 2.0 / GLB importer, dual-quaternion skinning, spline camera paths; HDR bloom, SSAO, ribbon trails / beams / orbit paths, projected decals, live GLSL hot reload; heightmap terrain with splatmaps and height-field collision, instanced foliage; joints (hinge, ball-socket, prismatic, spring-damper) on both physics backends; 3D audio (panning, inverse-square, Doppler); navigation grid + A* + crowd steering; embedded Lua 5.4 scripting |
| P8 | `p8-studio` | Windows / Linux / macOS builds (platform layer, CMake presets, CI); native rigid-body backend; OpenGL 4.1 renderer tier + multi-threaded CPU compute backend; PhisiksLab ImGui studio (hierarchy, inspector, gizmos, classroom lessons, telemetry, timeline); JSON scenes; replay ring buffer with scrubbing; synthetic-vision export; FSR 1.0-style EASU + RCAS upscaling and quality presets; particle emitters; two-way particle / soft-body to rigid-body coupling |
| P10 | `p10-showcase` (moved in P11) | **Core engine feature freeze.** Six industry showcases in `examples/` on a shared framework (camera shots, view modes, RetroDeck telemetry, runtime harness): stunt car + destructible towers, screen-space fluid harbor with buoyancy, 6-DOF arm + rover, glass villa with time of day and CAD views, LiDAR / segmentation proving ground with vision export, black-hole disk with an integrator split-test; engine additions: 4-pass screen-space fluid, wireframe, disk / fill particle spawns, Doppler particle colouring, entity-id colour targets, non-blocking particle readback, GPU zone timers |
| P11 | `p10-showcase` (re-tagged) | Showcase overhaul: the six P10 demos replaced by four hero demos (6-DOF robot cell, refractive tiled pool, ray-traced Schwarzschild black hole, turbine CAD cutaway) with assetgen hero meshes; dark retro hardware-console UI for PhisiksLab and the showcases (menu bar, etched control groups, LCD fields, orientation triad); engine additions inside the freeze rules (general features, documented in ADR-25 / ADR-26): custom post-pass shaders with float data textures, see-through material opacity |
| P12 | `p10-showcase` (re-tagged) | Standalone release packaging: CMake `install()` + CPack (version 1.0.0, ZIP on Windows / macOS, TGZ on Linux) with the programs, PhysX DLLs, app-local MSVC runtime, `assets/`, engine `shaders/`, licences and `QUICKSTART.txt`; executable-relative asset lookup; `release.yml` builds Windows / Linux / macOS archives on `v*` tags and publishes them to the GitHub Release |

## Next

### Open follow-ups
From P7 (still open):
- **Culling:** two-phase Hi-Z (no one-frame pop on disocclusion); GPU timers per pass.
- **Visualiser:** marching-cubes extraction of SDFs; streamlines.
- **XPBD:** continuous SDF collision queries; GPU particle / soft-body state in the deterministic CSV export.
- **Backends:** OpenCL and SYCL implementations of `IComputeBackend`.

From P8:
- **Platforms:** the Linux and macOS builds exist (CMake presets, `ci.yml`) but have not been run yet; the first CI run will be the first real compile with GCC / Apple Clang. Linux PhysX via vcpkg is wired in CMake but untested. The same holds for `release.yml` (P12): its Linux / macOS / Windows-runner packaging has not run yet; only the local Windows archives were built and tested.
- **Native physics:** convex hulls use their bounding box for inertia; boxes and convex shapes resting on triangle meshes rely on an incremental manifold (less stable stacks than PhysX).
- **Replay:** covers transforms, rigid bodies, camera and input actions; particles and XPBD soft bodies are not snapshotted (they keep their current state when scrubbing); inputs are recorded, not re-injected for re-simulation.
- **Vision export:** depth / normals / ids cover meshes and soft bodies; particle sprites and the SDF visualiser appear only in the colour image.
- **Studio:** no native file dialogs (a path field); no undo; the gizmo works on the primary camera only (not quad view).
- **Quality presets:** values chosen for an i3 / 8 GB / UHD-class laptop; frame times per preset have not been benchmarked on such a machine yet.

From P9:
- **Animation:** CPU skinning (all tiers); a GPU skinning path for many characters; no morph targets, sparse accessors or KHR extensions; no blending between clips.
- **Navigation:** a walkable-cell grid (with ledge / radius erosion), not a polygon navmesh; agents move kinematically (no character-controller agents); foliage has no colliders, so agents walk through trees.
- **VFX:** decals / SSAO / bloom apply to window views (offscreen targets get no post FX); SSAO normals are reconstructed from depth; ribbons are additive only.
- **Scenes:** JSON scene files do not yet store the P9 components (Joint, SkinnedModel, Terrain, Script, ...).
- **Joints:** no breakable joints, no articulation (reduced-coordinate) solver; PhysX prismatic is a D6 joint.

From P10:
- **Frame budgets:** on this machine (Intel UHD Graphics, 1920x1080, Medium preset) all four P11 hero showcases average 70-93 FPS on the OpenGL 4.6 tier and 86-117 FPS on the 4.1 tier; none has been measured on the i3 / 8 GB target laptop. The 4.6 tier's GPU-driven path is still slower than the 4.1 tier's CPU culling on this iGPU, which needs profiling.
- **Screen-space fluid:** a granular particle model (spatial-hash separation), not SPH / PBF pressure; the surface is smooth from typical views but shows particles at grazing close-ups; window views only.
- **Showcases:** buoyancy uses a particle-column surface estimate; the robot arm's links are gravity-compensated and the gripper attaches the crate kinematically; the rover is a raycast vehicle (no articulated rocker-bogie joints); the segmentation view colours instances, classes come from the export's class table.

### After the freeze
- Linux / macOS CI bring-up; GPU skinning; light culling (clustered or tiled forward); SPH / PBF fluid; polygon navmesh; Vulkan or Metal evaluation.

## Principles
- **Public API in `engine/include/Phisiks`, everything else private.** A strict build check enforces it: apps and tests include only `Phisiks/...`, glm, EnTT and the standard library.
- **Every feature gets a measured verification.** Analytic unit tests where a closed form exists; pixel-level or entity-id checks for rendering; byte-identical replays for determinism.
- **All code in the repository is original and MIT-licensed.** Third-party code stays in `vendor/` under its own licence (`THIRD_PARTY_NOTICES.md`), and sample assets are generated and CC0.
