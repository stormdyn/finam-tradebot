add_subdirectory(core)
add_subdirectory(auth)
add_subdirectory(market_data)
add_subdirectory(order)
add_subdirectory(strategy)        # OFI / MLOFI / TFI / SpoofFilter

add_executable(finam_tradebot main.cpp)

target_link_libraries(finam_tradebot PRIVATE
    finam_core
    finam_auth
    finam_market_data
    finam_order
    finam_strategy               # header-only, подключает всё из src/strategy/
    spdlog::spdlog
)

target_include_directories(finam_tradebot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
