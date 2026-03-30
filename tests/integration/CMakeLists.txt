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
    gRPC::grpc++
    protobuf::libprotobuf
    spdlog::spdlog
    project_warnings
)

include(CTest)
include(Catch)
catch_discover_tests(integration_tests
    TEST_PREFIX "integration/"
)
