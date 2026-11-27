# Phisiks: CMake build (Windows / Linux / macOS).
#
# Targets
#   PhisiksCore        static library: the engine (engine/include public API, engine/src private)
#   PhisiksImGui       static library: Dear ImGui + GLFW/OpenGL3 backends + ImGuizmo (vendor/)
#   PhisiksLua         static library: Lua 5.4 (vendor/lua, C)
#   PhisiksEditor      static library: the editor (apps/editor/src), reused by every app below
#   Phisiks            the PHISIKS editor (apps/editor)
#   PhisiksSamples     the engine sample scenes in the editor (examples/samples)
#   Showcase01..04     four standalone demo programs (examples/0N_*)
#   ProjectMarina, OldMcDonald   separate example projects built on the engine (projects/)
#   PhisiksRuntimeTests  runtime verification harness (tests/runtime) hosting all of the above
#   PhisiksUnitTests   headless unit tests (tests/unit)
#   assetgen           regenerates the CC0 engine / example assets in assets/
#   projectassetgen    regenerates the example projects' meshes in projects/assets
#   glprobe, stacksample (Windows), check_layering   tools
#
# Backends (cache options)
#   PHISIKS_PHYSICS_BACKEND  PhysX | Native   PhysX: Windows default (vendored MSVC binaries), or any
#                                             platform with the vcpkg "physx" port; Native: the
#                                             built-in solver (macOS and Linux default)
#   PHISIKS_AUDIO_BACKEND    WASAPI | miniaudio
#   PHISIKS_OUTPUT_DIR       executables go to <dir>/<config> (default Build/bin)
#   PHISIKS_BUNDLED_GLFW     ON: always build GLFW from source as a static library (release packages:
#                            no libglfw shared library needed on the user's machine)
# Release package: cmake --install / cpack (see "Install + package" below and docs/BUILD.md).
# Dependencies: vendor/ (python tools/fetch_deps.py downloads the portable ones; see docs/BUILD.md).

cmake_minimum_required(VERSION 3.25)
project(Phisiks VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(PHISIKS_ROOT   ${CMAKE_CURRENT_SOURCE_DIR})
set(PHISIKS_VENDOR ${PHISIKS_ROOT}/vendor CACHE PATH "Third-party dependency folder (tools/fetch_deps.py)")
set(PHISIKS_LIBCFG $<IF:$<CONFIG:Debug>,debug,release>)

if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PHISIKS_DEFAULT_PHYSICS PhysX)
    set(PHISIKS_DEFAULT_AUDIO WASAPI)
else()
    set(PHISIKS_DEFAULT_PHYSICS Native)
    set(PHISIKS_DEFAULT_AUDIO miniaudio)
endif()
set(PHISIKS_PHYSICS_BACKEND ${PHISIKS_DEFAULT_PHYSICS} CACHE STRING "Rigid-body backend: PhysX or Native")
set_property(CACHE PHISIKS_PHYSICS_BACKEND PROPERTY STRINGS PhysX Native)
set(PHISIKS_AUDIO_BACKEND ${PHISIKS_DEFAULT_AUDIO} CACHE STRING "Audio device backend: WASAPI or miniaudio")
set_property(CACHE PHISIKS_AUDIO_BACKEND PROPERTY STRINGS WASAPI miniaudio)
set(PHISIKS_OUTPUT_DIR ${PHISIKS_ROOT}/Build/bin CACHE PATH "Executable output root")
if(PHISIKS_AUDIO_BACKEND STREQUAL "WASAPI" AND NOT WIN32)
    message(FATAL_ERROR "WASAPI is Windows-only; use -DPHISIKS_AUDIO_BACKEND=miniaudio")
endif()
message(STATUS "Phisiks: physics ${PHISIKS_PHYSICS_BACKEND}, audio ${PHISIKS_AUDIO_BACKEND}, output ${PHISIKS_OUTPUT_DIR}")

