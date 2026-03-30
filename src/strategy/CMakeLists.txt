# Header-only library — все компоненты стратегии в .hpp
# Линкуется к finam_tradebot через target_link_libraries
add_library(finam_strategy INTERFACE)

target_include_directories(finam_strategy INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # даёт #include "strategy/xxx.hpp"
)

target_link_libraries(finam_strategy INTERFACE
    finam_core
)
