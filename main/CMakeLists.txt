idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES display common
    PRIV_REQUIRES esp_timer nvs_flash
)
