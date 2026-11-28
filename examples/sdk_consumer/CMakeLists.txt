# An application built against an INSTALLED PHISIKS SDK: nothing here refers to the PHISIKS
# source tree. Configure with the SDK's prefix on CMAKE_PREFIX_PATH:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk prefix> -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   build/Release/sdk_consumer --headless       (or build/sdk_consumer on single-config generators)
cmake_minimum_required(VERSION 3.25)
project(PhisiksSdkConsumer LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PHISIKS 1.1 CONFIG REQUIRED)
message(STATUS "PHISIKS ${PHISIKS_VERSION} (${PHISIKS_PHYSICS_BACKEND} physics) from ${PHISIKS_ROOT_DIR}")

add_executable(sdk_consumer main.cpp)
target_link_libraries(sdk_consumer PRIVATE Phisiks::Editor)   # includes Phisiks::Core and Phisiks::Cad
phisiks_copy_runtime(sdk_consumer)                           # Windows: PhisiksCad + Open CASCADE (+ PhysX) DLLs next to the exe
