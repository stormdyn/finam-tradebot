cmake_minimum_required(VERSION 3.25)
project(finam_tradebot CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Conan toolchain подключается через -DCMAKE_TOOLCHAIN_FILE
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

# ── Зависимости ───────────────────────────────────────────────────────────────
find_package(gRPC      REQUIRED)   # системный /usr
find_package(Protobuf  REQUIRED)   # системный /usr
find_package(spdlog    REQUIRED)   # Conan
find_package(nlohmann_json REQUIRED) # Conan

# clangd и компилятор используют одинаковый fmt
add_compile_definitions(SPDLOG_FMT_EXTERNAL)

# ── Warnings target ───────────────────────────────────────────────────────────
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow
    -Wno-unused-parameter
)

add_subdirectory(proto)
add_subdirectory(src)