add_library(finam_market_data STATIC
    market_data_client.cpp
)

target_include_directories(finam_market_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(finam_market_data
    PUBLIC
        finam_auth
        finam_proto
        gRPC::grpc++
        spdlog::spdlog
    PRIVATE
        project_warnings
)
