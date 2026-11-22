# Phase log

Every phase ends with a clean Release build, all automated checks green, a commit and a tag.

## P0 — Skeleton (tag `p0-skeleton`)
- Layout `engine/` (public `include/Phisiks`, private `src`), `apps/lab`, `tests/runtime`.
- CMake: project Phisiks, `PhisiksCore` static library, `PhisiksLab`, the inherited game kept as a temporary regression target, `check_layering`.
- Engine no longer depends on game or tests (`ITestHarness`). Namespace `Phisiks::`.
- Verified: 0 build errors; runtime autotest 54/54.

## P1 — Core and platform (tag `p1-core`)
Clean-room code (see docs/CLEAN_ROOM.md):
- `Phisiks::Platform` window + OpenGL 4.6 context (GLFW): fullscreen (borderless), cursor modes incl. captured raw motion, focus, events. Fixes the inherited window ignoring the requested title.
- `Phisiks::InputState`: event-driven device state. Taps shorter than a frame aren't lost; focus loss releases held keys.
- `Phisiks::Time` / `FixedStepClock`: testable fixed-step accumulator (clamp 0.1 s, max 5 steps/frame).
- `Phisiks::EventBus`: typed publish/subscribe, queued dispatch, RAII subscriptions. Engine events: window resize, focus, close request, scene loaded.
- `Phisiks::Profiler` (CPU zones, rolling averages) + `Phisiks::GpuTimer` (GL timestamp ring, reads only available results: **never waits on the GPU**).
- `Phisiks::IGame` replaces the ad-hoc game definition.
- Inherited window/input/time are now shims over the new code.

Engine→game links cut (all 8):
- `BackEnd.cpp`: `Config::Init` moved to the game entry points.
- `Physics.cpp`, `PhysicsUpdate.cpp`, `Ragdoll.cpp`: read engine-owned physics settings; the original game's config pushes into them.
- `PhysicsDebug.cpp`: local colours instead of game constants.
- `GL_back_end.h/.cpp`: unused game includes removed.
- `GL_resource_manager.cpp`: shader hot-reload reports through a registered callback instead of calling the game's debug overlay.

`check_layering` now fails the build if `src/Hell` includes game code again.

Measured:
- The inherited renderer's GPU timer did a blocking `GL_QUERY_RESULT` readback every frame. It's now disabled for the engine (opt-out flag; the original game keeps it). GpuTimer: 368 samples, 0 dropped in a full autotest run.
- **No FPS change (~15 FPS):** CPU render time (61 ms) still tracks GPU time (64 ms), so the inherited renderer has other CPU↔GPU sync points. They disappear with that renderer in P4.

Tests:
- `PhisiksUnitTests` (ctest): 16 headless cases (clock, input, events, profiler) + the layering test.
- Runtime autotest 54/54.

## P2 + P2b — Physics and advanced math (tag `p2-physics`)
**Phisiks::Physics** (clean-room, PhysX 5 hidden behind `Phisiks/Physics/Physics.h`):
- Static / dynamic / kinematic bodies; box, sphere, Y-up capsule, convex hull (cooked), triangle mesh (static/kinematic only).
- Physics materials with **realistic defaults: static 0.6, dynamic 0.5, restitution 0** (replaces the inherited 7.0/0.85 quirk for engine bodies).
- 32 layers + symmetric collision matrix. Raycast / RaycastAll (sorted) / sphere-, box-, capsule-casts / sphere and box overlaps, with layer masks, trigger inclusion and ignore-body/character options.
- Collision enter/exit events (contact point, normal, impact speed) and trigger enter/exit events (incl. character controllers, via kinematic pair filtering).
- Character controller (capsule, step offset, slope limit, pushes dynamic bodies).
- Generational body/character ids (stale ids are detected); per-step sub-stepping; stats.
- Owns the process-wide PhysX foundation; inherited physics borrows it (PhysX allows one foundation per process).
- Gameplay framework (entities, player, camera, interaction, overlay) and the runtime loop moved onto Phisiks::Physics. Hits carry the entity id as user data (the inherited per-object `PhysicsUserData` allocations are gone).

Design note: PhysX detects first contact speculatively, so the solver impulse on an Enter event is often 0. Events therefore report `impactSpeed` (relative normal velocity at first contact), which the tests check against sqrt(2gh).

**Phisiks::Math** (header-only, P2b):
- `DualQuat`: rigid transforms, composition, inverse, screw decomposition (Chasles), fractional power, ScLerp, dual-quaternion linear blending (DLB) for skinning.
- `Rotor3` / `Bivector3`: geometric-algebra rotors (plane+angle, from-vectors, geometric product, sandwich rotation derived by hand, slerp, quaternion isomorphism).
- `Integrate`: explicit Euler, semi-implicit (symplectic) Euler, velocity Verlet, Euler and RK4 for y' = f(t,y); reference systems (harmonic oscillator, pendulum, Kepler orbit) with energy functions.
- `SDF`: exact sphere/box/round box/capsule/torus/cylinder/plane, CSG and polynomial smooth union/intersection/subtraction, gradient/normal/closest point, sphere-tracing ray march.

Friction-sensitive runtime check re-derived (documented in tests/runtime/AutoTest.cpp):
- Old: "impulse pushes the body > 0.05 m" (measured 0.29 m under 7.0/0.85 friction).
- New: the slide distance must lie between the analytic bounds, slide-only v0^2/(2 mu_d g) = 1.13 m and with-hop (0.2 lift: airborne 2vy/g) = 1.59 m, +-10%. Measured: **1.36 m**.
- All other runtime checks were re-run unchanged and pass with the new materials (stack, resting contact, spawner landing, player/wall).

Tests:
- `PhisiksUnitTests`: **54 headless cases** (16 core + 20 physics + 18 math), all analytic expectations: free fall, resting contact, friction holds at 20 deg / slides at 40 deg (a = g(sin - mu_d cos)), stopping distance v^2/(2 mu_d g), restitution e^2 h, J = m dv, kinematic pushing, layers, triggers incl. character, sweeps, overlaps, Y-up capsule, convex/triangle meshes, CCT wall/step/block, stale ids; DQ vs matrices, screw round trip, ScLerp on the arc, DLB rigidity vs LBS collapse, rotor/quaternion agreement, integrator energy behaviour, RK4 order 16x / Euler 2x, Kepler, pendulum period, exact SDF distances, unit gradients, CSG, ray-march distances.
- Runtime autotest 54/54; original game regression OK (shares the PhysX core).

## P3 — Scene & entity/component system (tag `p3-scene`)
**Phisiks::Scene / Phisiks::Entity** (`Phisiks/Scene/Scene.h`, `Components.h`; EnTT 3.15, header-only in `vendor/entt`):
- Every entity has `Name` + `Transform` (local). Parent/child hierarchy: `SetParent(parent, keepWorldTransform)` (rejects cycles and cross-scene parents), `GetChildren`, `IsDescendantOf`; world matrix/position/rotation/scale derived through the chain; `SetWorldPose` converts to local (and teleports the body).
- Components: `Transform`, `MeshRenderer` (unit Cube/Sphere/Plane scaled by Transform), `Light`, `Camera`, `Collider`, `Rigidbody`, `AudioSource`; gameplay `Health`, `Interactable` (`Phisiks/Framework/GameplayComponents.h`).
- Lifecycle: `Destroy()` is deferred to the end of the frame and recursive; `DestroyImmediate`, `Clear`. Versioned ids (`FromId` rejects stale ids; `FromBody` maps physics hits to entities). Physics bodies, render objects, lights and voices are released by EnTT destroy hooks, so removing a component or destroying an entity/scene can't leak them.
- Systems: `SyncPhysicsBodies` (Collider → static body, + Rigidbody → dynamic/kinematic; shapes scaled by world scale; rebuild on component change or `MarkPhysicsDirty`; kinematic targets), `PullPhysicsPoses` (dynamic bodies → local transforms), `SyncPresentation` (renderer/light/camera/audio through engine-private backends; only changed objects are updated).
- Frame order: sync bodies → fixed steps (scene FixedUpdate, sync, step) → pull poses → camera → harness → flush destroyed → sync presentation.

