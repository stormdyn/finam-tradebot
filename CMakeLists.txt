cmake_minimum_required(VERSION 3.25)
project(finam_tradebot CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Conan toolchain подключается через -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

# ── Зависимости ───────────────────────────────────────────────────────────────
find_package(gRPC          REQUIRED)   # системный /usr
find_package(Protobuf      REQUIRED)   # системный /usr
find_package(spdlog        REQUIRED)   # Conan
find_package(fmt           REQUIRED)   # Conan — явный fmt для SPDLOG_FMT_EXTERNAL
find_package(nlohmann_json REQUIRED)   # Conan

# spdlog использует внешний fmt (Conan fmt/11.1.4), clangd тоже
add_compile_definitions(SPDLOG_FMT_EXTERNAL)

# ── Warnings target ───────────────────────────────────────────────────────────
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow
    -Wno-unused-parameter
)

add_subdirectory(proto)
add_subdirectory(src)
