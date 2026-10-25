# MiniMario NES port / OSD layer for the ESP32-C6.
#
# Depends on the nofrendo core (components/nofrendo), ESP-IDF esp_timer and
# the shared pad.h from components/common.
idf_component_register(
    SRCS "nes_port.c"
    INCLUDE_DIRS "include"
    REQUIRES nofrendo common
    PRIV_REQUIRES esp_timer
)
