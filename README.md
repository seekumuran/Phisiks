# Phisiks

A general-purpose 3D simulation, visualisation and game engine in C++20 for Windows, Linux and macOS (OpenGL 4.6, with an OpenGL 4.1 tier for macOS and older GPUs).

- **Scenes:** EnTT entity-component scenes with transform hierarchies.
- **Physics:** rigid bodies, colliders, queries and a character controller behind a small `Phisiks::Physics` API, with two backends: NVIDIA PhysX 5 (Windows default) and the built-in native solver (macOS, Linux).
- **Renderer:** forward PBR with GPU-driven indirect drawing (compute frustum + Hi-Z occlusion culling), directional/spot/point shadows and analytic fog, plus a persistent shader binary cache.
- **Compute:** `Phisiks::Compute::IComputeBackend` with an OpenGL 4.6 compute backend and a multi-threaded CPU backend (OpenCL/SYCL hooks); GPU spatial hash and particles, with the same systems on the CPU worker pool on the 4.1 tier and headless.
- **Upscaling:** FSR 1.0-style EASU + RCAS spatial upscaling (clean-room) and Low / Medium / High quality presets for integrated GPUs.
- **Advanced physics:** XPBD rope, cloth and soft bodies colliding with the scene; particle emitters; two-way coupling (particles and soft bodies push rigid bodies); force fields (planetary gravity, vortex, N-body).
- **Visualisers:** raymarched SDF / smooth-CSG shapes and vector-field arrows.
- **Cameras:** multiple viewports and orthographic cameras, offscreen targets, a CAD-style orbit camera.
- **Simulation:** deterministic stepped simulation (headless or offscreen) with PNG frame and CSV state export, synthetic-vision export (aligned RGB, linear depth, world normals, entity-id masks, camera CSV), a replay ring buffer with timeline scrubbing, and JSON scene files.
- **PhisiksLab studio:** Dear ImGui hierarchy, inspector with viewport gizmos, classroom lesson switcher, live tuning, CPU/GPU/RAM telemetry and the replay timeline.
- **Audio:** software mixer with WASAPI (Windows) or miniaudio (macOS, Linux) output.
- **Assets:** loaded on demand; the sample assets are procedurally generated and CC0.
- **Look:** a dark studio with a rainbow calibration-grid floor.

```powershell
# Windows (PhysX + WASAPI; vendor/ as described in docs/BUILD.md)
cmake --preset vs2026-x64
cmake --build --preset release
ctest -C Release --test-dir Build/vs2026
.\Build\bin\Release\PhisiksLab.exe
```
```sh
# Linux / macOS (native physics + miniaudio)
python3 tools/fetch_deps.py
cmake --preset linux-release        # or macos-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/PhisiksLab
```

| Doc | What |
|---|---|
| `docs/BUILD.md` | requirements, `vendor/` layout, outputs, controls, simulation/export flags |
| `docs/ROADMAP.md` | completed phases, open follow-ups and what comes next |
| `docs/ARCHITECTURE_DECISIONS.md` | key design decisions |
| `docs/PHASE_LOG.md` | phase-by-phase history with measurements |
| `docs/KNOWN_ISSUES.md` | known limitations |
| `docs/CLEAN_ROOM.md` | provenance: all code original |
| `THIRD_PARTY_NOTICES.md` | third-party libraries and their licences |

MIT licensed, see `LICENSE`.
