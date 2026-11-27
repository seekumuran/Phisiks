# Known Issues

Status for PHISIKS 1.1. The first sections cover the 1.1 product (platforms, editor, example projects, packaging). The later sections are the engine's known limitations, recorded per development phase. They still apply unless marked fixed.

## Platforms (1.1)

| Platform | Verified | Not verified |
|---|---|---|
| **Windows 10/11 x64** | Builds with MSVC (local: VS 2026 / v145, PhysX; CI: VS 2022, native physics). All ctest entries pass on a real GPU (Intel UHD), including the runtime tests on the OpenGL 4.6 and 4.1 tiers. | AMD / NVIDIA GPUs were not tested in 1.1. A machine without Visual Studio was not tested (the archive ships the MSVC runtime app-local). |
| **Linux x64** | Builds with GCC on Ubuntu 24.04 (CI, the distribution default GCC 13) and GCC 12 on Ubuntu 22.04 (release workflow). Unit, layering and deterministic-replay tests pass on the runner. The runtime tests run on Mesa llvmpipe (software OpenGL 4.5, 4.1 tier) under Xvfb. See PHISIKS_RELEASE_LOG.md for the latest result. | No run on a Linux desktop with a hardware GPU. Wayland not tested. |
| **macOS (arm64)** | Builds with Apple Clang on macOS 15 (CI). Unit, layering and deterministic-replay tests pass. | **The editor and every rendered program are untested on macOS.** GitHub's macOS runners cannot create an OpenGL context (`NSGL: Failed to find a suitable pixel format`: no OpenGL-capable GPU in the VM), and no physical Mac was available. Treat macOS as experimental. |

macOS-specific limitations, by design:

- macOS OpenGL stops at 4.1 and is deprecated by Apple. PHISIKS always uses its 4.1 tier there: CPU culling, CPU compute (particles, XPBD, fields), and texture-buffer storage instead of SSBOs. GPU compute paths and Hi-Z occlusion are not available.
- PhysX is not supported. macOS uses the built-in physics solver.
- Programs are plain executables (not `.app` bundles), unsigned and not notarised. Gatekeeper blocks them until the quarantine flag is removed (`xattr -dr com.apple.quarantine <folder>`).
- A future Metal or Vulkan backend would be a renderer rewrite. It is not part of 1.1.

## Editor (1.1)

- **No undo / redo.** Deleting asks for confirmation; reopen the scene file to get back to the saved state.
- **No asset-import pipeline.** Meshes are OBJ files referenced by path (anything under `assets/` or a folder added with `--assets`); the inspector's mesh list scans those folders. glTF models are loaded through `SkinnedModel` in code, not from the editor.
- **No prefab / project system.** A "project" is a scene file plus the folders it references. There is no recent-files list.
- **Play / Pause run the live scene.** There is no separate edit mode that restores the scene on Stop. Use the Reload button (it asks about unsaved changes) or reopen the file.
- **Scene files do not store some components:** `Joint`, `Script`, `NavigationSurface`, `NavAgent`, `Terrain`, `FoliageScatter`, `Decal`, `Trail`, `Beam`, `OrbitPath`, `SkinnedModel`, `CameraPath`, `Health` and `Interactable`. Scenes that use them must re-create them in code after loading.
  - Saving warns first. File > Save, Save as, the unsaved-changes prompt and the workspace Save buttons list the affected types with their counts. They write nothing until you choose SAVE WITHOUT THEM or CANCEL; cancelling leaves the file untouched.
  - `Editor::SaveScene` called from code refuses such a save unless it passes `allowComponentLoss`.
  - A workspace that re-creates components itself after loading declares them (`ComponentsRestoredAfterLoad`) and isn't warned about. Old-McDonald does this for its navigation components.
  - The editor can only *add* stored components (mesh renderer, light, box collider, rigidbody), so scenes built entirely in the editor never hit the warning.
- **Material edits are shared.** A material is edited by name, so every part that uses it changes (the inspector says so).
- **Unsaved-changes tracking** covers edits made through the editor (inspector, gizmo, hierarchy, create / delete, workspace panels). Changes made by running scene logic, such as physics moving bodies or a mission animating parts, are not counted as edits.
- **Panel layout** is fixed: left column, right column and bottom dock, each resizable with its splitter. Panels cannot be undocked or rearranged, and the layout is not saved between sessions.
- **UI scale** follows the monitor's content scale at startup (Windows / Linux). Moving the window to a monitor with a different scale does not rescale it. Override with `PHISIKS_UI_SCALE`.
- The Windows programs are console applications, so a console window with the log opens next to the editor.

## Example projects (1.1)

- **Project Marina** does not simulate. The flow-direction animation is schematic: constant speed, no data. No thermal or fluid model is run and no values are shown.
- **Old-McDonald:** the rover route (A* on the navigation grid) and the arm pose (IK) are computed by the engine. The drone flight, crop mapping, data transfer and measurements are scripted: no flight dynamics, image processing or sensor values. The panel tags every mission stage with what drives it.
- The CAD workspaces and the mission scene disable the replay timeline. Their state (explode offsets, mission progress) is more than entity transforms, and a transform replay would leave the two out of step.
- The models are generic models of their component classes, styled from photographs where noted in the project READMEs. They are not measured from the prototypes.
- The workspace tools find parts **by name** (explode offsets, subsystem rows, wiring groups, mission parts). Renaming a part in the inspector detaches it from those tools until the scene is rebuilt; moving, scaling and re-materialing are safe.

## Packaging (1.1)

