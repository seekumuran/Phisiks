# Building PHISIKS

The short version for each platform. The full guides (requirements, options, outputs, packaging,
what was tested) are [BUILD_WINDOWS.md](BUILD_WINDOWS.md), [BUILD_LINUX.md](BUILD_LINUX.md) and
[BUILD_MACOS.md](BUILD_MACOS.md).

## Requirements

| | |
|---|---|
| Compiler | Visual Studio 2022 or 2026 (MSVC), GCC 11+ / Clang 15+, Apple Clang (Xcode 15+) |
| CMake | 3.25 or newer (Ninja on Linux / macOS) |
| Python | 3.8 or newer (`tools/fetch_deps.py`, `tools/build_occt.py`, the PAI example client) |
| GPU | OpenGL 4.6 (full tier) or 4.1 (`--gl41`, always on macOS) to run the programs; building needs none |

## Steps

```sh
# 1. third-party sources at pinned versions into vendor/ (glm, EnTT, Dear ImGui, Lua, ...)
python tools/fetch_deps.py

# 2. the CAD kernel: Open CASCADE 7.9.3 from checksummed source into vendor/occt (once)
python tools/build_occt.py --generator "Visual Studio 17 2022"    # Linux / macOS: --generator Ninja

# 3. configure, build, test
cmake --preset windows-portable          # Linux: linux-release, macOS: macos-release
cmake --build --preset portable-release  # Linux: linux-release, macOS: macos-release
ctest --preset portable-release          # unit, layering, replay, showcases, runtime tests, PAI, SDK consumer
```

The Windows presets use Visual Studio 2026. With Visual Studio 2022, configure directly (as CI does):

```powershell
cmake -S . -B Build/vs2022 -G "Visual Studio 17 2022" -A x64 -DPHISIKS_PHYSICS_BACKEND=Native -DPHISIKS_AUDIO_BACKEND=miniaudio
cmake --build Build/vs2022 --config Release --parallel
ctest --test-dir Build/vs2022 -C Release
```

On Windows, `vs2026-x64` / `release` is the PhysX configuration (NVIDIA PhysX binaries placed by
hand, see BUILD_WINDOWS.md); `windows-portable` / `portable-release` uses the built-in physics and
needs nothing beyond the steps above.

Run development builds from the repository root, for example `Build/bin-portable/Release/Phisiks`.

## Options

| Option | Default | |
|---|---|---|
| `PHISIKS_PHYSICS_BACKEND` | `PhysX` on Windows, else `Native` | rigid-body backend |
| `PHISIKS_AUDIO_BACKEND` | `WASAPI` on Windows, else `miniaudio` | audio output |
| `PHISIKS_CAD_KERNEL` | `Auto` | `OCCT`, `None` (no solids), `Auto` |
| `PHISIKS_TEST_SKIP_RENDER` | `OFF` | skip tests that need an OpenGL window |
| `PHISIKS_INSTALL_SDK` | `ON` | install rules for the SDK component |

## Install the SDK, make packages

```sh
cmake --install <build dir> --config Release --component sdk --prefix <sdk dir>   # SDK.md
cpack --config <build dir>/CPackConfig.cmake -C Release                          # runtime + SDK archives in dist/
```

Packaging refuses pre-release toolchains (Visual Studio Preview / Insiders, CMake release
candidates): published archives are built by the `release` workflow on GitHub with released
toolchains, and carry `BUILD_INFO.txt` and `SHA256SUMS.txt`.
