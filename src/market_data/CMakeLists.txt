add_library(finam_market_data STATIC
    # market_data_client.cpp  # add when implemented
)
target_include_directories(finam_market_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_market_data PUBLIC
    finam_core
    finam_auth
    gRPC::grpc++
    spdlog::spdlog
)
