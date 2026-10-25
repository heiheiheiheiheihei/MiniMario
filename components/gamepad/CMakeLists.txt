idf_component_register(
    SRCS "gamepad.c" "gamepad_map.c"
    INCLUDE_DIRS "include"
    REQUIRES common
    PRIV_REQUIRES bluepad32 btstack driver nvs_flash esp_timer
)
