# Host unit tests for the hardware-independent parts of the ESP32 firmware.
cmake_minimum_required(VERSION 3.16)
project(fes_firmware_host_tests C)

set(CMAKE_C_STANDARD 99)
set(FW ${CMAKE_CURRENT_LIST_DIR}/..)
set(PROTO ${FW}/../proto)

add_library(fwcore STATIC
  ${FW}/components/keystore/hostkey_policy.c
  ${FW}/components/link/link_dispatch.c
  ${FW}/components/net/join_policy.c
  ${FW}/components/net/ping_stats.c
  ${FW}/components/net/route_policy.c
  ${FW}/components/bootinfo/bootstats.c
  ${FW}/components/statusled/statusled_sm.c
  ${PROTO}/src/proto_frame.c
  ${PROTO}/src/proto_msg.c
  ${PROTO}/src/proto_link.c
)
target_include_directories(fwcore PUBLIC
  ${FW}/components/bootinfo/include ${FW}/components/statusled/include ${FW}/components/keystore/include ${FW}/components/link/include ${FW}/components/net/include ${PROTO}/include ${PROTO}/tests)
target_compile_options(fwcore PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(fwcore INTERFACE -fsanitize=address,undefined)

enable_testing()
foreach(t hostkey_policy link_dispatch join_policy ping_stats route_policy bootstats statusled)
  add_executable(test_${t} test_${t}.c)
  target_link_libraries(test_${t} PRIVATE fwcore)
  target_compile_options(test_${t} PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
  add_test(NAME fw_${t} COMMAND test_${t})
endforeach()