Migration:
- Removed `EntityRegistry`, framework `Entity.h`/`Scene.h`. Gameplay services, player, camera (drives a `MainCamera` entity), scene manager, debug overlay and the runtime loop use the ECS.
- Sandbox scenes and `SceneBuilder` are pure ECS (ground = Plane renderer + child box collider; lights are entities). `games/` and `tests/` have **no** inherited includes any more (layering report: clean).
- The only scene → inherited code path is a temporary presentation bridge in `engine/src/runtime/` (temporary backends; replaced by the P4 renderer / P5 audio). Screenshot capture moved into the engine (`Phisiks::CaptureScreenshot`).

Bug found during migration: the inherited `MeshNodes::Update()` bakes material/visibility into render items, so they must be applied **before** the transform update. The autotest passed with the wrong order (it checks component state, not pixels); the screenshots exposed it (fallback checker materials, invisible player body in third person). Fixed and verified against the P2 screenshots.

Runtime check changes (all derived from the new scene content, not tuned to pass):
- Tower entity count is now 6 + 16 + 4 (ground, ground collider, 2 lights, main camera, player body are real entities now).
- "no rigid statics leaked": engine-owned statics are now 0 (the ground collider is an entity).
- New check: the spawner's `AudioSource` plays when interacted with (55 checks total).

Tests:
- `PhisiksUnitTests`: **73 headless cases** (+19 scene): TRS composition through 3 levels, keep-world reparenting with scaled/rotated parents, cycle rejection, SetWorldPose under transformed parents, deferred/recursive/idempotent destroy, child detachment, Clear, component add/replace/remove, static/dynamic/kinematic body creation/rebuild/removal, collider world scaling (bounds), free fall pulled into parent space (10 − g t²/2), impulse dv = J/m via entity, presentation lifecycle/dirty updates/camera view/teardown and AudioSource play/stop/release against recording backends, headless no-op.
- Runtime autotest **55/55**; original game 40 s regression: no crash, same log error set as before P3.

## P4 — Forward PBR renderer and shader cache (tag `p4-renderer`)
**Phisiks::Render** (`Phisiks/Render/Renderer.h`, `engine/src/render/**`, clean-room, OpenGL 4.6 core only):
- GL wrappers (`gl/GLObjects.*`): RAII DSA buffers, textures (2D, MSAA, arrays, cube arrays, depth-compare), framebuffers, program build with logged errors.
- Indirect drawing: every mesh lives in one vertex and one index buffer. Per-object data goes in an instance SSBO, indexed with `gl_BaseInstance`. Objects are frustum-culled (bounding spheres) and sorted by mesh, so there is one `DrawElementsIndirectCommand` per mesh type and one `glMultiDrawElementsIndirect` per pass. Sandbox: 3 commands for 12-19 instances.
- Forward PBR: Cook-Torrance (GGX, height-correlated Smith, Schlick), hemisphere ambient plus sky-gradient specular, directional, point (1/d^2 with a smooth range window) and spot (cone) lights.
- Shadows:
  - directional: bounding-sphere fit, texel-snapped, 3x3 PCF
  - spot: up to 4, in a 2D array
  - point: up to 4, cube array storing linear distance, bilinear compare
- Analytic distance fog plus exponential height fog: closed-form optical depth, applied in the forward pass and blended into the sky at the horizon.
- Analytic sky (gradient, ground, sun disc and glow).
- MSAA 4x HDR target plus an entity-id (R32UI) attachment, resolved and then filmic tonemapped to an LDR target. The id buffer is the basis of the pixel tests.
- Debug lines (depth-tested) and overlay text with an original 5x7 bitmap font.
- Parametric/procedural materials (Checker, Grid, Noise, Wood, Tiles), mapped tri-planar in object space scaled to metres. The sandbox no longer uses any inherited texture or model.
- 15 GLSL files written new: 10 stages plus 5 includes (common, brdf, fog, shadows, patterns). They are embedded at build time (`tools/embed_shaders.cmake`), so there are no runtime shader paths. The renderer builds 7 programs from them.

**Shader cache** (`ShaderCache.*`, `ShaderCacheFormat.*`): `glGetProgramBinary`/`glProgramBinary`.
- Each file is keyed by FNV-1a over the preprocessed sources and the driver identity, and has a 40-byte header with a checksum.
- Files are written atomically (temp file + rename) to `%LOCALAPPDATA%/Phisiks/ShaderCache` (override with `PHISIKS_SHADER_CACHE_DIR`).
- Programs compile only at startup, serially, behind a crash journal. A run that died mid-compile is detected and that program is rebuilt in safe mode.
- Invalid, stale or driver-rejected binaries are recompiled. Editing a shader recompiles only the programs that use it (verified: 6 loaded from cache, 1 compiled).

**Startup and frame:**
- The Application now creates the window, renderer, physics and audio itself. `EngineBridge` and the temporary presentation bridge are deleted, and the inherited engine is never initialised by PhisiksLab.
- The only inherited code PhisiksLab still calls is the FMOD wrapper (P5).
- `PhisiksLab.exe` is emitted under its own name; the workaround that emitted the sandbox under the original game's executable name, and its CMake option, are removed.
- Layering: `games/`, `tests/` and `engine/include` are clean; engine to inherited game code: 0.

Measured on Intel UHD (driver 32.0.101.7076), 1440x900, MSAA 4x:

| | Before (inherited renderer, P3) | After (P4) |
|---|---|---|
| GPU frame | ~64 ms | 4.2-4.7 ms |
| CPU render | ~56-61 ms | ~1 ms |
| FPS | ~15 | 165-355 |
| Shader startup, cold | crash-prone (INTEL-SHADER-CRASH) | 610 ms first ever; 108-138 ms over 5 later cold runs, no crash |
| Shader startup, warm | — | 5-7 ms (7/7 from cache) |

Verified recovery paths:
- Planted journal for `forward`: detected, rebuilt in safe mode, 71/71.
- Corrupted cache file: checksum mismatch detected, recompiled, 71/71.

Runtime checks: 55 → **71** (all pass). 16 new:
- Pixel checks on the entity-id buffer and the final image:
  - frame not blank
  - ground covers the lower view, and its grid pattern is visible
  - wall drawn with its neutral tile material
  - every id belongs to a live entity
  - first person: 0 player-body pixels
  - third person: body covers more than 1% of the frame
  - the crosshair pixel is TargetBox, and its pixels are red
  - the destroyed TargetBox draws 0 pixels
- A/B frames of a static view:
  - with shadows on, part of the ground gets more than 25% darker, and no ground pixel gets brighter
  - fog changes far ground about 4x more than near ground
- Shader cache stats.
- Indirect command count.
- No `GL_DEBUG_TYPE_ERROR` messages for the whole run.

Expectation changes:
- Tower entities are 7 + 20 (the sun is a new light entity).
- "Resume continues simulation" now waits 6 frames **and** 0.1 s of real time from the first running frame. At 160+ FPS, 6 frames can be shorter than one 1/60 s fixed step, and the pause screenshot's PNG write (~180 ms) happens while still paused. This was a test-timing assumption, not an engine defect; resume was measured to advance the simulation (20.967 → 21.067 s).

Tests:
- `PhisiksUnitTests`: **89 headless cases** (+16), covering:
  - cache format (round trip; corrupt, truncated, version and key mismatches; key sensitivity; atomic writes)
  - preprocessor (include-once, recursion, unknown includes, defines)
  - every embedded program preprocesses
  - primitive winding and normals
  - GGX normalisation and white furnace, against an independent integration (0.981/0.683/0.379)
  - Fresnel, falloff and spot cone
  - fog closed form against numeric integration
  - directional shadow fit and texel snapping, spot and point views
  - frustum culling, font atlas
  - scene backend entity ids and light direction
- Runtime autotest **71/71**, cold and warm cache.
- ctest 2/2.
- Original game 40 s regression: no crash, same pre-existing log error set.

## Rename to Phisiks (commit `b6c96bb`)
NovaEngine became **Phisiks**:
- namespace `Phisiks::` and headers under `engine/include/Phisiks/`;
- CMake targets `PhisiksCore`, `PhisiksLab` (the app, moved from `games/sandbox` to `apps/lab`, namespace `Lab`) and `PhisiksUnitTests`;
- `PHISIKS_*` CMake and environment variables;
- `[PHISIKS ...]` log prefix;
- shader cache in `%LOCALAPPDATA%/Phisiks` (`*.phsc`, magic "PHSC").

Verified on its own commit: 89/89 unit tests, 71/71 runtime checks. The roadmap expansion is in `ENGINE_REFACTOR_PLAN.md` §9.

