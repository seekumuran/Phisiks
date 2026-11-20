# Phisiks Roadmap

Phisiks is a general-purpose 3D simulation, visualisation and game engine:
- C++20 and OpenGL 4.6;
- EnTT scenes and NVIDIA PhysX 5;
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

## Next

### P7 follow-ups (not yet implemented)
These parts of the original P7/P7b plan were not delivered in `p7-compute`:
- **Culling:** two-phase Hi-Z (re-test last frame's occluded objects against this frame's depth, so nothing pops for one frame on disocclusion); GPU timers per pass.
- **Particles:** emission and compaction (systems currently have a fixed particle count).
- **Visualiser:** marching-cubes mesh extraction of SDFs; streamlines (only arrow glyphs exist).
- **XPBD:** continuous SDF collision queries (sphere tracing / conservative advancement); two-way coupling with PhysX (PhysX bodies are one-way colliders today); recording GPU soft-body / particle state through the deterministic export path.
- **Backends:** OpenCL and SYCL implementations of `IComputeBackend` (the interface and enumeration hooks exist).

### P8: PhisiksLab and templates
**App and UI:**
- Dear ImGui (MIT, vendored at a pinned release) for live controls.
- Viewports and offscreen camera targets as ImGui panels, plus gizmos.
- Record/replay UI for deterministic runs.

**Lessons**, with a switcher and self-contained demos:
- transforms and math;
- rigid bodies and friction;
- raycasting and triggers;
- PBR lighting;
- GPU culling and particles;
- XPBD, force fields and integrator comparison;
- rotors and dual quaternions.

**Live tuning and visualisers:**
- live parameters: gravity, friction, solver substeps, light intensity, GPU culling on/off;
- visualisers: bounds, colliders, spatial grid, force vectors;
- CPU vs GPU metrics.

**Domain-neutral `README.md`, `docs/` and `examples/`** with three starters on `PhisiksCore`:
- (a) 3D game;
- (b) physics/math simulation (headless deterministic + data export);
- (c) interactive 3D visualiser (PhisiksLab module).

## Principles
- **Public API in `engine/include/Phisiks`, everything else private.** A strict build check enforces it: apps and tests include only `Phisiks/...`, glm, EnTT and the standard library.
- **Every feature gets a measured verification.** Analytic unit tests where a closed form exists; pixel-level or entity-id checks for rendering; byte-identical replays for determinism.
- **All code in the repository is original and MIT-licensed.** Third-party code stays in `vendor/` under its own licence (`THIRD_PARTY_NOTICES.md`), and sample assets are generated and CC0.
