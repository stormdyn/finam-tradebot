# ── gRPC plugin ───────────────────────────────────────────────────────────────
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)

# ── protoc из Conan ───────────────────────────────────────────────────────────
get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_RELEASE)
if(NOT _protoc_loc)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
endif()
if(NOT _protoc_loc OR NOT EXISTS "${_protoc_loc}")
    message(FATAL_ERROR "Cannot find Conan protoc binary")
endif()
set(PROTOC_EXECUTABLE "${_protoc_loc}")

# ── Пути ─────────────────────────────────────────────────────────────────────
# contracts/ — единый root: содержит proto/, grpc/, google/
set(FINAM_CONTRACTS ${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts)
set(PROTO_GEN_DIR   ${CMAKE_BINARY_DIR}/generated/proto)

# Protobuf well-known types (timestamp, wrappers, etc.)
get_filename_component(_protoc_bin_dir "${PROTOC_EXECUTABLE}" DIRECTORY)
get_filename_component(_pb_prefix      "${_protoc_bin_dir}"   DIRECTORY)
set(PROTOBUF_INCLUDE_DIR "${_pb_prefix}/include")

message(STATUS "protoc:           ${PROTOC_EXECUTABLE}")
message(STATUS "Protobuf include: ${PROTOBUF_INCLUDE_DIR}")
message(STATUS "grpc_cpp_plugin:  ${GRPC_CPP_PLUGIN}")
message(STATUS "contracts root:   ${FINAM_CONTRACTS}")

# ── Базовые аргументы protoc ─────────────────────────────────────────────────
# Два --proto_path: contracts/ (для finam imports) + protobuf include (для google/protobuf/)
set(PROTOC_BASE_ARGS
    --proto_path=${FINAM_CONTRACTS}
    --proto_path=${PROTOBUF_INCLUDE_DIR}
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
)

# ── Собираем список всех .proto файлов ───────────────────────────────────────
file(GLOB_RECURSE PROTO_MSG_FILES  "${FINAM_CONTRACTS}/proto/tradeapi/v1/*.proto")
file(GLOB_RECURSE PROTO_GRPC_FILES "${FINAM_CONTRACTS}/grpc/tradeapi/v1/*.proto")
set(GOOGLE_DATE_PROTO "${FINAM_CONTRACTS}/google/type/date.proto")

if(NOT PROTO_MSG_FILES)
    message(FATAL_ERROR "No .proto files in ${FINAM_CONTRACTS}/proto/tradeapi/v1/")
endif()
if(NOT PROTO_GRPC_FILES)
    message(FATAL_ERROR "No .proto files in ${FINAM_CONTRACTS}/grpc/tradeapi/v1/")
endif()

# ── Функция: добавить одну proto → custom_command ────────────────────────────
set(PROTO_SRCS "")
set(PROTO_HDRS "")

function(add_proto_file PROTO_FILE REL_DIR)
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(OUT_DIR "${PROTO_GEN_DIR}/${REL_DIR}")
    file(MAKE_DIRECTORY "${OUT_DIR}")

    set(PB_CC   "${OUT_DIR}/${PROTO_NAME}.pb.cc")
    set(PB_H    "${OUT_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${OUT_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H  "${OUT_DIR}/${PROTO_NAME}.grpc.pb.h")

    add_custom_command(
        OUTPUT  ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${PROTOC_EXECUTABLE} ${PROTOC_BASE_ARGS} ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "protoc: ${REL_DIR}/${PROTO_NAME}.proto"
        VERBATIM
    )
    set(PROTO_SRCS ${PROTO_SRCS} ${PB_CC} ${GRPC_CC} PARENT_SCOPE)
    set(PROTO_HDRS ${PROTO_HDRS} ${PB_H}  ${GRPC_H}  PARENT_SCOPE)
endfunction()

# google/type/date.proto — только pb, без grpc
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}/google/type")
set(DATE_PB_CC "${PROTO_GEN_DIR}/google/type/date.pb.cc")
set(DATE_PB_H  "${PROTO_GEN_DIR}/google/type/date.pb.h")
add_custom_command(
    OUTPUT  ${DATE_PB_CC} ${DATE_PB_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${FINAM_CONTRACTS}
            --proto_path=${PROTOBUF_INCLUDE_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            ${GOOGLE_DATE_PROTO}
    DEPENDS ${GOOGLE_DATE_PROTO}
    COMMENT "protoc: google/type/date.proto"
    VERBATIM
)
list(APPEND PROTO_SRCS ${DATE_PB_CC})
list(APPEND PROTO_HDRS ${DATE_PB_H})

# messages: proto/tradeapi/v1/
foreach(F ${PROTO_MSG_FILES})
    add_proto_file(${F} "proto/tradeapi/v1")
endforeach()

# services: grpc/tradeapi/v1/
foreach(F ${PROTO_GRPC_FILES})
    add_proto_file(${F} "grpc/tradeapi/v1")
endforeach()

# ── Библиотека ───────────────────────────────────────────────────────────────
add_library(finam_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(finam_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(finam_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
# Сгенерированный код — без строгих warning-флагов проекта