## P5 — Assets, audio, dark studio, multi-viewport, deterministic simulation (tag `p5-assets-audio`)
**The engine no longer includes or calls any inherited code** (`grep` of `engine/` for includes of the original code: 0). The inherited sources are still compiled into `PhisiksCore` only for the original game executable until P6.

**Phisiks::Audio** (`engine/src/audio`, clean-room, no FMOD):
- WASAPI shared-mode event-driven output (float32 stereo, OS converts the format);
- thread-safe software mixer: any number of voices, volume, pitch, looping, linear resampling, master volume, clipping;
- WAV decoder: PCM 8/16/24/32, float 32/64, WAVE_FORMAT_EXTENSIBLE, odd chunks;
- null device (headless or deterministic runs) that advances voices in simulated time;
- clips load on first `Play` and are cached.

**Phisiks::Assets** (`engine/src/assets`):
- search paths (`assets/`, next to the executable, added paths);
- background image-decode worker (`RequestImage`/`PollImage`), bilinear resize, PNG writing, OBJ parser (quads, relative indices, generated smooth normals).
- The renderer streams material albedo textures into a growable sRGB texture array (mipmapped, 8x anisotropic) and loads `.obj` meshes the first time a `MeshRenderer` references them.
- `tools/assetgen` procedurally generates every sample asset (brick and metal-plate textures, chime and impact sounds, torus mesh), released CC0 in `assets/`. PhisiksLab uses no inherited asset at all.

**Dark studio look** (now the engine default `RenderSettings`):
- charcoal backdrop and fog, studio hemisphere ambient (up/down colours), cool rim light on vertical surfaces;
- new `Pattern::CalibrationGrid`: dark 1 m tiles with rainbow column colours, a coloured inner frame, glowing labels A0–J9 drawn from the font atlas, and light border lines.
- PhisiksLab adds the bricks wall, metal-plate spawner, neon OBJ torus and copper sphere.

**Multi-viewport and orthographic cameras:**
- `Camera` now has: perspective/orthographic projection, viewport rect, order, offscreen `target` + `targetSize`, and `enabled`.
- Rendering: every enabled camera is rendered with its own culling and directional shadow fit; spot/point shadows are shared.
- Targets: views draw into scissored regions of the window target set, or into per-name offscreen sets (`Render::GetTargetTexture` / `CaptureTarget`).
- PhisiksLab: F3 toggles a quad view (perspective + top/front/side ortho).
- New CAD-style `CameraMode::Orbit` (F4): RMB orbit, MMB pan, wheel zoom; new input actions `Pan`, `ToggleViewLayout`, `ToggleOrbitCamera` and wheel input with injection for tests.

**Deterministic stepped simulation and export:**
- `Time::SetDeterministic` (exactly N fixed steps per frame);
- `AppOptions` `deterministic`, `headless` (no window, renderer or audio device), `hiddenWindow`, and export of PNG frames and CSV of dynamic-body state (`Recorder`, fixed-precision text);
- PhisiksLab flags `--deterministic --steps-per-frame --headless --hidden --export-dir --export-frames --export-data --export-every`.
- The startup frame's time is discarded, so game frame N is fixed step N.

Bugs found and fixed during P5:
- **Multi-view shadows:** each view's directional shadow pass ran with the previous view's sky vertex array bound, so the indirect draw had no element buffer (`GL_INVALID_OPERATION`, 12 errors). The "no GL errors" check caught it; the shadow pass now binds the mesh VAO.
- **Stale screenshots:** reading the window back buffer (`glReadPixels(GL_BACK)`) returned stale content on this driver in the frame where the layout changed. The renderer now composes the presented image (scene + overlay) in its own texture, and screenshots read that.
- **Upside-down PNGs:** the old screenshot code also set stb's global `flip_vertically_on_write`, which would have flipped every PNG written later.

Measured (Intel UHD, 1440x900, MSAA 4x):
- shader startup 116 ms cold / 8 ms warm;
- headless deterministic simulation about 1,650 frames/s (600 PhysicsTower frames in 0.36 s);
- quad view 4 cameras + 11 shadow views at ~5 ms GPU.

Runtime checks: 71 → **83**, all passing. New and changed:
- Brick texture visible (red-brown and textured).
- Textures streamed in on demand (2 resident, 0 pending, 0 failures).
- OBJ mesh loaded on demand and drawn.
- Audio mixer running with the generated clip.
- Quad view:
  - renders 4 cameras;
  - every viewport shows scene objects;
  - **orthographic scale measured in pixels**: top view wall width 225 px vs 225.0 expected; front view wall height 113 px vs 112.5 expected;
  - single view restored.
- Orbit camera:
  - exact pose;
  - the wall is at the centre pixel;
  - wheel zoom distance x0.88²;
  - pan target offset.
- The indirect-draw check now compares against the number of registered meshes (the torus adds a fourth mesh type).

Tests:
- `PhisiksUnitTests`: **100 headless cases** (+11): WAV round trips and hand-built 8/24-bit/extensible files, malformed rejection, mixer resampling and pitch measured by zero crossings, volumes, clipping, voice lifetime and looping, the null device playing a file clip in simulated time, asset search paths, async decode, resize, OBJ parsing and errors, multi-camera sync order and settings.
- ctest **3/3**, including `DeterministicReplay`: two headless 600-frame runs produce byte-identical CSV (11,980 rows, 1.46 MB) with bodies actually moving, and a hidden-window run exports PNG frames 10/20/30.
- Runtime autotest **83/83**, cold and warm shader cache, WASAPI device open.
- Original game 40 s regression: no crash, same pre-existing log error set.

## P6 — Clean engine and MIT licence (tag `p6-clean-engine`)
- **Safety tag:** `pre-strip` was set on the clean P5 commit before anything was removed (deleted later, see the cleanup note at the end of this entry).
- **Removed from git** (recoverable from `pre-strip`):
  - all inherited sources (`src/`, 863 files), including the original game executable target;
  - the tracked parts of the old data tree;
  - `DUMP/` and `baseline/`;
  - the shader repro tool that only served the old shaders.
- **Moved out of the repository**, because these were never in git (gitignored), so no tag could restore them. They went to `Desktop/Games/_phisiks_pre_strip_archive` (11 GB, with a README), to be deleted by the owner when no longer needed:
  - the old runtime data (~11 GB);
  - 18 unused vendor libraries (Vulkan, SDL3, assimp, FMOD, FluidSynth, Compressonator, OpenMesh, …), an old glm copy and 8 unused DLLs.
- **What remains:**
  - `vendor/` holds only glad, GLFW, glm, EnTT, stb_image and PhysX (131 MB).
  - The build compiles only Phisiks code plus the glad and stb implementation units.
  - Third-party include paths are private to `PhisiksCore` except glm/EnTT.
- **Strict check:** `tools/check_layering.cmake` now fails the build on any include-rule violation (public headers, apps, runtime tests, unit tests) or any reference to the original project's names in 124 project text files. It runs before the engine compiles and as a ctest. (It previously scanned a directory that no longer existed after the app moved to `apps/`.)
- **Docs:**
  - `ENGINE_REFACTOR_PLAN.md` became `ROADMAP.md`.
  - The baseline and old runtime-status audits were removed.
  - Architecture decisions, known issues and the clean-room ledger were rewritten for Phisiks.
  - New root files: `README.md`, MIT `LICENSE` ("The Phisiks authors"), `THIRD_PARTY_NOTICES.md`.
- **Tools:** `glprobe` now checks the Phisiks requirements (OpenGL 4.6 core; program binaries optional).

Verification, from an empty `Build/` directory and an empty shader cache:
- Release build of every target: **0 errors, 0 warnings, 136 s** (it took roughly 20 minutes while the inherited code was compiled too).
- `PhisiksUnitTests` **100/100**.
- ctest **3/3**: unit tests, layering + name check, deterministic replay.
- Runtime autotest **83/83**:
  - truly cold shader cache: 7 programs compiled in 138 ms;
  - warm: 7 ms from cache;
  - WASAPI device open; no errors or warnings in the log.

**Cleanup after P6 (before P7).** At the owner's request the out-of-repository archive (11 GB) was deleted permanently, the `baseline-original` and `pre-strip` tags were removed, and the git history was rewritten to a single commit (`Phisiks: clean engine (phases P0-P6)`, tag `p6-clean-engine`), so no object of the original code base remains in the repository. The repository moved to `Desktop/Games/Phisiks`. A clean rebuild there gave 0 errors, 100/100 unit tests and ctest 3/3.

