cmake_minimum_required(VERSION 3.25)
project(finam_tradebot CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

# ── Зависимости ───────────────────────────────────────────────────────────────────
find_package(gRPC          REQUIRED)
find_package(Protobuf      REQUIRED)
find_package(spdlog        REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Catch2        REQUIRED)

# ── Warnings target ──────────────────────────────────────────────────────────────
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow
    -Wno-unused-parameter
)

add_subdirectory(proto)
add_subdirectory(src)
add_subdirectory(src/connectivity_check)
add_subdirectory(tests)
