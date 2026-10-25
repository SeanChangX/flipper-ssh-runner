idf_component_register(
    SRCS "ssh_client.c"
    INCLUDE_DIRS "include"
    REQUIRES proto
    PRIV_REQUIRES libssh keystore net esp_timer
)
# snprintf truncation into fixed protocol buffers is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
