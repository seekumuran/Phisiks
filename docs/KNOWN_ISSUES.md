# Known Issues

## Intel OpenGL driver: cold shader compiles
Some Intel drivers (seen on UHD Graphics, driver 32.0.101.7076) corrupted the heap while compiling very large GLSL programs from source. The Phisiks renderer:
- keeps its programs small (29 programs since P7, each one compute or graphics pass);
- compiles them only at startup, one at a time;
- caches the binaries on disk (warm start 5–8 ms);
- journals each compile, so a run that dies inside the driver is detected and that program is rebuilt with optimisations off.

No crash has been observed with the Phisiks shaders: more than 10 cold compiles of 108–138 ms each (7 programs), plus a 610 ms first-ever compile; with P7's 29 programs a cold compile takes about 520 ms. If one occurs, the next start reports which program was being compiled.

## Intel OpenGL driver: limits and quirks found in P7
- Only 16 shader-storage buffer bindings (and 16 storage blocks per stage). Graphics passes use bindings 1–3 and 9–12, compute passes 4–8 and 13–15.
- Resolving the cleared integer MSAA entity-id attachment returned stale ids from earlier frames in pixels nothing was drawn to. The sky pass now writes id 0 to all of them; any new pass that leaves pixels untouched must keep that guarantee.

## Rendering
- Hi-Z occlusion culling is single-phase: it tests against the previous frame's depth, so an object that becomes visible because the camera moved (disocclusion) can be missing for one frame. Two-phase culling is a P7 follow-up. Only window views use occlusion; offscreen and extra views use frustum culling.
- With GPU culling, `RenderStats` instance counts are read back without stalling and lag the frame by about 2–5 frames.
- The SDF visualiser raymarches one bounding region per scene, with a fixed step budget, so very thin features or large regions can be missed. Field arrows are drawn in window views only.
- Calibration-grid labels use the 5x7 bitmap font sampled with nearest filtering, so they look blocky very close to the camera.
- Only the first 4 shadow-casting spot lights and 4 point lights get shadow maps; further lights are unshadowed.
- There is no light culling: every light is evaluated for every shaded pixel. That is fine for tens of lights; clustered lighting is not implemented.

## Audio
- 3D voices are stereo-panned (equal power) with inverse-square attenuation and Doppler; there is no HRTF, occlusion or reverb.
- WAV is the only clip format.

## Simulation
- GPU physics coupling is one-way: PhysX bodies are colliders for particles and XPBD, but particles and soft bodies do not push PhysX bodies. Only box and sphere `Collider`s (plus the ground) are used as XPBD/particle colliders.
- The fused XPBD solver runs a body inside one workgroup (one GPU subslice). It is the fastest path for the small and medium bodies in FieldLab, but very large bodies are better served by the multi-pass path (chosen automatically above 8192 nodes or 32768 constraints).
- GPU particles and soft bodies are not part of the deterministic CSV/replay export yet, and GPU floating point results may differ between drivers.
- Rigid-body N-body gravity runs on the CPU (O(n^2)); it is meant for tens of bodies. The GPU tiled N-body kernel is used for particles.
- Deterministic replays are guaranteed for the same build on the same machine (PhysX and floating point are not bit-identical across CPUs or compilers).
- Headless runs have no renderer, so they export data (CSV) but not PNG frames; use `--hidden` for frames.

## Platform
- Windows/MSVC x64 only for now: prebuilt PhysX and GLFW libraries, WASAPI audio.
- `vendor/` is not committed, so a fresh clone needs the libraries listed in `docs/BUILD.md`.

## Input focus
Keyboard state comes from GLFW, so the focused window receives keys. When PhisiksLab's window appears it takes focus. The autotest disables hardware input, so stray keystrokes can't affect a run.

## P8 (cross-platform, studio)
- **Linux / macOS not yet built:** the platform code, presets and CI workflow are written, and the portable configuration (native physics, miniaudio, FetchContent GLFW, fetched dependencies, OpenGL 4.1 tier) passes every test on Windows. No GCC or Apple Clang compile has happened yet; expect fixes on the first CI run.
- **CPU compute cost:** on the 4.1 tier the FieldLab simulation (4096 particles with N-body + 4 XPBD bodies) takes about 7 ms per fixed step on 12 threads, against 0.7 ms of command recording on the GPU path. Dual-core laptops will be slower.
- **FSR GPU vs CPU:** the GPU EASU + RCAS output differs from the C++ reference by at most 4/255 in 0.3% of values (float rounding on the GPU); the runtime check allows up to 6/255 in 1%.
- **RCAS overshoot:** like the reference algorithm, RCAS limits its negative lobe only to avoid clipping; it can overshoot an edge's local min/max a little (bounded at 25% of the step in the unit test). EASU itself never rings.
- **Two-way coupling latency:** the CPU backend applies particle / node reaction impulses on the next fixed step. The GPU backend reads them back without stalling, 1–3 steps later.
- **Native backend vs PhysX:** stacks are less stable and friction slightly different (each passes the analytic unit tests within their tolerances); there is no CCD sweep beyond speculative contacts.
- **4.1 tier shader compile:** cold compile of the 16 graphics programs took 2.9 s on Intel UHD (warm, from the binary cache: 30 ms).

