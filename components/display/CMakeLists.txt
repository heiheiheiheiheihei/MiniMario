idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    "." 
    PRIV_REQUIRES esp_lcd driver esp_timer
)
