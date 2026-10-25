# Host build of the shared protocol codec and its unit tests.
#   cmake -S proto -B proto/build && cmake --build proto/build && ctest --test-dir proto/build
cmake_minimum_required(VERSION 3.16)
project(flipper_eth_ssh_proto C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(proto STATIC src/proto_frame.c src/proto_msg.c src/proto_link.c src/keyfmt.c)
target_include_directories(proto PUBLIC include)
target_compile_options(proto PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic)

option(PROTO_SANITIZE "Build tests with ASan/UBSan" ON)

enable_testing()
foreach(t frame msg link keyfmt)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE proto)
  target_compile_options(test_${t} PRIVATE -Wall -Wextra -Werror)
  if(PROTO_SANITIZE)
    target_compile_options(test_${t} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
    target_compile_options(proto PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(proto INTERFACE -fsanitize=address,undefined)
  endif()
  add_test(NAME proto_${t} COMMAND test_${t})
endforeach()
