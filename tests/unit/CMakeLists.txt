add_executable(unit_tests
    test_backoff.cpp
    test_risk.cpp
    test_session_context.cpp
    test_confluence.cpp
    test_contract.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
    project_warnings
)

include(CTest)
include(Catch)
catch_discover_tests(unit_tests)