- Archives are unsigned. Windows SmartScreen may warn on first launch.
- The release contains only the three archives built by the `release` workflow on GitHub's runners (`-win64`, `-linux-x86_64`, `-macos-arm64`). All three use the native physics backend, so **no PhysX build is published**; PhysX is available when building from source on Windows.
- Each archive contains `BUILD_INFO.txt` (commit, CI run, compiler, CMake version, backends). Configure detects pre-release toolchains (Visual Studio Preview / Insiders, CMake release candidates) and blocks packaging with them; `PHISIKS_ALLOW_PREVIEW_TOOLCHAIN=ON` allows a local test package, which is named `...-preview-toolchain` and is never published.
- Logs, crash dumps and saves default to paths relative to the working directory (`Build/runs/latest`) and next to the executable (scene files). If those folders are not writable, they go to the per-user data folder instead: `%LOCALAPPDATA%\Phisiks`, `~/Library/Application Support/Phisiks`, `~/.local/share/phisiks`.

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

## Input focus
Keyboard state comes from GLFW, so the focused window receives keys. When a PHISIKS window appears it takes focus. The runtime tests disable hardware input, so stray keystrokes can't affect a run. Run them with `--hidden` (as ctest does): a visible test window that is resized, maximised or minimised by the desktop during a run changes the frame size and breaks pixel checks.

## P8 (cross-platform, studio)
- **Linux / macOS builds (fixed since):** both now build in CI. See "Platforms (1.1)" above for what is verified.
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
- **Frame budget:** measured at 1920x1080 on the Medium preset on this development machine (Intel UHD Graphics), not on an i3 laptop. P11 hero demos on the OpenGL 4.6 tier: RoboticArm 93 FPS, PoolFluid 77, BlackHole 77, TurbineCad 71 (numbers in `PHASE_LOG.md`); the 4.1 tier is again faster on this iGPU (86-117 FPS), still not explained. The showcase autotests run in a hidden window without the ImGui console, so the measured frame excludes the UI pass (~0.5 ms). The output stage (FSR 1 EASU + RCAS to 1080p plus the present copies) costs about 4 ms on this iGPU on its own.
- **Screen-space fluid:** the particles are a granular fluid (position-based separation, no pressure solve), so the basin settles denser than a lattice and splashes are clumpy; at grazing close-up angles neighbouring spheres differ by more than the narrow-range window and the surface looks bumpy. The fluid passes run on window views only; offscreen targets show plain spheres. Particles squeezed between a moving kinematic collider and a wall can escape through the wall (the harbor uses a force-field wave maker for that reason).
- **Buoyancy (FluidHarbor):** Archimedes forces use the column height of uncovered particle cells (read back asynchronously, 1-3 frames old); the measured boat draft is within 35 % of m / (rho A) (0.138 m vs 0.179 m in the last run), because particle contacts also support the hull.
- **Robot arm:** links are gravity-compensated (gravity off on the arm bodies, holding torques are computed, not simulated); the gripper attaches the crate kinematically instead of by friction; IK avoidance takes priority over the tip in transit (residual up to ~6 cm there, <= 1.6 cm while gripping / placing).
- **Joint anchors** (`Joint::anchor`, `connectedAnchor`) are in the entity's local, scaled units: on a box scaled to (0.2, 1.5, 0.2) an anchor of (0, -0.5, 0) is its bottom face. Passing metres moves the pivot (found while building the arm; now covered by a unit test).
- **Showcases** draw their own HUD instead of the editor panels (Tab still hides it).
- **Native `SetBodyState` with zero velocities** marks the body ready to sleep at once (meant for restoring resting replay snapshots), so a body restored in mid-air can hang there until something touches it; PhysX wakes it. Teleport with `Entity::SetWorldPose` / `Physics::SetPose` to drop it (the showcase vehicle reset does).
- **Raycast vehicles** hit every layer in `VehicleDesc::groundLayers`; the Robotics rover excludes the arm and crate layers (on the native backend its rays otherwise landed on the arm reaching over it).

## P11 hero showcases
- **Pool water** is a heightfield (one height per column): no breaking waves, splashes or droplets; the ball's displacement is added at half strength per tick for stability. Refraction is traced against the analytic pool box and ball, then the hit is looked up in the scene image, so geometry hidden from the camera (e.g. under the rim) shows what the camera sees at that screen position. Wall caustics reuse the floor map along the sun direction (approximate).
- **Black hole** is Schwarzschild (non-rotating) only: no Kerr frame dragging. The disk is thin and its particles move in a pseudo-Newtonian (Paczynski-Wiita) potential; colour uses a blackbody-like ramp, not a spectral integration. The Binet integration has a fixed 160-step budget: rays that orbit the photon sphere for more than ~4 turns stop early (they fall back to a dimmed background sample).
- **Robot arm** joints are kinematic servos (velocity-limited), not physics joints; the gripped part is pinned to the tool every tick rather than held by friction. IK is numerical (DLS) and can stop at a joint limit for unreachable targets (the residual is shown).
- **See-through materials** (`MaterialDesc::opacity`) are not sorted and draw both faces in one blended pass: fine for a few shells, not for many overlapping transparent objects. They cast no shadows and write no ids / depth (CAD ink and picking see the parts behind them).
- **Removed with the P10 demos:** the stunt-car `Vehicle` helper (and its unit test) and the vision-export ctest. Vision capture itself (`Render::RequestVisionCapture`, `--export-vision`) is unchanged and still covered by the runtime tests.

## P12 release packages (1.0; superseded by "Packaging (1.1)" above)
- 1.0 built Windows and Linux archives in CI. The Linux archive was never launched on a Linux desktop. 1.1 adds a macOS build and creates the GitHub Release as a draft.
- Executable names are the CMake target names (`Phisiks`, `PhisiksSamples`, `Showcase01_RoboticArm`, ...), not the `examples/` folder names.