## P7 + P7b — GPU compute, GPU physics, SDF and field visualisers (tag `p7-compute`)
**`Phisiks::Compute`** (`engine/include/Phisiks/Compute/Compute.h`, `engine/src/compute/`):
- `IComputeBackend` (buffers, upload/download, kernels from GLSL, bindings, uniforms, dispatch, barrier); OpenGL 4.6 implementation; `EnumerateBackends()` lists OpenCL and SYCL as unavailable hooks.
- User kernels are cached on disk by the engine shader cache, like the renderer's programs.

**GPU-driven rendering** (`engine/src/render/Renderer.cpp`, `engine/shaders/cull.comp.glsl`, `hiz.comp.glsl`):
- Per-frame instance and cull data stream through a persistent, coherently mapped ring buffer (`render/gl/RingBuffer`, 3 regions, one fence each). Blocked waits are counted in `RenderStats::ringBlockedWaits`.
- Instances are uploaded once per frame, sorted by mesh. Each view gets one `DrawElementsIndirectCommand` per mesh plus a slice of a remap buffer (`mesh.vert` reads `remap[gl_BaseInstance + gl_InstanceID]`).
- `cull.comp` tests each object's bounding sphere against the view frustum, then (window views) against a Hi-Z max-depth pyramid built from the previous frame's resolved depth, reprojected with the previous view-projection. Visible objects append to the remap slice and `atomicAdd` the command's `instanceCount`; the commands are then drawn with `glMultiDrawElementsIndirect`.
- Counters (visible / frustum-culled / occlusion-culled) are read back without stalling from a 6-slot fenced ring. `RenderSettings::gpuCulling` / `occlusionCulling` switch the paths; the CPU path remains.

**GPU particles and spatial hash** (`hash.comp`, `scan.comp`, `particles.comp`): count / prefix-scan / scatter counting sort into a hash grid; particle-particle collisions through 27-cell neighbour queries; force fields, scene colliders and an optional tiled shared-memory N-body kernel. Particles render as shaded sphere sprites with entity ids.

**`Phisiks::Physics::Advanced`** (`engine/include/Phisiks/Physics/Advanced.h`, `Xpbd.h`, `Fields.h`):
- Components `ParticleSystem`, `SoftBody` (rope / cloth / tetrahedral soft box) and `ForceField` (uniform, spherical planet gravity with interior `GM r/R^3`, vortex, N-body; smooth influence radius; per-target mask).
- XPBD (small-step, graph-coloured): distance and tet-volume constraints, collisions with the scene's box/sphere `Collider`s and the ground. CPU reference solver in `engine/src/physics/Xpbd.cpp`.
- Two GPU solvers with bit-identical results: a fused single-workgroup kernel (whole step in one dispatch, colours separated by `barrier()`) for bodies up to 8192 nodes / 32768 constraints, and one dispatch per colour per sub-step for larger bodies. The fused kernel cut the FieldLab GPU physics step from ~1100 dispatches (~20 ms CPU) to 13 dispatches (0.2-0.3 ms CPU).
- Force fields act on PhysX rigid bodies through `Scene::ApplyForceFields`, called before every fixed step. The field is sampled at the mean velocity-kick time of the sub-steps, which keeps a 60 Hz orbit within 0.4% of its radius (sampling at the current position gave 7%). An N-body field couples only the dynamic bodies inside its influence radius.

**Visualisers:** `Render::SetSdfVisualization` raymarches up to a few dozen SDF primitives combined with smooth CSG (sphere, box, round box, torus, capsule, cylinder), PBR-shaded with shadows, writing depth and entity id, with an optional signed-distance slice plane (implemented; not covered by a runtime check). `Render::SetFieldVisualization` samples the scene's force fields on a GPU grid and draws arrows.

**FieldLab scene** (PhisiksLab, `N` to cycle): planet with three orbiting moons, a vortex swirling 4096 GPU particles (with field arrows), a mutually attracting N-body cluster, a pinned cloth curtain, a cloth draped over a sphere, a swinging rope, a soft box and a metallic SDF sculpture.

**Engine fixes found by the P7 checks:**
- `Physics::AddForce` / `AddTorque` acted only during the first PhysX sub-step (PhysX clears its accumulators after each `simulate`). They now accumulate and are re-applied on every sub-step of the next `Step`, as documented.
- On the Intel UHD driver, pixels nothing was drawn to kept stale entity ids from earlier frames after resolving the cleared integer MSAA id attachment. The sky pass now writes id 0 to every such pixel.
- The Intel driver exposes only 16 SSBO bindings, so the compute bindings use 4–8 and 13–15 (graphics: 1–3, 9–12).
- Soft-box tetrahedra in mirrored lattice cells are re-oriented to positive rest volume.

**Verification** (Release, Intel UHD Graphics, OpenGL 4.6):
- Clean Release build of every target from an empty build directory: **0 errors, 0 warnings, 228 s**. Layering + name check: clean (154 files).
- `PhisiksUnitTests` **112/112** (+12): field math (inverse-square and interior gravity, vortex profile, influence fade, N-body momentum conservation), spatial hash = brute force exactly, XPBD colouring validity, rope length, analytic stretch `alpha*m*g`, soft-box rest volume and volume after landing, pinned cloth, PhysX orbit under a planetary field, backend hooks.
- ctest **3/3**.
- Runtime autotest **114/114** (+31), with 0 OpenGL errors:
  - **GPU vs CPU culling:** identical visible/culled counts (14/1) and an identical entity-id image (0 of 1,296,000 pixels differ).
  - **Hi-Z:** a box hidden behind the wall raises the occlusion-culled count by exactly 1 and has 0 pixels with occlusion off; the image with occlusion on is identical to the one with it off.
  - **Spatial hash:** GPU neighbour counts equal brute force for 8000 points (60,504 pairs, 0 mismatches).
  - **XPBD:** GPU vs CPU max node difference 3.7e-5 m (rope), 9.0e-7 m (cloth), 6.2e-3 m (soft box, 180 steps with ground contact); fused and multi-pass identical. Stretch `alpha*m*g`: 0.00981 m vs analytic 0.00981 m. Soft-box volume 99.99% of rest.
  - **N-body:** GPU vs CPU max error 2.5e-5 for |a| up to 182 m/s^2 (2048 bodies). **Field sampling:** max error 7.2e-6 for |a| up to 6.4 m/s^2.
  - **FieldLab:** 4096 particles finite and above ground; over 90% of the particles near the vortex axis turn counter-clockwise per frame (92-99% across runs); cloth pinned corners fixed; drape rests on its sphere without penetrating; soft box keeps its 0.8 m height; moons hold their orbit radius within 0.15%; the N-body cluster contracts; particles, both cloths and the SDF are visible in the id buffer.
  - **SDF raymarch:** a sphere's silhouette covers 33,900 px vs 33,835 px analytic (0.19%).
  - **Compute API:** a user saxpy kernel through `IComputeBackend` is exact for 10,000 elements.
  - **Shader cache:** 29 programs; truly cold 520 ms (29 compiled), warm 41 ms (29 from cache).

