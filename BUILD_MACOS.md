# Building PHISIKS on macOS (experimental)

> **Status in 1.1:** PHISIKS **builds** with Apple Clang on macOS 15 (arm64), and its **headless tests pass** (unit tests, layering, deterministic replay), both verified on GitHub's `macos-15` runner. The **editor and every rendered program are untested on macOS**: the runner is a virtual machine that cannot create an OpenGL context (`NSGL: Failed to find a suitable pixel format`), and no physical Mac was available. Until someone runs it on a Mac, treat macOS as experimental. Please report results.

## Platform audit

| Area | macOS situation | What PHISIKS does |
|---|---|---|
| Compiler | Apple Clang (Xcode command-line tools), C++20 | builds without changes (CI) |
| Graphics API | OpenGL is **deprecated** by Apple and frozen at **4.1 core** (on Apple silicon it is implemented over Metal) | always runs the **4.1 tier**. No compute shaders or SSBOs, so culling, Hi-Z, particles, XPBD and field sampling use the CPU paths, and storage uses texture buffers. GL 4.3+ debug output is unavailable, so GL errors are only checked with `glGetError` |
| Context creation | core profile needs `FORWARD_COMPAT`; Retina windows have a 2x framebuffer | GLFW: 4.1 core, forward-compatible, `COCOA_RETINA_FRAMEBUFFER`; the UI renders at the framebuffer scale (ImGui backend) and is not scaled again |
| Physics | no PhysX binaries for macOS | built-in solver (`PHISIKS_PHYSICS_BACKEND=Native`) |
| Audio | | miniaudio (CoreAudio); links CoreFoundation, CoreAudio and AudioToolbox |
| Paths | | executable folder via `_NSGetExecutablePath`; per-user data in `~/Library/Application Support/Phisiks`; shader cache in `~/Library/Caches/Phisiks` |
| Packaging | Gatekeeper blocks unsigned downloads | a zip of plain executables (no `.app` bundle), unsigned and not notarised; remove the quarantine flag (below) |
| Deprecation warnings | | `GL_SILENCE_DEPRECATION` |

A native Metal (or Vulkan via MoltenVK) renderer is the long-term answer to OpenGL's deprecation. It would be a large rewrite of `engine/src/render` and the GLSL shaders, and it is **not** part of 1.1.

## Requirements

- macOS 12 or newer (tested in CI: macOS 15, arm64).
- Xcode command-line tools: `xcode-select --install`.
- Homebrew packages: `brew install cmake ninja python`. GLFW is built from source with `-DPHISIKS_BUNDLED_GLFW=ON`, or use `brew install glfw`.

## Build and test

```sh
python3 tools/fetch_deps.py          # pinned sources into vendor/ (glad2 via a private venv if pip refuses Homebrew's Python)
cmake --preset macos-release -DPHISIKS_BUNDLED_GLFW=ON
cmake --build --preset macos-release
ctest --preset macos-release         # unit, layering, deterministic replay; rendered tests need a GPU-backed session
```

These are the exact commands the `macos` CI job runs, with `-DPHISIKS_TEST_SKIP_RENDER=ON` added there because the runner has no OpenGL. On a Mac with a display, leave that option off to run the rendered tests:

```sh
./Build/bin-macos-release/Release/PhisiksRuntimeTests --autotest --gl41 --run-dir Build/runs/macos
cat Build/runs/macos/autotest_report.txt
```

## Run

```sh
./Build/bin-macos-release/Release/Phisiks
./Build/bin-macos-release/Release/PhisiksSamples
./Build/bin-macos-release/Release/OldMcDonald
./Build/bin-macos-release/Release/glprobe            # reports the context the driver gives
```

Run from the repository root (assets are found next to the executable first, then in the working directory). `--gl41` is implied on macOS.

## Package

```sh
cpack --config Build/macos-release/CPackConfig.cmake   # dist/Phisiks-1.1.0-macos-arm64.zip
```

After downloading and extracting a package:

```sh
xattr -dr com.apple.quarantine Phisiks-1.1.0-macos-arm64
./Phisiks-1.1.0-macos-arm64/Phisiks
```

The release workflow checks with `otool -L` that the programs link only system libraries and frameworks.

## What to check on a real Mac

1. `glprobe` reports an OpenGL 4.1 core context.
2. `PhisiksRuntimeTests --autotest --gl41` finishes with 0 failed checks.
3. The editor opens, the panels are sharp on a Retina display, and picking selects the object under the cursor. Picking reads the id buffer at framebuffer resolution, so it has to be correct at a 2x scale.
4. `OldMcDonald` runs the mission (Mission camera preset).
