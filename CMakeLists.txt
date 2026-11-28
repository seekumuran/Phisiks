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
#   PhisiksRuntimeTests  runtime verification harness (tests/runtime): the editor, samples and showcases
#   PhisiksUnitTests   headless unit tests (tests/unit)
#   assetgen           regenerates the CC0 engine / example assets in assets/
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

# ---------------------------------------------------------------------------------------------
# Build provenance. Every package carries BUILD_INFO.txt (version, commit, toolchain, backends,
# CI run). A pre-release toolchain (Visual Studio Preview / Insiders, a CMake release candidate)
# must not produce release packages: the install step fails unless PHISIKS_ALLOW_PREVIEW_TOOLCHAIN
# is ON, and such a package is then named "...-preview-toolchain" so it can't pass for a release.
# ---------------------------------------------------------------------------------------------
set(PHISIKS_PREVIEW_TOOLCHAIN_REASONS "")
if(CMAKE_VERSION MATCHES "-rc|-g[0-9a-f]+|-dirty")   # release candidates and development builds
    list(APPEND PHISIKS_PREVIEW_TOOLCHAIN_REASONS "CMake ${CMAKE_VERSION} is not a release")
endif()
if(MSVC)
    foreach(path "${CMAKE_GENERATOR_INSTANCE}" "${CMAKE_CXX_COMPILER}")
        if(path MATCHES "[Ii]nsiders|[Pp]review")
            list(APPEND PHISIKS_PREVIEW_TOOLCHAIN_REASONS "Visual Studio pre-release (${path})")
            break()
        endif()
    endforeach()
