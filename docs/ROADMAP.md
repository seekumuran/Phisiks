# Phisiks Roadmap

Phisiks is a general-purpose 3D simulation, visualisation and game engine:
- C++20 on Windows, Linux and macOS; OpenGL 4.6 with an OpenGL 4.1 tier;
- EnTT scenes; NVIDIA PhysX 5 or the native rigid-body solver;
- the default look is a dark studio with a rainbow calibration-grid floor.

Every phase ends with a clean Release build, all unit and runtime checks green, a commit and a tag. Phase history and measurements are in `PHASE_LOG.md`.

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
| P8 | `p8-studio` | Windows / Linux / macOS builds (platform layer, CMake presets, CI); native rigid-body backend; OpenGL 4.1 renderer tier + multi-threaded CPU compute backend; PhisiksLab ImGui studio (hierarchy, inspector, gizmos, classroom lessons, telemetry, timeline); JSON scenes; replay ring buffer with scrubbing; synthetic-vision export; FSR 1.0-style EASU + RCAS upscaling and quality presets; particle emitters; two-way particle / soft-body to rigid-body coupling |

## Next

### Open follow-ups
From P7 (still open):
- **Culling:** two-phase Hi-Z (no one-frame pop on disocclusion); GPU timers per pass.
- **Visualiser:** marching-cubes extraction of SDFs; streamlines.
- **XPBD:** continuous SDF collision queries; GPU particle / soft-body state in the deterministic CSV export.
- **Backends:** OpenCL and SYCL implementations of `IComputeBackend`.

From P8:
- **Platforms:** the Linux and macOS builds exist (CMake presets, `ci.yml`) but have not been run yet; the first CI run will be the first real compile with GCC / Apple Clang. Linux PhysX via vcpkg is wired in CMake but untested.
- **Native physics:** convex hulls use their bounding box for inertia; boxes and convex shapes resting on triangle meshes rely on an incremental manifold (less stable stacks than PhysX); no joints (neither backend exposes joints yet).
- **Replay:** covers transforms, rigid bodies, camera and input actions; particles and XPBD soft bodies are not snapshotted (they keep their current state when scrubbing); inputs are recorded, not re-injected for re-simulation.
- **Vision export:** depth / normals / ids cover meshes and soft bodies; particle sprites and the SDF visualiser appear only in the colour image.
- **Studio:** no native file dialogs (a path field); no undo; the gizmo works on the primary camera only (not quad view).
- **Quality presets:** values chosen for an i3 / 8 GB / UHD-class laptop; frame times per preset have not been benchmarked on such a machine yet.

### P9 candidates
- Joints / articulations in `Phisiks::Physics` (both backends).
- Template projects in `examples/`: 3D game, headless simulation + data export, interactive visualiser.
- Vulkan or Metal backend evaluation for macOS beyond OpenGL 4.1.

## Principles
- **Public API in `engine/include/Phisiks`, everything else private.** A strict build check enforces it: apps and tests include only `Phisiks/...`, glm, EnTT and the standard library.
- **Every feature gets a measured verification.** Analytic unit tests where a closed form exists; pixel-level or entity-id checks for rendering; byte-identical replays for determinism.
- **All code in the repository is original and MIT-licensed.** Third-party code stays in `vendor/` under its own licence (`THIRD_PARTY_NOTICES.md`), and sample assets are generated and CC0.
