# Building PHISIKS on Linux

x86-64 (arm64 should work the same way, but it is not tested). Linux builds use the portable backends: the built-in physics solver (PhysX is not used on Linux) and miniaudio for audio.

## Requirements

| | Tested in CI | Minimum |
|---|---|---|
| Distribution | Ubuntu 24.04 (CI), Ubuntu 22.04 (release workflow) | glibc 2.35+ for the release archive |
| Compiler | GCC 13 (Ubuntu 24.04 default), GCC 12 (release workflow) | GCC 11+ or Clang 15+ (C++20) |
| CMake | the runner's preinstalled CMake | 3.25 |
| Build tool | Ninja | |
| Python | 3.8+ | `tools/fetch_deps.py` |
| GPU driver | Mesa 25 llvmpipe (software, OpenGL 4.5) in CI | OpenGL 4.6 for the full tier, 4.1 for the `--gl41` tier |

## 1. System packages

Ubuntu / Debian:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 python3-venv git \
    libgl1-mesa-dev libasound2-dev libglfw3-dev
```

`libglfw3-dev` is optional. Without it, or with `-DPHISIKS_BUNDLED_GLFW=ON`, CMake builds GLFW 3.4 from source as a static library. That needs the X11 / Wayland development headers:

```sh
sudo apt-get install -y libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols
```

Fedora: `sudo dnf install gcc-c++ cmake ninja-build python3 mesa-libGL-devel alsa-lib-devel glfw-devel` (not tested).

## 2. Third-party sources

```sh
python3 tools/fetch_deps.py
```

This downloads the pinned dependencies into `vendor/` and their licence texts into `vendor/licenses/`. On distributions whose Python is "externally managed" (PEP 668, e.g. Ubuntu 24.04), the `glad2` generator is installed into a private virtual environment (`vendor/.glad-venv`). That needs the `python3-venv` package.

## 3. Configure, build, test

```sh
cmake --preset linux-release                     # Ninja, Release, native physics + miniaudio
cmake --build --preset linux-release
ctest --preset linux-release                     # needs a display for the rendered tests
```

Without a display (servers, containers, CI), skip the tests that need an OpenGL window:

```sh
cmake --preset linux-release -DPHISIKS_TEST_SKIP_RENDER=ON
cmake --build --preset linux-release
ctest --preset linux-release                     # unit, layering, deterministic replay
```

The rendered runtime tests can also run on Mesa's software renderer under Xvfb:

```sh
sudo apt-get install -y xvfb libgl1-mesa-dri
xvfb-run -a -s "-screen 0 1280x800x24" ./Build/bin-linux-release/Release/PhisiksRuntimeTests --autotest --gl41 --hidden --run-dir Build/runs/linux
```

Debug build: `cmake --preset linux-debug && cmake --build --preset linux-debug`.

## 4. Run

```sh
./Build/bin-linux-release/Release/Phisiks                    # the editor
./Build/bin-linux-release/Release/PhisiksSamples --scene FieldLab
./Build/bin-linux-release/Release/OldMcDonald
./Build/bin-linux-release/Release/glprobe                    # what the driver offers
```

Run from the repository root: executables find `assets/` next to themselves first, then in the working directory. On GPUs without OpenGL 4.6, add `--gl41`. Logs go to `Build/runs/latest/engine.log`, or `~/.local/share/phisiks/runs/latest` if the working directory is not writable.

## 5. Install and package

```sh
cmake --preset linux-release -DPHISIKS_BUNDLED_GLFW=ON     # static GLFW: no libglfw needed on the target
cmake --build --preset linux-release
cmake --install Build/linux-release --prefix /opt/phisiks   # the release layout
cpack --config Build/linux-release/CPackConfig.cmake       # dist/Phisiks-1.1.0-linux-x86_64.tar.gz
```

The archive is one folder with the programs, `assets/`, `shaders/`, `projects/`, `THIRD_PARTY_LICENSES/`, the documents and `BUILD_INFO.txt` (commit, CI run, compiler, CMake version). Executables use `$ORIGIN` as their run path. The published Linux archive is the one built by the `release` workflow; see "Release packages" in the README.

Runtime dependencies on the target are those of any OpenGL application:
- the system C/C++ runtime (glibc 2.35+, libstdc++);
- `libGL` from Mesa or the GPU vendor;
- X11 or XWayland;
- ALSA, PulseAudio or PipeWire for sound.

The release workflow checks the archive with `ldd` for unresolved libraries.

## Tested

The results of the CI runs (build, headless tests, runtime tests on llvmpipe, packaging) are recorded with their dates in [PHISIKS_RELEASE_LOG.md](PHISIKS_RELEASE_LOG.md). Not tested: a Linux desktop with a hardware GPU, Wayland sessions, arm64, distributions other than Ubuntu.