endif()
option(PHISIKS_ALLOW_PREVIEW_TOOLCHAIN "Allow packaging with a pre-release toolchain (local testing only; never publish)" OFF)
set(PHISIKS_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${PHISIKS_ROOT}
                    OUTPUT_VARIABLE PHISIKS_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no WORKING_DIRECTORY ${PHISIKS_ROOT}
                    OUTPUT_VARIABLE PHISIKS_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(PHISIKS_GIT_DIRTY)
        string(APPEND PHISIKS_GIT_COMMIT " (uncommitted changes at configure time)")
    endif()
endif()
if(DEFINED ENV{GITHUB_RUN_ID})
    set(PHISIKS_BUILD_ORIGIN "GitHub Actions: $ENV{GITHUB_SERVER_URL}/$ENV{GITHUB_REPOSITORY}/actions/runs/$ENV{GITHUB_RUN_ID} (workflow $ENV{GITHUB_WORKFLOW}, ref $ENV{GITHUB_REF_NAME})")
else()
    set(PHISIKS_BUILD_ORIGIN "local build (not CI)")
endif()
if(PHISIKS_PREVIEW_TOOLCHAIN_REASONS)
    string(REPLACE ";" "; " PHISIKS_PREVIEW_TEXT "${PHISIKS_PREVIEW_TOOLCHAIN_REASONS}")
    set(PHISIKS_TOOLCHAIN_STATUS "PRE-RELEASE: ${PHISIKS_PREVIEW_TEXT}. NOT FOR PUBLIC RELEASE.")
    message(WARNING "Phisiks: pre-release toolchain (${PHISIKS_PREVIEW_TEXT}). Packages from this build are blocked unless PHISIKS_ALLOW_PREVIEW_TOOLCHAIN=ON, and must never be published.")
else()
    set(PHISIKS_TOOLCHAIN_STATUS "released")
endif()

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
set(PHISIKS_SDK_DEPENDENCIES "")          # find_dependency() lines for the installed package config
set(PHISIKS_SDK_EXTRA_LIBS "")            # vendored static libraries copied into the SDK's lib/
option(PHISIKS_BUNDLED_GLFW "Build GLFW from source (static) instead of using a system or vendored GLFW" OFF)
if(WIN32 AND EXISTS ${PHISIKS_VENDOR}/GLFW/lib AND NOT PHISIKS_BUNDLED_GLFW)
    set(PHISIKS_GLFW_VENDORED ON)
    target_include_directories(phisiks_glfw INTERFACE $<BUILD_INTERFACE:${PHISIKS_VENDOR}/GLFW/include>)
    target_link_libraries(phisiks_glfw INTERFACE
        $<BUILD_INTERFACE:${PHISIKS_VENDOR}/GLFW/lib/${PHISIKS_LIBCFG}/glfw3.lib>
        $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/lib/glfw3.lib>
        opengl32 user32 gdi32 shell32)
    list(APPEND PHISIKS_SDK_EXTRA_LIBS ${PHISIKS_VENDOR}/GLFW/lib/release/glfw3.lib)
else()
    if(NOT PHISIKS_BUNDLED_GLFW)
        find_package(glfw3 3.3 QUIET)
    endif()
    if(NOT PHISIKS_BUNDLED_GLFW AND glfw3_FOUND)
        string(APPEND PHISIKS_SDK_DEPENDENCIES "find_dependency(glfw3 3.3)\n")
    endif()
    if(PHISIKS_BUNDLED_GLFW OR NOT glfw3_FOUND)
        set(PHISIKS_GLFW_FETCHED ON)
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
    string(APPEND PHISIKS_SDK_DEPENDENCIES "find_dependency(OpenGL)\n")
    target_link_libraries(phisiks_glfw INTERFACE glfw OpenGL::GL)
endif()

# PhysX
set(PHISIKS_RUNTIME_DLLS)
if(PHISIKS_PHYSICS_BACKEND STREQUAL "PhysX")
    add_library(phisiks_physx INTERFACE)
    if(WIN32 AND EXISTS ${PHISIKS_VENDOR}/physx/lib)
        target_include_directories(phisiks_physx INTERFACE $<BUILD_INTERFACE:${PHISIKS_VENDOR}/physx/include> $<BUILD_INTERFACE:${PHISIKS_VENDOR}/physx/include/physx>)
        foreach(lib PhysX_64 PhysXCommon_64 PhysXCooking_64 PhysXFoundation_64 PhysXExtensions_static_64 PhysXPvdSDK_static_64 PhysXCharacterKinematic_static_64)
            target_link_libraries(phisiks_physx INTERFACE
                $<BUILD_INTERFACE:${PHISIKS_VENDOR}/physx/lib/${PHISIKS_LIBCFG}/${lib}.lib>
                $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/lib/${lib}.lib>)
            list(APPEND PHISIKS_SDK_EXTRA_LIBS ${PHISIKS_VENDOR}/physx/lib/release/${lib}.lib)
        endforeach()
        file(GLOB PHISIKS_RUNTIME_DLLS ${PHISIKS_VENDOR}/DLLs/*.dll)
    else()
        # vcpkg: `vcpkg install physx`, configure with CMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
        find_package(unofficial-omniverse-physx-sdk CONFIG REQUIRED)
        string(APPEND PHISIKS_SDK_DEPENDENCIES "find_dependency(unofficial-omniverse-physx-sdk CONFIG)\n")
        target_link_libraries(phisiks_physx INTERFACE unofficial::omniverse-physx-sdk::sdk)
    endif()
endif()

# Dear ImGui + backends + ImGuizmo (warnings off: third-party code).
add_library(PhisiksImGui STATIC
    ${PHISIKS_VENDOR}/imgui/imgui.cpp ${PHISIKS_VENDOR}/imgui/imgui_draw.cpp
    ${PHISIKS_VENDOR}/imgui/imgui_tables.cpp ${PHISIKS_VENDOR}/imgui/imgui_widgets.cpp
    ${PHISIKS_VENDOR}/imgui/backends/imgui_impl_glfw.cpp ${PHISIKS_VENDOR}/imgui/backends/imgui_impl_opengl3.cpp
    ${PHISIKS_VENDOR}/imguizmo/ImGuizmo.cpp)
target_include_directories(PhisiksImGui PUBLIC $<BUILD_INTERFACE:${PHISIKS_VENDOR}/imgui> $<BUILD_INTERFACE:${PHISIKS_VENDOR}/imguizmo> $<INSTALL_INTERFACE:include/imgui>)
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
target_include_directories(PhisiksLua PRIVATE ${PHISIKS_VENDOR}/lua/src)   # engine-private (Lua headers are not part of the SDK)
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
list(FILTER PHISIKS_ENGINE_SOURCES EXCLUDE REGEX "/engine/src/cad/|/include/Phisiks/Cad/")   # PhisiksCad (below)
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

# Public version header, generated from project(VERSION): Phisiks/Version.h.
configure_file(${PHISIKS_ROOT}/engine/include/Phisiks/Version.h.in ${PHISIKS_GENERATED_DIR}/include/Phisiks/Version.h @ONLY)

add_library(PhisiksCore STATIC ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS} ${PHISIKS_EMBEDDED_SHADERS} ${PHISIKS_VENDOR}/glad/src/gl.c)
add_library(Phisiks::Core ALIAS PhisiksCore)
source_group(TREE ${PHISIKS_ROOT}/engine PREFIX "engine" FILES ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS})

# Public API: engine headers + glm and EnTT (public headers/templates); Dear ImGui comes with PhisiksImGui.
target_include_directories(PhisiksCore PUBLIC
    $<BUILD_INTERFACE:${PHISIKS_ROOT}/engine/include>
    $<BUILD_INTERFACE:${PHISIKS_GENERATED_DIR}/include>
    $<BUILD_INTERFACE:${PHISIKS_VENDOR}/glm>
    $<BUILD_INTERFACE:${PHISIKS_VENDOR}/entt>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/entt>)
# Private: implementation headers and backends. Apps and tests cannot include these.
target_include_directories(PhisiksCore PRIVATE
    ${PHISIKS_ROOT}/engine/src
    ${PHISIKS_GENERATED_DIR}
    ${PHISIKS_VENDOR}/glad/include
    ${PHISIKS_VENDOR}/stb_image
    ${PHISIKS_VENDOR}/nlohmann
    ${PHISIKS_VENDOR}/miniaudio
    ${PHISIKS_VENDOR}/lua/src
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
    target_compile_options(PhisiksCore PUBLIC /utf-8 /bigobj /Zc:__cplusplus)
    target_compile_options(PhisiksCore PRIVATE /MP /W3)
    target_link_libraries(PhisiksCore PUBLIC dbghelp ole32 psapi)
else()
    target_compile_options(PhisiksCore PRIVATE -Wall)
    if(UNIX AND NOT APPLE)
        target_link_libraries(PhisiksCore PUBLIC m)
    endif()
    if(APPLE)
        target_link_libraries(PhisiksCore PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
        target_compile_definitions(PhisiksCore PUBLIC GL_SILENCE_DEPRECATION)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# PhisiksCad: sketches, constraint solver, parametric documents and the geometry kernel interface
# (engine/include/Phisiks/Cad). A shared library so Open CASCADE (LGPL-2.1 + exception) stays
# private to it and is linked dynamically. Independent of PhisiksCore (glm only).
#   PHISIKS_CAD_KERNEL=OCCT (default when found) | None (sketches / documents only, no solids)
#   PHISIKS_OCCT_DIR: an OCCT install (tools/build_occt.py installs into vendor/occt)
# ---------------------------------------------------------------------------------------------
set(PHISIKS_OCCT_DIR ${PHISIKS_VENDOR}/occt CACHE PATH "Open CASCADE install prefix (tools/build_occt.py)")
set(PHISIKS_CAD_KERNEL "Auto" CACHE STRING "CAD geometry kernel: Auto, OCCT or None")
set_property(CACHE PHISIKS_CAD_KERNEL PROPERTY STRINGS Auto OCCT None)
set(PHISIKS_OCCT_TOOLKITS TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool
    TKFillet TKOffset TKShHealing TKMesh TKXSBase TKDE TKDESTEP TKDEIGES TKDESTL)
set(PHISIKS_CAD_HAS_OCCT OFF)
if(NOT PHISIKS_CAD_KERNEL STREQUAL "None")
    find_package(OpenCASCADE CONFIG QUIET PATHS ${PHISIKS_OCCT_DIR} ${PHISIKS_OCCT_DIR}/lib/cmake/opencascade NO_DEFAULT_PATH)
    if(NOT OpenCASCADE_FOUND)
        find_package(OpenCASCADE CONFIG QUIET NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)
    endif()
    if(OpenCASCADE_FOUND AND OpenCASCADE_VERSION VERSION_GREATER_EQUAL 7.8)
        set(PHISIKS_CAD_HAS_OCCT ON)
    elseif(PHISIKS_CAD_KERNEL STREQUAL "OCCT")
        message(FATAL_ERROR "PHISIKS_CAD_KERNEL=OCCT but Open CASCADE >= 7.8 was not found (run: python tools/build_occt.py)")
    endif()
endif()
set(PHISIKS_CAD_KERNEL_NAME None)
if(PHISIKS_CAD_HAS_OCCT)
    set(PHISIKS_CAD_KERNEL_NAME OCCT)
    message(STATUS "Phisiks: CAD kernel Open CASCADE ${OpenCASCADE_VERSION} (${OpenCASCADE_DIR})")
else()
    message(WARNING "Phisiks: no CAD geometry kernel (Open CASCADE not found): CAD sketches and documents work, solids do not. Run tools/build_occt.py")
endif()

file(GLOB PHISIKS_CAD_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/engine/src/cad/*.cpp ${PHISIKS_ROOT}/engine/src/cad/*.h ${PHISIKS_ROOT}/engine/include/Phisiks/Cad/*.h)
add_library(PhisiksCad SHARED ${PHISIKS_CAD_SOURCES})
add_library(Phisiks::Cad ALIAS PhisiksCad)
source_group(TREE ${PHISIKS_ROOT}/engine PREFIX "engine" FILES ${PHISIKS_CAD_SOURCES})
target_compile_features(PhisiksCad PUBLIC cxx_std_20)
target_compile_definitions(PhisiksCad PRIVATE PHISIKS_CAD_BUILD)
target_include_directories(PhisiksCad PUBLIC
    $<BUILD_INTERFACE:${PHISIKS_ROOT}/engine/include>
    $<BUILD_INTERFACE:${PHISIKS_VENDOR}/glm>
    $<INSTALL_INTERFACE:include>)
target_include_directories(PhisiksCad PRIVATE ${PHISIKS_ROOT}/engine/src/cad ${PHISIKS_VENDOR}/nlohmann)
set_target_properties(PhisiksCad PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
set(PHISIKS_OCCT_RUNTIME "")              # shared libraries to ship next to the programs
if(PHISIKS_CAD_HAS_OCCT)
    target_compile_definitions(PhisiksCad PRIVATE PHISIKS_CAD_HAS_OCCT=1)
    target_include_directories(PhisiksCad SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(PhisiksCad PRIVATE ${PHISIKS_OCCT_TOOLKITS})
    foreach(tk ${OpenCASCADE_LIBRARIES})
        if(TARGET ${tk})
            get_target_property(loc ${tk} IMPORTED_LOCATION_RELEASE)
            if(NOT loc)
                get_target_property(loc ${tk} IMPORTED_LOCATION)
            endif()
            if(loc AND EXISTS ${loc})
                list(APPEND PHISIKS_OCCT_RUNTIME ${loc})
            endif()
        endif()
    endforeach()
    if(WIN32)
        list(APPEND PHISIKS_RUNTIME_DLLS ${PHISIKS_OCCT_RUNTIME})
    else()
        # Linux / macOS: the OCCT libraries (by their soname) go next to PhisiksCad, which finds
        # them through its $ORIGIN / @loader_path rpath; release packages ship them the same way.
        set(PHISIKS_OCCT_SONAMES "")
        foreach(tk ${OpenCASCADE_LIBRARIES})
            if(TARGET ${tk})
                list(APPEND PHISIKS_OCCT_SONAMES $<TARGET_SONAME_FILE:${tk}>)
            endif()
        endforeach()
        add_custom_command(TARGET PhisiksCad POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PHISIKS_OCCT_SONAMES} $<TARGET_FILE_DIR:PhisiksCad>
            COMMENT "Copying Open CASCADE libraries next to PhisiksCad" VERBATIM)
    endif()
endif()
if(MSVC)
    target_compile_options(PhisiksCad PRIVATE /MP /W3 /utf-8 /bigobj /Zc:__cplusplus)
    target_compile_definitions(PhisiksCad PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(PhisiksCad PRIVATE -Wall)
endif()

function(phisiks_output target)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(cfg Debug Release RelWithDebInfo)
            string(TOUPPER ${cfg} CFG)
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${PHISIKS_OUTPUT_DIR}/${cfg})
        endforeach()
    else()
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PHISIKS_OUTPUT_DIR}/${CMAKE_BUILD_TYPE}
                                                   LIBRARY_OUTPUT_DIRECTORY ${PHISIKS_OUTPUT_DIR}/${CMAKE_BUILD_TYPE})
    endif()
    if(APPLE)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()
endfunction()
phisiks_output(PhisiksCad)

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
add_library(Phisiks::Editor ALIAS PhisiksEditor)
target_include_directories(PhisiksEditor PUBLIC $<BUILD_INTERFACE:${PHISIKS_ROOT}/apps/editor/src> $<INSTALL_INTERFACE:include/PhisiksEditor>)
target_link_libraries(PhisiksEditor PUBLIC PhisiksCore PhisiksCad)   # CAD parts (EditorCad.cpp)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_EDITOR_SOURCES})

# PAI, the PHISIKS Agent Interface (apps/pai): JSON-RPC over stdio for external tools; nlohmann/json
# (header-only) is private to its sources.
file(GLOB PHISIKS_PAI_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/apps/pai/src/*.cpp ${PHISIKS_ROOT}/apps/pai/src/*.h)
add_library(PhisiksPAI STATIC ${PHISIKS_PAI_SOURCES})
add_library(Phisiks::PAI ALIAS PhisiksPAI)
target_include_directories(PhisiksPAI PUBLIC $<BUILD_INTERFACE:${PHISIKS_ROOT}/apps/pai/src> $<INSTALL_INTERFACE:include/PhisiksPAI>)
target_include_directories(PhisiksPAI PRIVATE ${PHISIKS_VENDOR}/nlohmann)
target_link_libraries(PhisiksPAI PUBLIC PhisiksEditor)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_PAI_SOURCES})

add_executable(Phisiks ${PHISIKS_ROOT}/apps/editor/src/Main.cpp)
target_link_libraries(Phisiks PRIVATE PhisiksEditor PhisiksPAI)
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
# Runtime verification harness (tests/runtime): the editor, the samples and the showcases in the
# real application loop.
# ---------------------------------------------------------------------------------------------
file(GLOB PHISIKS_RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/tests/runtime/*.cpp ${PHISIKS_ROOT}/tests/runtime/*.h)
add_executable(PhisiksRuntimeTests ${PHISIKS_RUNTIME_TEST_SOURCES})
target_include_directories(PhisiksRuntimeTests PRIVATE ${PHISIKS_ROOT}/tests/runtime)
target_link_libraries(PhisiksRuntimeTests PRIVATE PhisiksSampleScenes PhisiksShowcase)
phisiks_executable(PhisiksRuntimeTests)
source_group(TREE ${PHISIKS_ROOT} FILES ${PHISIKS_RUNTIME_TEST_SOURCES})

# ---------------------------------------------------------------------------------------------
# Layering + brand check (strict, runs before the engine builds): apps/, examples/ and
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
target_link_libraries(PhisiksUnitTests PRIVATE PhisiksShowcase PhisiksCad)   # demo kinematics / solvers (examples/), CAD
target_include_directories(PhisiksUnitTests PRIVATE ${PHISIKS_ROOT}/examples)
# Expected version for the Version.h test, straight from project(VERSION).
target_compile_definitions(PhisiksUnitTests PRIVATE PHISIKS_EXPECTED_MAJOR=${PROJECT_VERSION_MAJOR} PHISIKS_EXPECTED_MINOR=${PROJECT_VERSION_MINOR}
                           PHISIKS_EXPECTED_PATCH=${PROJECT_VERSION_PATCH} PHISIKS_EXPECTED_STRING="${PROJECT_VERSION}")

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
# The runtime verification harness on both feature tiers (editor, samples, showcases; exit code =
# failed checks). It needs a window with an OpenGL context.
add_test(NAME RuntimeTests COMMAND PhisiksRuntimeTests --autotest --hidden --run-dir ${CMAKE_BINARY_DIR}/runtime_tests/gl46 WORKING_DIRECTORY ${PHISIKS_ROOT})
add_test(NAME RuntimeTestsGL41 COMMAND PhisiksRuntimeTests --autotest --gl41 --hidden --run-dir ${CMAKE_BINARY_DIR}/runtime_tests/gl41 WORKING_DIRECTORY ${PHISIKS_ROOT})
set_tests_properties(RuntimeTests RuntimeTestsGL41 PROPERTIES TIMEOUT 1200 RUN_SERIAL TRUE)
# PAI: the example client drives the editor over stdio (protocol, permissions, unsafe requests,
# undo, edit / play). Needs Python 3 and a window with an OpenGL context.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME PaiProtocol COMMAND ${Python3_EXECUTABLE} ${PHISIKS_ROOT}/examples/pai_client/pai_client.py --check --exe $<TARGET_FILE:Phisiks>
             WORKING_DIRECTORY ${PHISIKS_ROOT})
    set_tests_properties(PaiProtocol PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endif()
endif()

# ---------------------------------------------------------------------------------------------
# Install + package: a self-contained, portable release folder (no build tools needed to run).
#   cmake --install Build/vs2026 --config Release --prefix <dir>
#   cpack --config Build/vs2026/CPackConfig.cmake -C Release      -> dist/Phisiks-<version>-win64[-physx].zip
# Layout (everything flat next to the executables; assets resolve relative to the executable):
#   Phisiks (editor), PhisiksSamples, Showcase01_RoboticArm .. Showcase04_TurbineCad,
#   glprobe
#   *.dll / runtime libraries (PhysX on Windows when that backend is built, the MSVC runtime)
#   assets/   meshes, textures, models, audio, post-pass shaders (assets/shaders), Lua (assets/scripts)
#   shaders/  the engine GLSL sources (embedded in the executables; used by --hot-reload)
#   THIRD_PARTY_LICENSES/ (vendor/licenses), LICENSE, THIRD_PARTY_NOTICES.md, README.md, KNOWN_ISSUES.md, QUICKSTART.txt
# ---------------------------------------------------------------------------------------------
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME runtime)   # the application package; the SDK is component "sdk"
set(PHISIKS_PACKAGE_TARGETS Phisiks PhisiksSamples glprobe)
foreach(entry ${PHISIKS_SHOWCASES})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 target)
    list(APPEND PHISIKS_PACKAGE_TARGETS ${target})
endforeach()
install(TARGETS ${PHISIKS_PACKAGE_TARGETS} RUNTIME DESTINATION .)
# PhisiksCad (shared) and the Open CASCADE libraries it uses, next to the programs.
install(TARGETS PhisiksCad RUNTIME DESTINATION . LIBRARY DESTINATION .)
if(PHISIKS_CAD_HAS_OCCT)
    if(WIN32)
        install(FILES ${PHISIKS_OCCT_RUNTIME} DESTINATION .)
    else()
        install(FILES ${PHISIKS_OCCT_SONAMES} DESTINATION .)
    endif()
    install(FILES ${PHISIKS_OCCT_DIR}/LICENSE_LGPL_21.txt ${PHISIKS_OCCT_DIR}/OCCT_LGPL_EXCEPTION.txt DESTINATION THIRD_PARTY_LICENSES/OpenCASCADE OPTIONAL)
endif()
if(PHISIKS_RUNTIME_DLLS)
    install(FILES ${PHISIKS_RUNTIME_DLLS} DESTINATION .)                      # vendored PhysX DLLs
endif()
if(WIN32)
    # Imported shared libraries (e.g. a vcpkg PhysX), if any.
    install(FILES $<TARGET_RUNTIME_DLLS:Phisiks> DESTINATION . OPTIONAL)
    # The MSVC C/C++ runtime, app-local (no Visual C++ Redistributable install needed).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)                                      # UCRT ships with Windows 10+
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
    include(InstallRequiredSystemLibraries)
endif()
# Executables find shared libraries placed next to them (none are needed by default).
if(APPLE)
    set_target_properties(${PHISIKS_PACKAGE_TARGETS} PROPERTIES INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(${PHISIKS_PACKAGE_TARGETS} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(DIRECTORY ${PHISIKS_ROOT}/assets/ DESTINATION assets)
install(DIRECTORY ${PHISIKS_ROOT}/engine/shaders/ DESTINATION shaders FILES_MATCHING PATTERN "*.glsl")
# Licence texts of the third-party code compiled into the programs (tools/fetch_deps.py collects them).
if(EXISTS ${PHISIKS_VENDOR}/licenses)
    install(DIRECTORY ${PHISIKS_VENDOR}/licenses/ DESTINATION THIRD_PARTY_LICENSES)
else()
    message(WARNING "vendor/licenses is missing: run 'python tools/fetch_deps.py' so release packages include the third-party licence texts")
endif()
# Provenance file (see "Build provenance" above), written at configure time.
set(PHISIKS_BUILD_INFO ${CMAKE_BINARY_DIR}/BUILD_INFO.txt)
file(WRITE ${PHISIKS_BUILD_INFO}
"PHISIKS ${PROJECT_VERSION}
commit:        ${PHISIKS_GIT_COMMIT}
built by:      ${PHISIKS_BUILD_ORIGIN}
system:        ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}
compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}
generator:     ${CMAKE_GENERATOR}
cmake:         ${CMAKE_VERSION}
toolchain:     ${PHISIKS_TOOLCHAIN_STATUS}
physics:       ${PHISIKS_PHYSICS_BACKEND}
audio:         ${PHISIKS_AUDIO_BACKEND}
")
if(PHISIKS_PREVIEW_TOOLCHAIN_REASONS AND NOT PHISIKS_ALLOW_PREVIEW_TOOLCHAIN)
    install(CODE "message(FATAL_ERROR \"Phisiks: packaging is blocked: pre-release toolchain (${PHISIKS_PREVIEW_TEXT}). Build release packages with a released Visual Studio / CMake, or configure with -DPHISIKS_ALLOW_PREVIEW_TOOLCHAIN=ON for a local, unpublishable test package.\")")
endif()
install(FILES ${PHISIKS_BUILD_INFO} DESTINATION .)
install(FILES ${PHISIKS_ROOT}/LICENSE ${PHISIKS_ROOT}/THIRD_PARTY_NOTICES.md ${PHISIKS_ROOT}/README.md ${PHISIKS_ROOT}/KNOWN_ISSUES.md
              ${PHISIKS_ROOT}/packaging/QUICKSTART.txt DESTINATION .)


# ---------------------------------------------------------------------------------------------
# SDK (install component "sdk", archive "...-sdk"): headers, static libraries and a CMake package.
#   find_package(PHISIKS 1.1 CONFIG REQUIRED)        # or find_package(Phisiks ...)
#   target_link_libraries(MyApp PRIVATE Phisiks::Core)      # engine
#   target_link_libraries(MyApp PRIVATE Phisiks::Editor)    # + the editor (Editor.h, EditorApp.h)
# Layout: include/ (Phisiks/, PhisiksEditor/, glm/, entt/, imgui/), lib/ (static libraries, plus the
# vendored GLFW / PhysX import libraries on Windows), bin/ (PhysX DLLs), lib/cmake/phisiks/,
# share/phisiks/ (licences, SDK guide). Release configuration (MSVC: /MD runtime).
# ---------------------------------------------------------------------------------------------
option(PHISIKS_INSTALL_SDK "Install the SDK (headers, libraries, CMake package) as component 'sdk'" ON)
if(PHISIKS_INSTALL_SDK)
    include(CMakePackageConfigHelpers)
    set_target_properties(PhisiksCore PROPERTIES EXPORT_NAME Core)
    set_target_properties(PhisiksEditor PROPERTIES EXPORT_NAME Editor)
    set_target_properties(PhisiksPAI PROPERTIES EXPORT_NAME PAI)
    set_target_properties(PhisiksImGui PROPERTIES EXPORT_NAME ImGui)
    set_target_properties(PhisiksLua PROPERTIES EXPORT_NAME Lua)
    set_target_properties(phisiks_glfw PROPERTIES EXPORT_NAME GlfwDependency)
    set_target_properties(PhisiksCad PROPERTIES EXPORT_NAME Cad)
    set(PHISIKS_SDK_TARGETS PhisiksCore PhisiksCad PhisiksEditor PhisiksPAI PhisiksImGui PhisiksLua phisiks_glfw)
    if(PHISIKS_CAD_HAS_OCCT)   # the kernel's shared libraries, beside PhisiksCad (bin/ on Windows, lib/ elsewhere)
        if(WIN32)
            install(FILES ${PHISIKS_OCCT_RUNTIME} DESTINATION bin COMPONENT sdk)
        else()
            install(FILES ${PHISIKS_OCCT_SONAMES} DESTINATION lib COMPONENT sdk)
        endif()
        install(FILES ${PHISIKS_OCCT_DIR}/LICENSE_LGPL_21.txt ${PHISIKS_OCCT_DIR}/OCCT_LGPL_EXCEPTION.txt DESTINATION share/phisiks/THIRD_PARTY_LICENSES/OpenCASCADE COMPONENT sdk OPTIONAL)
    endif()
    if(TARGET phisiks_physx)
        set_target_properties(phisiks_physx PROPERTIES EXPORT_NAME PhysXDependency)
        list(APPEND PHISIKS_SDK_TARGETS phisiks_physx)
    endif()
    if(PHISIKS_GLFW_FETCHED)
        list(APPEND PHISIKS_SDK_TARGETS glfw)   # the static GLFW built from source
    endif()
    if(PHISIKS_PREVIEW_TOOLCHAIN_REASONS AND NOT PHISIKS_ALLOW_PREVIEW_TOOLCHAIN)
        install(CODE "if(NOT DEFINED ENV{PHISIKS_LOCAL_INSTALL})\n message(FATAL_ERROR \"Phisiks: SDK packaging is blocked: pre-release toolchain (${PHISIKS_PREVIEW_TEXT}). Set the environment variable PHISIKS_LOCAL_INSTALL=1 for a local test install.\")\n endif()" COMPONENT sdk)
    endif()
    install(TARGETS ${PHISIKS_SDK_TARGETS} EXPORT PhisiksTargets COMPONENT sdk
            ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
    if(PHISIKS_SDK_EXTRA_LIBS)
        install(FILES ${PHISIKS_SDK_EXTRA_LIBS} DESTINATION lib COMPONENT sdk)
    endif()
    if(PHISIKS_RUNTIME_DLLS)
        install(FILES ${PHISIKS_RUNTIME_DLLS} DESTINATION bin COMPONENT sdk)
    endif()
    install(DIRECTORY ${PHISIKS_ROOT}/engine/include/Phisiks DESTINATION include COMPONENT sdk FILES_MATCHING PATTERN "*.h")
    install(FILES ${PHISIKS_GENERATED_DIR}/include/Phisiks/Version.h DESTINATION include/Phisiks COMPONENT sdk)
    install(DIRECTORY ${PHISIKS_VENDOR}/glm/glm DESTINATION include COMPONENT sdk FILES_MATCHING PATTERN "*.hpp" PATTERN "*.inl" PATTERN "*.h")
    install(DIRECTORY ${PHISIKS_VENDOR}/entt/ DESTINATION include/entt COMPONENT sdk FILES_MATCHING PATTERN "*.hpp")
    install(FILES ${PHISIKS_VENDOR}/imgui/imgui.h ${PHISIKS_VENDOR}/imgui/imconfig.h ${PHISIKS_VENDOR}/imgui/imgui_internal.h
                  ${PHISIKS_VENDOR}/imgui/imstb_rectpack.h ${PHISIKS_VENDOR}/imgui/imstb_textedit.h ${PHISIKS_VENDOR}/imgui/imstb_truetype.h
                  ${PHISIKS_VENDOR}/imguizmo/ImGuizmo.h DESTINATION include/imgui COMPONENT sdk)
    install(FILES ${PHISIKS_ROOT}/apps/editor/src/Editor.h ${PHISIKS_ROOT}/apps/editor/src/EditorApp.h DESTINATION include/PhisiksEditor COMPONENT sdk)
    install(FILES ${PHISIKS_ROOT}/apps/pai/src/Pai.h DESTINATION include/PhisiksPAI COMPONENT sdk)
    install(EXPORT PhisiksTargets NAMESPACE Phisiks:: DESTINATION lib/cmake/phisiks FILE phisiks-targets.cmake COMPONENT sdk)
    configure_package_config_file(${PHISIKS_ROOT}/cmake/phisiks-config.cmake.in ${CMAKE_BINARY_DIR}/sdk/phisiks-config.cmake
                                  INSTALL_DESTINATION lib/cmake/phisiks)
    write_basic_package_version_file(${CMAKE_BINARY_DIR}/sdk/phisiks-config-version.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES ${CMAKE_BINARY_DIR}/sdk/phisiks-config.cmake ${CMAKE_BINARY_DIR}/sdk/phisiks-config-version.cmake DESTINATION lib/cmake/phisiks COMPONENT sdk)
    install(FILES ${PHISIKS_ROOT}/LICENSE ${PHISIKS_ROOT}/THIRD_PARTY_NOTICES.md ${PHISIKS_ROOT}/SDK.md ${PHISIKS_BUILD_INFO} DESTINATION share/phisiks COMPONENT sdk)
    if(EXISTS ${PHISIKS_VENDOR}/licenses)
        install(DIRECTORY ${PHISIKS_VENDOR}/licenses/ DESTINATION share/phisiks/THIRD_PARTY_LICENSES COMPONENT sdk)
    endif()
    install(DIRECTORY ${PHISIKS_ROOT}/examples/sdk_consumer/ DESTINATION share/phisiks/examples/sdk_consumer COMPONENT sdk)
    # ctest: install the SDK into the build folder, copy the consumer example out of the source tree,
    # configure it against the installed package only, build and run it.
    add_test(NAME SdkConsumer COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG> -DWORK=${CMAKE_BINARY_DIR}/sdk_consumer_test
        -DSOURCE=${PHISIKS_ROOT}/examples/sdk_consumer -DGENERATOR=${CMAKE_GENERATOR} -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DC_COMPILER=${CMAKE_C_COMPILER} -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${PHISIKS_ROOT}/tools/test_sdk_consumer.cmake)
    set_tests_properties(SdkConsumer PROPERTIES TIMEOUT 900)
endif()

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
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PHISIKS: a resource-aware 3D visualisation and simulation engine with an integrated editor (C++20, OpenGL)")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${PHISIKS_ROOT}/LICENSE)
set(CPACK_GENERATOR ${PHISIKS_PACKAGE_GENERATOR})
if(PHISIKS_PREVIEW_TOOLCHAIN_REASONS)
    string(APPEND PHISIKS_PACKAGE_SYSTEM "-preview-toolchain")   # never a release name
endif()
set(CPACK_PACKAGE_FILE_NAME "Phisiks-${PROJECT_VERSION}-${PHISIKS_PACKAGE_SYSTEM}")
set(CPACK_PACKAGE_DIRECTORY ${PHISIKS_ROOT}/dist)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)                                       # one Phisiks-<version>-<os>/ folder
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)                             # (component archives read this one)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)                   # runtime archive + SDK archive
set(CPACK_COMPONENTS_ALL runtime sdk)
set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
set(CPACK_ARCHIVE_SDK_FILE_NAME "Phisiks-${PROJECT_VERSION}-${PHISIKS_PACKAGE_SYSTEM}-sdk")
set(CPACK_STRIP_FILES ON)
include(CPack)
