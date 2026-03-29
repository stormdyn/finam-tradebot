# ── protoc из Conan (та же версия что libprotobuf) ──────────────────────────
get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protobuf::protoc target has no IMPORTED_LOCATION. Run conan install first.")
endif()
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)

message(STATUS "protoc (Conan):   ${PROTOC_EXECUTABLE}")
message(STATUS "grpc_cpp_plugin:  ${GRPC_CPP_PLUGIN}")

# ── Пути ────────────────────────────────────────────────────────────────────
# contracts/ — корень: содержит proto/tradeapi/v1/*.proto и google/type/date.proto
set(FINAM_PROTO_ROOT ${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts)

# well-known types из Conan (google/protobuf/timestamp.proto и т.д.)
get_target_property(PROTOBUF_INCLUDE_DIR protobuf::libprotobuf
    INTERFACE_INCLUDE_DIRECTORIES)
list(GET PROTOBUF_INCLUDE_DIR 0 PROTOBUF_INCLUDE_DIR)
message(STATUS "Protobuf include:  ${PROTOBUF_INCLUDE_DIR}")

set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated/proto)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR}/proto/tradeapi/v1)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR}/google/type)

# ── Базовые аргументы protoc ─────────────────────────────────────────────────
set(PROTOC_BASE_ARGS
    --proto_path=${FINAM_PROTO_ROOT}
    --proto_path=${PROTOBUF_INCLUDE_DIR}
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
)

# ── 1. Генерируем google/type/date.pb.{h,cc} ─────────────────────────────────
set(DATE_PROTO ${FINAM_PROTO_ROOT}/google/type/date.proto)
set(DATE_PB_CC ${PROTO_GEN_DIR}/google/type/date.pb.cc)
set(DATE_PB_H  ${PROTO_GEN_DIR}/google/type/date.pb.h)

if(NOT EXISTS ${DATE_PROTO})
    message(FATAL_ERROR "Missing: ${DATE_PROTO}\nRun: mkdir -p vendor/finam-docs/contracts/google/type && curl -Lo vendor/finam-docs/contracts/google/type/date.proto https://raw.githubusercontent.com/googleapis/googleapis/master/google/type/date.proto")
endif()

add_custom_command(
    OUTPUT  ${DATE_PB_CC} ${DATE_PB_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${FINAM_PROTO_ROOT}
            --proto_path=${PROTOBUF_INCLUDE_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            ${DATE_PROTO}
    DEPENDS ${DATE_PROTO}
    COMMENT "Generating google/type/date.pb"
    VERBATIM
)

# ── 2. Генерируем proto/tradeapi/v1/*.pb.{h,cc} ──────────────────────────────
file(GLOB_RECURSE PROTO_FILES
    "${FINAM_PROTO_ROOT}/proto/tradeapi/v1/*.proto"
)
if(NOT PROTO_FILES)
    message(FATAL_ERROR "No .proto files found in ${FINAM_PROTO_ROOT}/proto/tradeapi/v1/")
endif()

set(PROTO_SRCS ${DATE_PB_CC})
set(PROTO_HDRS ${DATE_PB_H})

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PB_CC   ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.pb.cc)
    set(PB_H    ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.pb.h)
    set(GRPC_CC ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.grpc.pb.cc)
    set(GRPC_H  ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.grpc.pb.h)

    list(APPEND PROTO_SRCS ${PB_CC} ${GRPC_CC})
    list(APPEND PROTO_HDRS ${PB_H}  ${GRPC_H})

    add_custom_command(
        OUTPUT  ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${PROTOC_EXECUTABLE} ${PROTOC_BASE_ARGS} ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} ${DATE_PB_H}
        COMMENT "Generating protobuf/gRPC for ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

# ── Библиотека ───────────────────────────────────────────────────────────────
add_library(finam_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(finam_proto PUBLIC
    ${PROTO_GEN_DIR}
)

target_link_libraries(finam_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)
# Сгенерированный код — без строгих warning-флагов проекта
