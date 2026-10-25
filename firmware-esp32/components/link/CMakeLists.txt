idf_component_register(
    SRCS "link.c" "link_dispatch.c"
    INCLUDE_DIRS "include"
    REQUIRES proto
    PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag esp_timer
)
# snprintf truncation into fixed protocol buffers is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