## P9 (3D, VFX, terrain, joints, audio, navigation, Lua)
- **Studio UI on screen (fixed in P9):** the renderer leaves its own framebuffers bound after presenting (4.6 tier blits by name), and in P8 the ImGui pass drew into the renderer's present texture instead of the window. `UI::EndFrame` now binds the default framebuffer; the P9 runtime check reads the window back buffer and finds the LCD's black / phosphor-green pixels.
- **Trails at high frame rates (fixed in P9):** the head point used to be the sample it was compared with, so above ~70 FPS a slow trail never committed a sample. Found by the 4.1-tier autotest (frames of 10 ms); the head is now separate from the committed samples (unit test at 240 Hz).
- **Skinning** runs on the CPU (0.7 ms for 3 x 1,025 vertices); hundreds of skinned characters need a GPU path. Dual-quaternion skinning ignores joint scale.
- **glTF:** sparse accessors, morph targets, cameras / lights and KHR extensions are not supported (required extensions are logged and ignored); only the base-colour texture of a material is used.
- **Post FX** apply to window views only. SSAO derives normals from depth (faceted on curved surfaces at grazing angles); their GPU cost has not been timed per pass (the Low preset turns SSAO off and shortens the bloom chain for integrated GPUs).
- **Decals** project onto everything inside their box (including dynamic objects passing through).
- **Navigation:** the grid is built once (set `NavigationSurface::rebuild` after moving static geometry); foliage has no colliders; agents are kinematic and ignore rigid bodies.
- **PhysX spring-damper** (PxDistanceJoint spring) is softer than the native implicit spring: period 0.667 s vs the analytic 0.628 s for k = 200 N/m, m = 2 kg (native: 0.628 s).
- **Hot reload** rebuilds every program on any change (compiled from source, not via the binary cache); a failed compile keeps the previous program.
- **Lua:** no debugger; scripts see entities by name / id only (no component reflection beyond the documented API).
- **JSON scenes** do not store the P9 components yet.

## P10 (showcases, feature freeze)
- **Frame budget:** measured at 1920x1080 on the Medium preset on this development machine (Intel UHD Graphics), not on an i3 laptop. On the OpenGL 4.6 tier Villa averages 17.1 ms (58.4 FPS) and fails the 16.7 ms gate; FluidHarbor (16.5 ms) and Vision (16.4 ms) pass with almost no margin and dipped below 60 FPS in earlier runs. Every showcase runs faster on the 4.1 tier (CPU culling, plain draws: 80-102 FPS) than on the 4.6 tier's GPU-driven path on this iGPU; not yet explained (numbers in `PHASE_LOG.md`). Forward shading loops over every light for every pixel, so scenes with several point / spot lights pay for them everywhere; the output stage (FSR 1 EASU + RCAS to 1080p plus the present copies) costs about 4 ms on this iGPU on its own.
- **Screen-space fluid:** the particles are a granular fluid (position-based separation, no pressure solve), so the basin settles denser than a lattice and splashes are clumpy; at grazing close-up angles neighbouring spheres differ by more than the narrow-range window and the surface looks bumpy. The fluid passes run on window views only; offscreen targets show plain spheres. Particles squeezed between a moving kinematic collider and a wall can escape through the wall (the harbor uses a force-field wave maker for that reason).
- **Buoyancy (FluidHarbor):** Archimedes forces use the column height of uncovered particle cells (read back asynchronously, 1-3 frames old); the measured boat draft is within 35 % of m / (rho A) (0.138 m vs 0.179 m in the last run), because particle contacts also support the hull.
- **Robot arm:** links are gravity-compensated (gravity off on the arm bodies, holding torques are computed, not simulated); the gripper attaches the crate kinematically instead of by friction; IK avoidance takes priority over the tip in transit (residual up to ~6 cm there, <= 1.6 cm while gripping / placing).
- **Joint anchors** (`Joint::anchor`, `connectedAnchor`) are in the entity's local, scaled units: on a box scaled to (0.2, 1.5, 0.2) an anchor of (0, -0.5, 0) is its bottom face. Passing metres moves the pivot (found while building the arm; now covered by a unit test).
- **Vision:** the live mask colours instances (hashed ids); classes come from `classes.csv` in the export. LiDAR is a physics raycast model (no beam divergence, noise or intensity).
- **Lab:** showcase scenes replace the studio panels with their own HUD while they run (Tab still hides it).
- **Native `SetBodyState` with zero velocities** marks the body ready to sleep at once (meant for restoring resting replay snapshots), so a body restored in mid-air can hang there until something touches it; PhysX wakes it. Teleport with `Entity::SetWorldPose` / `Physics::SetPose` to drop it (the showcase vehicle reset does).
- **Raycast vehicles** hit every layer in `VehicleDesc::groundLayers`; the Robotics rover excludes the arm and crate layers (on the native backend its rays otherwise landed on the arm reaching over it).

