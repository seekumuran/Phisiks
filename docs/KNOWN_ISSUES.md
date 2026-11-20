# Known Issues

## Intel OpenGL driver: cold shader compiles
Some Intel drivers (seen on UHD Graphics, driver 32.0.101.7076) corrupted the heap while compiling very large GLSL programs from source. The Phisiks renderer:
- keeps its programs small (7 programs);
- compiles them only at startup, one at a time;
- caches the binaries on disk (warm start 5–8 ms);
- journals each compile, so a run that dies inside the driver is detected and that program is rebuilt with optimisations off.

No crash has been observed with the Phisiks shaders: more than 10 cold compiles of 108–138 ms each, plus a 610 ms first-ever compile. If one occurs, the next start reports which program was being compiled.

## Rendering
- Calibration-grid labels use the 5x7 bitmap font sampled with nearest filtering, so they look blocky very close to the camera.
- Only the first 4 shadow-casting spot lights and 4 point lights get shadow maps; further lights are unshadowed.
- There is no light culling: every light is evaluated for every shaded pixel. That is fine for tens of lights; clustered lighting is not implemented.

## Audio
- Sounds play without 3D positioning (no distance attenuation or panning yet).
- WAV is the only clip format.

## Simulation
- Deterministic replays are guaranteed for the same build on the same machine (PhysX and floating point are not bit-identical across CPUs or compilers).
- Headless runs have no renderer, so they export data (CSV) but not PNG frames; use `--hidden` for frames.

## Platform
- Windows/MSVC x64 only for now: prebuilt PhysX and GLFW libraries, WASAPI audio.
- `vendor/` is not committed, so a fresh clone needs the libraries listed in `docs/BUILD.md`.

## Input focus
Keyboard state comes from GLFW, so the focused window receives keys. When PhisiksLab's window appears it takes focus. The autotest disables hardware input, so stray keystrokes can't affect a run.
