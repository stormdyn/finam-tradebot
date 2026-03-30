add_executable(backtest_runner
    backtest_runner.cpp
    backtest_main.cpp
)

target_include_directories(backtest_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(backtest_runner PRIVATE
    spdlog::spdlog
    project_warnings
)
