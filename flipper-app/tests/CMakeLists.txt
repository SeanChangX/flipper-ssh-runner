# Host unit tests for the Furi-free parts of the SSH Runner app (flipper-app/core).
cmake_minimum_required(VERSION 3.16)
project(ssh_runner_core_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# POSIX bits (strcasecmp), as provided by newlib on the Flipper
add_compile_definitions(_POSIX_C_SOURCE=200809L)

set(APP ${CMAKE_CURRENT_LIST_DIR}/..)
add_library(core STATIC
  ${APP}/core/ffkv.c
  ${APP}/core/cstr.c
  ${APP}/core/profile.c
  ${APP}/core/textproc.c
  ${APP}/core/wrap.c
  ${APP}/core/fontw.c
  ${APP}/core/heapcheck.c
  ${APP}/core/keydir.c
  ${APP}/views/kbd_layout.c
  ${APP}/core/session.c
  ${APP}/core/scanlist.c
  ${APP}/core/mock_backend.c
  ${APP}/lib/proto/src/proto_frame.c
  ${APP}/lib/proto/src/proto_msg.c
  ${APP}/lib/proto/src/proto_link.c
  ${APP}/lib/proto/src/keyfmt.c
)
# as in application.fam: the FAP's keyfmt has no DER parser
set_source_files_properties(${APP}/lib/proto/src/keyfmt.c PROPERTIES COMPILE_DEFINITIONS KEYFMT_NO_DER)
target_include_directories(core PUBLIC ${APP}/core ${APP}/views ${APP}/lib/proto/include ${APP}/../proto/tests)
target_compile_options(core PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(core INTERFACE -fsanitize=address,undefined)

enable_testing()
foreach(t profile textproc session wrap kbd heapcheck keydir)
  add_executable(test_${t} test_${t}.c)
  target_link_libraries(test_${t} PRIVATE core)
  target_compile_options(test_${t} PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
  add_test(NAME app_${t} COMMAND test_${t})
endforeach()
# the files the PC config tool (cli/) writes, read and written back here byte for byte
target_compile_definitions(test_profile PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/golden")

# Render test: the whole app (not backend_uart.c) on a fake SDK that records what is drawn,
# driven like the on-device walks; every frame is checked (see test_render.c).
file(GLOB APP_SCENES ${APP}/scenes/*.c)
add_executable(test_render
  test_render.c
  fake_gui/fake.c
  ${APP}/backend_none.c
  ${APP}/ssh_runner.c
  ${APP}/store.c
  ${APP}/views/textview.c
  ${APP}/views/kbd.c
  ${APP}/views/home_view.c
  ${APP}/views/progress_view.c
  ${APP}/views/wifi_list_view.c
  ${APP}/views/tofu.c
  ${APP_SCENES}
)
target_include_directories(test_render PRIVATE ${CMAKE_CURRENT_LIST_DIR}/fake_gui ${APP} ${APP}/scenes)
target_link_libraries(test_render PRIVATE core m)
target_compile_options(test_render PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
  -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(test_render PRIVATE
  -Wl,--wrap=text_view_set_text,--wrap=text_view_set_ref,--wrap=text_view_reset)
add_test(NAME app_render COMMAND test_render WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
