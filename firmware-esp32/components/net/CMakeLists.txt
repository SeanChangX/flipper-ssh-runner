idf_component_register(
    SRCS "net.c" "join_policy.c" "ping_stats.c" "route_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES proto esp_netif
    PRIV_REQUIRES keystore esp_timer esp_eth esp_wifi esp_event lwip driver esp_driver_spi esp_driver_gpio
)
# snprintf truncation into fixed protocol buffers is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
