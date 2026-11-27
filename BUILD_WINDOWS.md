# Building PHISIKS on Windows

Windows 10 / 11 x64. Two configurations:

| Configuration | Physics | Audio | Third-party binaries | Used by |
|---|---|---|---|---|
| **PhysX** (`vs2026-x64` preset, the Windows default) | NVIDIA PhysX 5.3.1 | WASAPI | PhysX SDK + GLFW placed by hand in `vendor/` | local development, the `-win64-physx` archive |
| **Portable** (`windows-portable` preset, or the CI command below) | built-in solver | miniaudio | none (GLFW built from source) | CI, the `-win64` archive |

## Requirements

| Tool | Version | Notes |
|---|---|---|
| Visual Studio C++ tools | **2022 (v143)** or **2026 (v145)** | the `vs2026-x64` preset uses the "Visual Studio 18 2026" generator. With VS 2022, configure with `-G "Visual Studio 17 2022" -A x64` (as CI does) |
| CMake | 3.25 or newer | |
| Python | 3.8 or newer | `tools/fetch_deps.py` |
| Git | any | |
| GPU driver | OpenGL 4.6 (full tier) or 4.1 (`--gl41`) | `glprobe` reports what the driver offers |

## 1. Third-party sources

```powershell
python tools/fetch_deps.py
```

This downloads the portable dependencies at pinned versions into `vendor/`: glm, EnTT, stb, Dear ImGui, ImGuizmo, miniaudio, nlohmann/json, Lua 5.4.7 (SHA-256 checked), and glad (generated with the `glad2` Python package, in a private virtual environment if pip refuses the system interpreter). It also collects their licence texts into `vendor/licenses/`. It is enough for the portable configuration.

The PhysX configuration also needs two prebuilt SDKs placed by hand:

| Folder | Contents |
|---|---|
| `vendor/physx/` | PhysX 5.3.1 `include/` and `lib/{release,debug}` (`PhysX_64`, `PhysXCommon_64`, `PhysXCooking_64`, `PhysXFoundation_64`, `PhysXExtensions_static_64`, `PhysXPvdSDK_static_64`, `PhysXCharacterKinematic_static_64`) |
| `vendor/DLLs/` | the PhysX runtime DLLs (`PhysX_64.dll`, `PhysXCommon_64.dll`, `PhysXCooking_64.dll`, `PhysXFoundation_64.dll`, `PVDRuntime_64.dll`), copied next to every executable after each build |
| `vendor/GLFW/` | GLFW `include/` and `lib/{release,debug}/glfw3.lib`. Without it, CMake builds GLFW 3.4 from source |

## 2. Configure and build

```powershell
# PhysX + WASAPI (VS 2026)
cmake --preset vs2026-x64
cmake --build --preset release                 # or relwithdebinfo / debug

# Portable backends (native physics + miniaudio), output in Build/bin-portable
cmake --preset windows-portable
cmake --build --preset portable-release

# Portable backends with VS 2022 (the CI command)
cmake -S . -B Build/ci -G "Visual Studio 17 2022" -A x64 -DPHISIKS_PHYSICS_BACKEND=Native -DPHISIKS_AUDIO_BACKEND=miniaudio
cmake --build Build/ci --config Release --parallel
```

From Git Bash, pass MSBuild switches with a dash (`-- -m`), not a slash.

Options (CMake cache):

| Option | Default | Effect |
|---|---|---|
| `PHISIKS_PHYSICS_BACKEND` | `PhysX` (Windows) | `PhysX` or `Native` |
| `PHISIKS_AUDIO_BACKEND` | `WASAPI` (Windows) | `WASAPI` or `miniaudio` |
| `PHISIKS_BUILD_PROJECTS` | `ON` | the example projects in `projects/` and the runtime tests that host them |
| `PHISIKS_TEST_SKIP_RENDER` | `OFF` | skip the ctest entries that need an OpenGL window (machines without a GPU) |
| `PHISIKS_BUNDLED_GLFW` | `OFF` | always build GLFW from source as a static library |
| `PHISIKS_OUTPUT_DIR` | `Build/bin` | executables go to `<dir>/<config>` |

## 3. Outputs

`Build/bin/Release/` (`Build/bin-portable/Release/` for the portable preset):

