add_library(finam_auth STATIC
    token_manager.cpp
)

target_include_directories(finam_auth PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)


target_link_libraries(finam_auth PUBLIC
    finam_proto        # ← proto должен собраться ПЕРВЫМ
    gRPC::grpc++
    spdlog::spdlog
)

# Явная зависимость — CMake сначала генерирует proto, потом компилирует auth
add_dependencies(finam_auth finam_proto)