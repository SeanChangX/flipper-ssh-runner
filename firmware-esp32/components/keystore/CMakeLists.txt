idf_component_register(
    SRCS "keystore.c" "keystore_sec.c" "hostkey_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES proto nvs_flash
    PRIV_REQUIRES libssh efuse nvs_sec_provider esp_hw_support mbedtls esp_partition
)
# snprintf truncation into fixed protocol buffers is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
