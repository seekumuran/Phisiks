# Phisiks

A 3D simulation, visualisation and game engine in C++20. Runs on Windows and Linux.

## Features

- **Scenes:** EnTT ECS with transform hierarchies.
- **Physics:** rigid bodies, colliders (incl. heightfields), hinges, ball-and-sockets, sliders, spring-dampers, character controller. PhysX 5 on Windows, native solver on macOS/Linux.
- **Renderer:** forward PBR, GPU culling, shadows, fog, shader cache, GPU timers.
- **Compute:** OpenGL 4.6 compute or multi-threaded CPU backend (also 4.1 tier).
- **Post FX:** upscaling, bloom, SSAO, decals, fluid renderer, trails, live GLSL hot reload.
- **Animation:** glTF 2.0/GLB import, skinning, camera paths.
- **World:** terrain, splatmaps, foliage scattering.
- **AI:** navmesh, A* pathfinding, crowd steering.
- **Scripting:** sandboxed Lua 5.4 with hot reload.
- **Advanced physics:** rope, cloth, soft bodies, particles, force fields (incl. N-body gravity).
- **Simulation/export:** multiple viewports, deterministic stepping, PNG/CSV export, replay ring buffer, JSON scenes.
- **PhisiksLab:** editor with retro hardware-console UI.
- **Audio:** software mixer, 3D positional sound, Doppler.

## Build

**Windows** (PhysX + WASAPI — see `docs/BUILD.md` for `vendor/` setup):

\`\`\`powershell
cmake --preset vs2026-x64
cmake --build --preset release
ctest --preset release
.\Build\bin\Release\PhisiksLab.exe
\`\`\`

**Linux / macOS:**

\`\`\`sh
python3 tools/fetch_deps.py
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/Showcase03_BlackHole
\`\`\`

Run dev builds from the repo root (executables look for `assets/` next to themselves, then the working directory).

## No build tools?

Download a portable archive from GitHub Releases: `Phisiks-<version>-win64.zip` or `-linux-x86_64.tar.gz` (no macOS build; use the `macos-release` preset). Unzip and run `PhisiksLab`. See `QUICKSTART.txt` for controls.

## Showcases

| # | Executable | Industry | Shows |
|---|---|---|---|
| 01 | `Showcase01_RoboticArm` | Robotics | 6-DOF arm, IK pick-and-place, joint jogging, scrubbable timeline |
| 02 | `Showcase02_PoolFluid` | Real-time VFX | Floating ball, waves, caustics, interactive water |
| 03 | `Showcase03_BlackHole` | Science | Per-pixel Schwarzschild black hole, photon ring, lensed starfield, 16,000 disk particles |
| 04 | `Showcase04_TurbineCad` | CAD | 20-part turbine assembly, exploded view, BOM, orthographic blueprints |

## Controls

- **Camera:** RMB orbit, MMB pan, wheel zoom, `F4` orbit/first-person, `V` cycle modes, `WASD` move.
- **Shots/views:** `1`-`5` cinematic shots, `0` free camera, `M` cycle view modes, `F3` CAD four-way view.
- **General:** `Tab` hide panels, `F1`/`F2` debug draws, `Esc` pause, `Q` quit (while paused).

Full per-demo controls in `QUICKSTART.txt`.

## Docs

- `docs/BUILD.md` — requirements, `vendor/` layout, export flags
- `docs/ROADMAP.md` — status and plans
- `docs/ARCHITECTURE_DECISIONS.md` — design rationale
- `docs/PHASE_LOG.md` — project history
- `docs/KNOWN_ISSUES.md` — known bugs
- `docs/CLEAN_ROOM.md` — originality documentation
- `THIRD_PARTY_NOTICES.md` — third-party licences

MIT licensed — see `LICENSE`.
