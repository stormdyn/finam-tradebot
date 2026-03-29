add_library(finam_core INTERFACE)
target_include_directories(finam_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_core INTERFACE project_warnings)
