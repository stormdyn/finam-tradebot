add_executable(connectivity_check main.cpp)

target_include_directories(connectivity_check PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated/proto
)

target_link_libraries(connectivity_check PRIVATE
    finam_auth
    finam_core
    finam_proto
    spdlog::spdlog
    gRPC::grpc++
    protobuf::libprotobuf
    project_warnings
)
