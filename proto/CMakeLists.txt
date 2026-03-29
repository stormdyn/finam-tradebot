find_program(PROTOC_EXECUTABLE protoc REQUIRED)
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)

message(STATUS "protoc: ${PROTOC_EXECUTABLE}")
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")

# Финам использует import "proto/tradeapi/v1/common.proto"
# => proto_path должен указывать на contracts/ (родитель папки proto/)
set(FINAM_PROTO_ROOT ${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts)

# Пути до well-known types из Conan (определяем динамически)
find_path(PROTOBUF_INCLUDE_DIR google/protobuf/timestamp.proto
    HINTS ${CONAN_INCLUDE_DIRS}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
)
if(NOT PROTOBUF_INCLUDE_DIR)
    # Fallback: ищем в Conan-кэше
    file(GLOB_RECURSE _TS_PROTO
        "$ENV{HOME}/.conan2/p/b/proto*/p/include/google/protobuf/timestamp.proto"
    )
    if(_TS_PROTO)
        list(GET _TS_PROTO 0 _TS_PROTO_FIRST)
        get_filename_component(PROTOBUF_INCLUDE_DIR ${_TS_PROTO_FIRST} DIRECTORY)
        get_filename_component(PROTOBUF_INCLUDE_DIR ${PROTOBUF_INCLUDE_DIR} DIRECTORY)
        get_filename_component(PROTOBUF_INCLUDE_DIR ${PROTOBUF_INCLUDE_DIR} DIRECTORY)
    endif()
endif()
message(STATUS "Protobuf include (well-known types): ${PROTOBUF_INCLUDE_DIR}")

# googleapis (google/type/date.proto)
find_path(GOOGLEAPIS_INCLUDE_DIR google/type/date.proto
    HINTS ${CONAN_INCLUDE_DIRS}
    NO_DEFAULT_PATH
)
if(NOT GOOGLEAPIS_INCLUDE_DIR)
    file(GLOB_RECURSE _DATE_PROTO
        "$ENV{HOME}/.conan2/p/b/googl*/p/include/google/type/date.proto"
        "$ENV{HOME}/.conan2/p/googl*/p/include/google/type/date.proto"
    )
    if(_DATE_PROTO)
        list(GET _DATE_PROTO 0 _DATE_PROTO_FIRST)
        get_filename_component(GOOGLEAPIS_INCLUDE_DIR ${_DATE_PROTO_FIRST} DIRECTORY)
        get_filename_component(GOOGLEAPIS_INCLUDE_DIR ${GOOGLEAPIS_INCLUDE_DIR} DIRECTORY)
        get_filename_component(GOOGLEAPIS_INCLUDE_DIR ${GOOGLEAPIS_INCLUDE_DIR} DIRECTORY)
    endif()
endif()
message(STATUS "googleapis include: ${GOOGLEAPIS_INCLUDE_DIR}")

file(GLOB_RECURSE PROTO_FILES
    "${FINAM_PROTO_ROOT}/proto/tradeapi/v1/*.proto"
)

if(NOT PROTO_FILES)
    message(FATAL_ERROR "No .proto files found in ${FINAM_PROTO_ROOT}/proto/tradeapi/v1/")
endif()

set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated/proto)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
file(MAKE_DIRECTORY ${PROTO_GEN_DIR}/proto/tradeapi/v1)

set(PROTO_SRCS "")
set(PROTO_HDRS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PB_CC   ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.pb.cc)
    set(PB_H    ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.pb.h)
    set(GRPC_CC ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.grpc.pb.cc)
    set(GRPC_H  ${PROTO_GEN_DIR}/proto/tradeapi/v1/${PROTO_NAME}.grpc.pb.h)

    list(APPEND PROTO_SRCS ${PB_CC} ${GRPC_CC})
    list(APPEND PROTO_HDRS ${PB_H}  ${GRPC_H})

    # Три proto_path: контракты Финама + protobuf well-known + googleapis
    set(PROTOC_ARGS
        --proto_path=${FINAM_PROTO_ROOT}
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    )
    if(PROTOBUF_INCLUDE_DIR)
        list(APPEND PROTOC_ARGS --proto_path=${PROTOBUF_INCLUDE_DIR})
    endif()
    if(GOOGLEAPIS_INCLUDE_DIR)
        list(APPEND PROTOC_ARGS --proto_path=${GOOGLEAPIS_INCLUDE_DIR})
    endif()
    list(APPEND PROTOC_ARGS ${PROTO_FILE})

    add_custom_command(
        OUTPUT  ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${PROTOC_EXECUTABLE} ${PROTOC_ARGS}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating protobuf/gRPC for ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

add_library(finam_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(finam_proto PUBLIC
    ${PROTO_GEN_DIR}  # #include "proto/tradeapi/v1/orders.pb.h"
)

target_link_libraries(finam_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)
# Сгенерированный код не проверяем строгими флагами