# The vendored PhysX libraries use the dynamic CRT.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Third-party (see THIRD_PARTY_NOTICES.md): glad, GLFW, glm, EnTT, stb_image(_write), Dear ImGui,
# ImGuizmo, miniaudio, nlohmann/json, Lua 5.4, NVIDIA PhysX 5 (optional backend).
# ---------------------------------------------------------------------------------------------
foreach(dep glad/src/gl.c glm/glm/glm.hpp entt/entt.hpp stb_image/stb_image.h imgui/imgui.h imguizmo/ImGuizmo.h nlohmann/json.hpp miniaudio/miniaudio.h lua/src/lua.h)
    if(NOT EXISTS ${PHISIKS_VENDOR}/${dep})
        message(FATAL_ERROR "vendor/${dep} is missing: run 'python tools/fetch_deps.py' (see docs/BUILD.md)")
    endif()
endforeach()

# GLFW: vendored prebuilt on Windows; a system / package-manager install elsewhere, else fetched.
add_library(phisiks_glfw INTERFACE)
if(WIN32 AND EXISTS ${PHISIKS_VENDOR}/GLFW/lib)
    target_include_directories(phisiks_glfw INTERFACE ${PHISIKS_VENDOR}/GLFW/include)
    target_link_directories(phisiks_glfw INTERFACE ${PHISIKS_VENDOR}/GLFW/lib/${PHISIKS_LIBCFG})
    target_link_libraries(phisiks_glfw INTERFACE glfw3 opengl32 user32 gdi32 shell32)
else()
    option(PHISIKS_BUNDLED_GLFW "Build GLFW from source (static) instead of using a system GLFW" OFF)
    if(NOT PHISIKS_BUNDLED_GLFW)
        find_package(glfw3 3.3 QUIET)
    endif()
    if(PHISIKS_BUNDLED_GLFW OR NOT glfw3_FOUND)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
    endif()
    find_package(OpenGL REQUIRED)
    target_link_libraries(phisiks_glfw INTERFACE glfw OpenGL::GL)
endif()

