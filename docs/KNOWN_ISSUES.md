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
- Sounds play without 3D positioning (no distance attenuation or panning yet).
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
