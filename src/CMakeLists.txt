add_subdirectory(core)
add_subdirectory(auth)

add_executable(finam_bot main.cpp)

target_link_libraries(finam_bot PRIVATE
    finam_core
    finam_auth
    finam_proto
    spdlog::spdlog
)

target_link_libraries(finam_bot PRIVATE project_warnings)

target_compile_definitions(finam_bot PRIVATE SPDLOG_FMT_EXTERNAL)