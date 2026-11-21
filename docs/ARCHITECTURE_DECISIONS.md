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
One shared vertex/index buffer, an instance SSBO, and one `DrawElementsIndirectCommand` per mesh type per view, with frustum culling on the CPU (GPU culling since P7, ADR-10). Every view writes HDR colour and an entity-id attachment. Screenshots read a renderer-owned presented image (scene + overlay), not the window back buffer, so they don't depend on swap-chain behaviour.

## ADR-7: Engine-owned program-binary cache + crash journal
The renderer stores every linked program with `glGetProgramBinary`. Each file is keyed by an FNV-1a hash of the preprocessed sources and the driver identity (vendor, renderer, version), carries a header and checksum, and is written with temp-file + rename. Programs compile only during startup, serially, behind a journal file naming the program being compiled. A leftover journal triggers a safe-mode rebuild of that program on the next run. Invalid, stale or driver-rejected binaries fall back to compiling.

## ADR-8: Original code and generated assets only
All code in the repository is original and MIT-licensed (`LICENSE`). Third-party libraries are used unmodified from `vendor/` under their own licences (`THIRD_PARTY_NOTICES.md`). Sample assets are generated procedurally by `tools/assetgen` and released under CC0, so the repository contains no third-party art, audio or models.

## ADR-9: GPU compute on OpenGL 4.6 behind `IComputeBackend`
Compute uses the existing GL 4.6 context and shader cache rather than a second API: it shares buffers with the renderer without interop and runs on any GL 4.6 iGPU. Engine systems (culling, particles, XPBD) dispatch renderer-owned programs directly. Tools and user code go through `Phisiks::Compute::IComputeBackend`, whose OpenCL and SYCL enumerations exist as hooks so another backend can be added without changing callers. Every GPU kernel has a CPU reference that tests compare against.

## ADR-10: GPU-driven culling writes the indirect commands through a remap list
Instances are uploaded once per frame (persistent-mapped ring); each view has one indirect command per mesh and a remap slice that the vertex shader reads via `gl_BaseInstance + gl_InstanceID`. The cull shader appends visible instances and increments `instanceCount` with atomics, so the GPU path and the CPU path share all drawing code and can be compared pixel-for-pixel. Hi-Z uses the previous frame's depth (single phase, see Known Issues).

## ADR-11: Small-step XPBD with graph colouring; fused single-workgroup solver
XPBD runs one iteration per sub-step with Lagrange multipliers reset each sub-step, which is stable and makes `alpha*m*g` an exact analytic test. Constraints are graph-coloured so each colour is solved in parallel without atomics. Dispatch overhead, not arithmetic, dominated on the iGPU (a 64-node soft box needs 51 colours x 16 sub-steps), so bodies that fit run the whole fixed step in one workgroup with `barrier()` between colours; large bodies use one dispatch per colour. Both produce bit-identical results.

## ADR-12: Two physics backends behind one API
PhysX 5 does not support macOS, and the repository only vendors Windows binaries. `Phisiks::Physics` therefore has two interchangeable implementations chosen at configure time (`PHISIKS_PHYSICS_BACKEND`): PhysX (`engine/src/physics/physx/`) and a native solver (`engine/src/physics/native/`: GJK/EPA, box SAT clipping, speculative contacts, sequential impulses with warm starting, split-impulse position correction, conservative-advancement sweeps, a three-pass capsule character controller). The same 112 physics/scene unit tests and the runtime autotest run against both.

## ADR-13: An OpenGL 4.1 feature tier instead of a second renderer
macOS stops at OpenGL 4.1. Rather than a separate renderer, the renderer runs in one of two tiers chosen at startup. `render/gl/GLObjects` hides direct state access behind tier-aware helpers. Shaders read storage through `Load*()` accessors: SSBOs on 4.6, texture buffers on 4.1. A preprocessor rewrite turns `#version 460` + `layout(binding = N)` into 4.10 source and records the bindings, which are applied after linking. On 4.1 the compute-based systems (culling, Hi-Z, particles, XPBD, field sampling) are replaced by the CPU culling path and the CPU compute backend (worker thread pool). `--gl41` forces the tier on any GPU, so it is tested on Windows.

