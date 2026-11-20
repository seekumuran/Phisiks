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

## Next

### P7: `Phisiks::Compute` (OpenGL 4.6 compute + SSBOs)
**API:**
- `ComputeProgram`, cached like the graphics programs;
- `GpuBuffer` (SSBO / indirect / atomic counter);
- `PersistentRingBuffer` (`glBufferStorage` persistent + coherent, triple-buffered, fence-guarded);
- `Dispatch`/`Barrier` helpers;
- GPU timers per pass.

**Techniques, each implemented and measured against the CPU path:**
1. GPU frustum culling into the indirect command buffer with an atomic count (`glMultiDrawElementsIndirectCount`), verified to produce the same visible set as the CPU culling.
2. Hi-Z occlusion culling: a depth pyramid from the previous frame, two-phase.
3. GPU particles: emit, simulate and compact in compute, with a spatial-hash grid (counting sort + prefix scan) for neighbour queries; particle conservation is checked.
4. Persistent-mapped uploads for per-frame object, light and draw data.

**Backends:** an `IComputeBackend` interface with the GL implementation. OpenCL/SYCL hooks are declared, not implemented.

**Analytical SDF and vector-field visualiser:**
- a raymarched pass for `Phisiks::Math::SDF` primitives and smooth CSG;
- GPU mesh extraction (marching cubes);
- vector-field glyphs and streamlines for force fields.

### P7b: `Phisiks::Physics::Advanced`
**GPU XPBD on GL compute:**
- distance, bending and volume constraints;
- graph-coloured parallel solve with substeps;
- for rope, cloth and soft bodies.

**Programmable force fields:**
- uniform;
- spherical planet gravity;
- vortex;
- N-body, with a GPU tiled O(N²) kernel.

**Other:**
- Continuous SDF collision queries (sphere tracing, conservative advancement).
- Coupling with PhysX rigid bodies is one-way at first: PhysX bodies act as kinematic colliders for XPBD.
- Simulations are recorded through the deterministic export path.

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
