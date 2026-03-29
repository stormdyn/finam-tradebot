# Используем системный protoc — он должен совпадать с версией libprotobuf из Conan
# protoc 33.1 == protobuf 5.29.x
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)

message(STATUS "protoc:          ${PROTOC_EXECUTABLE}")
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")

# Финам: import "proto/tradeapi/v1/common.proto"
# => proto_path = contracts/ (родитель папки proto/)
set(FINAM_PROTO_ROOT ${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts)

# well-known types (google/protobuf/timestamp.proto) из Conan
find_path(PROTOBUF_INCLUDE_DIR google/protobuf/timestamp.proto
    HINTS ${CONAN_INCLUDE_DIRS}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
)
if(NOT PROTOBUF_INCLUDE_DIR)
    file(GLOB_RECURSE _TS_PROTO
        "$ENV{HOME}/.conan2/p/b/proto*/p/include/google/protobuf/timestamp.proto"
        "$ENV{HOME}/.conan2/p/proto*/p/include/google/protobuf/timestamp.proto"
    )
    if(_TS_PROTO)
        list(GET _TS_PROTO 0 _first)
        cmake_path(GET _first PARENT_PATH _d)
        cmake_path(GET _d PARENT_PATH _d)
        cmake_path(GET _d PARENT_PATH PROTOBUF_INCLUDE_DIR)
    endif()
endif()
message(STATUS "Protobuf well-known: ${PROTOBUF_INCLUDE_DIR}")

# googleapis (google/type/date.proto) — лежит в vendor/ после curl
set(GOOGLEAPIS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/vendor/finam-docs/contracts)
message(STATUS "googleapis:          ${GOOGLEAPIS_INCLUDE_DIR}")

file(GLOB_RECURSE PROTO_FILES
    "${FINAM_PROTO_ROOT}/proto/tradeapi/v1/*.proto"
)
if(NOT PROTO_FILES)
    message(FATAL_ERROR "No .proto files found in ${FINAM_PROTO_ROOT}/proto/tradeapi/v1/")
endif()

set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated/proto)
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

    set(PROTOC_ARGS
        --proto_path=${FINAM_PROTO_ROOT}
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    )
    if(PROTOBUF_INCLUDE_DIR)
        list(APPEND PROTOC_ARGS --proto_path=${PROTOBUF_INCLUDE_DIR})
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
    ${PROTO_GEN_DIR}
)

target_link_libraries(finam_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)
# Сгенерированный protobuf-код — без строгих warning-флагов проекта
