add_executable(connectivity_check main.cpp)

target_include_directories(connectivity_check PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(connectivity_check PRIVATE
    spdlog::spdlog
    gRPC::grpc++
    protobuf::libprotobuf
    project_warnings
)
