Here's a looser, funnier take. I kept every fact that matters (build steps, controls, the showcases, docs) and cut the padding.

# Phisiks

Phisiks is a 3D simulation, visualisation and game engine in C++20. It runs on Windows, Linux and macOS.

The engine is feature-frozen at **P10** (`p10-showcase`). No more new features, which means the bugs you find from here on are officially "character-building."

## What's in the box

- **Scenes:** EnTT entity-component stuff with transform hierarchies, so your objects can be parented to other objects and still somehow know where they are.
- **Physics:** rigid bodies, colliders (heightfields included), hinges, ball-and-sockets, sliders, spring-dampers, and a character controller. Two backends: PhysX 5 on Windows, and a native solver on macOS and Linux. Both do physics. Neither will explain themselves.
- **Renderer:** forward PBR with GPU culling, shadows, fog, a shader cache so you stop waiting for it to compile every launch, and GPU timers so you can see exactly which pass is ruining your day.
- **Compute:** OpenGL 4.6 compute or a multi-threaded CPU backend, with the same systems on the 4.1 tier. If your GPU is old, the CPU has your back.
- **Post FX:** upscaling, bloom, SSAO, decals, a fluid renderer, trails, and live GLSL hot reload, so you can break your shaders without restarting the app.
- **Animation:** glTF 2.0 / GLB import, skinning, and camera paths.
- **World:** terrain, splatmaps, and foliage that scatters itself so you don't have to.
- **AI:** navmesh, A* pathfinding, and crowd steering. Your NPCs can now walk into each other with purpose.
- **Scripting:** sandboxed Lua 5.4, with hot reload.
- **Advanced physics:** rope, cloth, soft bodies, particles that push things, and force fields including N-body gravity. Yes, the black hole is physically accurate. Yes, we're proud of that.
- **Cameras, simulation, export:** multiple viewports, deterministic stepping, PNG and CSV export, a replay ring buffer, and JSON scenes.
- **PhisiksLab:** the editor, in a retro hardware-console skin with a menu bar, LCD readouts and a transport bar. It looks like a piece of 90s studio gear, and that's the point.
- **Audio:** a software mixer with 3D positional sound and Doppler. Doppler shows up everywhere in this project, honestly.

## Build

**Windows** (PhysX + WASAPI, see `docs/BUILD.md` for the `vendor/` setup):

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
ctest --preset release
.\Build\bin\Release\PhisiksLab.exe
```

**Linux / macOS:**

```sh
python3 tools/fetch_deps.py
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
./Build/bin-linux-release/Release/Showcase03_BlackHole
```

Executables look for `assets/` next to themselves, then in the working directory, so run dev builds from the repo root.

## No build tools? No problem

Grab the portable archive for your OS from the GitHub Releases page: `Phisiks-<version>-win64.zip`, `-linux-x86_64.tar.gz`, or `-macos.zip`. Unzip anywhere and run `PhisiksLab`. `QUICKSTART.txt` has the controls, for when you inevitably forget them.

## The showcases

Four demos, one shared framework, one very 1990s engineering-console UI. Each one exists to make you go "wait, that's running in real time?"

| # | Executable | Industry | What it shows |
|---|---|---|---|
| 01 | `Showcase01_RoboticArm` | Robotics | A 6-DOF industrial arm doing pick-and-place with IK, joint jogging, and a timeline you can scrub. It's the arm you'd want at your next dinner party. |
| 02 | `Showcase02_PoolFluid` | Real-time VFX | A pool with a ball that floats, makes waves, and casts caustics. Click the water. Drop the ball. Watch the shader breakdown and feel smart. |
| 03 | `Showcase03_BlackHole` | Science | A Schwarzschild black hole ray-traced per pixel, with a photon ring, a lensed starfield, and 16,000 disk particles. Yes, it's accurate. No, we will not be taking questions. |
| 04 | `Showcase04_TurbineCad` | CAD | A 20-part turbine assembly with an exploded-view slider, a bill of materials, and orthographic blueprints. Your engineering manager will want screenshots. |

## Controls (the short version)

- **Camera:** RMB orbit, MMB pan, wheel zoom. `F4` toggles orbit and first person. `V` cycles camera modes. `WASD` to move, because we are not animals.
- **Shots and views:** `1`-`5` for cinematic camera shots, `0` to go back to free camera, `M` to cycle view modes, `F3` for the CAD four-way view.
- **Panels and general:** `Tab` hides the panels, `F1`/`F2` toggle debug draws, `Esc` pauses, and `Q` quits while paused. Yes, you have to pause first. We're sorry.

Full controls for each demo live in `QUICKSTART.txt`.

## Docs

- `docs/BUILD.md`: requirements, `vendor/` layout, and export flags
- `docs/ROADMAP.md`: what got done, and what's still on the list
- `docs/ARCHITECTURE_DECISIONS.md`: why things are the way they are
- `docs/PHASE_LOG.md`: the full history, with measurements
- `docs/KNOWN_ISSUES.md`: the stuff we already know is broken
- `docs/CLEAN_ROOM.md`: all code is original, and here's the paperwork
- `THIRD_PARTY_NOTICES.md`: the libraries we borrowed, and their licences

MIT licensed. See `LICENSE`.
