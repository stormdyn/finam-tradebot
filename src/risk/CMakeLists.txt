add_library(finam_risk STATIC
    # risk_manager.cpp  # add when implemented
)
target_include_directories(finam_risk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(finam_risk PUBLIC
    finam_core
    spdlog::spdlog
)
