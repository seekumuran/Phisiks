# Architecture Decisions

## ADR-1: One static engine library, thin executables
`PhisiksCore` is a static library with a public API (`engine/include/Phisiks`) and private implementation (`engine/src`). Apps (`apps/`), tests and tools link it. Third-party include paths are private to `PhisiksCore`, except glm and EnTT, which appear in public headers. `tools/check_layering.cmake` runs before every build and fails it on any layering or naming violation.

## ADR-2: Scenes are EnTT registries with a thin Entity handle
`Phisiks::Scene` owns an `entt::registry`; `Phisiks::Entity` is a (scene, handle) pair with versioned ids. Every entity has `Name` and a local `Transform`; world transforms are derived on demand through the parent chain. Runtime resources (physics bodies, render objects, lights, voices) live in engine-private components, and EnTT destroy hooks release them, so removing a component or destroying an entity or scene can't leak.

## ADR-3: Presentation through engine-private backends
`Scene::SyncPresentation` talks to `IRenderBackend` / `IAudioBackend`, installed by the application. Headless runs and unit tests install none (or recording fakes), so scene logic is testable without a GPU or audio device.

## ADR-4: Gameplay runs at the physics rate
Player movement and `Scene::FixedUpdate` run inside the fixed-step loop, before each physics step. Edge-triggered input (jump) is latched per frame and consumed by the next fixed step. Camera, presentation and UI run per frame. In deterministic mode every frame advances exactly N fixed steps, independent of the wall clock, and the startup frame's time is discarded (game frame N = step N).

## ADR-5: Verification through the real runtime, driven by input actions
The runtime harness (`tests/runtime/AutoTest`) runs inside PhisiksLab with the real renderer, PhysX and audio. It drives gameplay only through injected input actions, records a measured value for every check, and verifies rendering through the renderer's entity-id buffer and final image (pixel checks, A/B frames). A feature isn't reported complete without a passing measured check.

## ADR-6: Forward renderer with indirect drawing and an id buffer
One shared vertex/index buffer, an instance SSBO, and one `DrawElementsIndirectCommand` per mesh type per view, with frustum culling on the CPU (moves to compute in P7). Every view writes HDR colour and an entity-id attachment. Screenshots read a renderer-owned presented image (scene + overlay), not the window back buffer, so they don't depend on swap-chain behaviour.

## ADR-7: Engine-owned program-binary cache + crash journal
The renderer stores every linked program with `glGetProgramBinary`. Each file is keyed by an FNV-1a hash of the preprocessed sources and the driver identity (vendor, renderer, version), carries a header and checksum, and is written with temp-file + rename. Programs compile only during startup, serially, behind a journal file naming the program being compiled. A leftover journal triggers a safe-mode rebuild of that program on the next run. Invalid, stale or driver-rejected binaries fall back to compiling.

## ADR-8: Original code and generated assets only
All code in the repository is original and MIT-licensed (`LICENSE`). Third-party libraries are used unmodified from `vendor/` under their own licences (`THIRD_PARTY_NOTICES.md`). Sample assets are generated procedurally by `tools/assetgen` and released under CC0, so the repository contains no third-party art, audio or models.
