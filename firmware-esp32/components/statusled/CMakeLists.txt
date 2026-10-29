idf_component_register(
    SRCS "statusled.c" "statusled_sm.c" "statusled_drv.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bootinfo net ssh proto nvs_flash esp_timer esp_driver_rmt esp_driver_gpio
)
