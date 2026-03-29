add_library(finam_risk INTERFACE)
target_include_directories(finam_risk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_risk INTERFACE
    finam_core
    spdlog::spdlog
)