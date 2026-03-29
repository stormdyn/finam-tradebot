# Пути к исполняемым файлам берём из Conan, не из системы
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)

message(STATUS "protoc: ${PROTOC_EXECUTABLE}")
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")

# Корень proto-контрактов Финама (в сабмодуле)
set(FINAM_PROTO_ROOT "${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts/proto")

# Все .proto файлы из tradeapi/v1
file(GLOB_RECURSE PROTO_FILES "${FINAM_PROTO_ROOT}/tradeapi/v1/*.proto")

if(NOT PROTO_FILES)
    message(FATAL_ERROR "No .proto files found in ${FINAM_PROTO_ROOT}/tradeapi/v1/")
endif()

# Директория для сгенерированных файлов (внутри build/, не коммитим)
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated/proto")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_SRCS "")
set(PROTO_HDRS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PB_CC   "${PROTO_GEN_DIR}/tradeapi/v1/${PROTO_NAME}.pb.cc")
    set(PB_H    "${PROTO_GEN_DIR}/tradeapi/v1/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${PROTO_GEN_DIR}/tradeapi/v1/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H  "${PROTO_GEN_DIR}/tradeapi/v1/${PROTO_NAME}.grpc.pb.h")

    list(APPEND PROTO_SRCS ${PB_CC} ${GRPC_CC})
    list(APPEND PROTO_HDRS ${PB_H}  ${GRPC_H})

    # Создаём поддиректорию для вывода заранее
    file(MAKE_DIRECTORY "${PROTO_GEN_DIR}/tradeapi/v1")

    add_custom_command(
        OUTPUT  ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${PROTOC_EXECUTABLE}
            # --proto_path = корень, чтобы import "tradeapi/v1/common.proto" резолвился корректно
            --proto_path="${FINAM_PROTO_ROOT}"
            --cpp_out="${PROTO_GEN_DIR}"
            --grpc_out="${PROTO_GEN_DIR}"
            --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
            "${PROTO_FILE}"
        DEPENDS "${PROTO_FILE}"
        COMMENT "Generating protobuf/gRPC for ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

# STATIC-библиотека: к ней линкуются модули бота
add_library(finam_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(finam_proto PUBLIC
    ${PROTO_GEN_DIR}   # #include "tradeapi/v1/orders.pb.h" работает из любого модуля
)

target_link_libraries(finam_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

# Сгенерированный код нарушает -Wconversion/-Wshadow — НЕ применяем project_warnings
# (tradeoff: строгие флаги только для нашего кода, не для protobuf-артефактов)
