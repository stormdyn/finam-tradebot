add_library(finam_order STATIC
    order_client.cpp
)

target_include_directories(finam_order PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(finam_order
    PUBLIC
        finam_auth
        finam_proto
        gRPC::grpc++
        spdlog::spdlog
    PRIVATE
        project_warnings
)