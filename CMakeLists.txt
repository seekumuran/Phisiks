# Phisiks: CMake build (Windows / Linux / macOS).
#
# Targets
#   PhisiksCore        static library: the engine (engine/include public API, engine/src private)
#   PhisiksImGui       static library: Dear ImGui + GLFW/OpenGL3 backends + ImGuizmo (vendor/)
#   PhisiksLua         static library: Lua 5.4 (vendor/lua, C)
#   PhisiksLab         interactive studio app (apps/lab) + runtime verification harness (tests/runtime)
#   PhisiksUnitTests   headless unit tests (tests/unit)
#   assetgen           regenerates the CC0 sample assets in assets/
#   glprobe, stacksample (Windows), check_layering   tools
#
# Backends (cache options)
#   PHISIKS_PHYSICS_BACKEND  PhysX | Native   PhysX: Windows default (vendored MSVC binaries), or any
#                                             platform with the vcpkg "physx" port; Native: the
#                                             built-in solver (macOS and Linux default)
#   PHISIKS_AUDIO_BACKEND    WASAPI | miniaudio
#   PHISIKS_OUTPUT_DIR       executables go to <dir>/<config> (default Build/bin)
# Dependencies: vendor/ (python tools/fetch_deps.py downloads the portable ones; see docs/BUILD.md).

cmake_minimum_required(VERSION 3.25)
project(Phisiks LANGUAGES C CXX)

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
    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
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
# Showcases (examples/): PhisiksShowcase = the shared framework (examples/common) + the six demo
# scenes; one executable per demo (examples/NN_name/main.cpp). PhisiksLab links the library too
# (its SHOWCASE playlist launches the same scenes).
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
    Showcase01_StuntCar:01_game_stunt_car
    Showcase02_FluidHarbor:02_vfx_fluid_harbor
    Showcase03_Robotics:03_robotics_6dof_terrain
    Showcase04_Villa:04_cad_modern_house
    Showcase05_Vision:05_ai_synthetic_vision
    Showcase06_BlackHole:06_science_black_hole)
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
# PhisiksLab
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE PHISIKS_LAB_SOURCES CONFIGURE_DEPENDS
    ${PHISIKS_ROOT}/apps/lab/src/*.cpp ${PHISIKS_ROOT}/apps/lab/src/*.h
    ${PHISIKS_ROOT}/tests/runtime/*.cpp ${PHISIKS_ROOT}/tests/runtime/*.h)
add_executable(PhisiksLab ${PHISIKS_LAB_SOURCES})
target_include_directories(PhisiksLab PRIVATE ${PHISIKS_ROOT}/apps/lab/src ${PHISIKS_ROOT}/tests/runtime)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_LAB_SOURCES})
phisiks_executable(PhisiksLab)
target_link_libraries(PhisiksLab PRIVATE PhisiksShowcase)

# ---------------------------------------------------------------------------------------------
# Layering + brand check (strict, runs before the engine builds): apps/ and tests/runtime use only
# the public engine API, and no project file references names of the code base Phisiks replaced.
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
add_executable(assetgen ${PHISIKS_ROOT}/tools/assetgen/assetgen.cpp)
target_include_directories(assetgen PRIVATE ${PHISIKS_VENDOR}/stb_image)
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
target_link_libraries(PhisiksUnitTests PRIVATE PhisiksShowcase)   # Vehicle (examples/common)

add_test(NAME PhisiksUnitTests COMMAND PhisiksUnitTests WORKING_DIRECTORY ${PHISIKS_ROOT})
add_test(NAME Layering COMMAND ${CMAKE_COMMAND} -DPHISIKS_ROOT=${PHISIKS_ROOT} -P ${PHISIKS_ROOT}/tools/check_layering.cmake)
# Deterministic simulation: two headless runs must export byte-identical data; a hidden-window
# run must export PNG frames (skipped with -DSKIP_RENDER=ON where no display is available).
option(PHISIKS_TEST_SKIP_RENDER "Skip the rendered part of the DeterministicReplay test (no display)" OFF)
add_test(NAME DeterministicReplay
    COMMAND ${CMAKE_COMMAND} -DLAB=$<TARGET_FILE:PhisiksLab> -DWORK=${CMAKE_BINARY_DIR}/replay_test
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
# Synthetic-vision batch export of the LiDAR proving ground (--headless --export-vision).
add_test(NAME Showcase05_VisionExport
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:Showcase05_Vision> -DWORK=${CMAKE_BINARY_DIR}/vision_export
            -P ${PHISIKS_ROOT}/tools/test_vision_export.cmake
    WORKING_DIRECTORY ${PHISIKS_ROOT})
endif()
