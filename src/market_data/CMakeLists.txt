add_library(finam_market_data INTERFACE)
target_include_directories(finam_market_data INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_market_data INTERFACE
    finam_core
    finam_auth
    gRPC::grpc++
    spdlog::spdlog
)