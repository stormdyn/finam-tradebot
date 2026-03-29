add_library(finam_order INTERFACE)
target_include_directories(finam_order INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_order INTERFACE
    finam_core
    finam_auth
    gRPC::grpc++
    spdlog::spdlog
)