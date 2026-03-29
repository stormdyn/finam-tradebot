add_subdirectory(core)
add_subdirectory(auth)
add_subdirectory(market_data)
add_subdirectory(risk)
add_subdirectory(order)

add_executable(finam_bot main.cpp)
target_include_directories(finam_bot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(finam_bot PRIVATE
    finam_core
    finam_auth
    finam_market_data
    finam_risk
    finam_order
    spdlog::spdlog
    project_warnings
)