## ADR-14: Clean-room FSR 1.0-style upscaling with a CPU reference
The window scene renders at `renderScale` × the window size. It is then upscaled by an edge-adaptive EASU pass and sharpened by an RCAS pass, both single fragment passes that work on both tiers. The shaders were written from the published description of the algorithm, not from AMD's source. The same maths exists in C++ (`Phisiks/Render/Upscaler.h`) so unit tests can check its properties headlessly and the runtime check can compare GPU output with it.

## ADR-15: Studio UI in the app, ImGui integration in the engine
`Phisiks/UI/ImGuiLayer.h` owns Dear ImGui's lifetime and its GLFW/OpenGL3 backends (GLSL 410, both tiers). The Application draws the UI after the frame is presented, so captures, exports and pixel tests never contain UI. The studio itself (`apps/lab/src/Studio.cpp`) uses only the public engine API plus `<imgui.h>` / `<ImGuizmo.h>`, which the layering check allows for apps.

## ADR-16: Joints solved inside each backend, not on top of it
PhysX joints map onto PxRevoluteJoint, PxSphericalJoint, PxD6Joint (prismatic, so the slide axis can have a velocity drive as its motor) and PxDistanceJoint with a spring. The native solver adds joint rows to its sequential-impulse loop: velocity rows (point, angular, limit and motor rows, warm started) are solved with the contacts and never add energy, positional drift is removed with the same split-impulse pseudo velocities as contacts, and the spring-damper is an implicit soft constraint (gamma / bias from the stiffness and damping), so its period matches sqrt(k/m) exactly. Destroying a body destroys its joints in both backends.

## ADR-17: Post-processing on the resolved window target, decals deferred-style
The forward renderer keeps MSAA and its entity-id pass. Post effects run after the MSAA resolve, on the single-sample HDR image of the window: depth is resolved once, projected decals are drawn as box volumes that reconstruct the scene position from that depth, SSAO (hemisphere kernel, normals from depth) is blurred and multiplied in, and bloom runs a 13-tap downsample / tent upsample mip chain that the tonemapper adds. Everything is a fragment pass, so the 4.1 tier gets the same effects. Ribbons are drawn into the MSAA target after the sky (additive, depth-tested).

## ADR-18: CPU skinning with dual quaternions
Skinned glTF meshes are deformed on the CPU and uploaded with `Render::UpdateMeshVertices`, so they go through the same instanced, GPU-culled path as every other mesh on both renderer tiers. Dual-quaternion blending keeps volume under twist (the unit test measures the twisted radius: DQ 0.250 m vs 0.204 m with linear blending, rest 0.250 m); linear blend skinning stays available for comparison.

## ADR-19: Navigation from physics queries
The navigation grid is built from raycasts and overlap boxes against the static colliders of whichever physics backend runs, so it needs no separate geometry export and works for meshes, boxes and height fields alike. Ledges (neighbour steps above the climb height) count as obstacles before the agent-radius erosion, so wall tops and narrow ledges disappear and agents keep their radius away from walls.

## ADR-20: Lua as a vendored, sandboxed VM per scene
Scripting uses the official Lua 5.4 sources (MIT), fetched like the other dependencies, rather than a custom interpreter. One VM exists per scene instance; each Script runs its chunk with its own `_ENV` table that falls back to the shared API, so scripts keep private state and can be re-run (hot reload) without losing it. Only the base / table / string / math / utf8 / coroutine libraries are opened (no io, os, package, debug), text chunks only, and an instruction-count hook stops any callback after 200 ms.

## ADR-21: The studio skin is drawn, not loaded
The retro deck look is produced by an ImGui style plus widgets that draw bevels, gradients, LCD digits and LEDs with draw lists (`Phisiks/UI/RetroDeck.h`). No skin bitmaps or third-party assets are used, the widgets take plain types (apps need no ImGui types for them), and per-frame counters let the runtime test verify the skin on the presented back buffer.

