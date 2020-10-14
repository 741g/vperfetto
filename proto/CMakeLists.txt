protobuf_generate_cpp(PROTO_SRC PROTO_HEADER perfetto_trace.proto)

add_library(perfetto_trace ${PROTO_HEADER} ${PROTO_SRC})
target_include_directories(perfetto_trace PUBLIC ${PROTOBUF_INCLUDE_DIR})
