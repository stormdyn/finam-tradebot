add_library(finam_strategy INTERFACE)

target_include_directories(finam_strategy INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(finam_strategy INTERFACE
    finam_core
    spdlog::spdlog
)