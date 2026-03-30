add_executable(unit_tests
    test_backoff.cpp
    test_risk.cpp
    test_session_context.cpp
    test_confluence.cpp
    test_contract.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated/proto
)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    finam_core
    finam_risk
    finam_strategy
    spdlog::spdlog
    project_warnings
)

# Register the unit_tests binary as a single CTest test.
# add_test() is simpler and more reliable than catch_discover_tests()
# when Catch2 is installed via Conan (CMakeDeps doesn't expose the
# Catch2 extras/ CMake module needed for catch_discover_tests).
add_test(NAME unit_tests COMMAND unit_tests)