## P8 — Cross-platform, PhisiksLab studio, core polish (tag `p8-studio`)
**Cross-platform:**
- One CMake build for Windows / Linux / macOS: `PHISIKS_PHYSICS_BACKEND` (PhysX | Native), `PHISIKS_AUDIO_BACKEND` (WASAPI | miniaudio), GLFW from `vendor/` (Windows) or the system / FetchContent, `PHISIKS_VENDOR` and `PHISIKS_OUTPUT_DIR` overrides. Presets: `vs2026-x64`, `windows-portable`, `linux-release/-debug`, `macos-release/-debug` (build + test presets). `tools/fetch_deps.py` downloads the portable dependencies at pinned versions (glad generated with glad2). `.github/workflows/ci.yml` builds and tests on Ubuntu 24.04, macOS 14 and Windows Server 2022.
- Platform layer: `std::filesystem` paths with per-OS executable / cache directories (`platform/Paths`), POSIX fatal-signal handler with `execinfo` backtraces and `crash.txt`, miniaudio output device, process / system memory telemetry (`Phisiks/Platform/System.h`), GL context fallback 4.6 → 4.5 → 4.3 → 4.1 (macOS: 4.1 forward-compatible).
- **Native rigid-body backend** (`engine/src/physics/native/`): GJK distance + EPA, box-box SAT with face clipping, capsule/sphere analytic contacts, persistent manifolds, speculative contacts, sequential impulses with warm starting and Coulomb static/dynamic friction, restitution pass, split-impulse position correction, sleeping, triggers and collision events, raycasts / conservative-advancement sweeps / overlaps, triangle meshes (static), and a capsule character controller (up / side / down passes, step offset, slope limit, pushing). New API for both backends: `Physics::BackendName`, `Physics::SetBodyState`.
- **OpenGL 4.1 tier:** tier-aware GL helpers (direct state access on 4.6, bind-to-edit on 4.1), shader storage through `Load*()` accessors (SSBO / texture buffer), automatic GLSL 4.60 → 4.10 rewrite with binding records, CPU culling with one instanced draw per command, no Hi-Z; ring buffer with CPU staging; `glGetError` polling instead of debug output.
- **CPU compute backend** (`Compute::BackendType::CPU`): worker thread pool, buffers / bindings / uniforms / work-group dispatch identical to the GL backend, kernels registered as C++ functions (`RegisterCpuKernel`). The engine's particles (spatial hash, neighbours, fields, N-body, colliders), XPBD bodies (`Xpbd::Step` per body in parallel) and field sampling run on it on the 4.1 tier and headless (`PHISIKS_CPU_COMPUTE=1` forces it on 4.6).

**PhisiksLab studio** (`apps/lab/src/Studio.cpp`, `Phisiks/UI/ImGuiLayer.h`): Dear ImGui 1.93 with a dark studio theme; hierarchy, inspector (Transform, mesh renderer + live material editing, light, collider, rigidbody, force field, particle system, soft body), ImGuizmo translate / rotate / scale gizmos, click-to-pick from the entity-id buffer, classroom lesson switcher (8 lessons; new lesson scenes `Rotors`, `Orbits`, `PBR`, `Fountain`), live gravity / friction / substeps / culling / quality / render-scale / sharpness controls, CPU / GPU / RAM telemetry with history plots, JSON save / load, replay timeline. Options: `--no-studio`, `--quality`, `--gl41`.

**Core polish:**
- **JSON scenes** (`Phisiks/Scene/SceneSerializer.h`, nlohmann/json): every built-in component, hierarchy and the procedural materials used; lossless round trip.
- **Replay** (`Phisiks/Framework/Replay.h`, `GameContext::replay`): ring buffer of fixed-step snapshots (transforms, body velocities, camera, input actions); `Restore` puts the scene back exactly, playing resumes from there and drops the newer timeline.
- **Synthetic vision export** (`--export-vision`, `Render::RequestVisionCapture`): an extra G-buffer pass of the first window view writes world normals, linear depth and entity ids aligned with the tonemapped colour; exported as PNG / PFM plus camera intrinsics + view matrices and an id → name table.
- **Upscaling:** EASU + RCAS (clean-room implementation of the FSR 1.0 algorithm) as two fragment passes, with a C++ reference (`Phisiks/Render/Upscaler.h`); `RenderSettings::renderScale / upscaler / sharpness`; `Render::ApplyQualityPreset(Low | Medium | High)`.
- **Particle emitters** (`ParticleSystem::emissionRate / lifetime / emitDirection / emitSpeed / emitSpread`) on both backends; **two-way coupling**: particle and soft-body node contacts with dynamic rigid-body colliders apply the reaction impulse `-m Δx / h` to the body (fixed-point integer atomics on the GPU, so deterministic; read back without stalling).

**Bug found and fixed:** after a scene change, `Gameplay::UpdatePlayerBody` called `Entity::Valid()` on a player-body handle whose scene had been destroyed (use-after-free; it surfaced as a crash in the native-physics Release build on the FieldLab → Fountain transition).

**Verification** (Windows 11, Intel UHD Graphics, 12 threads):
- Clean Release build of every target from an empty build directory: **0 errors, 0 warnings, 304 s**. Layering + name check: clean (182 files).
- `PhisiksUnitTests` **122/122** (+10): worker pool coverage, CPU backend 2D dispatch, FSR (constant images preserved, ramp within 2/255, no ringing at a step edge, sharper than bilinear, higher PSNR than bilinear on a downsampled ring pattern), GLSL 4.10 rewrite, lossless JSON round trip, replay restore + resume truncation, headless CPU emitter (600/s, 0.5 s lifetime → 600 emitted, ~300 alive) pushing a body, `SetBodyState`. The same 122 pass with the native physics backend.
- ctest **3/3** (PhysX build, portable build, and the CI-like build).
- Runtime autotest, 0 OpenGL errors in every run:
  | Configuration | Checks |
  |---|---|
  | PhysX + WASAPI, OpenGL 4.6 tier | **131/131** |
  | PhysX + WASAPI, `--gl41` (real 4.1 core context, CPU compute) | **118/118** (GPU-only checks replaced by tier checks) |
  | Native physics + miniaudio, 4.6 tier | **131/131** |
  | Native physics + miniaudio, `--gl41` (the macOS combination) | **118/118** |
  | CI-like: dependencies freshly fetched by `tools/fetch_deps.py`, GLFW from FetchContent, native + miniaudio | **131/131**, unit 122/122, ctest 3/3 |
  New P8 checks: ImGui panels drawn (6,568 vertices); CPU-backend saxpy exact for 100,000 elements; GPU EASU + RCAS vs CPU reference max 4/255 (0.3% of values > 1); FSR PSNR 27.73 dB vs bilinear 26.77 dB against the native frame; Low-preset scale renders 960x600 → 1440x900; vision ids agree with the colour pass on 95% of labelled pixels (sprites draw over meshes in colour only), all normals unit length, linear depth vs physics raycasts max error 0.6 mm; emitter 2,715 alive after 3 s (expected ~2,700); 214 reaction impulses moved a crate 0.53 m; replay restore deviation 0 m; resume truncates the timeline; live JSON round trip identical and rebuilt.
- FieldLab rendered on the 4.1 tier is visually identical to the 4.6 tier (except particle positions, which evolve independently). CPU-backend FieldLab step: ~7 ms on 12 threads (GPU path: 13 dispatches, 0.7 ms to record).
- Shader cache: 4.6 tier 33 programs (warm 47 ms); 4.1 tier 16 programs (cold 2.9 s, warm 30 ms).
- **Not verified:** no Linux or macOS compile has run yet (no WSL / Mac / remote here). The CI workflow is written but has not been executed.

