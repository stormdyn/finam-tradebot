add_library(finam_auth STATIC token_manager.cpp)
target_include_directories(finam_auth PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_auth PUBLIC
    finam_core
    gRPC::grpc++
    spdlog::spdlog
    project_warnings
)
