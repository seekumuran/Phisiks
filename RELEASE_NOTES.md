# PHISIKS 1.1.0 release notes

PHISIKS is a modular, resource-aware 3D engineering visualisation and simulation engine with an
integrated editor, CAD modelling capabilities, a reusable C++ SDK and controlled external-agent
integration. It is written in C++20 on OpenGL (4.6, with a 4.1 tier for older GPUs and macOS),
runs on integrated GPUs, needs no cloud service and contains no AI.

1.1.0 is the planned final feature release. It turns the 1.0 demo collection into a product: the
`Phisiks` editor opens an empty document, and the samples and showcases are separate programs.

## New in 1.1.0

**Editor**
- **Undo / redo** for every edit made in edit mode (inspector values, gizmo transforms, create /
  delete / duplicate, re-parenting, components, materials, asset assignments, CAD edits, agent
  edits). A continuous interaction is one step; the history keeps 256 steps or 64 MiB.
- **Edit / play separation:** Play captures the authored scene, Stop restores it exactly; saving,
  undo and CAD editing are disabled while playing.
- **Scene format 2:** every component is stored (joints, scripts, terrain, ... were dropped before),
  entities have stable ids, version-1 files are migrated, saves are atomic and a damaged or newer
  file never replaces the open scene.

**Asset pipeline**
- Projects (`assets/` + `scenes/`) with an asset database: stable ids in `.meta` files, import,
  duplicate detection, rename / move tracking (in the editor or outside it), missing-asset
  reports, reimport on change, `asset:<id>/<name>` references in scenes.
- An ASSETS browser: folders, search, type filter, metadata, import, assign, add to scene.
- STL meshes (binary and ASCII) join OBJ, glTF / GLB, PNG / JPG / TGA / BMP and WAV.

**CAD modelling** ([docs/CAD.md](docs/CAD.md))
- Constrained 2D sketches (points, lines, circles, arcs; 21 geometric constraints and dimensions)
  with a numerical solver that reports degrees of freedom and conflicting constraints.
- Parametric parts: named parameters and expressions, and a feature history (extrude, revolve,
  box / cylinder / sphere, fillet, chamfer, booleans, linear / circular patterns, mirror, import)
  with dependency checks, suppress, roll back and per-feature errors.
- Exact B-rep geometry, measurement and STEP / IGES / STL / BREP exchange through
  **Open CASCADE Technology 7.9.3**, behind a PHISIKS-owned interface in a shared library
  (`PhisiksCad`). Built from source by `tools/build_occt.py`; LGPL-2.1 with the OCCT exception,
  linked dynamically (see THIRD_PARTY_NOTICES.md).
- An editor workspace: sketch editing in the viewport, face / edge picking, inspection, export, and
  STEP / IGES / `.phcad` assets as parts. CAD parts are saved with scenes and are undoable.

**SDK** ([SDK.md](SDK.md))
- `cmake --install ... --component sdk` installs headers, libraries and a CMake package:
  `find_package(PHISIKS)` with `Phisiks::Core`, `Phisiks::Cad`, `Phisiks::Editor`, `Phisiks::PAI`.
- `examples/sdk_consumer` builds out of tree against the installed package (ctest `SdkConsumer`).
- `Phisiks/Version.h` is generated from the project version.

**PAI, the PHISIKS Agent Interface** ([docs/PAI.md](docs/PAI.md))
- JSON-RPC 2.0 over the editor's stdin / stdout for an external program you start (an agent's tool
  layer, a script, a test tool). Off unless the editor is started with `--pai`.
- 35 methods: inspection, editing (one undo step each), simulation control and CAD parts, with
  separate read / write / simulate permissions, no file paths, no code execution and request limits.
- `examples/pai_client/pai_client.py`: client library, demo and protocol checks (ctest `PaiProtocol`).

**Packaging**
- Release archives are built only by the `release` workflow with released toolchains; each
  contains `BUILD_INFO.txt` (commit, CI run, compiler, toolchain) and the release has
  `SHA256SUMS.txt`. Configure refuses to package with preview toolchains.

## Verification

| Check | Where | Result |
|---|---|---|
| Unit tests (175 cases, 13 for CAD) | Windows, local | pass |
| Runtime tests on the real renderer: OpenGL 4.6 / 4.1 (editor, undo, edit / play, assets, CAD workflow, samples, showcases) | Windows, Intel UHD, local | all checks pass on both tiers |
| CAD workspace driven by real UI input (sketch, constrain, extrude) at 9 resolution / scale combinations | Windows, local | pass |
| PAI protocol, permission and safety checks | Windows, local | pass |
| SDK install + out-of-tree consumer build and run | Windows, local | pass |
| Linux (GCC) and macOS (Apple Clang) builds, headless tests, Linux runtime tests on Mesa llvmpipe | GitHub Actions | see the CI run for the release commit |
| Release archives | GitHub Actions `release` workflow | see the release page (BUILD_INFO.txt, SHA256SUMS.txt) |

Not verified: macOS rendering (no OpenGL-capable runner or Mac was available; macOS is
experimental), Linux on a hardware GPU, AMD / NVIDIA GPUs on Windows, the CAD UI on a touch or pen
device. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Compatibility

- Scene files: format 2; version-1 files load and are migrated. CAD documents: `phisiks-cad` 1.
- Programs from 1.0 renamed: `PhisiksLab` is now `Phisiks` (editor), `PhisiksSamples` (samples).
- The SDK is new: there is no 1.0 SDK to stay compatible with.
