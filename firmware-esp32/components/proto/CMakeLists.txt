# The shared codec lives in <repo>/proto; compile it as an IDF component.
set(PROTO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../proto")
idf_component_register(
    SRCS "${PROTO_ROOT}/src/proto_frame.c" "${PROTO_ROOT}/src/proto_msg.c" "${PROTO_ROOT}/src/proto_link.c"
         "${PROTO_ROOT}/src/keyfmt.c"
    INCLUDE_DIRS "${PROTO_ROOT}/include"
)
