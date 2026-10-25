# libssh 0.11.5 via LibSSH-ESP32 5.10.0, built as a plain ESP-IDF component
# (upstream's CMakeLists requires the Arduino core). LGPL-2.1, see COPYING.
idf_component_register(
    SRC_DIRS "src" "src/threads" "src/external"
    INCLUDE_DIRS "src"
    REQUIRES mbedtls lwip pthread vfs esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format -Wno-error=char-subscripts -Wno-unused-variable
    -Wno-unused-function -Wno-error=unused-but-set-variable -Wno-error=maybe-uninitialized
    -Wno-error=stringop-truncation -Wno-error=deprecated-declarations)
# The port's sources test for the Arduino-provided ESP32 macro.
target_compile_definitions(${COMPONENT_LIB} PUBLIC ESP32=1)
