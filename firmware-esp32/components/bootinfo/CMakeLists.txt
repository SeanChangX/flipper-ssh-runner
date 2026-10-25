idf_component_register(
    SRCS "bootinfo.c" "bootstats.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_system
)
