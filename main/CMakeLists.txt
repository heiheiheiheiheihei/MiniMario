# MiniMario application component.
idf_component_register(
    SRCS "main.c" "app.c" "rom_store.c" "ui_menu.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES display common gamepad nes_port
    PRIV_REQUIRES
        esp_timer
        nvs_flash
        esp_partition
        fatfs
        esp_driver_sdspi
        esp_driver_spi
        esp_driver_gpio
        esp_driver_usb_serial_jtag
)