# PhysX
set(PHISIKS_RUNTIME_DLLS)
if(PHISIKS_PHYSICS_BACKEND STREQUAL "PhysX")
    add_library(phisiks_physx INTERFACE)
    if(WIN32 AND EXISTS ${PHISIKS_VENDOR}/physx/lib)
        target_include_directories(phisiks_physx INTERFACE ${PHISIKS_VENDOR}/physx/include ${PHISIKS_VENDOR}/physx/include/physx)
        target_link_directories(phisiks_physx INTERFACE ${PHISIKS_VENDOR}/physx/lib/${PHISIKS_LIBCFG})
        target_link_libraries(phisiks_physx INTERFACE
            PhysX_64 PhysXCommon_64 PhysXCooking_64 PhysXFoundation_64
            PhysXExtensions_static_64 PhysXPvdSDK_static_64 PhysXCharacterKinematic_static_64)
        file(GLOB PHISIKS_RUNTIME_DLLS ${PHISIKS_VENDOR}/DLLs/*.dll)
    else()
        # vcpkg: `vcpkg install physx`, configure with CMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
        find_package(unofficial-omniverse-physx-sdk CONFIG REQUIRED)
        target_link_libraries(phisiks_physx INTERFACE unofficial::omniverse-physx-sdk::sdk)
    endif()
endif()

# Dear ImGui + backends + ImGuizmo (warnings off: third-party code).
add_library(PhisiksImGui STATIC
    ${PHISIKS_VENDOR}/imgui/imgui.cpp ${PHISIKS_VENDOR}/imgui/imgui_draw.cpp
    ${PHISIKS_VENDOR}/imgui/imgui_tables.cpp ${PHISIKS_VENDOR}/imgui/imgui_widgets.cpp
    ${PHISIKS_VENDOR}/imgui/backends/imgui_impl_glfw.cpp ${PHISIKS_VENDOR}/imgui/backends/imgui_impl_opengl3.cpp
    ${PHISIKS_VENDOR}/imguizmo/ImGuizmo.cpp)
target_include_directories(PhisiksImGui PUBLIC ${PHISIKS_VENDOR}/imgui ${PHISIKS_VENDOR}/imguizmo)
target_include_directories(PhisiksImGui PRIVATE ${PHISIKS_VENDOR}/imgui/backends)
target_link_libraries(PhisiksImGui PRIVATE phisiks_glfw)
if(MSVC)
    target_compile_options(PhisiksImGui PRIVATE /W0 /utf-8)
    target_compile_definitions(PhisiksImGui PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
else()
    target_compile_options(PhisiksImGui PRIVATE -w)
    target_compile_definitions(PhisiksImGui PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
endif()

# Lua 5.4 (C sources; the interpreter / compiler mains are not part of the library).
file(GLOB PHISIKS_LUA_SOURCES ${PHISIKS_VENDOR}/lua/src/*.c)
list(FILTER PHISIKS_LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(PhisiksLua STATIC ${PHISIKS_LUA_SOURCES})
target_include_directories(PhisiksLua PUBLIC ${PHISIKS_VENDOR}/lua/src)
set_target_properties(PhisiksLua PROPERTIES LINKER_LANGUAGE C)
if(MSVC)
    target_compile_options(PhisiksLua PRIVATE /W0)
    target_compile_definitions(PhisiksLua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(PhisiksLua PRIVATE -w)
    if(APPLE)
        target_compile_definitions(PhisiksLua PRIVATE LUA_USE_MACOSX)
    elseif(UNIX)
        target_compile_definitions(PhisiksLua PRIVATE LUA_USE_LINUX)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# PhisiksCore
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE PHISIKS_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${PHISIKS_ROOT}/engine/src/*.cpp ${PHISIKS_ROOT}/engine/src/*.h ${PHISIKS_ROOT}/engine/include/*.h)
if(PHISIKS_PHYSICS_BACKEND STREQUAL "PhysX")
    list(FILTER PHISIKS_ENGINE_SOURCES EXCLUDE REGEX "/physics/native/NativePhysics\\.cpp$")
else()
    list(FILTER PHISIKS_ENGINE_SOURCES EXCLUDE REGEX "/physics/physx/")
endif()
if(NOT PHISIKS_AUDIO_BACKEND STREQUAL "miniaudio")
    list(FILTER PHISIKS_ENGINE_SOURCES EXCLUDE REGEX "/audio/MiniaudioDevice\\.cpp$")
endif()

# Engine GLSL sources are embedded into the library at build time (no runtime shader paths).
file(GLOB PHISIKS_ENGINE_SHADERS CONFIGURE_DEPENDS ${PHISIKS_ROOT}/engine/shaders/*.glsl)
set(PHISIKS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(PHISIKS_EMBEDDED_SHADERS ${PHISIKS_GENERATED_DIR}/phisiks/EmbeddedShaders.inc)
add_custom_command(
    OUTPUT ${PHISIKS_EMBEDDED_SHADERS}
    COMMAND ${CMAKE_COMMAND} -DSHADER_DIR=${PHISIKS_ROOT}/engine/shaders -DOUTPUT=${PHISIKS_EMBEDDED_SHADERS} -P ${PHISIKS_ROOT}/tools/embed_shaders.cmake
    DEPENDS ${PHISIKS_ENGINE_SHADERS} ${PHISIKS_ROOT}/tools/embed_shaders.cmake
    COMMENT "Embedding engine shaders"
    VERBATIM)

add_library(PhisiksCore STATIC ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS} ${PHISIKS_EMBEDDED_SHADERS} ${PHISIKS_VENDOR}/glad/src/gl.c)
source_group(TREE ${PHISIKS_ROOT}/engine PREFIX "engine" FILES ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS})

# Public API: engine headers + glm and EnTT (public headers/templates); Dear ImGui comes with PhisiksImGui.
target_include_directories(PhisiksCore PUBLIC
    ${PHISIKS_ROOT}/engine/include
    ${PHISIKS_VENDOR}/glm
    ${PHISIKS_VENDOR}/entt)
# Private: implementation headers and backends. Apps and tests cannot include these.
target_include_directories(PhisiksCore PRIVATE
    ${PHISIKS_ROOT}/engine/src
    ${PHISIKS_GENERATED_DIR}
    ${PHISIKS_VENDOR}/glad/include
    ${PHISIKS_VENDOR}/stb_image
    ${PHISIKS_VENDOR}/nlohmann
    ${PHISIKS_VENDOR}/miniaudio
    ${PHISIKS_VENDOR}/imgui/backends)
target_link_libraries(PhisiksCore PUBLIC PhisiksImGui Threads::Threads)
target_link_libraries(PhisiksCore PRIVATE phisiks_glfw PhisiksLua ${CMAKE_DL_LIBS})
if(PHISIKS_PHYSICS_BACKEND STREQUAL "PhysX")
    target_link_libraries(PhisiksCore PRIVATE phisiks_physx)
    target_compile_definitions(PhisiksCore PUBLIC PHISIKS_PHYSICS_PHYSX=1)
else()
    target_compile_definitions(PhisiksCore PUBLIC PHISIKS_PHYSICS_NATIVE=1)
endif()
if(PHISIKS_AUDIO_BACKEND STREQUAL "miniaudio")
    target_compile_definitions(PhisiksCore PRIVATE PHISIKS_AUDIO_MINIAUDIO=1)
endif()

if(MSVC)
    target_compile_definitions(PhisiksCore PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(PhisiksCore PUBLIC /utf-8 /bigobj /Zc:__cplusplus /W3)
    target_compile_options(PhisiksCore PRIVATE /MP)
    target_link_libraries(PhisiksCore PUBLIC dbghelp ole32 psapi)
else()
    target_compile_options(PhisiksCore PUBLIC -Wall)
    if(UNIX AND NOT APPLE)
        target_link_libraries(PhisiksCore PUBLIC m)
    endif()
    if(APPLE)
        target_link_libraries(PhisiksCore PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
        target_compile_definitions(PhisiksCore PUBLIC GL_SILENCE_DEPRECATION)
    endif()
endif()

function(phisiks_output target)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(cfg Debug Release RelWithDebInfo)
            string(TOUPPER ${cfg} CFG)
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${PHISIKS_OUTPUT_DIR}/${cfg})
        endforeach()
    else()
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PHISIKS_OUTPUT_DIR}/${CMAKE_BUILD_TYPE})
    endif()
endfunction()

function(phisiks_executable target)
    target_link_libraries(${target} PRIVATE PhisiksCore)
    phisiks_output(${target})
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${PHISIKS_ROOT})
    foreach(dll ${PHISIKS_RUNTIME_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:${target}>)
    endforeach()
endfunction()

# ---------------------------------------------------------------------------------------------
# Showcases (examples/): PhisiksShowcase = the shared framework (examples/common) + the four hero demo
# scenes; one executable per demo (examples/NN_name/main.cpp). The runtime tests and the CAD
# workspaces of the example projects (CAD ink post pass) link the library too.
# ---------------------------------------------------------------------------------------------
file(GLOB PHISIKS_SHOWCASE_SOURCES CONFIGURE_DEPENDS
    ${PHISIKS_ROOT}/examples/common/*.cpp ${PHISIKS_ROOT}/examples/common/*.h
    ${PHISIKS_ROOT}/examples/0*/*.cpp ${PHISIKS_ROOT}/examples/0*/*.h)
list(FILTER PHISIKS_SHOWCASE_SOURCES EXCLUDE REGEX "/main\\.cpp$")
add_library(PhisiksShowcase STATIC ${PHISIKS_SHOWCASE_SOURCES})
target_include_directories(PhisiksShowcase PUBLIC ${PHISIKS_ROOT}/examples/common)
target_link_libraries(PhisiksShowcase PUBLIC PhisiksCore)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_SHOWCASE_SOURCES})
set(PHISIKS_SHOWCASES
    Showcase01_RoboticArm:01_robotics_6dof_arm
    Showcase02_PoolFluid:02_vfx_pool_fluid
    Showcase03_BlackHole:03_science_black_hole
    Showcase04_TurbineCad:04_cad_turbine_cutaway)
foreach(entry ${PHISIKS_SHOWCASES})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 target)
    list(GET parts 1 dir)
    add_executable(${target} ${PHISIKS_ROOT}/examples/${dir}/main.cpp)
    target_link_libraries(${target} PRIVATE PhisiksShowcase)
    phisiks_executable(${target})
    set_target_properties(${target} PROPERTIES FOLDER Showcases)
endforeach()

# ---------------------------------------------------------------------------------------------
# Editor: PhisiksEditor (library) + Phisiks (the editor executable)
# ---------------------------------------------------------------------------------------------
file(GLOB PHISIKS_EDITOR_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/apps/editor/src/*.cpp ${PHISIKS_ROOT}/apps/editor/src/*.h)
list(FILTER PHISIKS_EDITOR_SOURCES EXCLUDE REGEX "/Main\\.cpp$")
add_library(PhisiksEditor STATIC ${PHISIKS_EDITOR_SOURCES})
target_include_directories(PhisiksEditor PUBLIC ${PHISIKS_ROOT}/apps/editor/src)
target_link_libraries(PhisiksEditor PUBLIC PhisiksCore)
target_compile_definitions(PhisiksEditor PRIVATE PHISIKS_VERSION_STRING="${PROJECT_VERSION}")
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_EDITOR_SOURCES})

add_executable(Phisiks ${PHISIKS_ROOT}/apps/editor/src/Main.cpp)
target_link_libraries(Phisiks PRIVATE PhisiksEditor)
phisiks_executable(Phisiks)

# ---------------------------------------------------------------------------------------------
# Samples (examples/samples): the engine sample scenes (library PhisiksSampleScenes) and the
# PhisiksSamples executable (the scenes inside the editor with a sample browser).
# ---------------------------------------------------------------------------------------------
file(GLOB PHISIKS_SAMPLE_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/examples/samples/src/*.cpp ${PHISIKS_ROOT}/examples/samples/src/*.h)
list(FILTER PHISIKS_SAMPLE_SOURCES EXCLUDE REGEX "/main\\.cpp$")
add_library(PhisiksSampleScenes STATIC ${PHISIKS_SAMPLE_SOURCES})
target_include_directories(PhisiksSampleScenes PUBLIC ${PHISIKS_ROOT}/examples/samples/src)
target_link_libraries(PhisiksSampleScenes PUBLIC PhisiksEditor)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_SAMPLE_SOURCES})
add_executable(PhisiksSamples ${PHISIKS_ROOT}/examples/samples/src/main.cpp)
target_link_libraries(PhisiksSamples PRIVATE PhisiksSampleScenes)
phisiks_executable(PhisiksSamples)
set_target_properties(PhisiksSamples PROPERTIES FOLDER Examples)

# ---------------------------------------------------------------------------------------------
# Example projects (projects/): separate applications built on the engine and the editor.
#   PhisiksProjectKit    shared building blocks + the CAD workspace framework (projects/common)
#   ProjectMarina        Project Marina engineering scene (projects/ProjectMarina)
#   OldMcDonald          Old-McDonald greenhouse mission + rover / drone CAD (projects/OldMcDonald)
# ---------------------------------------------------------------------------------------------
option(PHISIKS_BUILD_PROJECTS "Build the example projects in projects/ (and the runtime tests, which host them)" ON)
if(PHISIKS_BUILD_PROJECTS)
    file(GLOB PHISIKS_PROJECTKIT_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/projects/common/*.cpp ${PHISIKS_ROOT}/projects/common/*.h)
    add_library(PhisiksProjectKit STATIC ${PHISIKS_PROJECTKIT_SOURCES})
    target_include_directories(PhisiksProjectKit PUBLIC ${PHISIKS_ROOT}/projects/common)
    target_link_libraries(PhisiksProjectKit PUBLIC PhisiksEditor PhisiksShowcase)
    set_target_properties(PhisiksProjectKit PROPERTIES FOLDER Projects)
    foreach(project ProjectMarina OldMcDonald)
        file(GLOB sources CONFIGURE_DEPENDS ${PHISIKS_ROOT}/projects/${project}/src/*.cpp ${PHISIKS_ROOT}/projects/${project}/src/*.h)
        list(FILTER sources EXCLUDE REGEX "/Main\\.cpp$")
        add_library(${project}Scenes STATIC ${sources})
        target_include_directories(${project}Scenes PUBLIC ${PHISIKS_ROOT}/projects/${project}/src)
        target_link_libraries(${project}Scenes PUBLIC PhisiksProjectKit)
        source_group(TREE ${PHISIKS_ROOT} FILES ${sources})
        add_executable(${project} ${PHISIKS_ROOT}/projects/${project}/src/Main.cpp)
        target_link_libraries(${project} PRIVATE ${project}Scenes)
        phisiks_executable(${project})
        set_target_properties(${project} ${project}Scenes PROPERTIES FOLDER Projects)
    endforeach()

    # projectassetgen: regenerates projects/assets (deterministic; outputs are committed). The mesh
    # dimensions come from the projects' spec headers.
    add_executable(projectassetgen ${PHISIKS_ROOT}/projects/tools/projectassetgen.cpp
        ${PHISIKS_ROOT}/projects/tools/MarinaMeshes.cpp ${PHISIKS_ROOT}/projects/tools/OldMcDonaldMeshes.cpp)
    target_include_directories(projectassetgen PRIVATE ${PHISIKS_VENDOR}/glm ${PHISIKS_ROOT}/tools/assetgen
        ${PHISIKS_ROOT}/projects/ProjectMarina/src ${PHISIKS_ROOT}/projects/OldMcDonald/src)
    phisiks_output(projectassetgen)
    set_target_properties(projectassetgen PROPERTIES FOLDER Projects)

    # Runtime verification harness (tests/runtime): every scene above in the real application loop.
    file(GLOB PHISIKS_RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/tests/runtime/*.cpp ${PHISIKS_ROOT}/tests/runtime/*.h)
    add_executable(PhisiksRuntimeTests ${PHISIKS_RUNTIME_TEST_SOURCES})
    target_include_directories(PhisiksRuntimeTests PRIVATE ${PHISIKS_ROOT}/tests/runtime)
    target_link_libraries(PhisiksRuntimeTests PRIVATE PhisiksSampleScenes ProjectMarinaScenes OldMcDonaldScenes PhisiksShowcase)
    phisiks_executable(PhisiksRuntimeTests)
    source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_RUNTIME_TEST_SOURCES})
endif()

# ---------------------------------------------------------------------------------------------
# Layering + brand check (strict, runs before the engine builds): apps/, examples/, projects/ and
# tests/runtime use only the public engine API, and no project file references names of the code
# base Phisiks replaced.
# ---------------------------------------------------------------------------------------------
add_custom_target(check_layering ALL
    COMMAND ${CMAKE_COMMAND} -DPHISIKS_ROOT=${PHISIKS_ROOT} -P ${PHISIKS_ROOT}/tools/check_layering.cmake
    COMMENT "Checking engine/app layering"
    VERBATIM)
add_dependencies(PhisiksCore check_layering)

# ---------------------------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------------------------
# assetgen: regenerates the CC0 sample assets in assets/ (deterministic; outputs are committed).
add_executable(assetgen ${PHISIKS_ROOT}/tools/assetgen/assetgen.cpp ${PHISIKS_ROOT}/tools/assetgen/HeroMeshes.cpp
    ${PHISIKS_ROOT}/tools/assetgen/MeshKit.h)
target_include_directories(assetgen PRIVATE ${PHISIKS_VENDOR}/stb_image ${PHISIKS_VENDOR}/glm)
phisiks_output(assetgen)

# glprobe: checks whether the current GPU/driver meets the renderer's requirements.
add_executable(glprobe ${PHISIKS_ROOT}/tools/glprobe/glprobe.cpp ${PHISIKS_VENDOR}/glad/src/gl.c)
target_include_directories(glprobe PRIVATE ${PHISIKS_VENDOR}/glad/include)
target_link_libraries(glprobe PRIVATE phisiks_glfw)
phisiks_output(glprobe)

# stacksample: symbolized stack sampler for diagnosing hangs without a debugger (Windows).
if(WIN32)
    add_executable(stacksample ${PHISIKS_ROOT}/tools/stacksample/stacksample.cpp)
    target_link_libraries(stacksample PRIVATE dbghelp)
    phisiks_output(stacksample)
endif()

# ---------------------------------------------------------------------------------------------
# Tests (ctest --preset <preset>, or ctest -C Release --test-dir Build/vs2026)
# ---------------------------------------------------------------------------------------------
enable_testing()
file(GLOB PHISIKS_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/tests/unit/*.cpp ${PHISIKS_ROOT}/tests/unit/*.h)
add_executable(PhisiksUnitTests ${PHISIKS_UNIT_TEST_SOURCES})
# White-box access to engine-private headers (e.g. presentation backend interfaces for recording fakes).
target_include_directories(PhisiksUnitTests PRIVATE ${PHISIKS_ROOT}/engine/src)
phisiks_executable(PhisiksUnitTests)
target_link_libraries(PhisiksUnitTests PRIVATE PhisiksShowcase)   # demo kinematics / solvers (examples/)
target_include_directories(PhisiksUnitTests PRIVATE ${PHISIKS_ROOT}/examples)

add_test(NAME PhisiksUnitTests COMMAND PhisiksUnitTests WORKING_DIRECTORY ${PHISIKS_ROOT})
add_test(NAME Layering COMMAND ${CMAKE_COMMAND} -DPHISIKS_ROOT=${PHISIKS_ROOT} -P ${PHISIKS_ROOT}/tools/check_layering.cmake)
# Deterministic simulation: two headless runs must export byte-identical data; a hidden-window
# run must export PNG frames (skipped with -DSKIP_RENDER=ON where no display is available).
option(PHISIKS_TEST_SKIP_RENDER "Skip the rendered part of the DeterministicReplay test (no display)" OFF)
add_test(NAME DeterministicReplay
    COMMAND ${CMAKE_COMMAND} -DLAB=$<TARGET_FILE:PhisiksSamples> -DWORK=${CMAKE_BINARY_DIR}/replay_test
            -DSKIP_RENDER=${PHISIKS_TEST_SKIP_RENDER}
            -P ${PHISIKS_ROOT}/tools/test_deterministic_replay.cmake
    WORKING_DIRECTORY ${PHISIKS_ROOT})

# Showcases: each executable runs its own runtime verification in a hidden window (--quick: no
# frame-budget measurement, which depends on the GPU; run without --quick for the 60 FPS gate).
# They need an OpenGL context, so they are skipped with -DPHISIKS_TEST_SKIP_RENDER=ON (CI runners).
if(NOT PHISIKS_TEST_SKIP_RENDER)
foreach(entry ${PHISIKS_SHOWCASES})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 target)
    add_test(NAME ${target} COMMAND ${target} --autotest --quick --hidden --run-dir ${CMAKE_BINARY_DIR}/showcase_runs/${target}
             WORKING_DIRECTORY ${PHISIKS_ROOT})
endforeach()
# The runtime verification harness on both feature tiers (editor, samples, showcases, example
# projects; exit code = failed checks). It needs a window with an OpenGL context.
if(PHISIKS_BUILD_PROJECTS)
    add_test(NAME RuntimeTests COMMAND PhisiksRuntimeTests --autotest --hidden --run-dir ${CMAKE_BINARY_DIR}/runtime_tests/gl46 WORKING_DIRECTORY ${PHISIKS_ROOT})
    add_test(NAME RuntimeTestsGL41 COMMAND PhisiksRuntimeTests --autotest --gl41 --hidden --run-dir ${CMAKE_BINARY_DIR}/runtime_tests/gl41 WORKING_DIRECTORY ${PHISIKS_ROOT})
    set_tests_properties(RuntimeTests RuntimeTestsGL41 PROPERTIES TIMEOUT 1200 RUN_SERIAL TRUE)
endif()
endif()

# ---------------------------------------------------------------------------------------------
# Install + package: a self-contained, portable release folder (no build tools needed to run).
#   cmake --install Build/vs2026 --config Release --prefix <dir>
#   cpack --config Build/vs2026/CPackConfig.cmake -C Release      -> dist/Phisiks-<version>-win64[-physx].zip
# Layout (everything flat next to the executables; assets resolve relative to the executable):
#   Phisiks (editor), PhisiksSamples, Showcase01_RoboticArm .. Showcase04_TurbineCad,
#   ProjectMarina, OldMcDonald (projects/: their scene files, meshes and READMEs), glprobe
#   *.dll / runtime libraries (PhysX on Windows when that backend is built, the MSVC runtime)
#   assets/   meshes, textures, models, audio, post-pass shaders (assets/shaders), Lua (assets/scripts)
#   shaders/  the engine GLSL sources (embedded in the executables; used by --hot-reload)
#   LICENSE, THIRD_PARTY_NOTICES.md, README.md, QUICKSTART.txt
# ---------------------------------------------------------------------------------------------
set(PHISIKS_PACKAGE_TARGETS Phisiks PhisiksSamples glprobe)
if(PHISIKS_BUILD_PROJECTS)
    list(APPEND PHISIKS_PACKAGE_TARGETS ProjectMarina OldMcDonald)
endif()
foreach(entry ${PHISIKS_SHOWCASES})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 target)
    list(APPEND PHISIKS_PACKAGE_TARGETS ${target})
endforeach()
install(TARGETS ${PHISIKS_PACKAGE_TARGETS} RUNTIME DESTINATION .)
if(PHISIKS_RUNTIME_DLLS)
    install(FILES ${PHISIKS_RUNTIME_DLLS} DESTINATION .)                      # vendored PhysX DLLs
endif()
if(WIN32)
    # Imported shared libraries (e.g. a vcpkg PhysX), if any.
    install(FILES $<TARGET_RUNTIME_DLLS:Phisiks> DESTINATION . OPTIONAL)
    # The MSVC C/C++ runtime, app-local (no Visual C++ Redistributable install needed).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)                                      # UCRT ships with Windows 10+
    include(InstallRequiredSystemLibraries)
endif()
# Executables find shared libraries placed next to them (none are needed by default).
if(APPLE)
    set_target_properties(${PHISIKS_PACKAGE_TARGETS} PROPERTIES INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(${PHISIKS_PACKAGE_TARGETS} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(DIRECTORY ${PHISIKS_ROOT}/assets/ DESTINATION assets)
if(PHISIKS_BUILD_PROJECTS)   # example projects: scene files, meshes and READMEs (no sources)
    install(DIRECTORY ${PHISIKS_ROOT}/projects/ DESTINATION projects
            PATTERN "src" EXCLUDE PATTERN "tools" EXCLUDE PATTERN "common" EXCLUDE PATTERN "CMakeLists.txt" EXCLUDE)
endif()
install(DIRECTORY ${PHISIKS_ROOT}/engine/shaders/ DESTINATION shaders FILES_MATCHING PATTERN "*.glsl")
# Licence texts of the third-party code compiled into the programs (tools/fetch_deps.py collects them).
if(EXISTS ${PHISIKS_VENDOR}/licenses)
    install(DIRECTORY ${PHISIKS_VENDOR}/licenses/ DESTINATION THIRD_PARTY_LICENSES)
else()
    message(WARNING "vendor/licenses is missing: run 'python tools/fetch_deps.py' so release packages include the third-party licence texts")
endif()
install(FILES ${PHISIKS_ROOT}/LICENSE ${PHISIKS_ROOT}/THIRD_PARTY_NOTICES.md ${PHISIKS_ROOT}/README.md
              ${PHISIKS_ROOT}/packaging/QUICKSTART.txt DESTINATION .)

if(WIN32 AND PHISIKS_PHYSICS_BACKEND STREQUAL "PhysX")
    set(PHISIKS_PACKAGE_SYSTEM win64-physx)   # distinct name: the CI / portable archive is native physics
    set(PHISIKS_PACKAGE_GENERATOR ZIP)
elseif(WIN32)
    set(PHISIKS_PACKAGE_SYSTEM win64)
    set(PHISIKS_PACKAGE_GENERATOR ZIP)
elseif(APPLE)
    set(PHISIKS_PACKAGE_SYSTEM macos-${CMAKE_SYSTEM_PROCESSOR})
    set(PHISIKS_PACKAGE_GENERATOR ZIP)
else()
    set(PHISIKS_PACKAGE_SYSTEM linux-${CMAKE_SYSTEM_PROCESSOR})
    set(PHISIKS_PACKAGE_GENERATOR TGZ)
endif()
set(CPACK_PACKAGE_NAME Phisiks)
set(CPACK_PACKAGE_VENDOR "The Phisiks authors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PHISIKS: a general-purpose 3D engine and editor (C++20, OpenGL)")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${PHISIKS_ROOT}/LICENSE)
set(CPACK_GENERATOR ${PHISIKS_PACKAGE_GENERATOR})
set(CPACK_PACKAGE_FILE_NAME "Phisiks-${PROJECT_VERSION}-${PHISIKS_PACKAGE_SYSTEM}")
set(CPACK_PACKAGE_DIRECTORY ${PHISIKS_ROOT}/dist)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)                                       # one Phisiks-1.0.0-<os>/ folder
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
set(CPACK_STRIP_FILES ON)
include(CPack)