| Program | What |
|---|---|
| `Phisiks.exe` | the PHISIKS editor |
| `PhisiksSamples.exe` | the engine sample scenes in the editor |
| `Showcase01_RoboticArm.exe` .. `Showcase04_TurbineCad.exe` | the four standalone showcases |
| `ProjectMarina.exe`, `OldMcDonald.exe` | the example projects (`projects/`) |
| `PhisiksUnitTests.exe`, `PhisiksRuntimeTests.exe` | headless unit tests; runtime verification harness |
| `assetgen.exe`, `projectassetgen.exe` | regenerate `assets/` and `projects/assets` (deterministic) |
| `glprobe.exe`, `stacksample.exe` | GPU capability check; symbolised stack sampler for hang diagnosis |

Run development builds **from the repository root**. Executables find `assets/` next to themselves first, then in the working directory.

```powershell
.\Build\bin\Release\Phisiks.exe
.\Build\bin\Release\Phisiks.exe --open scenes\my.scene.json --quality low
.\Build\bin\Release\PhisiksSamples.exe --scene FieldLab
.\Build\bin\Release\OldMcDonald.exe
```

Every program accepts `--help`. Logs go to `<run dir>/engine.log` (default `Build/runs/latest`, relative to the working directory; if it isn't writable, `%LOCALAPPDATA%\Phisiks\runs\latest`). Crash dumps go to the same folder.

## 4. Tests

```powershell
ctest --preset release                                     # PhysX build
ctest --test-dir Build/vs2026 -C Release -R Unit           # a subset
```

| ctest | What | Needs a GPU |
|---|---|---|
| `PhisiksUnitTests` | headless unit tests (math, scene, physics, compute, render, audio / assets, serializer) | no |
| `Layering` | apps, examples, projects and tests use only the public engine API; brand check | no |
| `DeterministicReplay` | two headless runs export byte-identical CSV; a hidden-window run exports PNG frames | the PNG part |
| `Showcase01_RoboticArm` .. `Showcase04_TurbineCad` | each showcase's own runtime verification | yes |
| `RuntimeTests`, `RuntimeTestsGL41` | `PhisiksRuntimeTests --autotest --hidden`: editor, samples, showcases and example projects on the OpenGL 4.6 and 4.1 tiers | yes |

`PhisiksRuntimeTests --autotest [--gl41] --run-dir <dir>` writes `<dir>/autotest_report.txt`, screenshots and `engine.log`. The exit code is the number of failed checks. Use `--hidden` (as ctest does) so that nothing on the desktop can resize, cover or minimise the test window during the run.

## 5. Install and package

```powershell
cmake --install Build/vs2026 --config Release --prefix C:\Temp\Phisiks     # the release layout, unzipped
cpack --config Build/vs2026/CPackConfig.cmake -C Release                    # dist/Phisiks-1.1.0-win64-physx.zip
cpack --config Build/portable/CPackConfig.cmake -C Release                  # dist/Phisiks-1.1.0-win64.zip (portable)
```

The package is one folder with everything next to the executables:

- the programs above, except the unit / runtime tests and the asset tools;
- `glprobe`;
- the PhysX DLLs (PhysX build only);
- the MSVC runtime, app-local (the UCRT is part of Windows 10+);
- `assets/`, `shaders/`, and `projects/` (scene files, meshes and READMEs; no sources);
- `THIRD_PARTY_LICENSES/`;
- `LICENSE`, `THIRD_PARTY_NOTICES.md`, `README.md`, `QUICKSTART.txt`.

Release notes:

- **Toolchain:** build public packages with a released (non-preview) Visual Studio. The app-local MSVC runtime DLLs come from the toolchain that built the package.
- **PhysX:** a PhysX package ships NVIDIA's DLLs under BSD-3. Their licence text is in `THIRD_PARTY_LICENSES/physx.txt`.
- **Not signed:** Windows SmartScreen may warn on first launch.
- **Console window:** the programs are console applications, so double-clicking also opens a console window with the log.

## Tested

Windows 11 Home (26200), Intel Core i5-12450H, Intel UHD Graphics, Visual Studio 18 (2026) Insiders, CMake 4.5.0-rc1, PhysX configuration. Results are recorded in [PHISIKS_RELEASE_LOG.md](PHISIKS_RELEASE_LOG.md).
