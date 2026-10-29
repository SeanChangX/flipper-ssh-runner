idf_component_register(
    SRCS "main.c" "app.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES proto keystore net ssh link console esp_timer bootinfo statusled esp_app_format
)
# snprintf truncation into fixed protocol buffers is intentional.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
