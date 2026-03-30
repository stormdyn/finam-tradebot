find_program(PROTOC_EXECUTABLE  protoc          REQUIRED)
find_program(GRPC_CPP_PLUGIN    grpc_cpp_plugin REQUIRED)

message(STATUS "[proto] protoc: ${PROTOC_EXECUTABLE}")
message(STATUS "[proto] plugin: ${GRPC_CPP_PLUGIN}")

set(PROTO_ROOT    "${CMAKE_SOURCE_DIR}/vendor/finam-trade-api/proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated/proto")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

# API живёт в v1, не v2
if(NOT EXISTS "${PROTO_ROOT}/grpc/tradeapi/v1/auth/auth_service.proto")
    message(FATAL_ERROR "Proto files not found! Run: git submodule update --init --recursive")
endif()

# Всё кроме google/protobuf/* — они уже в системном protobuf
file(GLOB_RECURSE _ALL_PROTOS "${PROTO_ROOT}/*.proto")
set(ALL_COMPILE_PROTOS "")
foreach(F ${_ALL_PROTOS})
    if(NOT F MATCHES "/google/protobuf/")
        list(APPEND ALL_COMPILE_PROTOS "${F}")
    endif()
endforeach()

set(_PROTO_SRCS "")

foreach(PROTO_FILE ${ALL_COMPILE_PROTOS})
    file(RELATIVE_PATH REL "${PROTO_ROOT}" "${PROTO_FILE}")
    get_filename_component(REL_DIR "${REL}" DIRECTORY)
    get_filename_component(STEM    "${REL}" NAME_WE)

    set(OUT_DIR "${PROTO_GEN_DIR}/${REL_DIR}")
    file(MAKE_DIRECTORY "${OUT_DIR}")

    set(PB_CC "${OUT_DIR}/${STEM}.pb.cc")
    set(PB_H  "${OUT_DIR}/${STEM}.pb.h")
    set(GR_CC "${OUT_DIR}/${STEM}.grpc.pb.cc")
    set(GR_H  "${OUT_DIR}/${STEM}.grpc.pb.h")

    add_custom_command(
        OUTPUT  "${PB_CC}" "${PB_H}" "${GR_CC}" "${GR_H}"
        COMMAND "${PROTOC_EXECUTABLE}"
                "--proto_path=${PROTO_ROOT}"
                "--cpp_out=${PROTO_GEN_DIR}"
                "--grpc_out=${PROTO_GEN_DIR}"
                "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
                "${PROTO_FILE}"
        DEPENDS "${PROTO_FILE}"
        COMMENT "protoc → ${REL}"
        VERBATIM
    )
    list(APPEND _PROTO_SRCS "${PB_CC}" "${GR_CC}")
endforeach()

add_library(finam_proto STATIC ${_PROTO_SRCS})
target_include_directories(finam_proto PUBLIC "${PROTO_GEN_DIR}")
target_link_libraries(finam_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
target_compile_options(finam_proto PRIVATE -w)