## P9 — Retro studio skin, 3D animation, VFX, terrain, joints, audio, navigation, Lua (tag `p9-3d-vfx`)
**Studio skin** (`Phisiks/UI/RetroDeck.h`, original artwork drawn with ImGui draw lists; no third-party skin assets): brushed charcoal-slate panels (#2B2B3B / #38384C) with two-pixel chiselled bevels and square corners, gold double-striped title bars, `=== PHISIKS LAB ===` menu title, chunky bevelled buttons with green / amber LEDs, recessed pitch-black LCD (sim clock `MM:SS.s` in phosphor green over ghosted `8` segments, FPS / step / state), green -> yellow -> red segmented LED meters (CPU, GPU, physics, RAM), frame-time spectrum analyser, lesson "playlist" and hierarchy in black list boxes with phosphor-green rows and gold / white selection, equaliser-style vertical gradient sliders with metallic thumbs (gravity, friction, substeps, exposure, render scale, bloom), transport bar (step back, play, pause, stop, step forward, record, eject). **HUD overlap fix:** the bitmap telemetry block and lesson captions are no longer drawn while the panels are open (captions move into the classroom panel); `Tab` hides the panels and brings the HUD back. **Bug found:** since P8 the ImGui pass drew into the renderer's present texture, not the window (the renderer leaves its framebuffers bound); `UI::EndFrame` now binds the default framebuffer.

**Animation** (`Phisiks/Animation/Animation.h`): clean-room glTF 2.0 importer (.gltf with external or base64 buffers, .glb; meshes, generated normals, JOINTS_0 / WEIGHTS_0, node TRS / matrix hierarchy, skins with inverse bind matrices, STEP / LINEAR / CUBICSPLINE translation / rotation / scale channels, metallic-roughness materials with embedded or external PNG / JPEG base-colour textures); clip sampling, dual-quaternion (and linear, for comparison) CPU skinning uploaded with `Render::UpdateMeshVertices`; `SkinnedModel` component; centripetal Catmull-Rom and cubic Bezier splines with arc-length parameterisation; `CameraPath` sequencer. `tools/assetgen` now writes `models/walker.glb` (4-bone tube, embedded PNG, a linear and a cubic-spline clip).

**VFX / post** (`Phisiks/VFX/VFX.h`, `RenderSettings::bloom / ssao / decals`): HDR bloom (soft-knee threshold, 13-tap downsample / tent upsample mip chain, added before tonemapping), SSAO (hemisphere kernel, normals from depth, depth-aware 4x4 blur, multiplied into the lit image), projected box decals (scorch / glowing ring / paint / hazard; multiply / additive / alpha) on the resolved depth, camera-facing additive ribbons (`Trail`, `Beam` with raycast and jitter, `OrbitPath`), live GLSL hot reload (`Render::EnableShaderHotReload`, `--hot-reload`, studio button; failed compiles keep the old program). Quality presets drive bloom mips and SSAO (Low: off; Medium: half-resolution, 8 samples; High: full resolution, 16 samples). All passes run on both renderer tiers (41 / 24 programs).

**World:** `Terrain` component (fBm + ridged noise or greyscale image heightmap, central flat pad), mesh with the same triangulation as the collider, 4-layer splatmap by height / slope (`Pattern::Splat`, `Render::RegisterTexture`), `ShapeType::HeightField` colliders (PhysX PxHeightField; native: grid-indexed triangles), `FoliageScatter` (deterministic rejection sampling by height, slope, spacing; procedural pine / bush / rock / grass meshes; one instanced, GPU-culled draw command per mesh).

**Joints** (`Physics::CreateJoint`, `Joint` component): hinge (limits, motor), ball-and-socket, prismatic (limits, motor), spring-damper, on PhysX (revolute / spherical / D6 / distance joints) and the native solver (joint rows in the sequential-impulse loop, split-impulse drift correction, implicit soft spring). Destroying a body destroys its joints.

**Audio:** 3D voices (`Audio::PlaySpatial`, `AudioSource::spatial`): equal-power stereo panning, inverse-square attenuation (amplitude minDistance / d, fade to silence at maxDistance), Doppler pitch from the source and listener velocities, gains / pitch ramped per block; the primary camera is the listener.

**AI** (`Phisiks/AI/Navigation.h`): `NavGrid` from physics raycasts + head-room overlap tests against static geometry, ledges and unwalkable cells eroded by the agent radius, connected regions; 8-connected A* (octile heuristic, no corner cutting) with string pulling; `NavigationSurface` + `NavAgent` crowd (arrival, separation, time-to-collision avoidance, kept on walkable cells and floor height).

**Scripting** (`Phisiks/Script/Script.h`): Lua 5.4.7 (MIT, fetched + SHA-256 checked, `PhisiksLua`); `Script` component with inline source or a file (re-run on save, state kept); callbacks `on_start / on_update / on_fixed_update / on_collision / on_trigger`; API for entity transforms, velocities, impulses / forces / torques, joints (motor, state), materials, visibility, sounds, scene find / spawn / raycast / gravity / events, input actions, logging; sandbox (no io / os / package / debug / dofile / loadfile, text chunks only, 200 ms per-call budget).

**PhisiksLab:** 12 lessons; new scenes `Animation`, `VFX`, `Terrain`, `Joints`; `--hot-reload [dir]`.

**Bugs found and fixed:** the studio UI not reaching the window (above); trails never committing samples above ~70 FPS (the live head was also the comparison sample; found by the 4.1-tier autotest at 10 ms frames); navigation treating wall tops as walkable (ledges now count as obstacles).

**Verification** (Windows 11, Intel UHD Graphics, 12 threads):
- Clean Release build of every target from scratch: **0 errors, 0 warnings, 166 s**. Layering + name check: clean (204 files).
- `PhisiksUnitTests` **136/136** (+14) on PhysX and on the native backend: glTF import (GLB with embedded texture; JSON with a base64 buffer; STEP and CUBICSPLINE sampling, 25 deg cubic key exact, half-way at s = 0.5 within 0.2 deg); bind pose exact (< 1e-5 m); twisted radius DQ 0.250 .. 0.250 m vs linear 0.204 m (rest 0.250); Catmull-Rom interpolates its points, arc-length steps within 0.8 %; pendulum length drift 0.0000 m and period 2.007 s (theory 2.006 s) on both backends; hinge motor 3.000 rad/s, axis error 0.0000, limit held at 45.4 deg; prismatic off-axis 0.0000 m, travel stops at 1.50 m; spring period 0.628 s native / 0.667 s PhysX (theory 0.628 s); height-field raycasts vs heightmap 0.0000 m, ball rests at 0.302 m (radius 0.3) on both; spatial mixer: right source L 0.0000 / R 0.0884, intensity ratio 2 m / 8 m = 16.00, Doppler 1111.1 Hz (theory 1111.1); nav grid detour 16.1 m vs 15.4 m taut, islands unreachable; 8-agent crossing: all arrive in 8.0 s, closest gap 0.005 m; Lua: callbacks, impulse, collision event, runtime error and timeout captured, sandbox; terrain determinism, mesh / collider triangle agreement < 1e-4 m, splat weights sum to 255, scatter rules; trails at 60 Hz and 240 Hz.
- ctest **3/3** (PhysX build and portable build).
- Runtime autotest, 0 OpenGL errors in every run:
  | Configuration | Checks |
  |---|---|
  | PhysX + WASAPI, OpenGL 4.6 tier | **165/165** |
  | PhysX + WASAPI, `--gl41` | **152/152** |
  | Native physics + miniaudio, 4.6 tier | **165/165** |
  | Native physics + miniaudio, `--gl41` | **152/152** |
  New P9 checks (4.6, PhysX): 5 retro panels, LCD / 4 LED meters / 8 EQ sliders / 7 transport buttons / 12 playlist rows; on the presented back buffer the LCD is 89 % black with 6 % phosphor-green text; 0 HUD telemetry lines and 2 overlay glyphs (crosshair) while the panels are open, 21 lines when hidden; 3 walkers, 3,075 vertices skinned in 0.69 ms, textured walker 23,651 px; camera on the spline within 0.0000 m, look dot 1.00000; bloom 6 mips + full-resolution SSAO; 3 trails (189 points), 3 orbit paths, beam stops on the crate face (0.600 vs 0.600 m); bloom raises mean luma 44.55 -> 46.16 (190,830 px brighter); SSAO darkens 6,742 corner pixels, 0 lighter; decals change floor luma: scorch -36.5, glowing ring +178.7, paint +56.4; hot reload: edited tonemap rebuilt (41 programs, frame magenta), broken edit reported with the old program kept, restored source renders normally; terrain 32,768 triangles, 999 foliage instances in 4 batches, height-field raycasts vs rendered heights 0.0001 m; nav grid 2,789 walkable cells in 3.9 ms, 8 agents, 103 paths, 0 failures, closest gap 0.004 m; drone Doppler pitch 0.971 .. 1.033; 9 joints, anchor separation 0.0002 m (native 0.0012 m), windmill 2.500 rad/s, Lua lift 0.000 .. 2.104 m, spring bob 1.319 .. 1.600 m, 4 scripts / 0 errors / 2,197 callbacks, file script rotated 89.4 deg, input -> Lua spawn -> bumper `on_collision` event.
- **Not verified:** Linux / macOS compiles (no CI run yet), GPU cost of bloom / SSAO per pass, i3-class laptop frame times.

## P10 - Showcase suite and core engine feature freeze (tag `p10-showcase`)
**Showcases** (`examples/`, six executables + PhisiksLab SHOWCASE playlist) on a shared framework (`examples/common`, library `PhisiksShowcase`): `ShowcaseScene` (five Catmull-Rom camera shots on keys 1-5, view modes Final / Wireframe / 3D SDF & vector fields / Raw compute particles / CAD 4-way on M and F3 with a blueprint grid), the RetroDeck HUD (LCD FPS + frame time, CPU / GPU / RAM / culling LED meters, frame analyser, GPU and CPU particle counts, RAM against 2.5 GB), `Showcase::Run` (1920x1080, Medium preset, all engine options) and the `ShowcaseTest` runtime harness.
1. **StuntCar** (game dev): raycast spring-damper vehicle (`examples/common/Vehicle`), Lua boost pads (`on_trigger` -> impulse), kicker ramp into five six-storey crate / glass towers that shatter on collision events, HDR spark ribbons, neon tyre trails, skid decals, 4 guard bots on the nav grid, spatial engine + siren audio (new CC0 clips `engine_loop.wav`, `siren.wav` from assetgen).
2. **FluidHarbor** (VFX): 9,000-particle basin + 2,000-particle waterfall emitter rendered by the new screen-space fluid pipeline; buoyancy from a free-surface height field built from the particle columns (non-blocking readback), whirlpool vortex field, force-field wave maker, breakdown wipe slider.
3. **Robotics**: 6-DOF arm (5 motorised hinges + prismatic slide, gravity-compensated links, P servos on the joint motors), dual-quaternion forward kinematics + damped-least-squares IK with whole-arm SDF avoidance rows (dome + gantry beam), pick -> SDF-safe transit -> place on a 6-wheel rover that drives a loop over fBm terrain; joint angle / holding torque meters; replay timeline transport.
4. **Villa** (architecture): glass-and-concrete villa on a cliff plateau, instanced landscaping on a terrain, time-of-day model (sun elevation / colour, sky, fog, exposure, interior + garden lights at dusk), CAD view with slab outlines.
5. **Vision** (AI / CV): signalised intersection, 9 kinematic cars obeying the lights, 10 pedestrians on the nav grid, 16 x 720 LiDAR at 5 Hz from the ego car (labelled raycasts, distance-coloured cloud), split-screen live instance mask, `--headless --export-vision` batch export + LiDAR sweep CSVs + class table.
6. **BlackHole** (science): 52,000 disk-spawned particles on circular orbits in a SphericalGravity field, Doppler colouring + beaming (`engine/shaders/doppler.glsl`, hot-reloadable), photon ring / ISCO orbit paths, helical polar jets, Euler / velocity Verlet / RK4 probes in the Paczynski-Wiita potential.

**Engine additions (then frozen):** `Render::FluidSettings` 4-pass screen-space fluid (sphere depth R32F -> narrow-range bilateral blur, strided, 1-3 iterations -> normals -> refraction / Beer-Lambert / Fresnel / foam; passes 1-3 at half resolution; breakdown wipe), `RenderSettings::wireframe`, `ParticleSpawn::Disk` (orbital velocities) and `::Fill` (even lattice), `ParticleRender::Fluid`, `ParticleColor::Doppler` + `glow`, `ReadParticlesAsync` (GPU copy + fence), `Camera::entityIdColors` + `Render::EntityIdColor` (live instance masks, program `tonemap_ids`), GPU zone timers (Prepass, Scene, SSF, Post, Output, UI, Simulation), `AppOptions::windowWidth / windowHeight / vsync`. Programs: 46 (4.6 tier) / 29 (4.1 tier). The layering check now covers `examples/`.

**Bugs found:** (1) the fluid depth buffer was cleared while depth writes were masked (clears honour `glDepthMask`), so no fluid pixel survived; (2) per-frame synchronous particle readback stalled the GPU queue (~40 ms spikes), replaced by `ReadParticlesAsync`; (3) `Joint::anchor` is in local scaled units: metres moved the arm's pivots (now a unit test); (4) the rover's front and rear wheels steered the same way (crab steering); (5) native `SetBodyState` with zero velocity lets a mid-air body sleep at once; (6) the native rover's suspension rays landed on the arm (vehicle ground-layer mask); (7) point lights were the hidden cost of the harbor (forward shading of every light per pixel): removed there.

**Verification** (Windows 11, Intel UHD Graphics, 12 threads, 1920x1080 window):
- Clean Release builds of every target: **0 errors, 0 warnings** (PhysX + WASAPI 227 s, portable 218 s); layering + name check clean (226 files).
- `PhisiksUnitTests` **142/142** (+6) on PhysX and on the native backend: disk spawn keeps circular orbits (worst radius change 0.0072 over 2 s, CPU backend), fill lattice (no overlaps, first layer at half a spacing), entity-id colours (1,000 ids -> >= 995 distinct), vehicle sag 0.0981 m vs m g / 4k = 0.0981 m and right turn, split-test (Euler |dE/E| 0.701, Verlet 1.3e-6, RK4 2.3e-6, RK4 r 5.08 .. 8.00), Joint anchor in local units (world anchor (0, 1.000, 0)).
- ctest **10/10** on both builds (unit, layering, deterministic replay, the six showcase autotests `--quick --hidden`, the vision export check).
- PhisiksLab autotest: **166/166** on the 4.6 tier and **153/153** with `--gl41`, on both physics backends, including the new step that loads all six showcases inside the Lab (0 GL errors).
- Showcase autotests (full, with the 300-frame budget measurement while camera shot 1 plays; checks passed, average FPS, peak working set):

| Configuration | StuntCar | FluidHarbor | Robotics | Villa | Vision | BlackHole |
|---|---|---|---|---|---|---|
| PhysX, OpenGL 4.6 | 28/28, 83.8 FPS, 360 MB | 29/29, 60.6 FPS, 387 MB | 27/27, 65.2 FPS, 360 MB | 24/25, 58.4 FPS, 369 MB | 27/27, 60.8 FPS, 459 MB | 26/26, 83.9 FPS, 387 MB |
| PhysX, `--gl41` | 28/28, 97.9 FPS, 409 MB | 29/29, 80.4 FPS, 450 MB | 27/27, 89.1 FPS, 414 MB | 25/25, 89.4 FPS, 424 MB | 27/27, 80.9 FPS, 532 MB | 26/26, 95.3 FPS, 453 MB |
| Native, OpenGL 4.6 | 28/28, 87.2 FPS, 355 MB | 29/29, 60.6 FPS, 385 MB | 27/27, 63.8 FPS, 357 MB | 24/25, 58.4 FPS, 368 MB | 27/27, 60.8 FPS, 458 MB | 26/26, 83.0 FPS, 385 MB |
| Native, `--gl41` | 28/28, 98.1 FPS, 403 MB | 29/29, 80.1 FPS, 443 MB | 27/27, 85.2 FPS, 409 MB | 25/25, 89.9 FPS, 422 MB | 27/27, 81.1 FPS, 524 MB | 26/26, 102.0 FPS, 442 MB |

  The only failing check is Villa's 16.7 ms gate on the 4.6 tier (17.1 ms). Every run: 0 OpenGL errors, working set below 540 MB, every body and joint released on unload. GPU zones on the 4.6 tier (ms): StuntCar scene 3.1 / post 2.7 / output 4.0; FluidHarbor scene 3.9 / post 5.1 (SSF 2.3) / output 3.8 / particle simulation 1.0; Robotics scene 6.4; Villa scene 9.2; Vision scene 7.7 (two views); BlackHole scene 1.1 / simulation 2.1.
- Selected measurements (final PhysX 4.6 runs): StuntCar sag 98.1 mm vs 98.1 mm theory, boost pad +10.8 m/s, 32 crates and 36 glass panes broken, 1,372 sparks, siren Doppler pitch 0.47 .. 1.68; FluidHarbor boat draft 0.138 m vs 0.179 m Archimedes, 4 / 4 crates afloat, 3,299 coupling impulses, breakdown stages all distinct; Robotics IK residual <= 16.1 mm while gripping / placing (58 mm in transit, where avoidance takes priority), crate 0.11 m from the bed centre, minimum link / crate clearance 0.096 m, rover 63.7 m driven (1.33 m climb) and back at the dock, replay restore error 0.000000 m; Villa noon / golden / night luma 188.1 / 164.8 / 50.9, golden-hour R/B 1.20 vs 0.95 at noon; Vision 9,288 labelled LiDAR points per sweep, road points on the surface within 0.0000 m, segmentation mask matches the CPU hash on 1.0000 of pixels (109 instances), 2 cars stopped at red lights; BlackHole 1.0000 of 52,000 particles still in the disk after 5 s, approaching / receding B/R 0.93 / 0.52, hot-reloaded `doppler.glsl` turned the disk green (green ratio 0.82 -> 1.21).
- **Not verified:** the i3 / 8 GB target laptop, Linux / macOS builds (no CI run yet), why the 4.1 tier outruns the 4.6 tier on this iGPU.

## P11 - Hero showcases and the retro console UI (tag `p10-showcase` moved here)
**Showcases:** the six P10 demos (and the stunt-car `Vehicle` helper, the vision-export ctest) were replaced by four hero demos on the same framework, each also in PhisiksLab's SHOWCASE playlist:
1. **RoboticArm** (`examples/01_robotics_6dof_arm`): 6-DOF industrial arm from assetgen meshes (bolted base, turret, cast orange upper arm / forearm, black ribbed servo drums, wrist roll + yoke, wrist pitch, tool flange with bolts, parallel gripper fingers, cable conduits); kinematics in `ArmKinematics.h` (dual-quaternion FK, 6x6 damped-least-squares IK with position + orientation, joint limits); velocity-limited joint servos; PTP (S-curve joint interpolation) / LIN (straight TCP line, IK every tick) pick-and-place of a dynamic physics part between two stands; 24 coordinate-frame arrows (base, J1-J6, TCP; Z on each joint axis) with screen labels; glowing TCP ribbon trail; console groups Program, Joint Jog, Cartesian Jog (XYZ, Euler Z-Y-X, quaternion, IK residual), Timeline Replay (60 s joint-space recording, |< / step / play / pause / LIVE, scrubber), Display; SDF view of the link envelopes.
2. **PoolFluid** (`02_vfx_pool_fluid`): open-top tiled basin (cyan tiles, dark grout) + static colliders, white ball (density 0.5); `PoolSim.h`: 128 x 128 heightfield wave solver, two-way ball coupling (column displacement against the ambient surface, buoyancy of the submerged cap + drag), CPU caustics (2x supersampled Snell-refracted sun rays deposited on the floor, ball shadow); post pass `assets/shaders/pool_water.frag.glsl` (heightfield hit, refraction traced against the pool box and the ball, Fresnel sky / ball reflection, Beer-Lambert tint, caustics, breakdown Wireframe -> Depth -> Normals -> Caustics & Refraction with wipe); click-to-ripple, DROP BALL, rain drops.
3. **BlackHole** (`03_science_black_hole`): post pass `assets/shaders/black_hole.frag.glsl`: per-pixel null geodesics (Binet equation u'' + u = 3Mu^2, RK4 in the orbital angle): shadow, photon ring, lensed procedural starfield, disk crossings (the far side bent over / under the shadow) shaded from the particle density with a thin-disk temperature, Doppler + beaming (new `DopplerFactor` in `engine/shaders/doppler.glsl`) and gravitational redshift; `DiskSim.h`: 16,000 particles in the Paczynski-Wiita potential on Euler / velocity Verlet / RK4, split-test sectors, polar density texture; three probes on one eccentric orbit with trails; Black Hole Parameters (mass, inclination, Doppler, beaming, glow, time scale), Integrator Split-Test and Lens groups.
4. **TurbineCad** (`04_cad_turbine_cutaway`): 20 assetgen parts (intake bell-mouth, compressor / turbine casing shells, 3 hex-bolt rings = 106 bolts, shaft + spinner, 3 axial rotor stages with 24 / 28 / 32 cambered twisted blades, 2 stator rings with 30 / 36 vanes, gold impeller with 12 main vanes + 12 splitters, volute scroll with outlet flange, bearing housing, 26 nozzle vanes, 34-blade turbine wheel, exhaust cone, cradle); exploded view along the shaft, ghosted casing (opacity) with a CPU wireframe overlay, CAD ink, rotor speed, bill of materials playlist, F3 blueprints.

**Assets:** `tools/assetgen/HeroMeshes.cpp` (mesh kit: lathe with hard-edge splitting and per-triangle outward orientation, filleted rounded boxes, superellipse lofts, swept tubes, NACA-style cambered blades, hex bolts, rings of parts) writes 35 OBJ files (positions + faces, 5.3 MB, ~150k vertices) to `assets/meshes/hero/`.

**Engine additions (general, inside the freeze rules; ADR-25 / ADR-26):** `Render::PostPass` custom fullscreen passes from asset GLSL files (engine includes, 4.1 rewrite, hot reload / `ReloadShadersNow`, status, GPU zone "Custom"), `Render::SetDataTexture` (R/RG/RGB/RGBA32F), `MaterialDesc::opacity` (blended see-through pass, silhouette-boosted alpha, no shadows / ids / depth), `RenderStats::customPasses / transparentPasses`. Program counts unchanged (46 / 29).

**UI:** `Phisiks/UI/RetroDeck` restyled as a dark retro hardware console (obsidian #12131A / #1B1D26 panels, gunmetal #252834 faces, dark-steel #3E4356 / black #07080B bevels, amber #FFBE0B headings and stripes, silver-white #E2E6F0 text, black LCDs with phosphor green #00FF41 or amber), new widgets `BeginMenuBar` (title plate), `BeginGroup` / `EndGroup` (etched control groups), `LcdField`, amber `Lcd`; LCD value fields on meters and sliders. PhisiksLab and the showcases share it: menu bar File | View | Simulation | CAD | Help, showcase telemetry panel, demo control panel, corner X / Y / Z orientation triad. The 3D studios: light-grey CAD cyclorama (`CadStudio`) for the arm, pool and turbine, deep-space backdrop for the black hole.

**Bugs found:** (1) the pool coupling measured the ball against its own displaced column (feedback lifted the ball out of the water and made the surface noisy) - it now uses the surrounding surface, and the wave stiffness was lowered from 1.6 to 0.9; (2) a paused replay re-pinned the physics part only before the physics step, so it sagged 1.7 mm per tick - it is re-applied after physics too; (3) the impeller sat inside the volute in the exploded view (same spread) - separate spreads; (4) the robot-arm frame-colour check sampled a far camera - the test frames the arm first.

**Verification** (Windows 11, Intel UHD Graphics, 1920x1080, Medium preset):
- Clean Release builds of every target on both presets (PhysX + WASAPI, portable native + miniaudio): **0 errors, 0 warnings**; layering check clean.
- `PhisiksUnitTests` **147/147** on both backends (6 new P11 tests, the vehicle test removed); ctest **7/7** on both (unit, layering, deterministic replay, four showcase autotests `--quick --hidden`).
- PhisiksLab autotest **166/166** (4.6 tier) and **153/153** (`--gl41`) on both backends, with the dark console (P9 skin step: LCD black / phosphor-green pixel check passes).
- Showcase autotests (full: framework steps + demo steps + 300-frame budget during shot 1; checks, average FPS, peak working set); every run **0 OpenGL errors**:

| Configuration | RoboticArm | PoolFluid | BlackHole | TurbineCad |
|---|---|---|---|---|
| PhysX, OpenGL 4.6 | 34/34, 93.2 FPS, 361 MB | 29/29, 77.4 FPS, 422 MB | 27/27, 76.8 FPS, 366 MB | 26/26, 70.6 FPS, 384 MB |
| PhysX, `--gl41` | 34/34, 116.7 FPS, 432 MB | 29/29, 98.1 FPS, 489 MB | 27/27, 85.6 FPS, 438 MB | 26/26, 87.1 FPS, 453 MB |
| Native, OpenGL 4.6 | 34/34, 99.2 FPS, 359 MB | 29/29, 84.9 FPS, 419 MB | 27/27, 79.5 FPS, 368 MB | 26/26, 74.6 FPS, 385 MB |
| Native, `--gl41` | 34/34, 118.9 FPS, 412 MB | 29/29, 101.4 FPS, 474 MB | 27/27, 86.6 FPS, 426 MB | 26/26, 84.0 FPS, 437 MB |

- Selected measurements: IK 40/40 random poses (worst 0.171 mm, 0.024 deg), LIN residual <= 0.051 mm, part placed 0.0 mm from the stand centre, cycle 6.5 s; ball 48.5 % submerged (+3.9 mm from the level; 854 N buoyancy vs 881 N weight), water volume drift 1e-8 m, Snell ratio 1.33300, caustics mean 0.98 / max 5.1; shadow radius 128 px measured vs 144 px from b = 3 sqrt(3) M (11 % small: the edge detector stops at the inner side of the photon ring; inside the 12 % tolerance), Doppler brightness ratio 2.77, Euler |dE/E| 0.27 vs Verlet / RK4 < 2e-3; exploded wheel-impeller gap 1.449 m (1.45 expected), 8,815 rotor / impeller pixels seen through the 0.22-opacity casing, CAD ink 31,455 vs 12,413 dark pixels.
- **Not verified:** the i3 / 8 GB target laptop; Linux / macOS builds (no CI run yet); the showcase console UI is not drawn in the hidden-window autotests (its frame cost is not in the FPS numbers; it was checked visually in the PhisiksLab run).

