# Phisiks

A general-purpose 3D simulation, visualisation and game engine in C++20 / OpenGL 4.6.

- **Scenes:** EnTT entity-component scenes with transform hierarchies.
- **Physics:** NVIDIA PhysX 5 rigid bodies, colliders, queries and a character controller, behind a small `Phisiks::Physics` API.
- **Renderer:** forward PBR with indirect drawing, directional/spot/point shadows and analytic fog, plus a persistent shader binary cache.
- **Cameras:** multiple viewports and orthographic cameras, offscreen targets, a CAD-style orbit camera.
- **Simulation:** deterministic stepped simulation (headless or offscreen) with PNG frame and CSV state export.
- **Audio:** WASAPI output with a software mixer.
- **Assets:** loaded on demand; the sample assets are procedurally generated and CC0.
- **Look:** a dark studio with a rainbow calibration-grid floor.

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
ctest -C Release --test-dir Build/vs2026
.\Build\bin\Release\PhisiksLab.exe
```

| Doc | What |
|---|---|
| `docs/BUILD.md` | requirements, `vendor/` layout, outputs, controls, simulation/export flags |
| `docs/ROADMAP.md` | completed phases and what comes next (GPU compute, XPBD, PhisiksLab, templates) |
| `docs/ARCHITECTURE_DECISIONS.md` | key design decisions |
| `docs/PHASE_LOG.md` | phase-by-phase history with measurements |
| `docs/KNOWN_ISSUES.md` | known limitations |
| `docs/CLEAN_ROOM.md` | provenance: all code original |
| `THIRD_PARTY_NOTICES.md` | third-party libraries and their licences |

MIT licensed, see `LICENSE`.
