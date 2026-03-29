add_library(finam_order STATIC
    # order_executor.cpp  # add when implemented
)
target_include_directories(finam_order PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_order PUBLIC
    finam_core
    finam_auth
    gRPC::grpc++
    spdlog::spdlog
)
