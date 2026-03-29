add_library(finam_core INTERFACE)
target_include_directories(finam_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(finam_core INTERFACE spdlog::spdlog nlohmann_json::nlohmann_json)