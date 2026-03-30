# Интеграционные тесты с mock gRPC-сервером.
# Работают без FINAM_SECRET_TOKEN, не требуют сети.
add_executable(integration_tests
    test_auth_flow.cpp
)

target_include_directories(integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}  # для сгенерированных proto
)

target_link_libraries(integration_tests PRIVATE
    Catch2::Catch2WithMain
    finam_auth
    finam_core
    finam_proto
    gRPC::grpc++
    protobuf::libprotobuf
    spdlog::spdlog
    project_warnings
)

add_test(NAME integration_tests COMMAND integration_tests)
