/*
 * MiniMario - NES emulator for Waveshare ESP32-C6-LCD-1.47
 *
 * Stage 1: display bring-up + test pattern.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "nvs_flash.h"

#include "display.h"

static const char *TAG = "app";

static void print_board_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "chip=%s cores=%d rev=%d.%d flash=%luMB",
             CONFIG_IDF_TARGET, info.cores, info.revision / 100, info.revision % 100,
             (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "free heap=%u  DMA-capable=%u  min=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== MiniMario boot ===");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    print_board_info();

    ESP_ERROR_CHECK(display_init());
    display_set_scale(DISPLAY_SCALE_FILL);
    display_test_pattern();
    ESP_LOGI(TAG, "test pattern drawn, backlight=%d%%", display_get_backlight());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        display_stats_t st;
        display_get_stats(&st);
        ESP_LOGI(TAG, "display frames=%lu avg=%luus max=%luus heap=%u", (unsigned long)st.frames,
                 (unsigned long)st.us_avg, (unsigned long)st.us_max,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
}
