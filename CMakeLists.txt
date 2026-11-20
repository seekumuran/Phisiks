# Phisiks — CMake build (Windows / MSVC x64).
#
# Targets
#   PhisiksCore        static library: the engine (engine/include public API, engine/src private)
#   PhisiksLab         interactive sample app (apps/lab) + runtime verification harness (tests/runtime)
#   PhisiksUnitTests   headless unit tests (tests/unit)
#   assetgen           regenerates the CC0 sample assets in assets/
#   glprobe, stacksample, check_layering   tools

cmake_minimum_required(VERSION 3.25)
project(Phisiks LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Phisiks currently requires MSVC targeting x64 (prebuilt vendor libraries are MSVC x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The prebuilt PhysX libraries use the dynamic CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(PHISIKS_ROOT   ${CMAKE_CURRENT_SOURCE_DIR})
set(PHISIKS_VENDOR ${PHISIKS_ROOT}/vendor)
set(PHISIKS_LIBCFG $<IF:$<CONFIG:Debug>,debug,release>)

# Third-party code (see THIRD_PARTY_NOTICES.md): glad (GL loader), GLFW (window), glm (math),
# EnTT (ECS), stb_image / stb_image_write (image codecs), NVIDIA PhysX 5 (physics).

# ---------------------------------------------------------------------------------------------
# PhisiksCore
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE PHISIKS_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${PHISIKS_ROOT}/engine/src/*.cpp ${PHISIKS_ROOT}/engine/src/*.h ${PHISIKS_ROOT}/engine/include/*.h)
set(PHISIKS_VENDOR_SOURCES
    ${PHISIKS_VENDOR}/glad/src/gl.c
    ${PHISIKS_VENDOR}/stb_image/stb_image.cpp)

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

add_library(PhisiksCore STATIC ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS} ${PHISIKS_EMBEDDED_SHADERS} ${PHISIKS_VENDOR_SOURCES})
source_group(TREE ${PHISIKS_ROOT}/engine PREFIX "engine" FILES ${PHISIKS_ENGINE_SOURCES} ${PHISIKS_ENGINE_SHADERS})

# Public API: engine headers + glm and EnTT (used by public headers and templates).
target_include_directories(PhisiksCore PUBLIC
    ${PHISIKS_ROOT}/engine/include
    ${PHISIKS_VENDOR}/glm
    ${PHISIKS_VENDOR}/entt)
# Private: implementation headers and backends. Apps and tests cannot include these.
target_include_directories(PhisiksCore PRIVATE
    ${PHISIKS_ROOT}/engine/src
    ${PHISIKS_GENERATED_DIR}
    ${PHISIKS_VENDOR}/glad/include
    ${PHISIKS_VENDOR}/GLFW/include
    ${PHISIKS_VENDOR}/physx/include
    ${PHISIKS_VENDOR}/physx/include/physx
    ${PHISIKS_VENDOR}/stb_image)

target_compile_definitions(PhisiksCore PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(PhisiksCore PUBLIC /utf-8 /bigobj /Zc:__cplusplus /W3)
target_compile_options(PhisiksCore PRIVATE /MP)

target_link_directories(PhisiksCore PUBLIC
    ${PHISIKS_VENDOR}/GLFW/lib/${PHISIKS_LIBCFG}
    ${PHISIKS_VENDOR}/physx/lib/${PHISIKS_LIBCFG})
target_link_libraries(PhisiksCore PUBLIC
    opengl32 glfw3 user32 gdi32 shell32 dbghelp ole32
    PhysX_64 PhysXCommon_64 PhysXCooking_64 PhysXFoundation_64
    PhysXExtensions_static_64 PhysXPvdSDK_static_64 PhysXCharacterKinematic_static_64)

# Runtime DLLs (PhysX), copied next to every executable.
file(GLOB PHISIKS_RUNTIME_DLLS ${PHISIKS_VENDOR}/DLLs/*.dll)
function(phisiks_executable target)
    target_link_libraries(${target} PRIVATE PhisiksCore)
    set_target_properties(${target} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY ${PHISIKS_ROOT}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          ${PHISIKS_ROOT}/Build/bin/Debug
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        ${PHISIKS_ROOT}/Build/bin/Release
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${PHISIKS_ROOT}/Build/bin/RelWithDebInfo)
    foreach(dll ${PHISIKS_RUNTIME_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:${target}>)
    endforeach()
endfunction()

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
set_target_properties(assetgen PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PHISIKS_ROOT}/Build/bin/Release)

# glprobe: checks whether the current GPU/driver meets the renderer's requirements.
add_executable(glprobe ${PHISIKS_ROOT}/tools/glprobe/glprobe.cpp ${PHISIKS_VENDOR}/glad/src/gl.c)
target_include_directories(glprobe PRIVATE ${PHISIKS_VENDOR}/glad/include ${PHISIKS_VENDOR}/GLFW/include)
target_link_directories(glprobe PRIVATE ${PHISIKS_VENDOR}/GLFW/lib/${PHISIKS_LIBCFG})
target_link_libraries(glprobe PRIVATE opengl32 glfw3 user32 gdi32 shell32)
set_target_properties(glprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PHISIKS_ROOT}/Build/bin/Release)

# stacksample: symbolized stack sampler for diagnosing hangs without a debugger.
add_executable(stacksample ${PHISIKS_ROOT}/tools/stacksample/stacksample.cpp)
target_link_libraries(stacksample PRIVATE dbghelp)
set_target_properties(stacksample PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PHISIKS_ROOT}/Build/bin/Release)

# ---------------------------------------------------------------------------------------------
# Tests (ctest -C Release --test-dir Build/vs2026)
# ---------------------------------------------------------------------------------------------
enable_testing()
file(GLOB PHISIKS_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${PHISIKS_ROOT}/tests/unit/*.cpp ${PHISIKS_ROOT}/tests/unit/*.h)
add_executable(PhisiksUnitTests ${PHISIKS_UNIT_TEST_SOURCES})
# White-box access to engine-private headers (e.g. presentation backend interfaces for recording fakes).
target_include_directories(PhisiksUnitTests PRIVATE ${PHISIKS_ROOT}/engine/src)
phisiks_executable(PhisiksUnitTests)

add_test(NAME PhisiksUnitTests COMMAND PhisiksUnitTests)
add_test(NAME Layering COMMAND ${CMAKE_COMMAND} -DPHISIKS_ROOT=${PHISIKS_ROOT} -P ${PHISIKS_ROOT}/tools/check_layering.cmake)
# Deterministic simulation: two headless runs must export byte-identical data; a hidden-window
# run must export PNG frames.
add_test(NAME DeterministicReplay
    COMMAND ${CMAKE_COMMAND} -DLAB=$<TARGET_FILE:PhisiksLab> -DWORK=${CMAKE_BINARY_DIR}/replay_test
            -P ${PHISIKS_ROOT}/tools/test_deterministic_replay.cmake
    WORKING_DIRECTORY ${PHISIKS_ROOT})
