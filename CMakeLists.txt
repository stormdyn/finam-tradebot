cmake_minimum_required(VERSION 3.25)
project(finam_bot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Важно для твоего LSP (clangd)

# Подключаем зависимости из Conan
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

# Строгие флаги компилятора для безопасности и чистоты кода
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion -Wcast-align
    -Wformat=2 -Wshadow
)

add_subdirectory(proto)
add_subdirectory(src)
# add_subdirectory(tests)
