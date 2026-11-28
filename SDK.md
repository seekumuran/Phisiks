# PHISIKS SDK

The SDK lets you build your own C++ application on PHISIKS **outside this repository**: install it
once, then `find_package(PHISIKS)` from any CMake project. `examples/sdk_consumer` is such a project,
and the ctest `SdkConsumer` installs the SDK, copies that example out of the source tree, configures
it against the installed package only, builds it and runs it.

## 1. Installing

From a PHISIKS build tree (any configuration of this repository):

```sh
cmake --install <build dir> --config Release --component sdk --prefix <sdk dir>
```

Layout of `<sdk dir>`:

| Path | Contents |
|---|---|
| `include/Phisiks/` | the engine's public headers (including `Phisiks/Cad/` and the generated `Phisiks/Version.h`) |
| `include/PhisiksEditor/`, `include/PhisiksPAI/` | `Editor.h`, `EditorApp.h`; `Pai.h` |
| `include/glm/`, `include/entt/`, `include/imgui/` | the third-party headers that appear in the public API |
| `lib/` | static libraries (`PhisiksCore`, `PhisiksEditor`, `PhisiksPAI`, `PhisiksImGui`, `PhisiksLua`, GLFW / PhysX import libraries where vendored); on Linux / macOS also `libPhisiksCad` and the Open CASCADE libraries |
| `bin/` (Windows) | `PhisiksCad.dll`, the Open CASCADE DLLs, PhysX DLLs (PhysX builds) |
| `lib/cmake/phisiks/` | `phisiks-config.cmake`, `phisiks-config-version.cmake`, `phisiks-targets.cmake` |
| `share/phisiks/` | `LICENSE`, `THIRD_PARTY_NOTICES.md`, `THIRD_PARTY_LICENSES/` (including Open CASCADE's), this guide, `BUILD_INFO.txt`, the consumer example |

The installed targets contain no paths into the PHISIKS source or build tree (the `SdkConsumer` test
builds against the install with the source tree out of reach).

## 2. Using it from CMake

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApp LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(PHISIKS 1.1 CONFIG REQUIRED)      # -DCMAKE_PREFIX_PATH=<sdk dir>

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE Phisiks::Editor)   # or Phisiks::Core, Phisiks::Cad, Phisiks::PAI
phisiks_copy_runtime(MyApp)                            # Windows: copies the DLLs next to MyApp
```

| Target | What | Brings |
|---|---|---|
| `Phisiks::Core` | the engine (static) | `Phisiks::ImGui`, `Phisiks::Lua`, GLFW, OpenGL, threads (and PhysX on PhysX builds) |
| `Phisiks::Cad` | CAD sketches, constraint solver, parametric documents, geometry kernel (shared) | Open CASCADE, privately |
| `Phisiks::Editor` | the editor as a library (static) | `Phisiks::Core`, `Phisiks::Cad` |
| `Phisiks::PAI` | the agent interface (static) | `Phisiks::Editor` |

Variables set by the package: `PHISIKS_VERSION`, `PHISIKS_PHYSICS_BACKEND` (`PhysX` or `Native`),
`PHISIKS_CAD_KERNEL` (`OCCT` or `None`), `PHISIKS_ROOT_DIR`. The function
`phisiks_copy_runtime(<target>)` copies `bin/*.dll` next to a Windows executable; on Linux and macOS
the shared libraries are found through the rpath.

**Dependencies at build time:** a C++20 compiler of the same family and major version that built the
SDK (MSVC 2022+ with `/MD`, GCC 11+, Apple Clang), CMake 3.25+, OpenGL headers. On Linux, the SDK's
package config calls `find_dependency(glfw3)` / `find_dependency(OpenGL)` if PHISIKS was built against
a system GLFW (install `libglfw3-dev`); SDKs built with `PHISIKS_BUNDLED_GLFW=ON` carry GLFW
statically.

**At run time:** an OpenGL 4.1 driver (4.6 for the full tier) for windowed programs; headless runs need
no GPU. Ship `PhisiksCad` and the Open CASCADE libraries with your program (see section 6).

## 3. A minimal application

```cpp
#include "EditorApp.h"                     // PhisiksEditor

class MyScene : public Phisiks::Scene {
public:
    MyScene() : Phisiks::Scene("Main") {}
    void OnEnter(Phisiks::GameContext&) override {
        Phisiks::Entity box = CreateEntity("Box");
        box.Add<Phisiks::MeshRenderer>(Phisiks::MeshRenderer{ "Cube", "" });
    }
};

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "My application";
    launch.app.initialScene = "Main";
    if (int code = Editor::ParseCommandLine(argc, argv, "MyApp", launch)) return code == -1 ? 0 : code;
    Editor::Configure(Editor::AppConfig{});
    Editor::Host host;
    host.registerScenes = [](Phisiks::SceneManager& s) { s.Register("Main", [] { return std::make_unique<MyScene>(); }); };
    return Editor::Run(launch, host);
}
```

Without the editor, implement `Phisiks::IGame` and run `Phisiks::Application` (`Phisiks/Runtime/`);
`examples/sdk_consumer/main.cpp` does both: `--headless` runs a deterministic simulation, builds a
parametric CAD part, reads an asset database and exits with 0 on success.

## 4. Public API

The public API is the set of headers in `include/Phisiks/`, `include/PhisiksEditor/` and
`include/PhisiksPAI/`. Everything else is private, and the build enforces it: the layering check
(`tools/check_layering.cmake`, ctest `Layering`) fails if a public header includes anything but the
standard library, glm, EnTT, Dear ImGui or other public headers, and if the editor, PAI or the example
projects include private engine headers.

| Area | Headers |
|---|---|
| Application, game loop, scenes | `Runtime/Application.h`, `Runtime/Game.h`, `Framework/SceneManager.h`, `Framework/GameContext.h` |
| Entities and components | `Scene/Scene.h`, `Scene/Components.h`, `Scene/SceneSerializer.h` |
| Rendering | `Render/Renderer.h` (meshes, materials, textures, VFX primitives, frame capture), `Render/RenderStats.h` |
| Physics, compute | `Physics/*.h`, `Compute/Compute.h` |
| Assets | `Assets/Assets.h` (loaders, search paths, references), `Assets/AssetDatabase.h` (projects, stable ids, import, references) |
| CAD | `Cad/Sketch.h`, `Cad/Expression.h`, `Cad/Document.h`, `Cad/Kernel.h` (docs/CAD.md) |
| Editor | `Editor.h` (document, selection, history, edit / play, projects and assets, CAD parts, workspace extensions), `EditorApp.h` (command line, host) |
| Agent interface | `Pai.h` (docs/PAI.md) |
| Version | `Version.h`: `PHISIKS_VERSION_MAJOR / MINOR / PATCH`, `PHISIKS_VERSION_STRING`, `PHISIKS_VERSION` (major * 10000 + minor * 100 + patch) |

**Third-party types in the API:** GLM 1.0.1 (vectors, matrices, quaternions), EnTT 3.11.1
(`Scene::Registry()`), Dear ImGui 1.93.0 (`UI/ImGuiLayer.h`, `UI/RetroDeck.h`). Their versions are part
of the API. Open CASCADE is **not** part of it: no OCCT type appears in a public header.

## 5. Ownership, lifetimes and errors

- **Scenes** own their entities and components. `Phisiks::Entity` is a lightweight handle (scene
  pointer + id); it is invalid after the entity is destroyed. Keep `Entity::GetGuid()` (stable across
  saving, loading, undo and play mode) when you need to refer to an entity later, and look it up with
  `Scene::FindByGuid`.
- **The application** owns the scene manager, renderer, physics and audio; they live between
  `Application::Init` and the end of `Application::Run`. Engine functions that need a renderer (for
  example `Render::RegisterMesh`) do nothing and return false before `Init`.
- **Render resources** registered by name (meshes, materials, textures) live until the renderer shuts
  down, or until `Render::ReleaseMesh` for generated meshes.
- **CAD:** `Cad::Shape` is an immutable, reference-counted handle: copy it freely. `Cad::Document`
  is a value type; `Regenerate` rebuilds its results.
- **Errors:** functions that can fail return `bool` (or an empty / invalid result) and take an optional
  `std::string* error` / `status` that receives a readable reason. Loading never replaces a valid scene
  with a half-loaded one; saving writes to a temporary file and renames it. Engine code does not throw
  across the API; exceptions from your own callbacks propagate to you.
- **Threads:** the API is single-threaded (the main thread) unless a header says otherwise
  (`AssetDatabase::Resolve` and `Assets::Resolve` may be called from worker threads).

## 6. Versions, compatibility, redistribution

- The SDK version is the PHISIKS version (`find_package(PHISIKS 1.1)` accepts 1.1.x). Within 1.1.x the
  API stays source-compatible. The libraries are static (except `PhisiksCad`), so rebuild your
  application with the SDK it uses; there is no binary compatibility promise between versions or
  compilers.
- Scene files: format 2 (version 1 files are migrated on load). CAD documents: `phisiks-cad` version 1.
  Newer formats are refused, not misread.
- **Redistributing** an application built on the SDK: PHISIKS is MIT (`LICENSE`); include it and
  `THIRD_PARTY_NOTICES.md` with the licence texts in `share/phisiks/THIRD_PARTY_LICENSES/`.
  Open CASCADE is LGPL-2.1 with an exception: ship its shared libraries unmodified and replaceable next
  to `PhisiksCad`, with its licence texts (`THIRD_PARTY_NOTICES.md` explains this). An SDK built with
  `-DPHISIKS_CAD_KERNEL=None` contains no Open CASCADE.
- **Build provenance:** `share/phisiks/BUILD_INFO.txt` records the commit, toolchain and backends of the
  SDK. An SDK from a pre-release toolchain is refused by `cmake --install` unless
  `PHISIKS_LOCAL_INSTALL=1` is set, and must not be redistributed.
